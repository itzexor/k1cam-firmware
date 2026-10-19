/*
 * uvcd_camera.c -- the sensor, ISP and IMP system, as one long-lived camera
 *
 * The camera comes up with the daemon and stays up until it exits. With no
 * stream it runs the default output's view, at UVCD_IDLE_FPS while
 * exposure settles and UVCD_IDLE_SLOW_FPS once it has (idle pacing, below),
 * so auto-exposure, white balance and metering keep following the scene,
 * and a host that starts streaming gets a settled picture from the first
 * frame. A stream leases the camera (uvcd_camera_acquire): the lease sets
 * the frame rate, the view of the negotiated output size and the colour
 * space, and releasing it returns to idle. Every control applies here,
 * live, whether or not a host is streaming. The framesource and encoder
 * channels are the stream's (uvcd_pipeline.c).
 *
 * Single sensor (T31 + GC2083): its identity and GPIOs come from the kernel
 * sensor driver via procfs (/proc/jz/sensor/sensor0/...).
 */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>

#include "uvcd.h"

/* --------------------------------------------------------------------------
 * Controls whose HAL representation isn't a plain scalar.
 *
 * UVC (and V4L2) express gamma as a single number and white balance as a
 * colour temperature. The ISP wants a 129-point curve and a mode+gains
 * struct respectively. Keeping both translations here means uvcd_gadget.c
 * stays a pure UVC-protocol file with no ISP-shaped special cases.
 */

static int apply_gamma(uvcd_camera_t *cam, int gamma_x100)
{
	uint16_t curve[UVCD_GAMMA_POINTS];

	/* Without the factory curve there is no reference to re-shape, and
	 * inventing one would mean guessing the table's value range. */
	if (!cam->gamma_ref_valid)
		return -ENOTSUP;

	if (gamma_x100 < UVCD_GAMMA_MIN)
		gamma_x100 = UVCD_GAMMA_MIN;
	if (gamma_x100 > UVCD_GAMMA_MAX)
		gamma_x100 = UVCD_GAMMA_MAX;

	if (gamma_x100 == UVCD_GAMMA_DEF) {
		/* Exactly the vendor tuning, no floating point rounding. */
		memcpy(curve, cam->gamma_ref, sizeof(curve));
	} else {
		/* Normalize against the curve's own peak so this works whatever
		 * the table's full-scale value happens to be, apply the extra
		 * power law, then scale back. Output = in^(1/gamma), so a
		 * larger gamma brightens -- the UVC/V4L2 convention. */
		uint16_t peak = 0;
		for (int i = 0; i < UVCD_GAMMA_POINTS; i++) {
			if (cam->gamma_ref[i] > peak)
				peak = cam->gamma_ref[i];
		}
		if (peak == 0)
			return -ENOTSUP;

		float exponent = (float)UVCD_GAMMA_DEF / (float)gamma_x100;
		for (int i = 0; i < UVCD_GAMMA_POINTS; i++) {
			float norm = (float)cam->gamma_ref[i] / (float)peak;
			float out = powf(norm, exponent) * (float)peak;

			if (out < 0.0f)
				out = 0.0f;
			if (out > (float)peak)
				out = (float)peak;
			curve[i] = (uint16_t)(out + 0.5f);
		}
	}

	return RSS_HAL_CALL(cam->ops, isp_set_gamma, cam->hal_ctx, curve);
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

static int apply_wb(uvcd_camera_t *cam, int temp_kelvin, int auto_on)
{
	rss_wb_config_t cfg;

	memset(&cfg, 0, sizeof(cfg));

	if (auto_on) {
		cfg.mode = RSS_WB_AUTO;
		return RSS_HAL_CALL(cam->ops, isp_set_wb, cam->hal_ctx, &cfg);
	}

	if (temp_kelvin < UVCD_WB_TEMP_MIN)
		temp_kelvin = UVCD_WB_TEMP_MIN;
	if (temp_kelvin > UVCD_WB_TEMP_MAX)
		temp_kelvin = UVCD_WB_TEMP_MAX;

	float d_mired = 1e6f / (float)temp_kelvin - 1e6f / UVCD_WB_REF_KELVIN;
	cfg.mode = RSS_WB_MANUAL;
	cfg.r_gain = wb_gain(UVCD_WB_REF_RGAIN, UVCD_WB_R_PER_MIRED, d_mired);
	cfg.b_gain = wb_gain(UVCD_WB_REF_BGAIN, UVCD_WB_B_PER_MIRED, d_mired);

	int ret = RSS_HAL_CALL(cam->ops, isp_set_wb, cam->hal_ctx, &cfg);
	if (ret != RSS_OK)
		return ret;

	/* In manual mode the ISP takes the colour temperature for its
	 * CT-dependent tuning (colour matrix, saturation, shading) from here
	 * rather than from its own estimate, so keep it on the same light the
	 * gains assume. The gains are what matter; this only refines them. */
	unsigned int ct = (unsigned int)temp_kelvin;
	if (RSS_HAL_CALL(cam->ops, isp_set_awb_ct_attr, cam->hal_ctx, &ct) != RSS_OK)
		LOGD("isp_set_awb_ct_attr(%u) failed", ct);

	LOGD("white balance %d K: rgain=%u bgain=%u", temp_kelvin, cfg.r_gain, cfg.b_gain);
	return RSS_OK;
}

int uvcd_camera_awb_ct(uvcd_camera_t *cam)
{
	unsigned int ct = 0;

	if (!cam->up)
		return -1;
	if (RSS_HAL_CALL(cam->ops, isp_get_awb_ct_attr, cam->hal_ctx, &ct) != RSS_OK || ct == 0)
		return -1;
	return (int)ct;
}

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

struct uvcd_imp_csc { /* IMPISPCscAttr */
	int mode;      /* IMPISPCscCgMode, below */
	int coef[9];   /* custom 3x3 matrix, used in USER mode only */
	int offset[2]; /* UV, Y */
	int y_clip[2]; /* max, min */
	int c_clip[2]; /* max, min */
};

enum { /* IMPISPCscCgMode */
	UVCD_CSC_BT601_FULL,
	UVCD_CSC_BT601_CLIP,
	UVCD_CSC_BT709_FULL,
	UVCD_CSC_BT709_CLIP,
};

/* Make the ISP's YUV what the stream says it is. The T31 encoder tags
 * H.264 as BT.709 limited range and gives no way to change that, while the
 * ISP comes up producing BT.601 full range: every decoder then stretched
 * 16..235 to 0..255, crushing ~40% of a dim scene to black and clipping
 * whites. JPEG decoders assume BT.601 full range (JFIF), the ISP default.
 * The clip limits are left as read (255/0), so blown highlights land a
 * little above 235; decoders clip those to white anyway. */
static void apply_csc(uvcd_camera_t *cam, uint8_t format)
{
	struct uvcd_imp_csc c = {0};
	int was = -1;

	if (RSS_HAL_CALL(cam->ops, isp_get_csc_attr, cam->hal_ctx, &c) == RSS_OK)
		was = c.mode;
	c.mode = format == UVCD_FMT_H264 ? UVCD_CSC_BT709_CLIP : UVCD_CSC_BT601_FULL;
	if (c.mode == was)
		return;
	int ret = RSS_HAL_CALL(cam->ops, isp_set_csc_attr, cam->hal_ctx, &c);
	if (ret != RSS_OK)
		LOGW("ISP CSC mode %d rejected (%d)", c.mode, ret);
	else
		LOGI("ISP CSC mode %d -> %d", was, c.mode);
}

/* Exposure priority: with auto-exposure out of frame time and leaning on
 * gain, slow the sensor down a step so it can expose longer instead; once
 * the gain is low and the exposure would fit a faster frame, speed back up.
 * Gains are total gain as a plain multiple (the ISP reports [24.8]); the
 * gap between the two thresholds keeps it from hunting. */
#define UVCD_PRIORITY_PERIOD_MS 2000
#define UVCD_PRIORITY_SLOWER_GAIN 16.0f
#define UVCD_PRIORITY_FASTER_GAIN 8.0f

/* Run the sensor at `fps`. A stream keeps its encoder at the same rate
 * (uvcd_pipeline_tick), so rate control holds bits per second rather than
 * bits per frame. */
static int set_sensor_fps(uvcd_camera_t *cam, uint32_t fps)
{
	if (fps == cam->sensor_fps)
		return RSS_OK;
	int ret = RSS_HAL_CALL(cam->ops, isp_set_sensor_fps, cam->hal_ctx, fps, 1);
	if (ret != RSS_OK) {
		LOGW("sensor fps %u rejected (%d)", fps, ret);
		return ret;
	}
	if (cam->sensor_fps)
		LOGI("sensor at %u fps", fps);
	cam->sensor_fps = fps;
	/* A new rate brings the SDK's own integration limit for it. */
	cam->it_capped = false;
	return RSS_OK;
}

/* The rate the output runs at: the stream's negotiated one, else idle's. */
static uint32_t output_fps(const uvcd_camera_t *cam)
{
	if (cam->leased)
		return cam->lease_fps;
	return cam->idle_settled && cam->it_max_idle ? UVCD_IDLE_SLOW_FPS : UVCD_IDLE_FPS;
}

/* At the slow idle rate a frame has room for far longer exposures than a
 * UVCD_IDLE_FPS one, and auto-exposure would take them, trading gain for
 * exposure time; a stream at that rate would then start on an exposure
 * that no longer fits its frames. Hold the limit to the UVCD_IDLE_FPS one.
 * If the ISP refuses, idle stays at UVCD_IDLE_FPS. */
static int apply_idle_cap(uvcd_camera_t *cam)
{
	if (cam->it_capped)
		return RSS_OK;
	/* Still the SDK's own limit for this rate: keep it for a stream that
	 * runs the same rate (uvcd_camera_acquire). */
	uint32_t it = 0;
	if (!cam->it_max_slow &&
	    RSS_HAL_CALL(cam->ops, isp_get_ae_it_max, cam->hal_ctx, &it) == RSS_OK)
		cam->it_max_slow = it;
	if (RSS_HAL_CALL(cam->ops, isp_set_ae_it_max, cam->hal_ctx, cam->it_max_idle) == RSS_OK) {
		cam->it_capped = true;
		return RSS_OK;
	}
	LOGW("AE integration limit %u rejected; idle stays at %d fps", cam->it_max_idle,
	     UVCD_IDLE_FPS);
	cam->it_max_idle = 0;
	return set_sensor_fps(cam, UVCD_IDLE_FPS);
}

/* One listed step, at most `ceiling`, with a 15 fps priority floor.
 * A host explicitly negotiating 5/10 fps keeps that rate. */
static uint32_t fps_step(uint32_t fps, int dir, uint32_t ceiling)
{
	uint32_t best = fps;

	for (int i = 0; i < UVCD_NUM_INTERVALS; i++) {
		uint32_t f = uvcd_interval_fps(uvcd_intervals[i]);
		if (dir < 0 && f < fps && f >= (ceiling < 15 ? ceiling : 15) &&
		    (best == fps || f > best))
			best = f;
		if (dir > 0 && f > fps && f <= ceiling && (best == fps || f < best))
			best = f;
	}
	return best;
}

/* The zoom actually used at this output size. The control keeps what the
 * host asked for, so switching to a size that can take it gets it back. */
static int effective_zoom(const uvcd_camera_t *cam, const struct uvcd_control_state *c,
			  uint8_t frame)
{
	int zoom = c->zoom < UVCD_ZOOM_MIN ? UVCD_ZOOM_MIN : c->zoom;
	int max = cam->isp_clk_hz >= UVCD_ISP_FAST_HZ ? uvcd_frames[frame].max_zoom
						    : uvcd_frames[frame].max_zoom_slow;
	return zoom > max ? max : zoom;
}

/* The part of the sensor image the host sees for output `frame`: the
 * largest centred rectangle of the output's aspect ratio (so 4:3 sizes are
 * cropped, not squashed), narrowed by zoom and moved by pan/tilt. */
static void compute_view(const uvcd_camera_t *cam, const struct uvcd_control_state *c,
			 uint8_t frame, int *x, int *y, int *w, int *h)
{
	int sw = cam->sensor_w, sh = cam->sensor_h;
	int ow = uvcd_frames[frame].width, oh = uvcd_frames[frame].height;
	int zoom = effective_zoom(cam, c, frame);

	int bw = sw, bh = (int)((int64_t)sw * oh / ow);
	if (bh > sh) {
		bh = sh;
		bw = (int)((int64_t)sh * ow / oh);
	}
	/* The ISP wants the crop aligned; round down, so it stays inside. */
	*w = bw * UVCD_ZOOM_MIN / zoom;
	*h = bh * UVCD_ZOOM_MIN / zoom;
	*w &= ~1;
	*h &= ~1;

	int spare_x = sw - *w, spare_y = sh - *h;
	*x = spare_x / 2 + (int)((int64_t)(spare_x / 2) * c->pan / UVCD_PANTILT_MAX);
	*y = spare_y / 2 - (int)((int64_t)(spare_y / 2) * c->tilt / UVCD_PANTILT_MAX);
	*x = (*x < 0 ? 0 : *x > spare_x ? spare_x : *x) & ~1;
	*y = (*y < 0 ? 0 : *y > spare_y ? spare_y : *y) & ~1;
}

/* The output size the view is for: the stream's, or the idle default. */
static uint8_t output_frame(const uvcd_camera_t *cam)
{
	return cam->leased ? cam->lease_frame : UVCD_IDLE_FRAME;
}

/* Recompute the view for the output size and `c`'s zoom/pan/tilt. When the
 * rectangle moves, the metering weights no longer fit it; the caller writes
 * them (apply_metering), and the tick retries a write the ISP rejected. */
static void set_view(uvcd_camera_t *cam, const struct uvcd_control_state *c)
{
	int x, y, w, h;

	cam->view_frame = output_frame(cam);
	compute_view(cam, c, cam->view_frame, &x, &y, &w, &h);
	if (x == cam->view.x && y == cam->view.y && w == cam->view.w && h == cam->view.h)
		return;
	cam->view.x = x;
	cam->view.y = y;
	cam->view.w = w;
	cam->view.h = h;
	cam->metering_valid = false;
	int zoom = effective_zoom(cam, c, cam->view_frame);
	if (zoom < c->zoom)
		LOGI("zoom %d limited to %d at %ux%u (ISP upscale limit at %d MHz)", c->zoom, zoom,
		     uvcd_frames[cam->view_frame].width, uvcd_frames[cam->view_frame].height,
		     cam->isp_clk_hz / 1000000);
}

bool uvcd_camera_view_moves(const uvcd_camera_t *cam, const struct uvcd_control_state *c)
{
	int x, y, w, h;

	compute_view(cam, c, output_frame(cam), &x, &y, &w, &h);
	return x != cam->view.x || y != cam->view.y || w != cam->view.w || h != cam->view.h;
}

/* Build the SDK write for the rate that actually reached the sensor. */
static struct uvcd_imp_expr exposure_expr(const struct uvcd_control_state *c, uint32_t fps)
{
	struct uvcd_imp_expr e = {0};
	if (c->ae_mode == UVCD_AE_MANUAL) {
		uint32_t us = (uint32_t)c->exposure_time * 100;
		if (us > 1000000 / fps)
			us = 1000000 / fps;
		if (us > UINT16_MAX)
			us = UINT16_MAX;
		e.mode = 1;
		e.unit = 1;
		e.time = (uint16_t)us;
	}
	return e;
}

static int apply_exposure(uvcd_camera_t *cam, const struct uvcd_control_state *c)
{
	uint32_t fps = output_fps(cam), old_fps = cam->sensor_fps;
	bool old_capped = cam->it_capped;
	uint32_t old_limit = old_capped ? cam->it_max_idle : cam->it_max_slow;

	if (c->ae_mode == UVCD_AE_MANUAL && cam->leased && c->ae_priority) {
		uint32_t us = (uint32_t)c->exposure_time * 100;
		uint32_t fits = 1000000 / us;
		while (fps > fits) {
			uint32_t slower = fps_step(fps, -1, cam->lease_fps);
			if (slower == fps)
				break;
			fps = slower;
		}
	}
	int ret = set_sensor_fps(cam, fps);
	if (ret != RSS_OK)
		return ret;
	if (fps == UVCD_IDLE_SLOW_FPS && !cam->leased) {
		ret = apply_idle_cap(cam);
		if (ret != RSS_OK)
			goto restore_exposure;
	}
	struct uvcd_imp_expr e = exposure_expr(c, cam->sensor_fps);
	ret = RSS_HAL_CALL(cam->ops, isp_set_expr, cam->hal_ctx, &e);
	if (ret == RSS_OK) {
		cam->priority_checked_ms = uvcd_monotonic_ms();
		return RSS_OK;
	}

restore_exposure:
	LOGW("exposure update rejected (%d)", ret);
	if (old_fps) {
		int restore = set_sensor_fps(cam, old_fps);
		if (restore == RSS_OK && old_limit && old_fps == UVCD_IDLE_SLOW_FPS) {
			restore = RSS_HAL_CALL(cam->ops, isp_set_ae_it_max, cam->hal_ctx, old_limit);
			if (restore == RSS_OK)
				cam->it_capped = old_capped;
		}
		if (restore == RSS_OK) {
			struct uvcd_imp_expr old = exposure_expr(cam->controls, old_fps);
			restore = RSS_HAL_CALL(cam->ops, isp_set_expr, cam->hal_ctx, &old);
		}
		if (restore != RSS_OK)
			LOGE("could not restore exposure after rejected write (%d)", restore);
	}
	return ret;
}

int uvcd_camera_live_exposure(uvcd_camera_t *cam)
{
	rss_exposure_t e;

	if (!cam->up || RSS_HAL_CALL(cam->ops, isp_get_exposure, cam->hal_ctx, &e) != RSS_OK)
		return -1;
	int t = (int)((e.exposure_time + 50) / 100);
	return t < UVCD_EXPOSURE_MIN ? UVCD_EXPOSURE_MIN : t;
}

static void exposure_priority_tick(uvcd_camera_t *cam)
{
	const struct uvcd_control_state *c = cam->controls;

	if (!cam->leased || c->ae_mode == UVCD_AE_MANUAL || !c->ae_priority)
		return;
	int64_t now = uvcd_monotonic_ms();
	if (now - cam->priority_checked_ms < UVCD_PRIORITY_PERIOD_MS)
		return;
	cam->priority_checked_ms = now;

	rss_exposure_t e;
	if (RSS_HAL_CALL(cam->ops, isp_get_exposure, cam->hal_ctx, &e) != RSS_OK)
		return;

	uint32_t negotiated = cam->lease_fps;
	uint32_t fps = cam->sensor_fps ? cam->sensor_fps : negotiated;
	float gain = (float)e.total_gain / 256.0f;
	uint32_t frame_us = 1000000 / fps;

	LOGD("exposure priority: %u of %u us, gain %.2fx, %u fps", e.exposure_time, frame_us,
	     (double)gain, fps);

	if (e.exposure_time >= frame_us * 9 / 10 && gain >= UVCD_PRIORITY_SLOWER_GAIN) {
		set_sensor_fps(cam, fps_step(fps, -1, negotiated));
	} else if (fps < negotiated && gain <= UVCD_PRIORITY_FASTER_GAIN) {
		uint32_t faster = fps_step(fps, +1, negotiated);
		if (e.exposure_time <= (1000000 / faster) * 8 / 10)
			set_sensor_fps(cam, faster);
	}
}

/* The ISP meters on a 15x15 grid over the whole sensor. A zone counts as
 * in the view when its centre is. */
static bool zone_in_view(const uvcd_camera_t *cam, int r, int col)
{
	int zx = (2 * col + 1) * cam->sensor_w / 30;
	int zy = (2 * r + 1) * cam->sensor_h / 30;
	return zx >= cam->view.x && zx < cam->view.x + cam->view.w && zy >= cam->view.y &&
	       zy < cam->view.y + cam->view.h;
}

/* Fixed weights for `metering` (any mode but TUNING; SUBJECT starts from
 * CENTER). Zones outside the view get no weight, so what the host can't see
 * doesn't set the exposure. Weights run 0-8. */
static void metering_weights(const uvcd_camera_t *cam, int metering, uint8_t weight[15][15])
{
	int cx = cam->view.x + cam->view.w / 2, cy = cam->view.y + cam->view.h / 2;
	bool any = false;
	for (int r = 0; r < 15; r++) {
		for (int col = 0; col < 15; col++) {
			uint8_t w = 0;

			if (zone_in_view(cam, r, col)) {
				int zx = (2 * col + 1) * cam->sensor_w / 30;
				int zy = (2 * r + 1) * cam->sensor_h / 30;
				/* 0 at the centre of the view, 1 at its edge */
				float dx = fabsf((float)(zx - cx)) / (float)(cam->view.w / 2);
				float dy = fabsf((float)(zy - cy)) / (float)(cam->view.h / 2);
				float d = dx > dy ? dx : dy;

				if (metering == UVCD_METERING_AVERAGE)
					w = 8;
				else if (metering == UVCD_METERING_SPOT)
					w = d <= 0.2f ? 8 : 0;
				else
					w = (uint8_t)(8.0f - 6.0f * d + 0.5f);
			}
			weight[r][col] = w;
			any |= (w != 0);
		}
	}
	/* A spot smaller than one zone: weight the zone under the centre. */
	if (!any)
		weight[cy * 15 / cam->sensor_h][cx * 15 / cam->sensor_w] = 8;
}

static int apply_ae_comp(uvcd_camera_t *cam, const struct uvcd_control_state *c)
{
	int v = c->ae_comp;

	if (c->metering == UVCD_METERING_SUBJECT)
		v = (int)((float)v * (1.0f - cam->ae_comp_cut) + 0.5f);
	return RSS_HAL_CALL(cam->ops, isp_set_ae_comp, cam->hal_ctx, v);
}

/* Write the weights for c->metering over the current view, whatever the
 * ISP has now. */
static int write_metering(uvcd_camera_t *cam, const struct uvcd_control_state *c)
{
	uint8_t weight[15][15];

	if (c->metering == UVCD_METERING_TUNING) {
		/* Without the reference there is nothing to restore. */
		if (cam->ae_weight_ref_valid)
			memcpy(weight, cam->ae_weight_ref, sizeof(weight));
		else
			return -ENOTSUP;
	} else {
		metering_weights(cam, c->metering, weight);
	}

	int ret = RSS_HAL_CALL(cam->ops, isp_set_ae_weight, cam->hal_ctx,
			      (const uint8_t(*)[15])weight);
	if (ret != RSS_OK)
		return ret;
	/* Track accepted weights even if restoring compensation below
	 * fails. Subject metering starts from these on the next tick. */
	for (int r = 0; r < 15; r++)
		for (int col = 0; col < 15; col++)
			cam->subject_weight[r][col] = weight[r][col];
	memcpy(cam->subject_written, weight, sizeof(weight));
	cam->subject_checked_ms = cam->guard_checked_ms = uvcd_monotonic_ms();
	cam->metering_mode = c->metering;
	cam->metering_valid = false;
	/* Only subject metering holds compensation back. Keep its cut until
	 * the ISP accepts the uncut value. On failure the caller keeps its
	 * stored mode, which the tick restores because metering is invalid. */
	if (c->metering != UVCD_METERING_SUBJECT && cam->ae_comp_cut != 0.0f) {
		int ret = apply_ae_comp(cam, c);
		if (ret != RSS_OK)
			return ret;
		cam->ae_comp_cut = 0.0f;
	}
	cam->metering_valid = true;
	return RSS_OK;
}

/* Metering mode c->metering over the current view. The weights are only
 * re-initialized when the mode or the view changed, so a stream that starts
 * on the view the camera already has keeps what subject metering learned
 * while idle. */
static int apply_metering(uvcd_camera_t *cam, const struct uvcd_control_state *c)
{
	if (cam->metering_valid && cam->metering_mode == c->metering)
		return RSS_OK;
	return write_metering(cam, c);
}

/* Subject metering. The chamber camera looks at small, often white prints on
 * a dark bed in a dark chamber, and any fixed weighting averages the two:
 * the dark bed pulls the exposure up and the print clips. Two loops fix
 * that, both from the ISP's own statistics, so they cost nothing per frame:
 *
 * - Find the subject: zones well above the median brightness of the view
 *   are what stands out, and are eased up to full weight over the
 *   center-weighted base. On the K1 the median zone is the unlit chamber,
 *   so the lit bed and the prints on it both count. While neither zone
 *   clips or sits in the noise floor, the ratio to the median doesn't
 *   change with exposure, so this doesn't fight auto-exposure. With
 *   nothing standing out it is plain center-weighted.
 *
 * - Guard the highlights: metering a white print to auto-exposure's target
 *   alone would make it grey, so the weights only steer. What decides is
 *   the brightest UVCD_GUARD_TOP of the picture: while it clips,
 *   exposure compensation is held back (to at most UVCD_GUARD_MAX_CUT);
 *   once it sits well below clipping, it is given back. Each step is sized
 *   to bring it to UVCD_GUARD_AIM, so a light switched on or off takes a
 *   step or two, not ten. Anything smaller than UVCD_GUARD_TOP, such as a
 *   glint, may clip. Only what is in the view counts (view_share).
 *   It only judges a picture auto-exposure has locked on: the frames right
 *   after the light goes up clip until AE catches up, and holding the
 *   compensation back for those left the picture dark for seconds after
 *   (in flickering light, ~8 s after the cut reached its floor). */
#define UVCD_SUBJECT_PERIOD_MS 500
#define UVCD_SUBJECT_RATIO_LO 1.5f /* zone / median: no extra weight below */
#define UVCD_SUBJECT_RATIO_HI 3.0f /* full weight from here */
#define UVCD_SUBJECT_MEDIAN_FLOOR 4.0f /* below it, ratios are dark-zone noise */
#define UVCD_SUBJECT_EASE 0.5f     /* of the way to the target, per period */
#define UVCD_GUARD_PERIOD_MS 1000  /* at most one step per period */
#define UVCD_GUARD_TOP 0.01f       /* the brightest 1% of the picture */
/* The histogram is linear, taken before the gamma curve: on the K1, bin 219
 * came out at 251 of 255, bin 200 at about 238 and bin 150 at about 211.
 *
 * Sweeping exposure compensation over a fixed scene (comp 32..255), the
 * brightest 1% stayed at 3.3x the histogram mean to within 3% until it
 * saturated, so the top bin scales with exposure and one ratio sizes the
 * step. Compensation is not linear in exposure, though: AE's target was
 * proportional to it up to 128 (7 at 32, 27 at 128), then rose twice as
 * steeply (48 at 176, 86 at 255). There a step overshoots, and giving back
 * the full ratio after a cut hunts (comp 255 swinging 128 <-> 230 forever,
 * simulated on the measured curve), so give-back takes the square root of
 * it: settled within 4 steps for comp 64..255 in that simulation.
 *
 * UVCD_GUARD_AIM sits in the band where AE's -5%..+15% lock window keeps it.
 *
 * Clipped pixels pile up in the one bin at the white level, nothing above
 * it. Which bin that is moves with gain (247..250 near unity, 255 at 2.3x
 * on an LED hotspot), so the guard looks for the pile-up, not a bin. A top
 * that falls in it has saturated and its true level is unknown: halve. A
 * top below it, on the hotspot's falloff, is a real pixel and scales like
 * any other. */
#define UVCD_GUARD_CLIP_BIN 200
#define UVCD_GUARD_LOW_BIN 150
#define UVCD_GUARD_AIM 170.0f
#define UVCD_GUARD_MAX_CUT 0.5f /* keep at least half the requested compensation */

static int cmp_u32(const void *a, const void *b)
{
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
	return (x > y) - (x < y);
}

static void subject_weights(uvcd_camera_t *cam)
{
	uint32_t zone[15][15], sorted[225];
	uint8_t base[15][15], weight[15][15];
	int n = 0;

	if (RSS_HAL_CALL(cam->ops, isp_get_ae_zone, cam->hal_ctx, zone) != RSS_OK)
		return;
	for (int r = 0; r < 15; r++)
		for (int col = 0; col < 15; col++)
			if (zone_in_view(cam, r, col))
				sorted[n++] = zone[r][col];
	if (n == 0)
		return;
	qsort(sorted, (size_t)n, sizeof(sorted[0]), cmp_u32);
	float median = (float)sorted[n / 2];
	if (median < UVCD_SUBJECT_MEDIAN_FLOOR)
		median = UVCD_SUBJECT_MEDIAN_FLOOR;

	metering_weights(cam, UVCD_METERING_CENTER, base);
	int subject = 0;
	for (int r = 0; r < 15; r++) {
		for (int col = 0; col < 15; col++) {
			float target = base[r][col];

			if (zone_in_view(cam, r, col)) {
				float s = ((float)zone[r][col] / median - UVCD_SUBJECT_RATIO_LO) /
					  (UVCD_SUBJECT_RATIO_HI - UVCD_SUBJECT_RATIO_LO);
				s = s < 0.0f ? 0.0f : s > 1.0f ? 1.0f : s;
				target += (8.0f - target) * s;
				subject += s >= 0.5f;
			}
			float *w = &cam->subject_weight[r][col];
			*w += (target - *w) * UVCD_SUBJECT_EASE;
			weight[r][col] = (uint8_t)(*w + 0.5f);
		}
	}
	LOGD("subject metering: median %.0f, max %u, %d subject zones", (double)median,
	     sorted[n - 1], subject);
	if (memcmp(weight, cam->subject_written, sizeof(weight)) == 0)
		return;
	if (RSS_HAL_CALL(cam->ops, isp_set_ae_weight, cam->hal_ctx,
			 (const uint8_t(*)[15])weight) == RSS_OK)
		memcpy(cam->subject_written, weight, sizeof(weight));
}

/* How much of the histogram's top is in the view. The histogram covers the
 * whole sensor whatever the view: on the K1 it did not change with the
 * weights, the view or a stream's crop, and neither SetAeHist's zone counts
 * nor AE_SetROI narrow it. A crop beside a lit plate saw the plate clip
 * outside it, had its compensation halved, and lost 40% of its brightness.
 * The zones are in the histogram's units and say where the light is: the
 * view's brightest zone against the sensor's. 1 whenever the brightest zone
 * is in view, as it always is at full view. */
static float view_share(uvcd_camera_t *cam)
{
	uint32_t zone[15][15], in = 0, all = 0;

	if (RSS_HAL_CALL(cam->ops, isp_get_ae_zone, cam->hal_ctx, zone) != RSS_OK)
		return 1.0f;
	for (int r = 0; r < 15; r++) {
		for (int col = 0; col < 15; col++) {
			if (zone[r][col] > all)
				all = zone[r][col];
			if (zone_in_view(cam, r, col) && zone[r][col] > in)
				in = zone[r][col];
		}
	}
	return in < all ? (float)in / (float)all : 1.0f;
}

static void highlight_guard(uvcd_camera_t *cam)
{
	uint32_t bins[256];
	uint64_t total = 0, above = 0;
	int top = 255;

	if (RSS_HAL_CALL(cam->ops, isp_get_ae_hist_origin, cam->hal_ctx, bins) != RSS_OK)
		return;
	for (int i = 0; i < 256; i++)
		total += bins[i];
	if (total == 0)
		return;
	/* The darkest bin the brightest UVCD_GUARD_TOP reaches down into. */
	for (; top > 0; top--) {
		above += bins[top];
		if ((float)above >= (float)total * UVCD_GUARD_TOP)
			break;
	}

	/* Saturated: half the brightest UVCD_GUARD_TOP in the top bin alone.
	 * A pile-up brighter than anything in view is outside it. */
	float share = view_share(cam);
	float level = (float)top * share;
	bool saturated = share == 1.0f &&
			 (float)bins[top] * 2.0f >= (float)total * UVCD_GUARD_TOP;
	float scale = 1.0f - cam->ae_comp_cut;
	if (level >= UVCD_GUARD_CLIP_BIN && saturated)
		scale *= 0.5f;
	else if (level >= UVCD_GUARD_CLIP_BIN)
		scale *= UVCD_GUARD_AIM / level;
	else if (level < UVCD_GUARD_LOW_BIN)
		scale = level > 0.0f ? scale * sqrtf(UVCD_GUARD_AIM / level) : 1.0f;
	if (scale > 1.0f)
		scale = 1.0f;
	if (scale < 1.0f - UVCD_GUARD_MAX_CUT)
		scale = 1.0f - UVCD_GUARD_MAX_CUT;

	float cut = 1.0f - scale;
	LOGD("highlight guard: top %.1f%% at bin %d, %.0f in view, compensation cut %.0f%%",
	     (double)(UVCD_GUARD_TOP * 100.0f), top, (double)level, (double)(cut * 100.0f));
	if (cut == cam->ae_comp_cut)
		return;
	float old_cut = cam->ae_comp_cut;
	cam->ae_comp_cut = cut;
	if (apply_ae_comp(cam, cam->controls) != RSS_OK)
		cam->ae_comp_cut = old_cut; /* not applied: try again next time */
}

struct uvcd_imp_ae_state { /* IMPISPAEState, T31 1.1.6 */
	bool stable;
	unsigned int target;
	unsigned int mean;
};

/* Auto-exposure holds its target: GetAeState's stable is set once the
 * metered mean is within 1 of the target and stays set while it is within
 * -5%..+15% of it (reversed from tx-isp's ae0_tune2), and clear while AE
 * adjusts or is pinned at a limit. Without the call, take it as locked. */
static bool ae_locked(uvcd_camera_t *cam)
{
	struct uvcd_imp_ae_state state = {0};

	return RSS_HAL_CALL(cam->ops, isp_get_ae_state, cam->hal_ctx, &state) != RSS_OK ||
	       state.stable;
}

static void subject_metering_tick(uvcd_camera_t *cam)
{
	const struct uvcd_control_state *c = cam->controls;

	if (c->metering != UVCD_METERING_SUBJECT || c->ae_mode == UVCD_AE_MANUAL)
		return;
	int64_t now = uvcd_monotonic_ms();
	if (now - cam->subject_checked_ms < UVCD_SUBJECT_PERIOD_MS)
		return;
	cam->subject_checked_ms = now;
	subject_weights(cam);
	if (now - cam->guard_checked_ms >= UVCD_GUARD_PERIOD_MS && ae_locked(cam)) {
		cam->guard_checked_ms = now;
		highlight_guard(cam);
	}
}

static int apply_rotation(uvcd_camera_t *cam, const struct uvcd_control_state *c)
{
	int flip = c->rotation >= 90 ? 1 : 0;
	int old_h = cam->controls->rotation >= 90, old_v = old_h;
	/* Prefer the actual bits, including during startup. */
	int h = 0, v = 0;
	if (RSS_HAL_CALL(cam->ops, isp_get_hvflip, cam->hal_ctx, &h, &v) == RSS_OK) {
		old_h = h;
		old_v = v;
	}
	int ret = RSS_HAL_CALL(cam->ops, isp_set_hflip, cam->hal_ctx, flip);
	if (ret != RSS_OK)
		return ret;
	ret = RSS_HAL_CALL(cam->ops, isp_set_vflip, cam->hal_ctx, flip);
	if (ret != RSS_OK) {
		/* On T31 each setter writes both bits from the HAL's own copy,
		 * which the rejected call has already changed: restoring hflip
		 * alone left the K1 flipped vertically. Restore vflip first,
		 * which writes what the ISP still has, then hflip. */
		int rv = RSS_HAL_CALL(cam->ops, isp_set_vflip, cam->hal_ctx, old_v);
		int rh = RSS_HAL_CALL(cam->ops, isp_set_hflip, cam->hal_ctx, old_h);
		if (rv != RSS_OK || rh != RSS_OK)
			LOGE("could not restore flips after rejected rotation (%d)",
			     rv != RSS_OK ? rv : rh);
	}
	return ret;
}

/* --------------------------------------------------------------------------
 * Idle pacing. Auto-exposure and white balance take a step per frame, so
 * idle runs at UVCD_IDLE_FPS while they settle, and at UVCD_IDLE_SLOW_FPS
 * once they have: a sixth of the ISP work, with the scene still followed.
 *
 * What settled means comes from the ISP's statistics, not its AE state:
 * GetAeState's mean is only updated while AE holds its target (it keeps
 * the old value while AE adjusts, and while AE is pinned at a limit it
 * never updates), so it can't see the light change. Instead, every
 * UVCD_IDLE_PERIOD_MS: the brightness AE meters (the zone statistics under
 * the weights in the ISP) and the exposure it uses (time x gain).
 *
 * - Settling: once three samples in a row agree, after at least
 *   UVCD_SETTLE_MIN_MS, the camera has settled, also when AE is pinned at
 *   a limit in a scene too dark or too bright for it -- nothing more full
 *   rate could do there. UVCD_SETTLE_MAX_MS bounds a scene that never
 *   holds still.
 * - Settled: the scene changed when the brightness or the exposure moves
 *   away from what it settled on, for UVCD_CHANGE_SAMPLES samples in a
 *   row, so a single flickering frame doesn't count.
 *
 * A stream ending, a control that changes what AE does, and bring-up
 * start settling again. Manual exposure has nothing to settle.
 */
#define UVCD_IDLE_PERIOD_MS 500
#define UVCD_SETTLE_MIN_MS 1000  /* 30 frames at full rate, at least */
#define UVCD_SETTLE_MAX_MS 10000
#define UVCD_SETTLE_L_TOL 0.05f  /* brightness within 5% of the median */
#define UVCD_SETTLE_EV_TOL 0.03f /* exposure within 3% */
#define UVCD_CHANGE_L 1.25f      /* brightness 25% off what it settled on */
#define UVCD_CHANGE_EV 1.10f     /* exposure 10% off */
#define UVCD_CHANGE_SAMPLES 2
#define UVCD_IDLE_L_FLOOR 1.0f   /* below, ratios are quantization noise */

static void idle_reset(uvcd_camera_t *cam)
{
	cam->idle_samples = 0;
	cam->idle_changed = 0;
	cam->settle_since_ms = uvcd_monotonic_ms();
}

/* Settle again at UVCD_IDLE_FPS. */
static int idle_settle(uvcd_camera_t *cam, const char *why)
{
	if (cam->leased)
		return RSS_OK;
	bool was = cam->idle_settled;
	cam->idle_settled = false;
	idle_reset(cam);
	if (was && cam->up) {
		LOGI("idle: %s; settling at %d fps", why, UVCD_IDLE_FPS);
		return apply_exposure(cam, cam->controls);
	}
	return RSS_OK;
}

static void idle_go_slow(uvcd_camera_t *cam, float l, float ev)
{
	cam->idle_settled = true;
	cam->settled_l = l;
	cam->settled_ev = ev;
	idle_reset(cam);
	apply_exposure(cam, cam->controls);
	if (cam->sensor_fps == UVCD_IDLE_SLOW_FPS)
		LOGI("idle: settled; %d fps", UVCD_IDLE_SLOW_FPS);
}

/* The weights in the ISP now. */
static const uint8_t (*weights_in_isp(const uvcd_camera_t *cam))[15]
{
	if (cam->metering_mode == UVCD_METERING_TUNING && cam->ae_weight_ref_valid)
		return (const uint8_t(*)[15])cam->ae_weight_ref;
	return (const uint8_t(*)[15])cam->subject_written;
}

/* Brightness as AE meters it and the exposure it uses, or false while AE
 * has not run yet (time and gain still 0 after bring-up). */
static bool idle_sample(uvcd_camera_t *cam, float *l, float *ev)
{
	rss_exposure_t e;
	uint32_t zone[15][15];

	if (RSS_HAL_CALL(cam->ops, isp_get_exposure, cam->hal_ctx, &e) != RSS_OK ||
	    e.exposure_time == 0 || e.total_gain == 0 ||
	    RSS_HAL_CALL(cam->ops, isp_get_ae_zone, cam->hal_ctx, zone) != RSS_OK)
		return false;
	const uint8_t(*w)[15] = weights_in_isp(cam);
	float sum = 0.0f, weights = 0.0f;
	for (int r = 0; r < 15; r++)
		for (int col = 0; col < 15; col++) {
			sum += (float)w[r][col] * (float)zone[r][col];
			weights += w[r][col];
		}
	if (weights == 0.0f)
		return false;
	*l = sum / weights;
	if (*l < UVCD_IDLE_L_FLOOR)
		*l = UVCD_IDLE_L_FLOOR;
	*ev = (float)e.exposure_time * ((float)e.total_gain / 256.0f);
	return true;
}

static bool near(float v, float to, float tol)
{
	return fabsf(v - to) <= to * tol;
}

static bool moved(float v, float from, float ratio)
{
	return v > from * ratio || v * ratio < from;
}

static void idle_tick(uvcd_camera_t *cam)
{
	const struct uvcd_control_state *c = cam->controls;

	if (cam->leased || !cam->it_max_idle)
		return;
	if (c->ae_mode == UVCD_AE_MANUAL) {
		if (!cam->idle_settled)
			idle_go_slow(cam, 0.0f, 0.0f);
		return;
	}
	int64_t now = uvcd_monotonic_ms();
	if (now - cam->idle_checked_ms < UVCD_IDLE_PERIOD_MS)
		return;
	cam->idle_checked_ms = now;

	float l, ev;
	if (!idle_sample(cam, &l, &ev))
		return;
	if (!cam->idle_settled) {
		cam->idle_l[cam->idle_samples % 3] = l;
		cam->idle_ev[cam->idle_samples % 3] = ev;
		if (++cam->idle_samples < 3)
			return;
		bool steady = true;
		for (int i = 0; i < 3; i++)
			steady = steady && near(cam->idle_l[i], l, UVCD_SETTLE_L_TOL) &&
				 near(cam->idle_ev[i], ev, UVCD_SETTLE_EV_TOL);
		int64_t settling = now - cam->settle_since_ms;
		if ((steady && settling >= UVCD_SETTLE_MIN_MS) || settling >= UVCD_SETTLE_MAX_MS)
			idle_go_slow(cam, l, ev);
		return;
	}
	if (moved(l, cam->settled_l, UVCD_CHANGE_L) || moved(ev, cam->settled_ev, UVCD_CHANGE_EV))
		cam->idle_changed++;
	else
		cam->idle_changed = 0;
	if (cam->idle_changed >= UVCD_CHANGE_SAMPLES) {
		LOGD("idle: brightness %.1f -> %.1f, exposure %.0f -> %.0f", (double)cam->settled_l,
		     (double)l, (double)cam->settled_ev, (double)ev);
		idle_settle(cam, "the scene changed");
	}
}

/* --------------------------------------------------------------------------
 * Lifecycle
 */

/* Bring-up never stops on a rejected write: a camera that won't stream can
 * only be diagnosed from its console. The setting stays at the tuning's
 * value and the log names it. */
static void startup_write(const char *name, int ret)
{
	if (ret != RSS_OK)
		LOGW("startup %s rejected (%d)", name, ret);
}

/* Push every stored control to the ISP, which starts from its factory
 * tuning every time it is opened. Rate, view, exposure and metering follow
 * in uvcd_camera_up(), once the view is known. */
static void apply_controls(uvcd_camera_t *cam)
{
	const struct uvcd_control_state *c = cam->controls;

	startup_write("brightness",
		      RSS_HAL_CALL(cam->ops, isp_set_brightness, cam->hal_ctx, c->brightness));
	startup_write("contrast",
		      RSS_HAL_CALL(cam->ops, isp_set_contrast, cam->hal_ctx, c->contrast));
	startup_write("saturation",
		      RSS_HAL_CALL(cam->ops, isp_set_saturation, cam->hal_ctx, c->saturation));
	startup_write("sharpness",
		      RSS_HAL_CALL(cam->ops, isp_set_sharpness, cam->hal_ctx, c->sharpness));
	startup_write("spatial-denoise",
		      RSS_HAL_CALL(cam->ops, isp_set_sinter_strength, cam->hal_ctx, c->sinter));
	startup_write("temporal-denoise",
		      RSS_HAL_CALL(cam->ops, isp_set_temper_strength, cam->hal_ctx, c->temper));
	/* Hue is stored signed, -128..127; the HAL wants it offset to 0..255. */
	startup_write("hue", RSS_HAL_CALL(cam->ops, isp_set_hue, cam->hal_ctx, c->hue + 128));
	startup_write("exposure-compensation", apply_ae_comp(cam, c));
	startup_write("max-analog-gain",
		      RSS_HAL_CALL(cam->ops, isp_set_max_again, cam->hal_ctx, c->max_again));
	startup_write("max-digital-gain",
		      RSS_HAL_CALL(cam->ops, isp_set_max_dgain, cam->hal_ctx, c->max_dgain));
	startup_write("defective-pixel-correction",
		      RSS_HAL_CALL(cam->ops, isp_set_dpc_strength, cam->hal_ctx, c->dpc));
	startup_write("dynamic-range-compression",
		      RSS_HAL_CALL(cam->ops, isp_set_drc_strength, cam->hal_ctx, c->drc));
	/* Off, and not a control: at any strength auto-exposure hunted (frame
	 * brightness swinging 20x more than at 0) and clipped more. */
	startup_write("backlight-comp",
		      RSS_HAL_CALL(cam->ops, isp_set_backlight_comp, cam->hal_ctx, 0));
	startup_write("highlight-suppression",
		      RSS_HAL_CALL(cam->ops, isp_set_highlight_depress, cam->hal_ctx, c->highlight));
	startup_write("running-mode",
		      RSS_HAL_CALL(cam->ops, isp_set_running_mode, cam->hal_ctx, RSS_ISP_DAY));
	startup_write("bypass", RSS_HAL_CALL(cam->ops, isp_set_bypass, cam->hal_ctx, 1));
	startup_write("power-line-frequency",
		      RSS_HAL_CALL(cam->ops, isp_set_antiflicker, cam->hal_ctx,
				   (rss_antiflicker_t)c->power_line_frequency));
	startup_write("rotation", apply_rotation(cam, c));
	/* Without a reference the factory curve is already what gamma 100
	 * asks for: nothing to write, nothing to report. */
	if (cam->gamma_ref_valid || c->gamma != UVCD_GAMMA_DEF)
		startup_write("gamma", apply_gamma(cam, c->gamma));
	startup_write("white-balance", apply_wb(cam, c->wb_temp, c->wb_auto));
}

int uvcd_camera_init(uvcd_camera_t *cam, const struct uvcd_control_state *controls)
{
	memset(cam, 0, sizeof(*cam));
	cam->controls = controls;

	cam->hal_ctx = rss_hal_create();
	if (!cam->hal_ctx) {
		LOGE("rss_hal_create failed");
		return -1;
	}
	cam->ops = rss_hal_get_ops(cam->hal_ctx);

	rss_sensor_config_t *sensor = &cam->sensor;
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
	return 0;

fail:
	rss_hal_destroy(cam->hal_ctx);
	cam->hal_ctx = NULL;
	return -1;
}

void uvcd_camera_deinit(uvcd_camera_t *cam)
{
	uvcd_camera_down(cam);
	if (cam->hal_ctx) {
		rss_hal_destroy(cam->hal_ctx);
		cam->hal_ctx = NULL;
	}
}

int uvcd_camera_up(uvcd_camera_t *cam)
{
	bool first = !cam->brought_up_once;

	if (cam->up)
		return RSS_OK;

	int64_t t0 = uvcd_monotonic_ms();

	/* OSD pool must be set before HAL init even when unused (SDK requirement) */
	RSS_HAL_CALL(cam->ops, osd_set_pool_size, cam->hal_ctx, 0);

	rss_multi_sensor_config_t multi = {0};
	multi.sensor_count = 1;
	multi.sensors[0] = cam->sensor;

	int ret = RSS_HAL_CALL(cam->ops, init, cam->hal_ctx, &multi);
	if (ret != RSS_OK) {
		LOGE("HAL init failed: %d", ret);
		return ret;
	}
	cam->up = true;
	cam->sensor_fps = 0;          /* the driver boots at its own default rate */
	cam->it_capped = false;
	cam->metering_valid = false;  /* and the ISP with the tuning's weights */
	cam->idle_settled = false;    /* and auto-exposure has everything to learn */
	idle_reset(cam);

	cam->caps = cam->ops->get_caps ? cam->ops->get_caps(cam->hal_ctx) : NULL;

	if (first) {
		char ver[64];
		if (rss_hal_get_imp_version(ver, sizeof(ver)) == 0)
			LOGI("LIBIMP version %s", ver);
		if (cam->caps && !cam->caps->has_h265)
			LOGD("H.265 not supported (unused -- uvcd is H.264/MJPEG only)");

		/* Capture the vendor's gamma curve before anything touches it --
		 * it is the reference every later gamma change is derived from.
		 * Only on the first bring-up: the ISP driver outlives uvcd's
		 * sessions, so a later read could return our own reshaped curve. */
		cam->gamma_ref_valid =
			(RSS_HAL_CALL(cam->ops, isp_get_gamma, cam->hal_ctx, cam->gamma_ref) == 0);
		if (!cam->gamma_ref_valid)
			LOGW("could not read factory gamma curve; gamma control disabled");

		/* Same for the tuning file's metering weights. */
		cam->ae_weight_ref_valid = (RSS_HAL_CALL(cam->ops, isp_get_ae_weight, cam->hal_ctx,
							 cam->ae_weight_ref) == 0);
		if (!cam->ae_weight_ref_valid)
			LOGW("could not read the tuning's metering weights");
	}

	cam->sensor_w = read_procfs_int("/proc/jz/sensor/sensor0/width", 10, 0);
	cam->sensor_h = read_procfs_int("/proc/jz/sensor/sensor0/height", 10, 0);
	if (cam->sensor_w <= 0 || cam->sensor_h <= 0) {
		cam->sensor_w = 1920;
		cam->sensor_h = 1080;
		LOGW("could not read sensor resolution, assuming %dx%d", cam->sensor_w,
		     cam->sensor_h);
	} else if (first) {
		LOGI("sensor resolution: %dx%d", cam->sensor_w, cam->sensor_h);
	}
	/* Fixed while the module is loaded; it sets how far zoom can go. */
	cam->isp_clk_hz = read_procfs_int("/sys/module/tx_isp_t31/parameters/isp_clk", 10, 0);
	if (first)
		LOGI("ISP clock: %d MHz", cam->isp_clk_hz / 1000000);
	cam->brought_up_once = true;

	apply_controls(cam);
	/* Rate and view of the output, idle unless a stream already holds the
	 * camera (a stream start that had to bring it up again). */
	set_view(cam, cam->controls);
	startup_write("exposure", apply_exposure(cam, cam->controls));
	/* Retried by the tick while rejected. */
	startup_write("metering", apply_metering(cam, cam->controls));

	/* The integration limit idle holds while slow: what the SDK set for
	 * the UVCD_IDLE_FPS just applied. */
	cam->it_max_idle = 0;
	uint32_t it = 0;
	if (!cam->leased && cam->sensor_fps == UVCD_IDLE_FPS &&
	    RSS_HAL_CALL(cam->ops, isp_get_ae_it_max, cam->hal_ctx, &it) == RSS_OK && it > 0 &&
	    it <= UINT16_MAX)
		cam->it_max_idle = it;
	else
		LOGW("no AE integration limit at %d fps; idle stays at %d fps", UVCD_IDLE_FPS,
		     UVCD_IDLE_FPS);

	LOGI("sensor + ISP up in %lld ms", (long long)(uvcd_monotonic_ms() - t0));
	return RSS_OK;
}

/* Sensor stream off, ISP closed, IMP system exited. A stream's channels
 * must already be torn down (uvcd_pipeline_stop). */
void uvcd_camera_down(uvcd_camera_t *cam)
{
	if (!cam->up)
		return;

	int ret = RSS_HAL_CALL(cam->ops, deinit, cam->hal_ctx);
	if (ret != RSS_OK)
		LOGW("HAL deinit: %d", ret);
	cam->up = false;
	cam->leased = false;
	cam->metering_valid = false;
	LOGI("sensor + ISP down");
}

/* --------------------------------------------------------------------------
 * The stream's lease
 */

int uvcd_camera_acquire(uvcd_camera_t *cam, uint8_t frame, uint32_t fps)
{
	if (!cam->up)
		return -ENODEV;
	cam->leased = true;
	cam->lease_frame = frame;
	cam->lease_fps = fps;
	/* A new rate brings the SDK's own integration limit, but setting the
	 * rate the sensor already runs leaves idle's in place (seen at 5 fps
	 * on the K1): put the SDK's back. */
	if (cam->it_capped && fps == cam->sensor_fps) {
		if (cam->it_max_slow && RSS_HAL_CALL(cam->ops, isp_set_ae_it_max, cam->hal_ctx,
						     cam->it_max_slow) == RSS_OK)
			cam->it_capped = false;
		else
			LOGW("AE integration limit for %u fps not restored", fps);
	}
	/* The sensor sets the pace: a 30 fps channel on a 25 fps sensor still
	 * delivers 25 while the host was promised 30. A rejected rate is
	 * logged, and the stream runs at the sensor's current one. */
	set_sensor_fps(cam, fps);
	set_view(cam, cam->controls);
	return RSS_OK;
}

void uvcd_camera_stream_on(uvcd_camera_t *cam, uint8_t format)
{
	if (!cam->up)
		return;
	apply_csc(cam, format);
	/* Now that there is a frame rate to hold or give up: manual exposure
	 * with exposure priority may need a slower sensor. And metering
	 * follows the stream's view, keeping what it learned if the view is
	 * the one the camera already had. Both log a rejection and the stream
	 * runs anyway; metering is retried by the tick. */
	apply_exposure(cam, cam->controls);
	apply_metering(cam, cam->controls);
}

void uvcd_camera_release(uvcd_camera_t *cam)
{
	if (!cam->leased)
		return;
	cam->leased = false;
	if (!cam->up)
		return;
	/* The stream may have run another rate and exposure: settle again. */
	cam->idle_settled = false;
	idle_reset(cam);
	set_view(cam, cam->controls);
	apply_exposure(cam, cam->controls); /* back to the idle rate */
	apply_metering(cam, cam->controls);
}

/* How often a rejected metering write is retried. */
#define UVCD_METERING_RETRY_MS 500

void uvcd_camera_tick(uvcd_camera_t *cam)
{
	if (!cam->up)
		return;
	if (!cam->metering_valid &&
	    uvcd_monotonic_ms() - cam->metering_tried_ms >= UVCD_METERING_RETRY_MS) {
		cam->metering_tried_ms = uvcd_monotonic_ms();
		apply_metering(cam, cam->controls);
	}
	exposure_priority_tick(cam);
	subject_metering_tick(cam);
	idle_tick(cam);
}

/* --------------------------------------------------------------------------
 * Controls
 */

#define FIELD(name) offsetof(struct uvcd_control_state, name)

static int apply_field(uvcd_camera_t *cam, struct uvcd_control_state *next, size_t field)
{
	const struct uvcd_control_state *now = cam->controls;

	if (!cam->up)
		return RSS_OK;

	switch (field) {
	case FIELD(brightness):
		return RSS_HAL_CALL(cam->ops, isp_set_brightness, cam->hal_ctx, next->brightness);
	case FIELD(contrast):
		return RSS_HAL_CALL(cam->ops, isp_set_contrast, cam->hal_ctx, next->contrast);
	case FIELD(saturation):
		return RSS_HAL_CALL(cam->ops, isp_set_saturation, cam->hal_ctx, next->saturation);
	case FIELD(sharpness):
		return RSS_HAL_CALL(cam->ops, isp_set_sharpness, cam->hal_ctx, next->sharpness);
	case FIELD(hue):
		return RSS_HAL_CALL(cam->ops, isp_set_hue, cam->hal_ctx, next->hue + 128);
	case FIELD(power_line_frequency):
		return RSS_HAL_CALL(cam->ops, isp_set_antiflicker, cam->hal_ctx,
				    (rss_antiflicker_t)next->power_line_frequency);
	case FIELD(gamma):
		return apply_gamma(cam, next->gamma);
	/* The two white-balance controls are one HAL call between them. */
	case FIELD(wb_temp):
		return apply_wb(cam, next->wb_temp, next->wb_auto);
	case FIELD(wb_auto):
		/* Leaving auto: start manual from the temperature auto had
		 * measured, so the picture doesn't jump and the temperature
		 * control reads where the light actually is. */
		if (!next->wb_auto && now->wb_auto) {
			int ct = uvcd_camera_awb_ct(cam);
			if (ct > 0) {
				if (ct < UVCD_WB_TEMP_MIN)
					ct = UVCD_WB_TEMP_MIN;
				if (ct > UVCD_WB_TEMP_MAX)
					ct = UVCD_WB_TEMP_MAX;
				next->wb_temp = ct;
				LOGI("white-balance-temperature=%d (from auto)", ct);
			}
		}
		return apply_wb(cam, next->wb_temp, next->wb_auto);
	case FIELD(ae_mode):
		/* Leaving auto-exposure: start manual from the exposure auto was
		 * using, so the picture doesn't jump -- as for white balance. */
		if (next->ae_mode == UVCD_AE_MANUAL && now->ae_mode != UVCD_AE_MANUAL) {
			int t = uvcd_camera_live_exposure(cam);
			if (t > 0) {
				next->exposure_time = t > UVCD_EXPOSURE_MAX ? UVCD_EXPOSURE_MAX : t;
				LOGI("exposure-time=%d (from auto)", next->exposure_time);
			}
		}
		return apply_exposure(cam, next);
	case FIELD(ae_priority):
	case FIELD(exposure_time):
		return apply_exposure(cam, next);
	case FIELD(zoom):
	case FIELD(pan):
	case FIELD(tilt):
		/* A stream's view is its channel's crop, which only the stream
		 * can re-create (uvcd_pipeline_apply). */
		if (cam->leased)
			return -EBUSY;
		set_view(cam, next);
		apply_metering(cam, next); /* retried by the tick if rejected */
		return RSS_OK;
	case FIELD(rotation):
		return apply_rotation(cam, next);
	case FIELD(metering):
		return apply_metering(cam, next);
	case FIELD(max_again):
		return RSS_HAL_CALL(cam->ops, isp_set_max_again, cam->hal_ctx, next->max_again);
	case FIELD(max_dgain):
		return RSS_HAL_CALL(cam->ops, isp_set_max_dgain, cam->hal_ctx, next->max_dgain);
	case FIELD(ae_comp):
		return apply_ae_comp(cam, next);
	case FIELD(sinter):
		return RSS_HAL_CALL(cam->ops, isp_set_sinter_strength, cam->hal_ctx, next->sinter);
	case FIELD(temper):
		return RSS_HAL_CALL(cam->ops, isp_set_temper_strength, cam->hal_ctx, next->temper);
	case FIELD(dpc):
		return RSS_HAL_CALL(cam->ops, isp_set_dpc_strength, cam->hal_ctx, next->dpc);
	case FIELD(drc):
		return RSS_HAL_CALL(cam->ops, isp_set_drc_strength, cam->hal_ctx, next->drc);
	case FIELD(highlight):
		return RSS_HAL_CALL(cam->ops, isp_set_highlight_depress, cam->hal_ctx,
				    next->highlight);
	default:
		return RSS_OK; /* not the camera's: the encoder settings */
	}
}

/* Controls that change what auto-exposure aims at, what it meters or what
 * it may use. */
static bool changes_ae(size_t field)
{
	switch (field) {
	case FIELD(ae_mode):
	case FIELD(ae_comp):
	case FIELD(metering):
	case FIELD(zoom):
	case FIELD(pan):
	case FIELD(tilt):
	case FIELD(max_again):
	case FIELD(max_dgain):
	case FIELD(power_line_frequency):
	case FIELD(highlight):
		return true;
	default:
		return false;
	}
}

int uvcd_camera_apply(uvcd_camera_t *cam, struct uvcd_control_state *next, size_t field)
{
	/* Idle settles at full rate again before the change, so that
	 * auto-exposure takes it at full rate. */
	if (cam->up && changes_ae(field)) {
		int ret = idle_settle(cam, "a control changed");
		if (ret != RSS_OK)
			return ret;
	}
	return apply_field(cam, next, field);
}
