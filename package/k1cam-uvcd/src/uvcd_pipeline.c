/*
 * uvcd_pipeline.c -- the stream: framesource + encoder channels on the camera
 *
 * A stream exists only while a host streams. It leases the camera
 * (uvcd_camera.c), which sets the sensor rate and the view for the
 * negotiated output size, then creates one framesource channel cropped to
 * that view and one encoder group/channel, and runs the pump thread that
 * hands encoded frames to the gadget. Stopping tears all of that down and
 * releases the camera back to idle; the camera itself stays up.
 *
 * Also here: which controls the daemon starts with (load_controls), and the
 * camera-side crash guard.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>

#include "uvcd.h"
#include "uvcd_config.h"

/* max_size is about 1 byte per pixel (at least 384 KB): MJPEG at quality
 * 100 runs about 0.45 B/px on the K1, and 0.9 B/px with sharpness at its
 * 180 cap, so this is headroom, not a target. It sizes the gadget's buffers
 * and the host's (dwMaxVideoFrameSize); a frame over it is dropped.
 *
 * max_zoom, max_zoom_slow: last zoom that still streamed / first that
 * broke the ISP. At 200 MHz: 1080p 300/310, everything else streams at 400.
 * At 100 MHz: 1080p 131/137, 1280x960 250/300, 720p 342/345; 800x600,
 * 640x480 and 640x360 stream at 400. */
const struct uvcd_frame_info uvcd_frames[UVCD_NUM_FRAMES + 1] = {
	[0] = {0, 0, 0, 0, 0, 0},
	[UVCD_FRAME_1080P] = {1920, 1080, 2048 * 1024, 3000000, 290, 125},
	[UVCD_FRAME_1280X960] = {1280, 960, 1280 * 1024, 2400000, 400, 240},
	[UVCD_FRAME_720P] = {1280, 720, 1024 * 1024, 1800000, 400, 330},
	[UVCD_FRAME_800X600] = {800, 600, 512 * 1024, 1200000, 400, 400},
	[UVCD_FRAME_640X480] = {640, 480, 512 * 1024, 1000000, 400, 400},
	[UVCD_FRAME_360P] = {640, 360, 384 * 1024, 700000, 400, 400},
};

const uint32_t uvcd_intervals[UVCD_NUM_INTERVALS] = {
	333333,  /* 30 fps */
	400000,  /* 25 fps */
	500000,  /* 20 fps */
	666666,  /* 15 fps */
	1000000, /* 10 fps */
	2000000, /*  5 fps */
};

uint32_t uvcd_interval_fps(uint32_t interval)
{
	if (interval == 0)
		return 30;
	return (10000000u + interval / 2) / interval;
}

uint32_t uvcd_snap_interval(uint32_t interval)
{
	uint32_t best = uvcd_intervals[0];
	uint32_t best_diff = UINT32_MAX;

	for (int i = 0; i < UVCD_NUM_INTERVALS; i++) {
		uint32_t d = interval > uvcd_intervals[i] ? interval - uvcd_intervals[i]
							   : uvcd_intervals[i] - interval;
		if (d < best_diff) {
			best = uvcd_intervals[i];
			best_diff = d;
		}
	}
	return best;
}

/* The pump's copy of the newest frame; at least the largest max_size. */
#define UVCD_FRAME_BUF_CAP (2048 * 1024)

/* Controls persist by default -- the daemon writes the file back shortly
 * after any accepted change -- else the compiled-in neutral defaults. They
 * are held in p->controls; the camera applies them at bring-up and every
 * change live.
 *
 * A run that crashed before proving its controls (the crash guard,
 * uvcd_config.h) points at whatever changed since the last healthy stream,
 * so go back to the controls that stream proved. Only a run of them points
 * at those too. The two guard files each count the runs in a row; the
 * larger count is the streak. */
static int load_controls(uvcd_pipeline_t *p)
{
	uvcd_config_load(UVCD_CONFIG_GOOD_PATH, &p->good);
	int stream_streak = uvcd_config_streak_read(UVCD_CONFIG_DIRTY_PATH);
	int camera_streak = uvcd_config_streak_read(UVCD_CAMERA_GUARD_PATH);
	p->unproven_streak = stream_streak > camera_streak ? stream_streak : camera_streak;
	if (p->unproven_streak >= UVCD_CONFIG_UNPROVEN_LIMIT) {
		LOGW("%d runs in a row crashed before proving their controls; using factory controls",
		     p->unproven_streak);
		uvcd_config_defaults(&p->controls);
		p->good = p->controls;
		/* Keep the rejected values for diagnosis, but never reload them. */
		if (rename(UVCD_CONFIG_PATH, UVCD_CONFIG_PATH ".rejected") != 0 && errno != ENOENT) {
			LOGE("cannot quarantine unsafe config: %s", strerror(errno));
			return -1;
		}
		if (unlink(UVCD_CONFIG_GOOD_PATH) != 0 && errno != ENOENT)
			LOGW("clear %s: %s", UVCD_CONFIG_GOOD_PATH, strerror(errno));
		if (unlink(UVCD_CONFIG_DIRTY_PATH) != 0 && errno != ENOENT)
			LOGW("clear %s: %s", UVCD_CONFIG_DIRTY_PATH, strerror(errno));
		if (unlink(UVCD_CAMERA_GUARD_PATH) != 0 && errno != ENOENT)
			LOGW("clear %s: %s", UVCD_CAMERA_GUARD_PATH, strerror(errno));
		p->unproven_streak = 0;
	} else if (p->unproven_streak) {
		LOGW("previous run crashed before proving its controls (%d of %d); "
		     "using the last proven controls",
		     p->unproven_streak, UVCD_CONFIG_UNPROVEN_LIMIT);
		p->controls = p->good;
		/* Or the next start would load the suspect values again. */
		if (uvcd_config_save(UVCD_CONFIG_PATH, &p->controls) != 0)
			LOGW("persist %s: %s", UVCD_CONFIG_PATH, strerror(errno));
	} else {
		uvcd_config_load(UVCD_CONFIG_PATH, &p->controls);
	}
	return 0;
}

/* --------------------------------------------------------------------------
 * Camera-side crash guard (uvcd_config.h). The camera applies stored
 * controls at bring-up and every change made outside a stream, where no
 * stream guard is watching; a bad one can crash or hang the daemon right
 * there. The file is in RAM, so only a restart within the same boot finds
 * it: unplugging the camera, which is also its power, never counts.
 */
static void camera_guard_arm(uvcd_pipeline_t *p)
{
	p->camera_guard_since_ms = uvcd_monotonic_ms();
	if (p->camera_guard_armed)
		return;
	p->camera_guard_armed = true;
	if (uvcd_config_streak_write(UVCD_CAMERA_GUARD_PATH, p->unproven_streak + 1) != 0)
		LOGW("crash guard %s: %s", UVCD_CAMERA_GUARD_PATH, strerror(errno));
}

void uvcd_pipeline_camera_guard_arm(uvcd_pipeline_t *p)
{
	if (p->cam.up) /* a camera that is down takes nothing in */
		camera_guard_arm(p);
}

/* Proven, or released by a clean exit (which is not a crash). */
static void camera_guard_clear(uvcd_pipeline_t *p, bool proven)
{
	if (!p->camera_guard_armed)
		return;
	p->camera_guard_armed = false;
	if (proven)
		p->unproven_streak = 0;
	if (unlink(UVCD_CAMERA_GUARD_PATH) != 0 && errno != ENOENT)
		LOGW("clear %s: %s", UVCD_CAMERA_GUARD_PATH, strerror(errno));
}

/* The camera ran UVCD_CONFIG_HEALTHY_MS on what it was given. */
static void camera_guard_tick(uvcd_pipeline_t *p)
{
	if (p->camera_guard_armed && p->cam.up &&
	    uvcd_monotonic_ms() - p->camera_guard_since_ms >= UVCD_CONFIG_HEALTHY_MS)
		camera_guard_clear(p, true);
}

int uvcd_pipeline_init(uvcd_pipeline_t *p)
{
	memset(p, 0, sizeof(*p));

	pthread_mutex_init(&p->frame.lock, NULL);
	p->frame.data = malloc(UVCD_FRAME_BUF_CAP);
	if (!p->frame.data) {
		LOGE("frame buffer alloc failed");
		return -1;
	}
	p->frame.size = UVCD_FRAME_BUF_CAP;

	if (load_controls(p) != 0 || uvcd_camera_init(&p->cam, &p->controls) != 0) {
		free(p->frame.data);
		return -1;
	}

	/* Start measuring before the first host streams. A camera that won't
	 * come up is not fatal: the gadget still enumerates and answers, and
	 * the first stream tries again. */
	camera_guard_arm(p);
	if (uvcd_camera_up(&p->cam) != RSS_OK) {
		LOGW("camera did not come up; trying again at the first stream");
		camera_guard_clear(p, false);
	}
	return 0;
}

void uvcd_pipeline_deinit(uvcd_pipeline_t *p)
{
	uvcd_pipeline_stop(p);
	uvcd_camera_deinit(&p->cam);
	camera_guard_clear(p, false);
	pthread_mutex_destroy(&p->frame.lock);
	free(p->frame.data);
}

/* ── Pump thread: block on encoder, publish frames into the shared slot ── */

static uint16_t primary_nal_type(const rss_frame_t *f)
{
	for (uint32_t i = f->nal_count; i > 0; i--) {
		rss_nal_type_t t = f->nals[i - 1].type;
		if (t == RSS_NAL_H264_IDR || t == RSS_NAL_H264_SLICE || t == RSS_NAL_JPEG_FRAME)
			return (uint16_t)t;
	}
	return f->nal_count ? (uint16_t)f->nals[0].type : (uint16_t)RSS_NAL_UNKNOWN;
}

static void *pump_thread(void *arg)
{
	uvcd_pipeline_t *p = arg;

	LOGD("pump thread started");
	while (p->pump_run) {
		int ret = RSS_HAL_CALL(p->cam.ops, enc_poll, p->cam.hal_ctx, UVCD_ENC_CHN, 1000);
		if (ret != RSS_OK)
			continue; /* normal: sensor idle / timeout */

		rss_frame_t frame;
		ret = RSS_HAL_CALL(p->cam.ops, enc_get_frame, p->cam.hal_ctx, UVCD_ENC_CHN, &frame);
		if (ret != RSS_OK)
			continue;

		uint32_t total = 0;
		for (uint32_t n = 0; n < frame.nal_count; n++)
			total += frame.nals[n].length;

		pthread_mutex_lock(&p->frame.lock);
		if (total <= p->frame.size) {
			uint32_t off = 0;
			for (uint32_t n = 0; n < frame.nal_count; n++) {
				memcpy(p->frame.data + off, frame.nals[n].data,
				       frame.nals[n].length);
				off += frame.nals[n].length;
			}
			p->frame.len = off;
			p->frame.ts_us = p->ts_rebased && frame.timestamp > 0
						 ? frame.timestamp
						 : uvcd_monotonic_us();
			p->frame.is_key = frame.is_key;
			p->frame.seq++;
		} else {
			LOGW("frame too large (%u > %u), dropped", total, p->frame.size);
		}
		bool dropped = total > p->frame.size;
		pthread_mutex_unlock(&p->frame.lock);
		if (dropped)
			uvcd_enc_request_keyframe(p); /* see deliver_frame() */

		(void)primary_nal_type(&frame);
		RSS_HAL_CALL(p->cam.ops, enc_release_frame, p->cam.hal_ctx, UVCD_ENC_CHN, &frame);
	}
	LOGD("pump thread exiting");
	return NULL;
}

/* ── Configure FS + encoder channel for a given (format, frame size) ── */

/* --------------------------------------------------------------------------
 * Encoder settings. The effective values resolve the "auto" settings (0 /
 * -1) against the negotiated resolution and frame rate, so the same rule is
 * used at channel creation and when a setting changes live.
 */
static uint32_t effective_h264_bitrate(const uvcd_pipeline_t *p, uint8_t frame)
{
	if (p->controls.h264_bitrate_kbps > 0)
		return (uint32_t)p->controls.h264_bitrate_kbps * 1000u;
	return uvcd_frames[frame].h264_bitrate;
}

static rss_rc_mode_t h264_rc_mode(const uvcd_pipeline_t *p)
{
	switch (p->controls.h264_rate_control) {
	case UVCD_H264_RC_VBR:
		return RSS_RC_VBR;
	case UVCD_H264_RC_CAPPED_VBR:
		return RSS_RC_CAPPED_VBR;
	default:
		return RSS_RC_CBR;
	}
}

static bool running_h264(const uvcd_pipeline_t *p)
{
	return p->running && p->cur_format == UVCD_FMT_H264;
}

int uvcd_enc_apply_bitrate(uvcd_pipeline_t *p)
{
	if (!running_h264(p))
		return 0;
	return RSS_HAL_CALL(p->cam.ops, enc_set_bitrate, p->cam.hal_ctx, UVCD_ENC_CHN,
			    effective_h264_bitrate(p, p->cur_frame));
}

int uvcd_enc_request_keyframe(uvcd_pipeline_t *p)
{
	if (!running_h264(p))
		return 0; /* MJPEG is all keyframes; not streaming, nothing to do */
	return RSS_HAL_CALL(p->cam.ops, enc_request_idr, p->cam.hal_ctx, UVCD_ENC_CHN);
}

static int configure_channel(uvcd_pipeline_t *p, uint8_t format, uint8_t frame,
			     uint32_t interval)
{
	const struct uvcd_frame_info *fi = &uvcd_frames[frame];
	rss_codec_t codec = (format == UVCD_FMT_H264) ? RSS_CODEC_H264 : RSS_CODEC_JPEG;
	uint32_t fps_num = uvcd_interval_fps(interval);
	uint32_t fps_den = 1;

	rss_fs_config_t fs_cfg = {
		.width = fi->width,
		.height = fi->height,
		.pixfmt = RSS_PIXFMT_NV12,
		.fps_num = fps_num,
		.fps_den = fps_den,
		.nr_vbs = 2,
	};
	/* The camera's view for this output size (uvcd_camera_acquire), as a
	 * front crop before the scaler; a view change re-creates the channel
	 * (apply_view). */
	int vx = p->cam.view.x, vy = p->cam.view.y, vw = p->cam.view.w, vh = p->cam.view.h;
	if (p->cam.view_frame != frame)
		return -EINVAL; /* the camera was not acquired for this size */
	/* Never pass an invalid sensor-space rectangle to the ISP. */
	if (vw <= 0 || vh <= 0 || vx < 0 || vy < 0 ||
	    vx + vw > p->cam.sensor_w || vy + vh > p->cam.sensor_h)
		return -EINVAL;
	if (vw != p->cam.sensor_w || vh != p->cam.sensor_h) {
		fs_cfg.fcrop.enable = true;
		fs_cfg.fcrop.x = vx;
		fs_cfg.fcrop.y = vy;
		fs_cfg.fcrop.w = vw;
		fs_cfg.fcrop.h = vh;
	}
	if (fi->width != vw || fi->height != vh) {
		fs_cfg.scaler.enable = true;
		fs_cfg.scaler.out_width = fi->width;
		fs_cfg.scaler.out_height = fi->height;
	}

	int ret = RSS_HAL_CALL(p->cam.ops, fs_create_channel, p->cam.hal_ctx, UVCD_FS_CHN, &fs_cfg);
	if (ret != RSS_OK) {
		LOGE("fs_create_channel failed: %d", ret);
		return ret;
	}
	RSS_HAL_CALL(p->cam.ops, fs_set_fifo, p->cam.hal_ctx, UVCD_FS_CHN, 0);
	RSS_HAL_CALL(p->cam.ops, fs_set_frame_depth, p->cam.hal_ctx, UVCD_FS_CHN, 0);

	ret = RSS_HAL_CALL(p->cam.ops, enc_create_group, p->cam.hal_ctx, UVCD_ENC_CHN);
	if (ret != RSS_OK) {
		LOGE("enc_create_group failed: %d", ret);
		goto fail_fs;
	}

	const struct uvcd_control_state *c = &p->controls;
	bool jpeg = (codec == RSS_CODEC_JPEG);
	uint32_t bitrate = jpeg ? 0 : effective_h264_bitrate(p, frame);
	int16_t min_qp = (int16_t)c->h264_min_qp;
	int16_t max_qp = (int16_t)c->h264_max_qp;

	/* The HAL's own CBR floor is QP 34, which a still scene reaches at
	 * well under the target: every bitrate from 1 to 8 Mbps came out at
	 * ~0.7 Mbps at 720p. Automatic uses the floor it gives VBR instead,
	 * so the target is what sets the rate. */
	if (min_qp < 0)
		min_qp = max_qp >= 0 && max_qp < UVCD_H264_AUTO_MIN_QP ? max_qp
								  : UVCD_H264_AUTO_MIN_QP;

	/* Crossed bounds would make channel creation fail, and when this runs
	 * as an in-place restart the host is left with no frames at all. Fall
	 * back to the SDK's own bounds rather than refuse to encode. */
	if (min_qp >= 0 && max_qp >= 0 && min_qp > max_qp) {
		LOGW("h264 min QP %d > max QP %d, using SDK defaults", min_qp, max_qp);
		min_qp = -1;
		max_qp = -1;
	}
	rss_video_config_t enc_cfg = {
		.codec = codec,
		.width = fi->width,
		.height = fi->height,
		.profile = jpeg ? 0 : c->h264_profile,
		.rc_mode = jpeg ? RSS_RC_FIXQP : h264_rc_mode(p),
		.bitrate = bitrate,
		/* VBR and capped VBR treat the target as a ceiling-ish average;
		 * give them the same headroom the HAL uses for live changes. */
		.max_bitrate = jpeg ? 0 : bitrate * 4 / 3,
		.fps_num = fps_num,
		.fps_den = fps_den,
		/* An IDR every second. libimp only makes whole-second GOPs, every
		 * I-frame an IDR: AL_Codec_Encode_ValidateGopParam rounds the IDR
		 * interval (gop_length * max_same_scene_cnt) up to a multiple of the
		 * frame rate and sets the I-frame interval to match. */
		.gop_length = fps_num,
		.max_same_scene_cnt = 1, /* SDK default 2 doubles the IDR interval */
		/* On T31 a JPEG channel's quality *is* its initial QP, and it can
		 * only be set here -- enc_set_jpeg_qp is unsupported on this SDK. */
		.init_qp = jpeg ? (int16_t)c->mjpeg_quality : -1,
		.min_qp = jpeg ? -1 : min_qp,
		.max_qp = jpeg ? -1 : max_qp,
		.ip_delta = -1,
		.pb_delta = -1,
	};

	/* JPEG creation in this HAL bypasses enc_cfg's buffer options. Set
	 * them explicitly before CreateChn and check errors. Keep one large
	 * JPEG buffer because the pump copies/releases it immediately; two
	 * would waste scarce reserved memory. Reset both options for H.264.
	 * USB has a smaller cap (max_size): a JPEG over it is dropped, which at
	 * about 1 byte per pixel takes far more than quality 100 needs. */
	uint32_t stream_bytes = jpeg ? (fi->width * fi->height * 4u + 65535u) & ~4095u
				    : fi->max_size;
	ret = RSS_HAL_CALL(p->cam.ops, enc_set_max_stream_cnt, p->cam.hal_ctx, UVCD_ENC_CHN,
			   jpeg ? 1 : 2);
	if (ret < 0)
		goto fail_grp;
	ret = RSS_HAL_CALL(p->cam.ops, enc_set_stream_buf_size, p->cam.hal_ctx,
			   UVCD_ENC_CHN, stream_bytes);
	if (ret < 0) {
		LOGE("encoder stream buffer (%u bytes) rejected: %d", stream_bytes, ret);
		goto fail_grp;
	}

	ret = RSS_HAL_CALL(p->cam.ops, enc_create_channel, p->cam.hal_ctx, UVCD_ENC_CHN, &enc_cfg);
	if (ret != RSS_OK) {
		LOGE("enc_create_channel failed: %d", ret);
		goto fail_grp;
	}
	ret = RSS_HAL_CALL(p->cam.ops, enc_register_channel, p->cam.hal_ctx, UVCD_ENC_CHN, UVCD_ENC_CHN);
	if (ret != RSS_OK) {
		LOGE("enc_register_channel failed: %d", ret);
		goto fail_chn;
	}

	rss_cell_t fs_cell = {RSS_DEV_FS, UVCD_FS_CHN, 0};
	rss_cell_t enc_cell = {RSS_DEV_ENC, UVCD_ENC_CHN, 0};
	ret = RSS_HAL_CALL(p->cam.ops, bind, p->cam.hal_ctx, &fs_cell, &enc_cell);
	if (ret != RSS_OK) {
		LOGE("bind failed: %d", ret);
		goto fail_reg;
	}

	LOGI("pipeline configured: %s %ux%u", codec == RSS_CODEC_H264 ? "H.264" : "MJPEG",
	     fi->width, fi->height);
	return RSS_OK;

fail_reg:
	RSS_HAL_CALL(p->cam.ops, enc_unregister_channel, p->cam.hal_ctx, UVCD_ENC_CHN);
fail_chn:
	RSS_HAL_CALL(p->cam.ops, enc_destroy_channel, p->cam.hal_ctx, UVCD_ENC_CHN);
fail_grp:
	RSS_HAL_CALL(p->cam.ops, enc_destroy_group, p->cam.hal_ctx, UVCD_ENC_CHN);
fail_fs:
	RSS_HAL_CALL(p->cam.ops, fs_destroy_channel, p->cam.hal_ctx, UVCD_FS_CHN);
	return ret;
}

static void teardown_channel(uvcd_pipeline_t *p)
{
	rss_cell_t fs_cell = {RSS_DEV_FS, UVCD_FS_CHN, 0};
	rss_cell_t enc_cell = {RSS_DEV_ENC, UVCD_ENC_CHN, 0};

	RSS_HAL_CALL(p->cam.ops, unbind, p->cam.hal_ctx, &fs_cell, &enc_cell);
	RSS_HAL_CALL(p->cam.ops, enc_unregister_channel, p->cam.hal_ctx, UVCD_ENC_CHN);
	RSS_HAL_CALL(p->cam.ops, enc_destroy_channel, p->cam.hal_ctx, UVCD_ENC_CHN);
	RSS_HAL_CALL(p->cam.ops, enc_destroy_group, p->cam.hal_ctx, UVCD_ENC_CHN);
	RSS_HAL_CALL(p->cam.ops, fs_destroy_channel, p->cam.hal_ctx, UVCD_FS_CHN);
}

/* Keep the encoder at the sensor's rate, which exposure priority may move
 * below the negotiated one, so rate control holds bits per second. */
static void sync_encoder_rate(uvcd_pipeline_t *p)
{
	uint32_t fps = p->cam.sensor_fps;

	if (!p->configured || !fps || fps == p->enc_fps)
		return;
	if (RSS_HAL_CALL(p->cam.ops, enc_set_fps, p->cam.hal_ctx, UVCD_ENC_CHN, fps, 1) != RSS_OK)
		LOGW("encoder rate %u rejected", fps);
	p->enc_fps = fps; /* either way: not retried every tick */
}

int uvcd_pipeline_start(uvcd_pipeline_t *p, uint8_t format, uint8_t frame,
			uint32_t interval)
{
	if (frame < 1 || frame > UVCD_NUM_FRAMES ||
	    (format != UVCD_FMT_MJPEG && format != UVCD_FMT_H264))
		return -EINVAL;
	if (p->running && p->configured && p->cur_format == format && p->cur_frame == frame &&
	    p->cur_interval == interval)
		return RSS_OK; /* already running with these exact params */

	uvcd_pipeline_stop(p);

	/* Normally already up since daemon start; this retries one that
	 * failed then. */
	int ret = uvcd_camera_up(&p->cam);
	if (ret != RSS_OK)
		return ret;
	ret = uvcd_camera_acquire(&p->cam, frame, uvcd_interval_fps(interval));
	if (ret != RSS_OK)
		return ret;

	ret = configure_channel(p, format, frame, interval);
	if (ret != RSS_OK) {
		uvcd_camera_release(&p->cam);
		return ret;
	}
	p->configured = true;
	p->cur_format = format;
	p->cur_frame = frame;
	p->cur_interval = interval;
	p->enc_fps = uvcd_interval_fps(interval);

	/* Put the SDK's capture timestamps on CLOCK_MONOTONIC, the clock the
	 * gadget stamps USB payloads with, so a frame's PTS is when the
	 * sensor captured it rather than when it finished encoding. Redone on
	 * every start so the two clocks never drift far apart. */
	p->ts_rebased = RSS_HAL_CALL(p->cam.ops, sys_rebase_timestamp, p->cam.hal_ctx,
				     uvcd_monotonic_us()) == RSS_OK;
	if (!p->ts_rebased)
		LOGW("SDK timestamp rebase failed; stamping frames on receipt");

	ret = RSS_HAL_CALL(p->cam.ops, fs_enable_channel, p->cam.hal_ctx, UVCD_FS_CHN);
	if (ret != RSS_OK)
		goto fail_start;
	ret = RSS_HAL_CALL(p->cam.ops, enc_start, p->cam.hal_ctx, UVCD_ENC_CHN);
	if (ret != RSS_OK) {
		RSS_HAL_CALL(p->cam.ops, fs_disable_channel, p->cam.hal_ctx, UVCD_FS_CHN);
		goto fail_start;
	}

	uvcd_camera_stream_on(&p->cam, format);
	sync_encoder_rate(p);

	p->frame.seq = 0;
	p->frame.len = 0;
	p->pump_run = 1;
	if (pthread_create(&p->pump_tid, NULL, pump_thread, p) != 0) {
		LOGE("pump thread create failed");
		p->pump_run = 0;
		RSS_HAL_CALL(p->cam.ops, enc_stop, p->cam.hal_ctx, UVCD_ENC_CHN);
		RSS_HAL_CALL(p->cam.ops, fs_disable_channel, p->cam.hal_ctx, UVCD_FS_CHN);
		teardown_channel(p);
		p->configured = false;
		uvcd_camera_release(&p->cam);
		return -1;
	}

	p->running = true;
	LOGI("stream started");
	return RSS_OK;

fail_start:
	LOGE("stream enable failed: %d", ret);
	teardown_channel(p);
	p->configured = false;
	uvcd_camera_release(&p->cam);
	return ret;
}

bool uvcd_pipeline_restart_encoder(uvcd_pipeline_t *p, uint8_t format)
{
	if (!p->running || p->cur_format != format)
		return false;

	uint8_t frame = p->cur_frame;
	uint32_t interval = p->cur_interval;

	/* uvcd_pipeline_start() is a no-op for unchanged parameters, so tear
	 * down explicitly first. The host sees a short gap in frames, not a
	 * renegotiation: the gadget side keeps streaming throughout. */
	uvcd_pipeline_stop(p);
	if (uvcd_pipeline_start(p, format, frame, interval) != RSS_OK) {
		LOGE("encoder restart failed");
		return false;
	}
	LOGI("encoder restarted with new settings");
	return true;
}

void uvcd_pipeline_stop(uvcd_pipeline_t *p)
{
	if (p->pump_run) {
		p->pump_run = 0;
		pthread_join(p->pump_tid, NULL);
	}
	if (p->running) {
		RSS_HAL_CALL(p->cam.ops, enc_stop, p->cam.hal_ctx, UVCD_ENC_CHN);
		RSS_HAL_CALL(p->cam.ops, fs_disable_channel, p->cam.hal_ctx, UVCD_FS_CHN);
		p->running = false;
	}
	if (p->configured) {
		teardown_channel(p);
		p->configured = false;
		p->enc_fps = 0;
		uvcd_camera_release(&p->cam);
		LOGI("stream torn down; camera idle");
	}
}

/* A new zoom/pan/tilt. Without a stream the camera just moves its view.
 * A stream's view is its framesource crop: AutoZoom crops AFTER scaling on
 * T31, so re-create the channels with a front crop instead, and zoom never
 * needs a sensor-wide 7680-pixel scaler output. If the SDK rejects the new
 * view, the old one comes back. */
static int apply_view(uvcd_pipeline_t *p, struct uvcd_control_state *c, size_t field)
{
	if (!p->configured)
		return uvcd_camera_apply(&p->cam, c, field);
	if (!uvcd_camera_view_moves(&p->cam, c))
		return RSS_OK;

	struct uvcd_control_state old = p->controls;
	uint8_t format = p->cur_format, frame = p->cur_frame;
	uint32_t interval = p->cur_interval;
	uvcd_pipeline_stop(p);
	p->controls = *c;
	int ret = uvcd_pipeline_start(p, format, frame, interval);
	p->controls = old;
	if (ret != RSS_OK) {
		LOGW("view change failed; restoring previous view");
		uvcd_pipeline_start(p, format, frame, interval);
	}
	return ret;
}

int uvcd_pipeline_apply(uvcd_pipeline_t *p, struct uvcd_control_state *next, size_t field)
{
	if (field == offsetof(struct uvcd_control_state, zoom) ||
	    field == offsetof(struct uvcd_control_state, pan) ||
	    field == offsetof(struct uvcd_control_state, tilt))
		return apply_view(p, next, field);
	return uvcd_camera_apply(&p->cam, next, field);
}

void uvcd_pipeline_tick(uvcd_pipeline_t *p)
{
	uvcd_camera_tick(&p->cam);
	sync_encoder_rate(p);
	camera_guard_tick(p);
}
