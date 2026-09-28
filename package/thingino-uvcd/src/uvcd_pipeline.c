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
#include <time.h>

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

/* Manual white balance follows a model of this sensor's white locus: the
 * red and blue gains that make a grey surface neutral under light of a given
 * colour temperature. The ISP's own presets are no use for this -- they are
 * fixed gain pairs compiled into the tx-isp blob (tisp_s_wb_mode), identical
 * for every sensor, and on the GC2083 they land well off-white (FLUORESCENT
 * leaves a strong green cast) with nothing in between them.
 *
 * Both gains are close to exponential in mired (1e6 / K) over 2800-7500 K.
 * The slopes are a least-squares fit to the blob's own preset gains taken at
 * their nominal temperatures (INCANDESCENT 2856 K, DAYLIGHT 5000 K, CLOUDY
 * 6500 K, SHADE 7500 K); the reference point pins that curve to this sensor:
 * the gains the ISP's auto white balance settled on for a scene it measured
 * at 3640 K. Gains are in the ISP's manual-mode units, the same ones
 * IMP_ISP_Tuning_GetWB reports for auto. */
#define UVCD_WB_REF_KELVIN 3640.0f
#define UVCD_WB_REF_RGAIN 343.0f
#define UVCD_WB_REF_BGAIN 612.0f
#define UVCD_WB_R_PER_MIRED -0.0035f /* d ln(rgain) / d mired */
#define UVCD_WB_B_PER_MIRED 0.0042f  /* d ln(bgain) / d mired */
#define UVCD_WB_GAIN_MIN 32
#define UVCD_WB_GAIN_MAX 4095

static uint16_t wb_gain(float ref, float per_mired, float d_mired)
{
	float g = ref * expf(per_mired * d_mired);

	if (g < UVCD_WB_GAIN_MIN)
		g = UVCD_WB_GAIN_MIN;
	if (g > UVCD_WB_GAIN_MAX)
		g = UVCD_WB_GAIN_MAX;
	return (uint16_t)(g + 0.5f);
}

int uvcd_apply_wb(uvcd_pipeline_t *p, int temp_kelvin, int auto_on)
{
	rss_wb_config_t cfg;

	memset(&cfg, 0, sizeof(cfg));

	if (auto_on) {
		cfg.mode = RSS_WB_AUTO;
		return RSS_HAL_CALL(p->ops, isp_set_wb, p->hal_ctx, &cfg);
	}

	if (temp_kelvin < UVCD_WB_TEMP_MIN)
		temp_kelvin = UVCD_WB_TEMP_MIN;
	if (temp_kelvin > UVCD_WB_TEMP_MAX)
		temp_kelvin = UVCD_WB_TEMP_MAX;

	float d_mired = 1e6f / (float)temp_kelvin - 1e6f / UVCD_WB_REF_KELVIN;
	cfg.mode = RSS_WB_MANUAL;
	cfg.r_gain = wb_gain(UVCD_WB_REF_RGAIN, UVCD_WB_R_PER_MIRED, d_mired);
	cfg.b_gain = wb_gain(UVCD_WB_REF_BGAIN, UVCD_WB_B_PER_MIRED, d_mired);

	int ret = RSS_HAL_CALL(p->ops, isp_set_wb, p->hal_ctx, &cfg);
	if (ret != RSS_OK)
		return ret;

	/* In manual mode the ISP takes the colour temperature for its
	 * CT-dependent tuning (colour matrix, saturation, shading) from here
	 * rather than from its own estimate, so keep it on the same light the
	 * gains assume. The gains are what matter; this only refines them. */
	unsigned int ct = (unsigned int)temp_kelvin;
	if (RSS_HAL_CALL(p->ops, isp_set_awb_ct_attr, p->hal_ctx, &ct) != RSS_OK)
		LOGD("isp_set_awb_ct_attr(%u) failed", ct);

	LOGD("white balance %d K: rgain=%u bgain=%u", temp_kelvin, cfg.r_gain, cfg.b_gain);
	return RSS_OK;
}

int uvcd_awb_current_ct(uvcd_pipeline_t *p)
{
	unsigned int ct = 0;

	if (!p->hal_up)
		return -1;
	if (RSS_HAL_CALL(p->ops, isp_get_awb_ct_attr, p->hal_ctx, &ct) != RSS_OK || ct == 0)
		return -1;
	return (int)ct;
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

static int64_t monotonic_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Push every stored control to the ISP. Runs at each bring-up: the ISP
 * starts from its factory tuning every time it is opened. Hue is stored
 * and exchanged as a signed -128..127 value; the HAL wants it offset to
 * 0..255. */
static void apply_isp_controls(uvcd_pipeline_t *p)
{
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
	uvcd_apply_rotation(p, &p->controls);
	uvcd_apply_gamma(p, p->controls.gamma);
	uvcd_apply_wb(p, p->controls.wb_temp, p->controls.wb_auto);
	uvcd_apply_exposure(p, &p->controls);
	uvcd_apply_metering(p, &p->controls);
}

/* Sensor + ISP + IMP system up, with every stored control applied. */
static int hal_bring_up(uvcd_pipeline_t *p)
{
	static bool first = true;

	if (p->hal_up)
		return RSS_OK;

	int64_t t0 = monotonic_ms();

	/* OSD pool must be set before HAL init even when unused (SDK requirement) */
	RSS_HAL_CALL(p->ops, osd_set_pool_size, p->hal_ctx, 0);

	rss_multi_sensor_config_t multi = {0};
	multi.sensor_count = 1;
	multi.sensors[0] = p->sensor;

	int ret = RSS_HAL_CALL(p->ops, init, p->hal_ctx, &multi);
	if (ret != RSS_OK) {
		LOGE("HAL init failed: %d", ret);
		return ret;
	}
	p->hal_up = true;
	p->idle_since_ms = 0;
	p->sensor_fps = 0; /* the driver boots at its own default rate */

	p->caps = p->ops->get_caps ? p->ops->get_caps(p->hal_ctx) : NULL;

	if (first) {
		char ver[64];
		if (rss_hal_get_imp_version(ver, sizeof(ver)) == 0)
			LOGI("LIBIMP version %s", ver);
		if (p->caps && !p->caps->has_h265)
			LOGD("H.265 not supported (unused -- uvcd is H.264/MJPEG only)");

		/* Capture the vendor's gamma curve before anything touches it --
		 * it is the reference every later gamma change is derived from.
		 * Only on the first bring-up: the ISP driver outlives uvcd's
		 * sessions, so a later read could return our own reshaped curve. */
		p->gamma_ref_valid =
			(RSS_HAL_CALL(p->ops, isp_get_gamma, p->hal_ctx, p->gamma_ref) == 0);
		if (!p->gamma_ref_valid)
			LOGW("could not read factory gamma curve; gamma control disabled");

		/* Same for the tuning file's metering weights. */
		p->ae_weight_ref_valid = (RSS_HAL_CALL(p->ops, isp_get_ae_weight, p->hal_ctx,
						       p->ae_weight_ref) == 0);
		if (!p->ae_weight_ref_valid)
			LOGW("could not read the tuning's metering weights");
	}

	p->sensor_w = read_procfs_int("/proc/jz/sensor/sensor0/width", 10, 0);
	p->sensor_h = read_procfs_int("/proc/jz/sensor/sensor0/height", 10, 0);
	if (p->sensor_w <= 0 || p->sensor_h <= 0) {
		p->sensor_w = 1920;
		p->sensor_h = 1080;
		LOGW("could not read sensor resolution, assuming %dx%d", p->sensor_w,
		     p->sensor_h);
	} else if (first) {
		LOGI("sensor resolution: %dx%d", p->sensor_w, p->sensor_h);
	}
	/* Until a channel says otherwise, the whole sensor is in view. */
	p->view.x = 0;
	p->view.y = 0;
	p->view.w = p->sensor_w;
	p->view.h = p->sensor_h;

	apply_isp_controls(p);

	/* No sensor frame rate here: it follows the host's negotiated interval
	 * (uvcd_pipeline_start), since the channel cannot run faster than the
	 * sensor and the GC2083 driver boots at 25 of its 30 fps. */


	first = false;
	LOGI("sensor + ISP up in %lld ms", (long long)(monotonic_ms() - t0));
	return RSS_OK;
}

/* Sensor stream off, ISP closed, IMP system exited. Channels must already
 * be torn down (uvcd_pipeline_stop). */
static void hal_shut_down(uvcd_pipeline_t *p)
{
	if (!p->hal_up)
		return;

	int ret = RSS_HAL_CALL(p->ops, deinit, p->hal_ctx);
	if (ret != RSS_OK)
		LOGW("HAL deinit: %d", ret);
	p->hal_up = false;
	p->idle_since_ms = 0;
	LOGI("sensor + ISP down (no stream for %d ms)", UVCD_HAL_LINGER_MS);
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

	rss_sensor_config_t *sensor = &p->sensor;
	read_procfs_str("/proc/jz/sensor/sensor0/name", sensor->name, sizeof(sensor->name));
	if (!sensor->name[0]) {
		LOGE("no sensor registered at /proc/jz/sensor/sensor0 (gc2083 driver not loaded?)");
		goto fail;
	}
	sensor->i2c_addr = (uint16_t)read_procfs_int("/proc/jz/sensor/sensor0/i2c_addr", 0, 0);
	sensor->i2c_adapter = read_procfs_int("/proc/jz/sensor/sensor0/i2c_adapter", 10, 0);
	sensor->sensor_id = 0;
	sensor->pwdn_gpio = read_procfs_int("/proc/jz/sensor/sensor0/pwdn_gpio", 10, -1);
	sensor->rst_gpio = read_procfs_int("/proc/jz/sensor/sensor0/rst_gpio", 10, -1);
	sensor->power_gpio = -1;
	sensor->default_boot = read_procfs_int("/proc/jz/sensor/sensor0/boot", 10, 0);
	sensor->mclk = (rss_sensor_mclk_t)read_procfs_int("/proc/jz/sensor/sensor0/mclk", 10, 1);
	sensor->vin_type = (rss_sensor_vin_t)read_procfs_int(
		"/proc/jz/sensor/sensor0/video_interface", 10, 0);

	if (sensor->i2c_addr == 0) {
		LOGE("could not read sensor i2c_addr from procfs");
		goto fail;
	}

	LOGI("sensor: %s i2c=0x%02x bus=%d boot=%d", sensor->name, sensor->i2c_addr,
	     sensor->i2c_adapter, sensor->default_boot);

	/* Controls persist by default -- the daemon writes the file back
	 * whenever the host changes one -- else the compiled-in neutral
	 * defaults. They are held here and pushed to the ISP at every
	 * bring-up (hal_bring_up), since the ISP itself only exists while a
	 * host is streaming. */
	uvcd_config_load(UVCD_CONFIG_PATH, &p->controls);

	/* Nothing else: the sensor, ISP and IMP system come up on the first
	 * stream (uvcd_pipeline_start), so an unused camera never starts them
	 * and the gadget can enumerate without waiting on the sensor. */
	return 0;

fail:
	rss_hal_destroy(p->hal_ctx);
	p->hal_ctx = NULL;
	free(p->frame.data);
	return -1;
}

void uvcd_pipeline_deinit(uvcd_pipeline_t *p)
{
	uvcd_pipeline_stop(p);
	hal_shut_down(p);
	if (p->hal_ctx) {
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

static int64_t monotonic_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
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
			p->frame.ts_us = p->ts_rebased && frame.timestamp > 0
						 ? frame.timestamp
						 : monotonic_us();
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
		RSS_HAL_CALL(p->ops, enc_release_frame, p->hal_ctx, UVCD_ENC_CHN, &frame);
	}
	LOGD("pump thread exiting");
	return NULL;
}

/* --------------------------------------------------------------------------
 * Camera Terminal controls: exposure, exposure priority, zoom/pan/tilt --
 * and the two nonstandard ones that shape the same picture, rotation and
 * metering.
 */

/* SDK structs these HAL calls pass through as void *. uvcd builds against
 * raptor_hal.h only, so they are mirrored from the T31 1.1.6 imp_isp.h. */
struct uvcd_imp_expr { /* IMPISPExpr, s_attr side of the union */
	int mode;      /* 0 = auto, 1 = manual */
	int unit;      /* 0 = sensor lines, 1 = microseconds */
	uint16_t time; /* integration time in `unit` */
	uint16_t pad;  /* the union is 12 bytes (its g_attr side) */
};

struct uvcd_imp_autozoom { /* IMPISPAutoZoom */
	int chan;
	int scaler_enable;
	int scaler_outwidth;
	int scaler_outheight;
	int crop_enable;
	int crop_left;
	int crop_top;
	int crop_width;
	int crop_height;
};

/* Exposure priority: with auto-exposure out of frame time and leaning on
 * gain, slow the sensor down a step so it can expose longer instead; once
 * the gain is low and the exposure would fit a faster frame, speed back up.
 * Gains are total gain as a plain multiple (the ISP reports [24.8]); the
 * gap between the two thresholds keeps it from hunting. */
#define UVCD_PRIORITY_PERIOD_MS 2000
#define UVCD_PRIORITY_SLOWER_GAIN 4.0f
#define UVCD_PRIORITY_FASTER_GAIN 2.0f

/* Run the sensor at `fps`, and tell the encoder so its rate control keeps
 * bits per second rather than bits per frame. */
static void set_sensor_fps(uvcd_pipeline_t *p, uint32_t fps)
{
	if (fps == p->sensor_fps)
		return;
	if (RSS_HAL_CALL(p->ops, isp_set_sensor_fps, p->hal_ctx, fps, 1) != RSS_OK) {
		LOGW("sensor fps %u rejected", fps);
		return;
	}
	if (p->configured)
		RSS_HAL_CALL(p->ops, enc_set_fps, p->hal_ctx, UVCD_ENC_CHN, fps, 1);
	if (p->sensor_fps)
		LOGI("sensor at %u fps", fps);
	p->sensor_fps = fps;
}

/* The listed frame rate one step slower/faster than `fps`, never below the
 * slowest listed nor above `ceiling`. */
static uint32_t fps_step(uint32_t fps, int dir, uint32_t ceiling)
{
	uint32_t best = fps;

	for (int i = 0; i < UVCD_NUM_INTERVALS; i++) {
		uint32_t f = uvcd_interval_fps(uvcd_intervals[i]);
		if (dir < 0 && f < fps && (best == fps || f > best))
			best = f;
		if (dir > 0 && f > fps && f <= ceiling && (best == fps || f < best))
			best = f;
	}
	return best;
}

/* The part of the sensor image the host sees for output `frame`: the
 * largest centred rectangle of the output's aspect ratio (so 4:3 sizes are
 * cropped, not squashed), narrowed by zoom and moved by pan/tilt. */
static void compute_view(const uvcd_pipeline_t *p, const struct uvcd_control_state *c,
			 uint8_t frame, int *x, int *y, int *w, int *h)
{
	int sw = p->sensor_w, sh = p->sensor_h;
	int ow = uvcd_frames[frame].width, oh = uvcd_frames[frame].height;
	int zoom = c->zoom < UVCD_ZOOM_MIN ? UVCD_ZOOM_MIN : c->zoom;

	int bw = sw, bh = (int)((int64_t)sw * oh / ow);
	if (bh > sh) {
		bh = sh;
		bw = (int)((int64_t)sh * ow / oh);
	}
	/* The ISP wants the crop aligned; round down, so it stays inside. */
	*w = (bw * UVCD_ZOOM_MIN / zoom) & ~15;
	*h = (bh * UVCD_ZOOM_MIN / zoom) & ~7;

	int spare_x = sw - *w, spare_y = sh - *h;
	*x = spare_x / 2 + (int)((int64_t)(spare_x / 2) * c->pan / UVCD_PANTILT_MAX);
	*y = spare_y / 2 - (int)((int64_t)(spare_y / 2) * c->tilt / UVCD_PANTILT_MAX);
	*x = (*x < 0 ? 0 : *x > spare_x ? spare_x : *x) & ~1;
	*y = (*y < 0 ? 0 : *y > spare_y ? spare_y : *y) & ~1;
}

int uvcd_apply_view(uvcd_pipeline_t *p, const struct uvcd_control_state *c)
{
	/* The view is relative to an output size; with no channel there is
	 * none yet, and configure_channel() works it out at the next start. */
	if (!p->configured)
		return RSS_OK;

	int x, y, w, h;
	compute_view(p, c, p->cur_frame, &x, &y, &w, &h);

	const struct uvcd_frame_info *fi = &uvcd_frames[p->cur_frame];
	struct uvcd_imp_autozoom z = {
		.chan = UVCD_FS_CHN,
		.scaler_enable = (w != fi->width || h != fi->height),
		.scaler_outwidth = fi->width,
		.scaler_outheight = fi->height,
		.crop_enable = (w != p->sensor_w || h != p->sensor_h),
		.crop_left = x,
		.crop_top = y,
		.crop_width = w,
		.crop_height = h,
	};
	int ret = RSS_HAL_CALL(p->ops, isp_set_auto_zoom, p->hal_ctx, &z);
	if (ret != RSS_OK) {
		LOGW("view %dx%d+%d+%d rejected: %d", w, h, x, y, ret);
		return ret;
	}
	p->view.x = x;
	p->view.y = y;
	p->view.w = w;
	p->view.h = h;
	LOGD("view %dx%d+%d+%d", w, h, x, y);

	/* Metering follows what is in view. */
	uvcd_apply_metering(p, c);
	return RSS_OK;
}

int uvcd_apply_exposure(uvcd_pipeline_t *p, const struct uvcd_control_state *c)
{
	struct uvcd_imp_expr e = {0};
	uint32_t fps = p->configured ? uvcd_interval_fps(p->cur_interval) : 0;

	if (c->ae_mode == UVCD_AE_MANUAL) {
		uint32_t us = (uint32_t)c->exposure_time * 100;

		if (fps) {
			/* Exposure priority lets the frame rate drop to make
			 * room; otherwise the exposure is cut to fit a frame. */
			if (c->ae_priority) {
				uint32_t fits = 1000000 / us;
				while (fps > fits) {
					uint32_t slower = fps_step(fps, -1, fps);
					if (slower == fps)
						break;
					fps = slower;
				}
			}
			set_sensor_fps(p, fps);
			uint32_t running_fps = p->sensor_fps ? p->sensor_fps : fps;
			if (us > 1000000 / running_fps)
				us = 1000000 / running_fps;
		}
		if (us > UINT16_MAX)
			us = UINT16_MAX;
		e.mode = 1;
		e.unit = 1;
		e.time = (uint16_t)us;
	} else if (fps) {
		/* Auto: start from the negotiated rate; with exposure priority
		 * on, the tick slows it down if the light calls for it. */
		set_sensor_fps(p, fps);
	}
	p->priority_checked_ms = monotonic_ms();

	return RSS_HAL_CALL(p->ops, isp_set_expr, p->hal_ctx, &e);
}

int uvcd_live_exposure(uvcd_pipeline_t *p)
{
	rss_exposure_t e;

	if (!p->hal_up || RSS_HAL_CALL(p->ops, isp_get_exposure, p->hal_ctx, &e) != RSS_OK)
		return -1;
	int t = (int)((e.exposure_time + 50) / 100);
	return t < UVCD_EXPOSURE_MIN ? UVCD_EXPOSURE_MIN : t;
}

static void exposure_priority_tick(uvcd_pipeline_t *p)
{
	const struct uvcd_control_state *c = &p->controls;

	if (!p->running || c->ae_mode == UVCD_AE_MANUAL || !c->ae_priority)
		return;
	int64_t now = monotonic_ms();
	if (now - p->priority_checked_ms < UVCD_PRIORITY_PERIOD_MS)
		return;
	p->priority_checked_ms = now;

	rss_exposure_t e;
	if (RSS_HAL_CALL(p->ops, isp_get_exposure, p->hal_ctx, &e) != RSS_OK)
		return;

	uint32_t negotiated = uvcd_interval_fps(p->cur_interval);
	uint32_t fps = p->sensor_fps ? p->sensor_fps : negotiated;
	float gain = (float)e.total_gain / 256.0f;
	uint32_t frame_us = 1000000 / fps;

	LOGD("exposure priority: %u of %u us, gain %.2fx, %u fps", e.exposure_time, frame_us,
	     (double)gain, fps);

	if (e.exposure_time >= frame_us * 9 / 10 && gain >= UVCD_PRIORITY_SLOWER_GAIN) {
		set_sensor_fps(p, fps_step(fps, -1, negotiated));
	} else if (fps < negotiated && gain <= UVCD_PRIORITY_FASTER_GAIN) {
		uint32_t faster = fps_step(fps, +1, negotiated);
		if (e.exposure_time <= (1000000 / faster) * 8 / 10)
			set_sensor_fps(p, faster);
	}
}

int uvcd_apply_metering(uvcd_pipeline_t *p, const struct uvcd_control_state *c)
{
	uint8_t weight[15][15];

	if (c->metering == UVCD_METERING_TUNING) {
		if (!p->ae_weight_ref_valid)
			return RSS_OK; /* never changed, so nothing to restore */
		return RSS_HAL_CALL(p->ops, isp_set_ae_weight, p->hal_ctx,
				    (const uint8_t(*)[15])p->ae_weight_ref);
	}

	/* The ISP meters on a 15x15 grid over the whole sensor. Zones outside
	 * the view get no weight, so what the host can't see doesn't set the
	 * exposure. Weights run 0-8. */
	int cx = p->view.x + p->view.w / 2, cy = p->view.y + p->view.h / 2;
	bool any = false;
	for (int r = 0; r < 15; r++) {
		for (int col = 0; col < 15; col++) {
			int zx = (2 * col + 1) * p->sensor_w / 30;
			int zy = (2 * r + 1) * p->sensor_h / 30;
			uint8_t w = 0;

			if (zx >= p->view.x && zx < p->view.x + p->view.w && zy >= p->view.y &&
			    zy < p->view.y + p->view.h) {
				/* 0 at the centre of the view, 1 at its edge */
				float dx = fabsf((float)(zx - cx)) / (float)(p->view.w / 2);
				float dy = fabsf((float)(zy - cy)) / (float)(p->view.h / 2);
				float d = dx > dy ? dx : dy;

				if (c->metering == UVCD_METERING_AVERAGE)
					w = 8;
				else if (c->metering == UVCD_METERING_CENTER)
					w = (uint8_t)(8.0f - 6.0f * d + 0.5f);
				else
					w = d <= 0.2f ? 8 : 0;
			}
			weight[r][col] = w;
			any |= (w != 0);
		}
	}
	/* A spot smaller than one zone: weight the zone under the centre. */
	if (!any)
		weight[cy * 15 / p->sensor_h][cx * 15 / p->sensor_w] = 8;

	return RSS_HAL_CALL(p->ops, isp_set_ae_weight, p->hal_ctx,
			    (const uint8_t(*)[15])weight);
}

int uvcd_apply_rotation(uvcd_pipeline_t *p, const struct uvcd_control_state *c)
{
	int flip = c->rotation >= 90 ? 1 : 0;
	int ret = RSS_HAL_CALL(p->ops, isp_set_hflip, p->hal_ctx, flip);
	if (ret != RSS_OK)
		return ret;
	return RSS_HAL_CALL(p->ops, isp_set_vflip, p->hal_ctx, flip);
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

static uint32_t effective_h264_gop(const uvcd_pipeline_t *p, uint32_t fps)
{
	return p->controls.h264_gop_frames > 0 ? (uint32_t)p->controls.h264_gop_frames : fps;
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
	return RSS_HAL_CALL(p->ops, enc_set_bitrate, p->hal_ctx, UVCD_ENC_CHN,
			    effective_h264_bitrate(p, p->cur_frame));
}

int uvcd_enc_apply_gop(uvcd_pipeline_t *p)
{
	if (!running_h264(p))
		return 0;
	return RSS_HAL_CALL(p->ops, enc_set_gop, p->hal_ctx, UVCD_ENC_CHN,
			    effective_h264_gop(p, uvcd_interval_fps(p->cur_interval)));
}

int uvcd_enc_request_keyframe(uvcd_pipeline_t *p)
{
	if (!running_h264(p))
		return 0; /* MJPEG is all keyframes; not streaming, nothing to do */
	return RSS_HAL_CALL(p->ops, enc_request_idr, p->hal_ctx, UVCD_ENC_CHN);
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
	/* Start on the view zoom/pan/tilt ask for; uvcd_apply_view() moves
	 * it live from then on. */
	int vx, vy, vw, vh;
	compute_view(p, &p->controls, frame, &vx, &vy, &vw, &vh);
	if (vw != p->sensor_w || vh != p->sensor_h) {
		fs_cfg.crop.enable = true;
		fs_cfg.crop.x = vx;
		fs_cfg.crop.y = vy;
		fs_cfg.crop.w = vw;
		fs_cfg.crop.h = vh;
	}
	if (fi->width != vw || fi->height != vh) {
		fs_cfg.scaler.enable = true;
		fs_cfg.scaler.out_width = fi->width;
		fs_cfg.scaler.out_height = fi->height;
	}

	int ret = RSS_HAL_CALL(p->ops, fs_create_channel, p->hal_ctx, UVCD_FS_CHN, &fs_cfg);
	if (ret != RSS_OK) {
		LOGE("fs_create_channel failed: %d", ret);
		return ret;
	}
	p->view.x = vx;
	p->view.y = vy;
	p->view.w = vw;
	p->view.h = vh;
	RSS_HAL_CALL(p->ops, fs_set_fifo, p->hal_ctx, UVCD_FS_CHN, 0);
	RSS_HAL_CALL(p->ops, fs_set_frame_depth, p->hal_ctx, UVCD_FS_CHN, 0);

	ret = RSS_HAL_CALL(p->ops, enc_create_group, p->hal_ctx, UVCD_ENC_CHN);
	if (ret != RSS_OK) {
		LOGE("enc_create_group failed: %d", ret);
		goto fail_fs;
	}

	const struct uvcd_control_state *c = &p->controls;
	bool jpeg = (codec == RSS_CODEC_JPEG);
	uint32_t bitrate = jpeg ? 0 : effective_h264_bitrate(p, frame);
	int16_t min_qp = (int16_t)c->h264_min_qp;
	int16_t max_qp = (int16_t)c->h264_max_qp;

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
		.gop_length = effective_h264_gop(p, fps_num),
		/* On T31 a JPEG channel's quality *is* its initial QP, and it can
		 * only be set here -- enc_set_jpeg_qp is unsupported on this SDK. */
		.init_qp = jpeg ? (int16_t)c->mjpeg_quality : -1,
		.min_qp = jpeg ? -1 : min_qp,
		.max_qp = jpeg ? -1 : max_qp,
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

	int ret = hal_bring_up(p);
	if (ret != RSS_OK)
		return ret;

	/* The sensor sets the pace: a 30 fps channel on a 25 fps sensor still
	 * delivers 25 while the host was promised 30. */
	set_sensor_fps(p, uvcd_interval_fps(interval));

	ret = configure_channel(p, format, frame, interval);
	if (ret != RSS_OK)
		return ret;
	p->configured = true;
	p->cur_format = format;
	p->cur_frame = frame;
	p->cur_interval = interval;

	/* Put the SDK's capture timestamps on CLOCK_MONOTONIC, the clock the
	 * gadget stamps USB payloads with, so a frame's PTS is when the
	 * sensor captured it rather than when it finished encoding. Redone on
	 * every start so the two clocks never drift far apart. */
	p->ts_rebased = RSS_HAL_CALL(p->ops, sys_rebase_timestamp, p->hal_ctx,
				     monotonic_us()) == RSS_OK;
	if (!p->ts_rebased)
		LOGW("SDK timestamp rebase failed; stamping frames on receipt");

	RSS_HAL_CALL(p->ops, fs_enable_channel, p->hal_ctx, UVCD_FS_CHN);
	RSS_HAL_CALL(p->ops, enc_start, p->hal_ctx, UVCD_ENC_CHN);

	/* Now that there is a frame rate to hold or give up: manual exposure
	 * with exposure priority may need a slower sensor. And metering can
	 * follow the channel's view. */
	uvcd_apply_exposure(p, &p->controls);
	uvcd_apply_metering(p, &p->controls);

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

void uvcd_pipeline_tick(uvcd_pipeline_t *p)
{
	exposure_priority_tick(p);

	if (!p->hal_up || p->configured) {
		p->idle_since_ms = 0;
		return;
	}

	int64_t now = monotonic_ms();
	if (!p->idle_since_ms) {
		p->idle_since_ms = now;
		return;
	}
	if (now - p->idle_since_ms >= UVCD_HAL_LINGER_MS)
		hal_shut_down(p);
}
