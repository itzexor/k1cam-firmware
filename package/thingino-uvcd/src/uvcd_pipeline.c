/*
 * uvcd_pipeline.c -- sensor/ISP/encoder bring-up and teardown
 *
 * Single sensor (T31 + GC2083), single framesource channel, single
 * encoder group/channel. No OSD, no IVS, no multi-sensor, no config
 * file: sensor identity/GPIOs come from the kernel sensor driver via
 * procfs (/proc/jz/sensor/sensor0/...) -- same auto-discovery rvd
 * falls back to when raptor.conf doesn't override the [sensor]
 * section, which is exactly this board's current working setup.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>

#include "uvcd.h"
#include "uvcd_config.h"

/* --------------------------------------------------------------------------
 * Controls whose HAL representation isn't a plain scalar.
 *
 * UVC (and V4L2) express gamma as a single number and white balance as a
 * colour temperature. The ISP wants a 129-point curve and a mode+gains
 * struct respectively. Keeping both translations here means uvcd_gadget.c
 * stays a pure UVC-protocol file with no ISP-shaped special cases.
 */

int uvcd_apply_gamma(uvcd_pipeline_t *p, int gamma_x100)
{
	uint16_t curve[UVCD_GAMMA_POINTS];

	/* Without the factory curve there is no reference to re-shape, and
	 * inventing one would mean guessing the table's value range. */
	if (!p->gamma_ref_valid)
		return -ENOTSUP;

	if (gamma_x100 < UVCD_GAMMA_MIN)
		gamma_x100 = UVCD_GAMMA_MIN;
	if (gamma_x100 > UVCD_GAMMA_MAX)
		gamma_x100 = UVCD_GAMMA_MAX;

	if (gamma_x100 == UVCD_GAMMA_DEF) {
		/* Exactly the vendor tuning, no floating point rounding. */
		memcpy(curve, p->gamma_ref, sizeof(curve));
	} else {
		/* Normalize against the curve's own peak so this works whatever
		 * the table's full-scale value happens to be, apply the extra
		 * power law, then scale back. Output = in^(1/gamma), so a
		 * larger gamma brightens -- the UVC/V4L2 convention. */
		uint16_t peak = 0;
		for (int i = 0; i < UVCD_GAMMA_POINTS; i++) {
			if (p->gamma_ref[i] > peak)
				peak = p->gamma_ref[i];
		}
		if (peak == 0)
			return -ENOTSUP;

		float exponent = (float)UVCD_GAMMA_DEF / (float)gamma_x100;
		for (int i = 0; i < UVCD_GAMMA_POINTS; i++) {
			float norm = (float)p->gamma_ref[i] / (float)peak;
			float out = powf(norm, exponent) * (float)peak;

			if (out < 0.0f)
				out = 0.0f;
			if (out > (float)peak)
				out = (float)peak;
			curve[i] = (uint16_t)(out + 0.5f);
		}
	}

	return RSS_HAL_CALL(p->ops, isp_set_gamma, p->hal_ctx, curve);
}

int uvcd_apply_wb(uvcd_pipeline_t *p, int temp_kelvin, int auto_on)
{
	rss_wb_config_t cfg;

	memset(&cfg, 0, sizeof(cfg));

	if (auto_on) {
		cfg.mode = RSS_WB_AUTO;
	} else {
		/* The ISP offers named presets, not a continuous colour
		 * temperature, so snap to the nearest one. Boundaries sit
		 * midway between each preset's nominal CCT. */
		if (temp_kelvin <= 3200)
			cfg.mode = RSS_WB_INCANDESCENT; /* ~2800K */
		else if (temp_kelvin <= 4500)
			cfg.mode = RSS_WB_FLUORESCENT; /* ~4000K */
		else if (temp_kelvin <= 5800)
			cfg.mode = RSS_WB_DAYLIGHT; /* ~5500K */
		else if (temp_kelvin <= 7000)
			cfg.mode = RSS_WB_CLOUDY; /* ~6500K */
		else
			cfg.mode = RSS_WB_SHADE; /* ~7500K */
	}

	return RSS_HAL_CALL(p->ops, isp_set_wb, p->hal_ctx, &cfg);
}

const struct uvcd_frame_info uvcd_frames[UVCD_NUM_FRAMES + 1] = {
	[0] = {0, 0, 0, 0},
	[UVCD_FRAME_1080P] = {1920, 1080, 1024 * 1024, 3000000},
	[UVCD_FRAME_1280X960] = {1280, 960, 768 * 1024, 2400000},
	[UVCD_FRAME_720P] = {1280, 720, 768 * 1024, 1800000},
	[UVCD_FRAME_800X600] = {800, 600, 512 * 1024, 1200000},
	[UVCD_FRAME_640X480] = {640, 480, 512 * 1024, 1000000},
	[UVCD_FRAME_360P] = {640, 360, 384 * 1024, 700000},
};

#define UVCD_FRAME_BUF_CAP (1024 * 1024) /* 1MB scratch, covers worst-case 1080p I-frame */

static int read_procfs_int(const char *path, int base, int def)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return def;
	char buf[64] = {0};
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	if (n == 0)
		return def;
	return (int)strtol(buf, NULL, base);
}

static void read_procfs_str(const char *path, char *out, size_t out_len)
{
	FILE *f = fopen(path, "r");
	if (!f) {
		out[0] = '\0';
		return;
	}
	size_t n = fread(out, 1, out_len - 1, f);
	fclose(f);
	out[n] = '\0';
	char *nl = strchr(out, '\n');
	if (nl)
		*nl = '\0';
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

	p->hal_ctx = rss_hal_create();
	if (!p->hal_ctx) {
		LOGE("rss_hal_create failed");
		free(p->frame.data);
		return -1;
	}
	p->ops = rss_hal_get_ops(p->hal_ctx);

	rss_sensor_config_t sensor = {0};
	read_procfs_str("/proc/jz/sensor/sensor0/name", sensor.name, sizeof(sensor.name));
	if (!sensor.name[0]) {
		LOGE("no sensor registered at /proc/jz/sensor/sensor0 (gc2083 driver not loaded?)");
		goto fail;
	}
	sensor.i2c_addr = (uint16_t)read_procfs_int("/proc/jz/sensor/sensor0/i2c_addr", 0, 0);
	sensor.i2c_adapter = read_procfs_int("/proc/jz/sensor/sensor0/i2c_adapter", 10, 0);
	sensor.sensor_id = 0;
	sensor.pwdn_gpio = read_procfs_int("/proc/jz/sensor/sensor0/pwdn_gpio", 10, -1);
	sensor.rst_gpio = read_procfs_int("/proc/jz/sensor/sensor0/rst_gpio", 10, -1);
	sensor.power_gpio = -1;
	sensor.default_boot = read_procfs_int("/proc/jz/sensor/sensor0/boot", 10, 0);
	sensor.mclk = (rss_sensor_mclk_t)read_procfs_int("/proc/jz/sensor/sensor0/mclk", 10, 1);
	sensor.vin_type = (rss_sensor_vin_t)read_procfs_int(
		"/proc/jz/sensor/sensor0/video_interface", 10, 0);

	if (sensor.i2c_addr == 0) {
		LOGE("could not read sensor i2c_addr from procfs");
		goto fail;
	}

	LOGI("sensor: %s i2c=0x%02x bus=%d boot=%d", sensor.name, sensor.i2c_addr,
	     sensor.i2c_adapter, sensor.default_boot);

	/* OSD pool must be set before HAL init even when unused (SDK requirement) */
	RSS_HAL_CALL(p->ops, osd_set_pool_size, p->hal_ctx, 0);

	rss_multi_sensor_config_t multi = {0};
	multi.sensor_count = 1;
	multi.sensors[0] = sensor;

	int ret = RSS_HAL_CALL(p->ops, init, p->hal_ctx, &multi);
	if (ret != RSS_OK) {
		LOGE("HAL init failed: %d", ret);
		goto fail;
	}

	p->caps = p->ops->get_caps ? p->ops->get_caps(p->hal_ctx) : NULL;

	{
		char ver[64];
		if (rss_hal_get_imp_version(ver, sizeof(ver)) == 0)
			LOGI("LIBIMP version %s", ver);
	}

	/* Sensor FPS: use whatever the driver's default mode reports */
	int fps = read_procfs_int("/proc/jz/sensor/sensor0/max_fps", 10, 0);
	if (fps <= 0)
		fps = read_procfs_int("/proc/jz/sensor/sensor0/fps", 10, 0);
	if (fps <= 0)
		fps = 25;
	RSS_HAL_CALL(p->ops, isp_set_sensor_fps, p->hal_ctx, (uint32_t)fps, 1);
	LOGI("sensor fps: %d", fps);

	/* ISP tuning: from UVCD_CONFIG_PATH if present (controls persist by
	 * default -- the daemon writes the file back whenever the host changes
	 * one), else the compiled-in neutral defaults --
	 * this board has no [image] overrides. Hue is stored/exchanged as a
	 * signed -128..127 value; the HAL wants it offset to 0..255. */
	uvcd_config_load(UVCD_CONFIG_PATH, &p->controls);

	/* Capture the vendor's gamma curve before anything touches it -- it is
	 * the reference every later gamma change is derived from. */
	p->gamma_ref_valid =
		(RSS_HAL_CALL(p->ops, isp_get_gamma, p->hal_ctx, p->gamma_ref) == 0);
	if (!p->gamma_ref_valid)
		LOGW("could not read factory gamma curve; gamma control disabled");

	RSS_HAL_CALL(p->ops, isp_set_brightness, p->hal_ctx, p->controls.brightness);
	RSS_HAL_CALL(p->ops, isp_set_contrast, p->hal_ctx, p->controls.contrast);
	RSS_HAL_CALL(p->ops, isp_set_saturation, p->hal_ctx, p->controls.saturation);
	RSS_HAL_CALL(p->ops, isp_set_sharpness, p->hal_ctx, p->controls.sharpness);
	RSS_HAL_CALL(p->ops, isp_set_sinter_strength, p->hal_ctx, p->controls.sinter);
	RSS_HAL_CALL(p->ops, isp_set_temper_strength, p->hal_ctx, p->controls.temper);
	RSS_HAL_CALL(p->ops, isp_set_hue, p->hal_ctx, p->controls.hue + 128);
	RSS_HAL_CALL(p->ops, isp_set_ae_comp, p->hal_ctx, p->controls.ae_comp);
	RSS_HAL_CALL(p->ops, isp_set_max_again, p->hal_ctx, p->controls.max_again);
	RSS_HAL_CALL(p->ops, isp_set_max_dgain, p->hal_ctx, p->controls.max_dgain);
	RSS_HAL_CALL(p->ops, isp_set_dpc_strength, p->hal_ctx, p->controls.dpc);
	RSS_HAL_CALL(p->ops, isp_set_drc_strength, p->hal_ctx, p->controls.drc);
	RSS_HAL_CALL(p->ops, isp_set_backlight_comp, p->hal_ctx, p->controls.backlight);
	RSS_HAL_CALL(p->ops, isp_set_defog_strength, p->hal_ctx, p->controls.defog);
	RSS_HAL_CALL(p->ops, isp_set_highlight_depress, p->hal_ctx, p->controls.highlight);
	RSS_HAL_CALL(p->ops, isp_set_running_mode, p->hal_ctx, RSS_ISP_DAY);
	RSS_HAL_CALL(p->ops, isp_set_bypass, p->hal_ctx, 1);
	RSS_HAL_CALL(p->ops, isp_set_antiflicker, p->hal_ctx,
		    (rss_antiflicker_t)p->controls.power_line_frequency);
	RSS_HAL_CALL(p->ops, isp_set_hflip, p->hal_ctx, p->controls.hflip);
	RSS_HAL_CALL(p->ops, isp_set_vflip, p->hal_ctx, p->controls.vflip);
	uvcd_apply_gamma(p, p->controls.gamma);
	uvcd_apply_wb(p, p->controls.wb_temp, p->controls.wb_auto);

	p->sensor_w = read_procfs_int("/proc/jz/sensor/sensor0/width", 10, 0);
	p->sensor_h = read_procfs_int("/proc/jz/sensor/sensor0/height", 10, 0);
	if (p->sensor_w <= 0 || p->sensor_h <= 0) {
		p->sensor_w = 1920;
		p->sensor_h = 1080;
		LOGW("could not read sensor resolution, assuming %dx%d", p->sensor_w,
		     p->sensor_h);
	} else {
		LOGI("sensor resolution: %dx%d", p->sensor_w, p->sensor_h);
	}

	if (p->caps && !p->caps->has_h265)
		LOGD("H.265 not supported (unused -- uvcd is H.264/MJPEG only)");

	return 0;

fail:
	if (p->hal_ctx) {
		rss_hal_destroy(p->hal_ctx);
		p->hal_ctx = NULL;
	}
	free(p->frame.data);
	return -1;
}

void uvcd_pipeline_deinit(uvcd_pipeline_t *p)
{
	uvcd_pipeline_stop(p);
	if (p->hal_ctx) {
		RSS_HAL_CALL(p->ops, deinit, p->hal_ctx);
		rss_hal_destroy(p->hal_ctx);
		p->hal_ctx = NULL;
	}
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
		int ret = RSS_HAL_CALL(p->ops, enc_poll, p->hal_ctx, UVCD_ENC_CHN, 1000);
		if (ret != RSS_OK)
			continue; /* normal: sensor idle / timeout */

		rss_frame_t frame;
		ret = RSS_HAL_CALL(p->ops, enc_get_frame, p->hal_ctx, UVCD_ENC_CHN, &frame);
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
			p->frame.is_key = frame.is_key;
			p->frame.seq++;
		} else {
			LOGW("frame too large (%u > %u), dropped", total, p->frame.size);
		}
		pthread_mutex_unlock(&p->frame.lock);

		(void)primary_nal_type(&frame);
		RSS_HAL_CALL(p->ops, enc_release_frame, p->hal_ctx, UVCD_ENC_CHN, &frame);
	}
	LOGD("pump thread exiting");
	return NULL;
}

/* ── Configure FS + encoder channel for a given (format, frame size) ── */

static int configure_channel(uvcd_pipeline_t *p, uint8_t format, uint8_t frame,
			     uint32_t interval)
{
	const struct uvcd_frame_info *fi = &uvcd_frames[frame];
	rss_codec_t codec = (format == UVCD_FMT_H264) ? RSS_CODEC_H264 : RSS_CODEC_JPEG;
	uint32_t fps_num;
	uint32_t fps_den = 1;

	switch (interval) {
	case UVCD_INTERVAL_25FPS:
		fps_num = 25;
		break;
	case UVCD_INTERVAL_15FPS:
		fps_num = 15;
		break;
	default:
		fps_num = 30;
		break;
	}

	rss_fs_config_t fs_cfg = {
		.width = fi->width,
		.height = fi->height,
		.pixfmt = RSS_PIXFMT_NV12,
		.fps_num = fps_num,
		.fps_den = fps_den,
		.nr_vbs = 2,
	};
	if (fi->width != p->sensor_w || fi->height != p->sensor_h) {
		fs_cfg.scaler.enable = true;
		fs_cfg.scaler.out_width = fi->width;
		fs_cfg.scaler.out_height = fi->height;
	}

	int ret = RSS_HAL_CALL(p->ops, fs_create_channel, p->hal_ctx, UVCD_FS_CHN, &fs_cfg);
	if (ret != RSS_OK) {
		LOGE("fs_create_channel failed: %d", ret);
		return ret;
	}
	RSS_HAL_CALL(p->ops, fs_set_fifo, p->hal_ctx, UVCD_FS_CHN, 0);
	RSS_HAL_CALL(p->ops, fs_set_frame_depth, p->hal_ctx, UVCD_FS_CHN, 0);

	ret = RSS_HAL_CALL(p->ops, enc_create_group, p->hal_ctx, UVCD_ENC_CHN);
	if (ret != RSS_OK) {
		LOGE("enc_create_group failed: %d", ret);
		goto fail_fs;
	}

	rss_video_config_t enc_cfg = {
		.codec = codec,
		.width = fi->width,
		.height = fi->height,
		.profile = codec == RSS_CODEC_JPEG ? 0 : 2,
		.rc_mode = codec == RSS_CODEC_JPEG ? RSS_RC_FIXQP : RSS_RC_CBR,
		.bitrate = codec == RSS_CODEC_JPEG ? 0 : fi->h264_bitrate,
		.max_bitrate = 0,
		.fps_num = fps_num,
		.fps_den = fps_den,
		.gop_length = (fps_num + fps_den / 2) / fps_den,
		.init_qp = (codec == RSS_CODEC_JPEG) ? 80 : -1,
		.min_qp = -1,
		.max_qp = -1,
		.ip_delta = -1,
		.pb_delta = -1,
	};

	ret = RSS_HAL_CALL(p->ops, enc_create_channel, p->hal_ctx, UVCD_ENC_CHN, &enc_cfg);
	if (ret != RSS_OK) {
		LOGE("enc_create_channel failed: %d", ret);
		goto fail_grp;
	}
	ret = RSS_HAL_CALL(p->ops, enc_register_channel, p->hal_ctx, UVCD_ENC_CHN, UVCD_ENC_CHN);
	if (ret != RSS_OK) {
		LOGE("enc_register_channel failed: %d", ret);
		goto fail_chn;
	}

	rss_cell_t fs_cell = {RSS_DEV_FS, UVCD_FS_CHN, 0};
	rss_cell_t enc_cell = {RSS_DEV_ENC, UVCD_ENC_CHN, 0};
	ret = RSS_HAL_CALL(p->ops, bind, p->hal_ctx, &fs_cell, &enc_cell);
	if (ret != RSS_OK) {
		LOGE("bind failed: %d", ret);
		goto fail_reg;
	}

	LOGI("pipeline configured: %s %ux%u", codec == RSS_CODEC_H264 ? "H.264" : "MJPEG",
	     fi->width, fi->height);
	return RSS_OK;

fail_reg:
	RSS_HAL_CALL(p->ops, enc_unregister_channel, p->hal_ctx, UVCD_ENC_CHN);
fail_chn:
	RSS_HAL_CALL(p->ops, enc_destroy_channel, p->hal_ctx, UVCD_ENC_CHN);
fail_grp:
	RSS_HAL_CALL(p->ops, enc_destroy_group, p->hal_ctx, UVCD_ENC_CHN);
fail_fs:
	RSS_HAL_CALL(p->ops, fs_destroy_channel, p->hal_ctx, UVCD_FS_CHN);
	return ret;
}

static void teardown_channel(uvcd_pipeline_t *p)
{
	rss_cell_t fs_cell = {RSS_DEV_FS, UVCD_FS_CHN, 0};
	rss_cell_t enc_cell = {RSS_DEV_ENC, UVCD_ENC_CHN, 0};

	RSS_HAL_CALL(p->ops, unbind, p->hal_ctx, &fs_cell, &enc_cell);
	RSS_HAL_CALL(p->ops, enc_unregister_channel, p->hal_ctx, UVCD_ENC_CHN);
	RSS_HAL_CALL(p->ops, enc_destroy_channel, p->hal_ctx, UVCD_ENC_CHN);
	RSS_HAL_CALL(p->ops, enc_destroy_group, p->hal_ctx, UVCD_ENC_CHN);
	RSS_HAL_CALL(p->ops, fs_destroy_channel, p->hal_ctx, UVCD_FS_CHN);
}

int uvcd_pipeline_start(uvcd_pipeline_t *p, uint8_t format, uint8_t frame,
			uint32_t interval)
{
	if (p->running && p->configured && p->cur_format == format && p->cur_frame == frame &&
	    p->cur_interval == interval)
		return RSS_OK; /* already running with these exact params */

	uvcd_pipeline_stop(p);

	int ret = configure_channel(p, format, frame, interval);
	if (ret != RSS_OK)
		return ret;
	p->configured = true;
	p->cur_format = format;
	p->cur_frame = frame;
	p->cur_interval = interval;

	RSS_HAL_CALL(p->ops, fs_enable_channel, p->hal_ctx, UVCD_FS_CHN);
	RSS_HAL_CALL(p->ops, enc_start, p->hal_ctx, UVCD_ENC_CHN);

	p->frame.seq = 0;
	p->frame.len = 0;
	p->pump_run = 1;
	if (pthread_create(&p->pump_tid, NULL, pump_thread, p) != 0) {
		LOGE("pump thread create failed");
		p->pump_run = 0;
		RSS_HAL_CALL(p->ops, enc_stop, p->hal_ctx, UVCD_ENC_CHN);
		RSS_HAL_CALL(p->ops, fs_disable_channel, p->hal_ctx, UVCD_FS_CHN);
		teardown_channel(p);
		p->configured = false;
		return -1;
	}

	p->running = true;
	LOGI("pipeline started (on demand)");
	return RSS_OK;
}

void uvcd_pipeline_stop(uvcd_pipeline_t *p)
{
	if (p->pump_run) {
		p->pump_run = 0;
		pthread_join(p->pump_tid, NULL);
	}
	if (p->running) {
		RSS_HAL_CALL(p->ops, enc_stop, p->hal_ctx, UVCD_ENC_CHN);
		RSS_HAL_CALL(p->ops, fs_disable_channel, p->hal_ctx, UVCD_FS_CHN);
		p->running = false;
	}
	if (p->configured) {
		teardown_channel(p);
		p->configured = false;
		LOGI("pipeline torn down (no host attached)");
	}
}
