/*
 * uvcd_gadget.c -- UVC gadget event handling and frame delivery
 *
 * Adapted from raptor's rwc daemon, stripped of audio/ring-IPC: frames
 * come straight from uvcd_pipeline's shared buffer, which only exists
 * while a host has actually committed to a format (on-demand).
 */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <linux/videodev2.h>
#include <linux/usb/ch9.h>
#include <linux/usb/video.h>

#include "uvcd_gadget.h"
#include "uvcd_config.h"
#include "uvcd_ctl.h"

/* --------------------------------------------------------------------------
 * UVC gadget userspace API (from kernel uvc.h)
 */
#define UVC_EVENT_CONNECT    (V4L2_EVENT_PRIVATE_START + 0)
#define UVC_EVENT_DISCONNECT (V4L2_EVENT_PRIVATE_START + 1)
#define UVC_EVENT_STREAMON   (V4L2_EVENT_PRIVATE_START + 2)
#define UVC_EVENT_STREAMOFF  (V4L2_EVENT_PRIVATE_START + 3)
#define UVC_EVENT_SETUP	     (V4L2_EVENT_PRIVATE_START + 4)
#define UVC_EVENT_DATA	     (V4L2_EVENT_PRIVATE_START + 5)

struct uvc_request_data {
	int32_t length;
	uint8_t data[60];
};

struct uvc_event {
	union {
		enum usb_device_speed speed;
		struct usb_ctrlrequest req;
		struct uvc_request_data data;
	};
};

#define UVCIOC_SEND_RESPONSE _IOW('U', 1, struct uvc_request_data)

#define UVC_INTF_CONTROL   0
#define UVC_INTF_STREAMING 1
/* Entity IDs from the gadget descriptor (kernel webcam.c). */
#define UVCD_CAMERA_TERMINAL_ID 1
#define UVCD_PROCESSING_UNIT_ID 2
#define UVCD_CUSTOM_UNIT_ID 4
#define UVCD_CUSTOM_MAX_AGAIN 1
#define UVCD_CUSTOM_MAX_DGAIN 2
#define UVCD_CUSTOM_AE_COMP 3
#define UVCD_CUSTOM_SINTER 4
#define UVCD_CUSTOM_TEMPER 5
#define UVCD_CUSTOM_DPC 6
#define UVCD_CUSTOM_DRC 7
/* 8 was defog strength: this SDK only applies it with defog enabled, which
 * uvcd never did, so it had no effect at any value. Retired, not reused. */
#define UVCD_CUSTOM_HIGHLIGHT 9
/* Action control, not a setting: SET_CUR of a non-zero value restores every
 * control (standard and custom alike) to its compiled-in factory default.
 * GET_CUR always reads back 0 -- there is no state here to report. */
#define UVCD_CUSTOM_RESET 10
/* 11 and 12 were horizontal/vertical flip, replaced by rotation (22). Not
 * reused, so an old host tool can't set the wrong thing by number. */
/* Encoder settings. Bitrate and GOP apply live; the rest can only be set when
 * the encoder channel is created, so changing them mid-stream re-creates it
 * in place (a short gap in frames, no renegotiation with the host). */
#define UVCD_CUSTOM_H264_BITRATE 13
#define UVCD_CUSTOM_H264_RATE_CONTROL 14
#define UVCD_CUSTOM_H264_GOP 15
#define UVCD_CUSTOM_H264_MIN_QP 16
#define UVCD_CUSTOM_H264_MAX_QP 17
#define UVCD_CUSTOM_H264_PROFILE 18
#define UVCD_CUSTOM_MJPEG_QUALITY 19
/* Action control, like factory reset: SET_CUR of a non-zero value makes the
 * H.264 encoder emit an IDR frame now. */
#define UVCD_CUSTOM_H264_KEYFRAME 20
/* Which part of the view auto-exposure meters on (UVCD_METERING_*). */
#define UVCD_CUSTOM_METERING 21
/* Image rotation, 0 or 180 degrees. UVC does standardize this, as the
 * Camera Terminal's Roll control, but V4L2 has no control for it, so Linux
 * hosts could never reach it there. */
#define UVCD_CUSTOM_ROTATION 22
/* 23-25 were save, apply-on-boot and saved-query, from a short-lived
 * explicit save model. Retired, not reused: controls persist on their own. */
#define UVCD_CUSTOM_GAIN_MIN 0
#define UVCD_CUSTOM_GAIN_MAX 160

#define UVCD_CONTROL_MIN 0
#define UVCD_CONTROL_MAX 255
#define UVCD_CONTROL_DEF 128
/* Measured on the K1 with the shipped sensor tuning against a reference
 * scene: 128 leaves colour at ~0.84 of a phone raw's chroma, 136 matches it
 * (~1.04); the curve is steep above 128 (150 is already ~1.36). */
#define UVCD_SATURATION_DEF 136
#define UVCD_HUE_MIN -128
#define UVCD_HUE_MAX 127
#define UVCD_HUE_DEF 0
/* Sharpness adds so much detail at the top of the range that MJPEG frames
 * outgrow the USB frame cap: at 1080p and quality 100, 180 makes ~1.8 MB
 * frames (19 fps, bus-bound) and 255 ~2.4 MB, every one dropped. 160 is
 * already visibly oversharpened, so nothing useful is lost. */
#define UVCD_SHARPNESS_MAX 180
#define UVCD_POWER_LINE_MIN 0
#define UVCD_POWER_LINE_MAX 2
#define UVCD_POWER_LINE_DEF 0 /* off: anti-flicker holds exposure to 10 ms steps */

/* What the host is told a payload may be. It must match how the gadget
 * actually frames bulk payloads -- f_uvc.c sets max_payload_size to
 * ep->maxpacket * 32, i.e. 512 * 32 at high speed. A host that believes
 * payloads are larger than that relies entirely on short packets and ZLPs to
 * find payload boundaries; matching it lets a full payload end on size too. */
#define UVCD_BULK_PAYLOAD (512 * 32)

struct gadget_buffer {
	void *start;
	size_t length;
};

struct gadget_s {
	uvcd_pipeline_t *pipe;
	const char *device;

	int fd;
	struct gadget_buffer buffers[UVCD_MAX_BUFFERS];
	int buf_count;
	/* true while the buffer is with the gadget (QBUF'd, not yet DQBUF'd).
	 * A buffer is only ever queued once it holds a real frame. */
	bool buf_queued[UVCD_MAX_BUFFERS];
	bool streaming;
	/* When the gadget last ran out of free buffers with a frame waiting;
	 * 0 while buffers are coming back. */
	int64_t stalled_since_ms;

	uint8_t cur_format;
	uint8_t cur_frame;
	uint32_t cur_interval;
	struct uvc_streaming_control probe;
	struct uvc_streaming_control commit;
	/* No control state here: the one runtime copy is g->pipe->controls,
	 * which the pipeline also reads when it creates the encoder channel. */
	uint8_t last_cs;
	uint8_t last_intf;
	uint8_t last_unit; /* entity ID, for control-interface requests */
	uint8_t last_request;

	/* Controls are persistent by default -- there is no explicit SAVE.
	 * Every accepted change marks the shadow dirty; the poll loop writes
	 * it out once changes have settled for UVCD_CONFIG_SETTLE_MS. */
	bool config_dirty;
	int64_t config_dirty_at;
	bool guard_armed; /* this stream is counted in the crash guard file */
	int64_t healthy_since_ms;
	int64_t last_frame_ms;
	uint64_t watched_seq;

	/* While a factory reset walks every control, encoder re-creation is
	 * deferred and done once at the end instead of once per setting. */
	bool defer_encoder_restart;
	bool restart_h264, restart_mjpeg;

	uint64_t read_seq;
};

static void fill_streaming_control(struct uvc_streaming_control *ctrl, uint8_t format_idx,
				   uint8_t frame_idx, uint32_t interval)
{
	memset(ctrl, 0, sizeof(*ctrl));
	ctrl->bmHint = 1;

	if (format_idx < 1 || format_idx > UVCD_NUM_FORMATS)
		format_idx = UVCD_FMT_MJPEG;
	ctrl->bFormatIndex = format_idx;

	if (frame_idx < 1 || frame_idx > UVCD_NUM_FRAMES)
		frame_idx = UVCD_FRAME_1080P;
	ctrl->bFrameIndex = frame_idx;

	ctrl->dwFrameInterval = uvcd_snap_interval(interval);

	ctrl->dwMaxVideoFrameSize = uvcd_frames[frame_idx].max_size;
	ctrl->dwMaxPayloadTransferSize = UVCD_BULK_PAYLOAD;
	ctrl->bmFramingInfo = 3;
	ctrl->bPreferedVersion = 1;
	ctrl->bMinVersion = 1;
	ctrl->bMaxVersion = 1;
}

static int start_streaming(gadget_t *g);
static void stop_streaming(gadget_t *g);

static int *control_value(gadget_t *g, uint8_t selector)
{
	switch (selector) {
	case UVC_PU_HUE_CONTROL:
		return &g->pipe->controls.hue;
	case UVC_PU_BRIGHTNESS_CONTROL:
		return &g->pipe->controls.brightness;
	case UVC_PU_CONTRAST_CONTROL:
		return &g->pipe->controls.contrast;
	case UVC_PU_SATURATION_CONTROL:
		return &g->pipe->controls.saturation;
	case UVC_PU_SHARPNESS_CONTROL:
		return &g->pipe->controls.sharpness;
	case UVC_PU_POWER_LINE_FREQUENCY_CONTROL:
		return &g->pipe->controls.power_line_frequency;
	case UVC_PU_GAMMA_CONTROL:
		return &g->pipe->controls.gamma;
	case UVC_PU_WHITE_BALANCE_TEMPERATURE_CONTROL:
		return &g->pipe->controls.wb_temp;
	case UVC_PU_WHITE_BALANCE_TEMPERATURE_AUTO_CONTROL:
		return &g->pipe->controls.wb_auto;
	default:
		return NULL;
	}
}

/* UVC defines these two as single-byte controls; everything else the
 * Processing Unit carries here is 16-bit. */
static uint16_t control_length(uint8_t selector)
{
	return (selector == UVC_PU_POWER_LINE_FREQUENCY_CONTROL ||
		selector == UVC_PU_WHITE_BALANCE_TEMPERATURE_AUTO_CONTROL)
		       ? 1
		       : 2;
}

static int apply_control(gadget_t *g, uint8_t selector, int value)
{
	switch (selector) {
	case UVC_PU_HUE_CONTROL:
		if (value < UVCD_HUE_MIN)
			value = UVCD_HUE_MIN;
		if (value > UVCD_HUE_MAX)
			value = UVCD_HUE_MAX;
		return RSS_HAL_CALL(g->pipe->ops, isp_set_hue, g->pipe->hal_ctx, value + UVCD_CONTROL_DEF);
	case UVC_PU_BRIGHTNESS_CONTROL:
		if (value < UVCD_CONTROL_MIN)
			value = UVCD_CONTROL_MIN;
		if (value > UVCD_CONTROL_MAX)
			value = UVCD_CONTROL_MAX;
		return RSS_HAL_CALL(g->pipe->ops, isp_set_brightness, g->pipe->hal_ctx, value);
	case UVC_PU_CONTRAST_CONTROL:
		if (value < UVCD_CONTROL_MIN)
			value = UVCD_CONTROL_MIN;
		if (value > UVCD_CONTROL_MAX)
			value = UVCD_CONTROL_MAX;
		return RSS_HAL_CALL(g->pipe->ops, isp_set_contrast, g->pipe->hal_ctx, value);
	case UVC_PU_SATURATION_CONTROL:
		if (value < UVCD_CONTROL_MIN)
			value = UVCD_CONTROL_MIN;
		if (value > UVCD_CONTROL_MAX)
			value = UVCD_CONTROL_MAX;
		return RSS_HAL_CALL(g->pipe->ops, isp_set_saturation, g->pipe->hal_ctx, value);
	case UVC_PU_SHARPNESS_CONTROL:
		if (value < UVCD_CONTROL_MIN)
			value = UVCD_CONTROL_MIN;
		if (value > UVCD_SHARPNESS_MAX)
			value = UVCD_SHARPNESS_MAX;
		return RSS_HAL_CALL(g->pipe->ops, isp_set_sharpness, g->pipe->hal_ctx, value);
	case UVC_PU_POWER_LINE_FREQUENCY_CONTROL:
		if (value < UVCD_POWER_LINE_MIN)
			value = UVCD_POWER_LINE_MIN;
		if (value > UVCD_POWER_LINE_MAX)
			value = UVCD_POWER_LINE_MAX;
		return RSS_HAL_CALL(g->pipe->ops, isp_set_antiflicker, g->pipe->hal_ctx,
					(rss_antiflicker_t)value);
	case UVC_PU_GAMMA_CONTROL:
		if (value < UVCD_GAMMA_MIN)
			value = UVCD_GAMMA_MIN;
		if (value > UVCD_GAMMA_MAX)
			value = UVCD_GAMMA_MAX;
		return uvcd_apply_gamma(g->pipe, value);
	/* The two white-balance controls are one HAL call between them, so
	 * each re-applies using the other's current shadow value. */
	case UVC_PU_WHITE_BALANCE_TEMPERATURE_CONTROL:
		if (value < UVCD_WB_TEMP_MIN)
			value = UVCD_WB_TEMP_MIN;
		if (value > UVCD_WB_TEMP_MAX)
			value = UVCD_WB_TEMP_MAX;
		return uvcd_apply_wb(g->pipe, value, g->pipe->controls.wb_auto);
	case UVC_PU_WHITE_BALANCE_TEMPERATURE_AUTO_CONTROL:
		value = value ? 1 : 0;
		/* Leaving auto: start manual from the temperature auto had
		 * measured, so the picture doesn't jump and the temperature
		 * control reads where the light actually is. */
		if (!value && g->pipe->controls.wb_auto) {
			int ct = uvcd_awb_current_ct(g->pipe);
			if (ct > 0) {
				if (ct < UVCD_WB_TEMP_MIN)
					ct = UVCD_WB_TEMP_MIN;
				if (ct > UVCD_WB_TEMP_MAX)
					ct = UVCD_WB_TEMP_MAX;
				g->pipe->controls.wb_temp = ct;
				LOGI("white-balance-temperature=%d (from auto)", ct);
			}
		}
		return uvcd_apply_wb(g->pipe, g->pipe->controls.wb_temp, value);
	default:
		return -EINVAL;
	}
}

static int *custom_control_value(gadget_t *g, uint8_t selector)
{
	switch (selector) {
	case UVCD_CUSTOM_MAX_AGAIN:
		return &g->pipe->controls.max_again;
	case UVCD_CUSTOM_MAX_DGAIN:
		return &g->pipe->controls.max_dgain;
	case UVCD_CUSTOM_AE_COMP:
		return &g->pipe->controls.ae_comp;
	case UVCD_CUSTOM_SINTER:
		return &g->pipe->controls.sinter;
	case UVCD_CUSTOM_TEMPER:
		return &g->pipe->controls.temper;
	case UVCD_CUSTOM_DPC:
		return &g->pipe->controls.dpc;
	case UVCD_CUSTOM_DRC:
		return &g->pipe->controls.drc;
	case UVCD_CUSTOM_HIGHLIGHT:
		return &g->pipe->controls.highlight;
	case UVCD_CUSTOM_METERING:
		return &g->pipe->controls.metering;
	case UVCD_CUSTOM_ROTATION:
		return &g->pipe->controls.rotation;
	case UVCD_CUSTOM_H264_BITRATE:
		return &g->pipe->controls.h264_bitrate_kbps;
	case UVCD_CUSTOM_H264_RATE_CONTROL:
		return &g->pipe->controls.h264_rate_control;
	case UVCD_CUSTOM_H264_GOP:
		return &g->pipe->controls.h264_gop_frames;
	case UVCD_CUSTOM_H264_MIN_QP:
		return &g->pipe->controls.h264_min_qp;
	case UVCD_CUSTOM_H264_MAX_QP:
		return &g->pipe->controls.h264_max_qp;
	case UVCD_CUSTOM_H264_PROFILE:
		return &g->pipe->controls.h264_profile;
	case UVCD_CUSTOM_MJPEG_QUALITY:
		return &g->pipe->controls.mjpeg_quality;
	default:
		return NULL;
	}
}

/* Defined below, once uvcd_ctrl_defs[] is in scope. The table is the single
 * source of truth for every control's range and default; deriving the XU
 * GET_MIN/GET_MAX/GET_DEF replies from it keeps what the host is told in
 * lockstep with what the daemon actually enforces. */
static const struct uvcd_ctrl_def *custom_ctrl_def(uint8_t selector);

static int custom_control_min(uint8_t selector)
{
	const struct uvcd_ctrl_def *def = custom_ctrl_def(selector);
	return def ? def->min : UVCD_CUSTOM_GAIN_MIN;
}

static int custom_control_max(uint8_t selector)
{
	const struct uvcd_ctrl_def *def = custom_ctrl_def(selector);
	return def ? def->max : UVCD_CONTROL_MAX;
}

static int custom_control_def(uint8_t selector)
{
	const struct uvcd_ctrl_def *def = custom_ctrl_def(selector);
	return def ? def->def : UVCD_CUSTOM_GAIN_MIN;
}

static int custom_control_res(uint8_t selector)
{
	const struct uvcd_ctrl_def *def = custom_ctrl_def(selector);
	return def && def->res ? def->res : 1;
}

static int apply_custom_control(gadget_t *g, uint8_t selector, int value)
{
	if (value < custom_control_min(selector))
		value = custom_control_min(selector);
	if (value > custom_control_max(selector))
		value = custom_control_max(selector);

	switch (selector) {
	case UVCD_CUSTOM_MAX_AGAIN:
		return RSS_HAL_CALL(g->pipe->ops, isp_set_max_again, g->pipe->hal_ctx, value);
	case UVCD_CUSTOM_MAX_DGAIN:
		return RSS_HAL_CALL(g->pipe->ops, isp_set_max_dgain, g->pipe->hal_ctx, value);
	case UVCD_CUSTOM_AE_COMP:
		return RSS_HAL_CALL(g->pipe->ops, isp_set_ae_comp, g->pipe->hal_ctx, value);
	case UVCD_CUSTOM_SINTER:
		return RSS_HAL_CALL(g->pipe->ops, isp_set_sinter_strength, g->pipe->hal_ctx, value);
	case UVCD_CUSTOM_TEMPER:
		return RSS_HAL_CALL(g->pipe->ops, isp_set_temper_strength, g->pipe->hal_ctx, value);
	case UVCD_CUSTOM_DPC:
		return RSS_HAL_CALL(g->pipe->ops, isp_set_dpc_strength, g->pipe->hal_ctx, value);
	case UVCD_CUSTOM_DRC:
		return RSS_HAL_CALL(g->pipe->ops, isp_set_drc_strength, g->pipe->hal_ctx, value);
	case UVCD_CUSTOM_HIGHLIGHT:
		return RSS_HAL_CALL(g->pipe->ops, isp_set_highlight_depress, g->pipe->hal_ctx, value);
	case UVCD_CUSTOM_METERING: {
		struct uvcd_control_state c = g->pipe->controls;
		c.metering = value;
		return uvcd_apply_metering(g->pipe, &c);
	}
	case UVCD_CUSTOM_ROTATION: {
		struct uvcd_control_state c = g->pipe->controls;
		c.rotation = value;
		return uvcd_apply_rotation(g->pipe, &c);
	}
	case UVCD_CUSTOM_H264_BITRATE:
	case UVCD_CUSTOM_H264_RATE_CONTROL:
	case UVCD_CUSTOM_H264_GOP:
	case UVCD_CUSTOM_H264_MIN_QP:
	case UVCD_CUSTOM_H264_MAX_QP:
	case UVCD_CUSTOM_H264_PROFILE:
	case UVCD_CUSTOM_MJPEG_QUALITY:
		/* The pipeline reads these from the stored state, so they take
		 * effect in encoder_after_store(), once the value is in place. */
		return 0;
	default:
		return -EINVAL;
	}
}

/* --------------------------------------------------------------------------
 * Control table -- name<->selector mapping shared with config persistence
 * (uvcd_config.c), and the source of the XU GET_MIN/GET_MAX/GET_DEF
 * replies. Ranges here must match the per-selector clamps in apply_control()/
 * apply_custom_control() above; they're re-clamped there too, so a mismatch
 * would only ever narrow the effective range, never widen it.
 */
/* One control-table row. Only a few rows need .res or .part, so they are
 * named where used and zero everywhere else. */
#define UVCD_CTRL_ROW(n, k, sel, lo, hi, dv, field, ...)                                  \
	{.name = (n), .kind = (k), .selector = (sel), .min = (lo), .max = (hi), .def = (dv),   \
	 .state_offset = offsetof(struct uvcd_control_state, field), __VA_ARGS__}

static const struct uvcd_ctrl_def uvcd_ctrl_defs[] = {
	UVCD_CTRL_ROW("brightness", UVCD_CTRL_STANDARD, UVC_PU_BRIGHTNESS_CONTROL,
		      UVCD_CONTROL_MIN, UVCD_CONTROL_MAX, UVCD_CONTROL_DEF, brightness),
	UVCD_CTRL_ROW("contrast", UVCD_CTRL_STANDARD, UVC_PU_CONTRAST_CONTROL,
		      UVCD_CONTROL_MIN, UVCD_CONTROL_MAX, UVCD_CONTROL_DEF, contrast),
	UVCD_CTRL_ROW("saturation", UVCD_CTRL_STANDARD, UVC_PU_SATURATION_CONTROL,
		      UVCD_CONTROL_MIN, UVCD_CONTROL_MAX, UVCD_SATURATION_DEF, saturation),
	UVCD_CTRL_ROW("sharpness", UVCD_CTRL_STANDARD, UVC_PU_SHARPNESS_CONTROL,
		      UVCD_CONTROL_MIN, UVCD_SHARPNESS_MAX, UVCD_CONTROL_DEF, sharpness),
	UVCD_CTRL_ROW("hue", UVCD_CTRL_STANDARD, UVC_PU_HUE_CONTROL,
		      UVCD_HUE_MIN, UVCD_HUE_MAX, UVCD_HUE_DEF, hue),
	UVCD_CTRL_ROW("power-line-frequency", UVCD_CTRL_STANDARD, UVC_PU_POWER_LINE_FREQUENCY_CONTROL,
		      UVCD_POWER_LINE_MIN, UVCD_POWER_LINE_MAX, UVCD_POWER_LINE_DEF, power_line_frequency),
	UVCD_CTRL_ROW("gamma", UVCD_CTRL_STANDARD, UVC_PU_GAMMA_CONTROL,
		      UVCD_GAMMA_MIN, UVCD_GAMMA_MAX, UVCD_GAMMA_DEF, gamma),
	UVCD_CTRL_ROW("white-balance-temperature", UVCD_CTRL_STANDARD,
		      UVC_PU_WHITE_BALANCE_TEMPERATURE_CONTROL,
		      UVCD_WB_TEMP_MIN, UVCD_WB_TEMP_MAX, UVCD_WB_TEMP_DEF, wb_temp),
	UVCD_CTRL_ROW("white-balance-auto", UVCD_CTRL_STANDARD,
		      UVC_PU_WHITE_BALANCE_TEMPERATURE_AUTO_CONTROL,
		      0, 1, UVCD_WB_AUTO_DEF, wb_auto),
	/* Camera Terminal. auto-exposure holds the UVC bitmap value (1 or 8),
	 * so its GET_RES is the set of modes supported. */
	UVCD_CTRL_ROW("auto-exposure", UVCD_CTRL_CAMERA, UVC_CT_AE_MODE_CONTROL,
		      UVCD_AE_MANUAL, UVCD_AE_APERTURE_PRIORITY, UVCD_AE_APERTURE_PRIORITY, ae_mode,
		      .res = UVCD_AE_MODES),
	UVCD_CTRL_ROW("exposure-priority", UVCD_CTRL_CAMERA, UVC_CT_AE_PRIORITY_CONTROL,
		      0, 1, 0, ae_priority),
	UVCD_CTRL_ROW("exposure-time", UVCD_CTRL_CAMERA, UVC_CT_EXPOSURE_TIME_ABSOLUTE_CONTROL,
		      UVCD_EXPOSURE_MIN, UVCD_EXPOSURE_MAX, UVCD_EXPOSURE_DEF, exposure_time),
	UVCD_CTRL_ROW("zoom", UVCD_CTRL_CAMERA, UVC_CT_ZOOM_ABSOLUTE_CONTROL,
		      UVCD_ZOOM_MIN, UVCD_ZOOM_MAX, UVCD_ZOOM_DEF, zoom),
	UVCD_CTRL_ROW("pan", UVCD_CTRL_CAMERA, UVC_CT_PANTILT_ABSOLUTE_CONTROL,
		      -UVCD_PANTILT_MAX, UVCD_PANTILT_MAX, 0, pan, .res = UVCD_PANTILT_RES, .part = 0),
	UVCD_CTRL_ROW("tilt", UVCD_CTRL_CAMERA, UVC_CT_PANTILT_ABSOLUTE_CONTROL,
		      -UVCD_PANTILT_MAX, UVCD_PANTILT_MAX, 0, tilt, .res = UVCD_PANTILT_RES, .part = 1),
	/* Extension Unit 4. Named for what each knob does. */
	UVCD_CTRL_ROW("exposure-compensation", UVCD_CTRL_CUSTOM, UVCD_CUSTOM_AE_COMP,
		      0, 255, 128, ae_comp),
	UVCD_CTRL_ROW("max-analog-gain", UVCD_CTRL_CUSTOM, UVCD_CUSTOM_MAX_AGAIN,
		      UVCD_CUSTOM_GAIN_MIN, UVCD_CUSTOM_GAIN_MAX, 160, max_again),
	UVCD_CTRL_ROW("max-digital-gain", UVCD_CTRL_CUSTOM, UVCD_CUSTOM_MAX_DGAIN,
		      UVCD_CUSTOM_GAIN_MIN, UVCD_CUSTOM_GAIN_MAX, 80, max_dgain),
	/* Above the tuning's own 128: measured on the K1 indoors (AE at ~20x
	 * gain), 192 cuts temporal noise ~37% for ~10% edge detail and makes
	 * MJPEG frames ~20% smaller; 0-128 look alike. */
	UVCD_CTRL_ROW("spatial-denoise", UVCD_CTRL_CUSTOM, UVCD_CUSTOM_SINTER,
		      0, 255, 192, sinter),
	UVCD_CTRL_ROW("temporal-denoise", UVCD_CTRL_CUSTOM, UVCD_CUSTOM_TEMPER,
		      0, 255, 128, temper),
	UVCD_CTRL_ROW("defective-pixel-correction", UVCD_CTRL_CUSTOM, UVCD_CUSTOM_DPC,
		      0, 255, 128, dpc),
	UVCD_CTRL_ROW("dynamic-range-compression", UVCD_CTRL_CUSTOM, UVCD_CUSTOM_DRC,
		      0, 255, 128, drc),
	/* Not a 128-centred ratio like the knobs above: the SDK takes 0-10,
	 * 0 = off. At the old 0-255/128 it ran flat out, exposing for the
	 * brightest spot and leaving the rest of the picture black. */
	UVCD_CTRL_ROW("highlight-suppression", UVCD_CTRL_CUSTOM, UVCD_CUSTOM_HIGHLIGHT,
		      0, 10, 0, highlight),
	UVCD_CTRL_ROW("metering", UVCD_CTRL_CUSTOM, UVCD_CUSTOM_METERING,
		      UVCD_METERING_TUNING, UVCD_METERING_SPOT, UVCD_METERING_TUNING, metering),
	UVCD_CTRL_ROW("rotation", UVCD_CTRL_CUSTOM, UVCD_CUSTOM_ROTATION,
		      0, 180, 0, rotation, .res = 180),

	/* Encoder. Ranges fit the XU's 16-bit value, hence kbps. */
	UVCD_CTRL_ROW("h264-bitrate-kbps", UVCD_CTRL_CUSTOM, UVCD_CUSTOM_H264_BITRATE,
		      0, 16000, 0, h264_bitrate_kbps),
	UVCD_CTRL_ROW("h264-rate-control", UVCD_CTRL_CUSTOM, UVCD_CUSTOM_H264_RATE_CONTROL,
		      UVCD_H264_RC_CBR, UVCD_H264_RC_CAPPED_VBR, UVCD_H264_RC_CBR, h264_rate_control),
	/* The T31 encoder rounds this up to whole seconds: at 30 fps, 10 and 15
	 * give an IDR every 30 frames, 31 and 45 every 60. */
	UVCD_CTRL_ROW("h264-gop-frames", UVCD_CTRL_CUSTOM, UVCD_CUSTOM_H264_GOP,
		      0, 300, 0, h264_gop_frames),
	UVCD_CTRL_ROW("h264-min-qp", UVCD_CTRL_CUSTOM, UVCD_CUSTOM_H264_MIN_QP,
		      -1, 51, -1, h264_min_qp),
	UVCD_CTRL_ROW("h264-max-qp", UVCD_CTRL_CUSTOM, UVCD_CUSTOM_H264_MAX_QP,
		      -1, 51, -1, h264_max_qp),
	UVCD_CTRL_ROW("h264-profile", UVCD_CTRL_CUSTOM, UVCD_CUSTOM_H264_PROFILE,
		      0, 2, 2, h264_profile),
	UVCD_CTRL_ROW("mjpeg-quality", UVCD_CTRL_CUSTOM, UVCD_CUSTOM_MJPEG_QUALITY,
		      1, 100, 80, mjpeg_quality),
};
#define UVCD_CTRL_DEF_COUNT (sizeof(uvcd_ctrl_defs) / sizeof(uvcd_ctrl_defs[0]))

size_t uvcd_ctrl_count(void)
{
	return UVCD_CTRL_DEF_COUNT;
}

const struct uvcd_ctrl_def *uvcd_ctrl_at(size_t i)
{
	return i < UVCD_CTRL_DEF_COUNT ? &uvcd_ctrl_defs[i] : NULL;
}

static const struct uvcd_ctrl_def *ctrl_def_part(uvcd_ctrl_kind_t kind, uint8_t selector,
						 unsigned part)
{
	for (size_t i = 0; i < UVCD_CTRL_DEF_COUNT; i++) {
		const struct uvcd_ctrl_def *d = &uvcd_ctrl_defs[i];
		if (d->kind == kind && d->selector == selector && d->part == part)
			return d;
	}
	return NULL;
}

static const struct uvcd_ctrl_def *ctrl_def_by_selector(uvcd_ctrl_kind_t kind, uint8_t selector)
{
	return ctrl_def_part(kind, selector, 0);
}

static const struct uvcd_ctrl_def *custom_ctrl_def(uint8_t selector)
{
	return ctrl_def_by_selector(UVCD_CTRL_CUSTOM, selector);
}

static const struct uvcd_ctrl_def *standard_ctrl_def(uint8_t selector)
{
	return ctrl_def_by_selector(UVCD_CTRL_STANDARD, selector);
}

/* --------------------------------------------------------------------------
 * Persistence. There is no SAVE command on any control path: a control that
 * the host successfully changed is expected to still be in effect after a
 * reboot, so the write-back is the daemon's job, not the caller's. Streaming
 * or idle, a change is written once changes have settled (a slider sends a
 * burst of them); only a factory reset is written at once.
 */
#define UVCD_CONFIG_SETTLE_MS 2000
#define UVCD_CONFIG_HEALTHY_MS 10000
#define UVCD_FRAME_TIMEOUT_MS 5000

static int64_t monotonic_ms(void);

/* Crash guard. The encoder is where a bad setting takes the daemon (or the
 * kernel) down, so a stream is counted in UVCD_CONFIG_DIRTY_PATH from its
 * start, and again from any change made while it runs, until it proves
 * healthy or stops cleanly. A crash leaves the count behind for
 * uvcd_pipeline_init(), which then goes back to the controls the last
 * healthy stream proved (UVCD_CONFIG_GOOD_PATH). An idle camera writes
 * nothing, so unplugging it is never taken for a crash. */
static void stream_guard_arm(gadget_t *g)
{
	if (g->guard_armed)
		return;
	g->guard_armed = true;
	if (uvcd_config_streak_write(UVCD_CONFIG_DIRTY_PATH, g->pipe->unproven_streak + 1) != 0)
		LOGW("crash guard %s: %s", UVCD_CONFIG_DIRTY_PATH, strerror(errno));
}

static void stream_guard_clear(gadget_t *g)
{
	if (!g->guard_armed)
		return;
	g->guard_armed = false;
	g->pipe->unproven_streak = 0;
	if (unlink(UVCD_CONFIG_DIRTY_PATH) != 0 && errno != ENOENT)
		LOGW("clear %s: %s", UVCD_CONFIG_DIRTY_PATH, strerror(errno));
}

/* Ten seconds of healthy streaming prove the running controls: they become
 * what a later crash falls back to. */
static void stream_guard_tick(gadget_t *g)
{
	if (!g->guard_armed || !g->streaming || !g->healthy_since_ms ||
	    monotonic_ms() - g->healthy_since_ms < UVCD_CONFIG_HEALTHY_MS ||
	    monotonic_ms() - g->last_frame_ms >= 1000)
		return;
	if (memcmp(&g->pipe->good, &g->pipe->controls, sizeof(g->pipe->good)) != 0) {
		if (uvcd_config_save(UVCD_CONFIG_GOOD_PATH, &g->pipe->controls) != 0)
			LOGW("persist %s: %s", UVCD_CONFIG_GOOD_PATH, strerror(errno));
		else
			g->pipe->good = g->pipe->controls;
	}
	stream_guard_clear(g);
}

static void config_mark_dirty(gadget_t *g)
{
	g->config_dirty = true;
	g->config_dirty_at = monotonic_ms();
	/* Mid-stream, the change still has to prove itself. */
	if (g->streaming) {
		g->healthy_since_ms = 0;
		stream_guard_arm(g);
	}
}

/* Write pending changes once they have settled, or now if force. */
static void config_flush(gadget_t *g, bool force)
{
	if (!g->config_dirty ||
	    (!force && monotonic_ms() - g->config_dirty_at < UVCD_CONFIG_SETTLE_MS))
		return;
	if (uvcd_config_save(UVCD_CONFIG_PATH, &g->pipe->controls) != 0)
		/* Don't spin retrying a write that keeps failing (read-only
		 * rootfs, full flash): drop the flag and let the next accepted
		 * control change arm another attempt. */
		LOGW("persist %s: %s", UVCD_CONFIG_PATH, strerror(errno));
	else
		LOGI("controls persisted to %s", UVCD_CONFIG_PATH);
	g->config_dirty = false;
}

const struct uvcd_ctrl_def *uvcd_ctrl_find(const char *name)
{
	if (!name)
		return NULL;
	for (size_t i = 0; i < UVCD_CTRL_DEF_COUNT; i++) {
		if (strcasecmp(name, uvcd_ctrl_defs[i].name) == 0)
			return &uvcd_ctrl_defs[i];
	}
	return NULL;
}

int uvcd_ctrl_get(gadget_t *g, const struct uvcd_ctrl_def *def)
{
	return *uvcd_ctrl_field(&g->pipe->controls, def);
}

/* Encoder settings whose value the pipeline reads from the stored state. */
static void encoder_restart(gadget_t *g, uint8_t format)
{
	if (g->defer_encoder_restart) {
		if (format == UVCD_FMT_H264)
			g->restart_h264 = true;
		else
			g->restart_mjpeg = true;
		return;
	}
	/* The pipeline's frame sequence restarts at 0, so resync the reader or
	 * it would wait for the old count to be passed again. */
	if (uvcd_pipeline_restart_encoder(g->pipe, format)) {
		g->read_seq = 0;
		g->watched_seq = 0;
		g->last_frame_ms = monotonic_ms();
		g->healthy_since_ms = 0;
	}
}

static void encoder_after_store(gadget_t *g, uint8_t selector)
{
	int ret = 0;

	switch (selector) {
	case UVCD_CUSTOM_H264_BITRATE:
		ret = uvcd_enc_apply_bitrate(g->pipe);
		break;
	case UVCD_CUSTOM_H264_GOP:
		ret = uvcd_enc_apply_gop(g->pipe);
		break;
	case UVCD_CUSTOM_H264_RATE_CONTROL:
	case UVCD_CUSTOM_H264_MIN_QP:
	case UVCD_CUSTOM_H264_MAX_QP:
	case UVCD_CUSTOM_H264_PROFILE:
		encoder_restart(g, UVCD_FMT_H264);
		break;
	case UVCD_CUSTOM_MJPEG_QUALITY:
		encoder_restart(g, UVCD_FMT_MJPEG);
		break;
	default:
		break;
	}
	/* Stored either way: a setting the live encoder refused still applies
	 * from the next stream start. */
	if (ret != 0)
		LOGW("encoder did not take selector %u live (%d); applies next stream", selector,
		     ret);
}

/* Camera Terminal controls. The pipeline applies them from a whole control
 * state, so hand it a copy with the new value in place. */
static int apply_camera_control(gadget_t *g, const struct uvcd_ctrl_def *def, int value)
{
	struct uvcd_control_state *live = &g->pipe->controls;

	/* Leaving auto-exposure: start manual from the exposure auto was
	 * using, so the picture doesn't jump -- as for white balance. */
	if (def->selector == UVC_CT_AE_MODE_CONTROL && value == UVCD_AE_MANUAL &&
	    live->ae_mode != UVCD_AE_MANUAL) {
		int t = uvcd_live_exposure(g->pipe);
		if (t > 0) {
			live->exposure_time = t > UVCD_EXPOSURE_MAX ? UVCD_EXPOSURE_MAX : t;
			LOGI("exposure-time=%d (from auto)", live->exposure_time);
		}
	}

	struct uvcd_control_state c = *live;
	*uvcd_ctrl_field(&c, def) = value;

	switch (def->selector) {
	case UVC_CT_AE_MODE_CONTROL:
	case UVC_CT_AE_PRIORITY_CONTROL:
	case UVC_CT_EXPOSURE_TIME_ABSOLUTE_CONTROL:
		return uvcd_apply_exposure(g->pipe, &c);
	case UVC_CT_ZOOM_ABSOLUTE_CONTROL:
	case UVC_CT_PANTILT_ABSOLUTE_CONTROL: {
		bool changed = c.zoom != live->zoom || c.pan != live->pan || c.tilt != live->tilt;
		int ret = uvcd_apply_view(g->pipe, &c);
		/* Front-crop changes recreate the channel and reset its sequence,
		 * including when a rejected change restores the previous view. */
		if (changed) {
			g->read_seq = 0;
			g->watched_seq = 0;
			g->last_frame_ms = monotonic_ms();
			g->healthy_since_ms = 0;
		}
		return ret;
	}
	default:
		return -EINVAL;
	}
}

/* Controls whose values mean one of a few things: store the meaning, not
 * whatever in-between number arrived. */
static int normalize_value(const struct uvcd_ctrl_def *def, int value)
{
	if (def->kind == UVCD_CTRL_CUSTOM && def->selector == UVCD_CUSTOM_ROTATION)
		return value >= 90 ? 180 : 0;
	if (def->kind == UVCD_CTRL_CAMERA && def->selector == UVC_CT_AE_MODE_CONTROL)
		return value == UVCD_AE_MANUAL ? UVCD_AE_MANUAL : UVCD_AE_APERTURE_PRIORITY;
	return value;
}

int uvcd_ctrl_set(gadget_t *g, const struct uvcd_ctrl_def *def, int value, int *out_applied)
{
	if (value < def->min)
		value = def->min;
	if (value > def->max)
		value = def->max;
	value = normalize_value(def, value);

	/* With the sensor/ISP down (no stream) there is nothing to apply to:
	 * store the value and let the next bring-up push it with the rest. */
	int ret = 0;
	if (uvcd_pipeline_hal_up(g->pipe)) {
		if (def->kind == UVCD_CTRL_STANDARD)
			ret = apply_control(g, def->selector, value);
		else if (def->kind == UVCD_CTRL_CAMERA)
			ret = apply_camera_control(g, def, value);
		else
			ret = apply_custom_control(g, def->selector, value);
	}
	if (ret != 0)
		return -1;

	int *field = uvcd_ctrl_field(&g->pipe->controls, def);
	/* Hosts rewrite values they already have (uvcvideo probes
	 * power-line-frequency on every connect): nothing to persist or
	 * re-prove then. */
	if (*field != value) {
		*field = value;
		config_mark_dirty(g);
	}
	if (def->kind == UVCD_CTRL_CUSTOM)
		encoder_after_store(g, (uint8_t)def->selector);
	if (out_applied)
		*out_applied = value;
	return 0;
}

int uvcd_ctrl_keyframe(gadget_t *g)
{
	return uvcd_enc_request_keyframe(g->pipe);
}

void uvcd_ctrl_reset(gadget_t *g)
{
	g->defer_encoder_restart = true;
	g->restart_h264 = g->restart_mjpeg = false;
	for (size_t i = 0; i < UVCD_CTRL_DEF_COUNT; i++) {
		int applied;
		if (uvcd_ctrl_set(g, &uvcd_ctrl_defs[i], uvcd_ctrl_defs[i].def, &applied) != 0)
			LOGW("reset: HAL rejected default for %s", uvcd_ctrl_defs[i].name);
	}
	g->defer_encoder_restart = false;
	if (g->restart_h264)
		encoder_restart(g, UVCD_FMT_H264);
	if (g->restart_mjpeg)
		encoder_restart(g, UVCD_FMT_MJPEG);

	/* Written at once, even if nothing changed, and the old proven
	 * controls go with it: a crash after a reset must not bring back what
	 * the reset threw away. */
	g->config_dirty = true;
	config_flush(g, true);
	if (uvcd_config_save(UVCD_CONFIG_GOOD_PATH, &g->pipe->controls) != 0)
		LOGW("persist %s: %s", UVCD_CONFIG_GOOD_PATH, strerror(errno));
	else
		g->pipe->good = g->pipe->controls;
	LOGI("factory reset: all controls restored to defaults");
}

static void handle_custom_setup(gadget_t *g, const struct usb_ctrlrequest *req,
				struct uvc_request_data *resp)
{
	uint8_t selector = req->wValue >> 8;
	int16_t value;
	uint16_t len;
	bool is_action = (selector == UVCD_CUSTOM_RESET || selector == UVCD_CUSTOM_H264_KEYFRAME);
	int *current = is_action ? NULL : custom_control_value(g, selector);

	if (!is_action && current == NULL) {
		resp->length = -1;
		return;
	}

	if (!(req->bRequestType & USB_DIR_IN)) {
		if (req->bRequest != UVC_SET_CUR) {
			resp->length = -1;
			return;
		}
		g->last_cs = selector;
		g->last_intf = UVC_INTF_CONTROL;
		g->last_unit = UVCD_CUSTOM_UNIT_ID;
		g->last_request = req->bRequest;
		resp->length = req->wLength;
		return;
	}

	switch (req->bRequest) {
	case UVC_GET_CUR:
	case UVC_GET_MIN:
	case UVC_GET_MAX:
	case UVC_GET_RES:
	case UVC_GET_DEF:
		if (is_action) {
			/* Write-only in spirit: 0..1 with nothing to read back. */
			value = req->bRequest == UVC_GET_MAX ? 1 :
				req->bRequest == UVC_GET_RES ? 1 : 0;
		} else {
			value = req->bRequest == UVC_GET_CUR ? *current :
				req->bRequest == UVC_GET_MIN ? custom_control_min(selector) :
				req->bRequest == UVC_GET_MAX ? custom_control_max(selector) :
				req->bRequest == UVC_GET_DEF ? custom_control_def(selector) :
				custom_control_res(selector);
		}
		len = req->wLength < sizeof(value) ? req->wLength : sizeof(value);
		memcpy(resp->data, &value, len);
		resp->length = len;
		break;
	case UVC_GET_INFO: {
		uint8_t info = UVC_CONTROL_CAP_GET | UVC_CONTROL_CAP_SET;
		len = req->wLength < sizeof(info) ? req->wLength : sizeof(info);
		memcpy(resp->data, &info, len);
		resp->length = len;
		break;
	}
	case UVC_GET_LEN:
		value = sizeof(value);
		len = req->wLength < sizeof(value) ? req->wLength : sizeof(value);
		memcpy(resp->data, &value, len);
		resp->length = len;
		break;
	default:
		resp->length = -1;
		break;
	}
}

static void handle_control_setup(gadget_t *g, const struct usb_ctrlrequest *req,
				 struct uvc_request_data *resp)
{
	uint8_t selector = req->wValue >> 8;
	uint16_t length = req->wLength;
	int16_t value;
	uint8_t info;
	uint16_t len;

	/* Ranges come from the control table, so adding a control there is
	 * enough -- there is no second list to keep in step. */
	const struct uvcd_ctrl_def *cdef = standard_ctrl_def(selector);
	if (!cdef || !control_value(g, selector)) {
		resp->length = -1;
		return;
	}
	int min_value = cdef->min;
	int max_value = cdef->max;
	int def_value = cdef->def;

	if (!(req->bRequestType & USB_DIR_IN)) {
		if (req->bRequest != UVC_SET_CUR) {
			resp->length = -1;
			return;
		}
		g->last_cs = selector;
		g->last_intf = UVC_INTF_CONTROL;
		g->last_unit = UVCD_PROCESSING_UNIT_ID;
		g->last_request = req->bRequest;
		resp->length = length;
		return;
	}

	switch (req->bRequest) {
	case UVC_GET_CUR:
	case UVC_GET_MIN:
	case UVC_GET_MAX:
	case UVC_GET_RES:
	case UVC_GET_DEF:
		value = (req->bRequest == UVC_GET_CUR) ? *control_value(g, selector) :
			(req->bRequest == UVC_GET_MAX) ? max_value :
			(req->bRequest == UVC_GET_DEF) ? def_value :
			(req->bRequest == UVC_GET_RES) ? 1 : min_value;
		if (control_length(selector) == 1) {
			uint8_t byte_value = (uint8_t)value;
			len = length < sizeof(byte_value) ? length : sizeof(byte_value);
			memcpy(resp->data, &byte_value, len);
		} else {
			len = length < sizeof(value) ? length : sizeof(value);
			memcpy(resp->data, &value, len);
		}
		resp->length = len;
		break;
	case UVC_GET_INFO:
		info = UVC_CONTROL_CAP_GET | UVC_CONTROL_CAP_SET;
		len = length < sizeof(info) ? length : sizeof(info);
		memcpy(resp->data, &info, len);
		resp->length = len;
		break;
	case UVC_GET_LEN:
		value = (int16_t)control_length(selector);
		len = length < sizeof(value) ? length : sizeof(value);
		memcpy(resp->data, &value, len);
		resp->length = len;
		break;
	default:
		resp->length = -1;
		break;
	}
}

/* Camera Terminal. Control sizes are fixed by UVC, and pan/tilt is one
 * control carrying two values, so a reply is built part by part. */
static uint16_t camera_control_length(uint8_t selector)
{
	switch (selector) {
	case UVC_CT_AE_MODE_CONTROL:
	case UVC_CT_AE_PRIORITY_CONTROL:
		return 1;
	case UVC_CT_ZOOM_ABSOLUTE_CONTROL:
		return 2;
	case UVC_CT_EXPOSURE_TIME_ABSOLUTE_CONTROL:
		return 4;
	case UVC_CT_PANTILT_ABSOLUTE_CONTROL:
		return 8;
	default:
		return 0;
	}
}

static unsigned camera_control_parts(uint8_t selector)
{
	return selector == UVC_CT_PANTILT_ABSOLUTE_CONTROL ? 2 : 1;
}

static void put_le(uint8_t *out, int value, unsigned size)
{
	for (unsigned i = 0; i < size; i++)
		out[i] = (uint8_t)((uint32_t)value >> (8 * i));
}

static int get_le_signed(const uint8_t *in, unsigned size)
{
	uint32_t v = 0;
	for (unsigned i = 0; i < size; i++)
		v |= (uint32_t)in[i] << (8 * i);
	if (size < 4 && (v & (1u << (8 * size - 1))))
		v |= ~0u << (8 * size); /* sign-extend */
	return (int)v;
}

static void handle_camera_setup(gadget_t *g, const struct usb_ctrlrequest *req,
				struct uvc_request_data *resp)
{
	uint8_t selector = req->wValue >> 8;
	uint16_t size = camera_control_length(selector);
	unsigned parts = camera_control_parts(selector);

	if (size == 0 || !ctrl_def_part(UVCD_CTRL_CAMERA, selector, 0)) {
		resp->length = -1;
		return;
	}

	if (!(req->bRequestType & USB_DIR_IN)) {
		if (req->bRequest != UVC_SET_CUR) {
			resp->length = -1;
			return;
		}
		g->last_cs = selector;
		g->last_intf = UVC_INTF_CONTROL;
		g->last_unit = UVCD_CAMERA_TERMINAL_ID;
		g->last_request = req->bRequest;
		resp->length = req->wLength;
		return;
	}

	uint8_t buf[8] = {0};
	uint16_t len;

	switch (req->bRequest) {
	case UVC_GET_CUR:
	case UVC_GET_MIN:
	case UVC_GET_MAX:
	case UVC_GET_RES:
	case UVC_GET_DEF:
		for (unsigned i = 0; i < parts; i++) {
			const struct uvcd_ctrl_def *d = ctrl_def_part(UVCD_CTRL_CAMERA, selector, i);
			int v;
			switch (req->bRequest) {
			case UVC_GET_CUR:
				v = uvcd_ctrl_get(g, d);
				/* Under auto-exposure, report the exposure auto is
				 * actually using. */
				if (selector == UVC_CT_EXPOSURE_TIME_ABSOLUTE_CONTROL &&
				    g->pipe->controls.ae_mode != UVCD_AE_MANUAL) {
					int t = uvcd_live_exposure(g->pipe);
					if (t > 0)
						v = t;
				}
				break;
			case UVC_GET_MIN:
				v = d->min;
				break;
			case UVC_GET_MAX:
				v = d->max;
				break;
			case UVC_GET_RES:
				v = d->res ? d->res : 1;
				break;
			default:
				v = d->def;
				break;
			}
			put_le(buf + i * (size / parts), v, size / parts);
		}
		len = req->wLength < size ? req->wLength : size;
		memcpy(resp->data, buf, len);
		resp->length = len;
		break;
	case UVC_GET_INFO: {
		uint8_t info = UVC_CONTROL_CAP_GET | UVC_CONTROL_CAP_SET;
		if (selector == UVC_CT_EXPOSURE_TIME_ABSOLUTE_CONTROL)
			info |= UVC_CONTROL_CAP_AUTOUPDATE;
		len = req->wLength < sizeof(info) ? req->wLength : sizeof(info);
		memcpy(resp->data, &info, len);
		resp->length = len;
		break;
	}
	case UVC_GET_LEN:
		put_le(buf, size, 2);
		len = req->wLength < 2 ? req->wLength : 2;
		memcpy(resp->data, buf, len);
		resp->length = len;
		break;
	default:
		resp->length = -1;
		break;
	}
}

static void handle_camera_data(gadget_t *g, const struct uvc_request_data *data)
{
	uint8_t selector = g->last_cs;
	uint16_t size = camera_control_length(selector);
	unsigned parts = camera_control_parts(selector);

	if (size == 0 || data->length < (int32_t)size) {
		LOGW("short camera control data selector=%u length=%d", selector, data->length);
		return;
	}
	for (unsigned i = 0; i < parts; i++) {
		const struct uvcd_ctrl_def *d = ctrl_def_part(UVCD_CTRL_CAMERA, selector, i);
		int value = get_le_signed(data->data + i * (size / parts), size / parts);
		int applied;

		if (!d)
			return;
		if (uvcd_ctrl_set(g, d, value, &applied) != 0) {
			LOGW("HAL rejected %s=%d", d->name, value);
			continue;
		}
		LOGI("%s=%d", d->name, applied);
	}
}

static void handle_setup_event(gadget_t *g, const struct usb_ctrlrequest *req)
{
	struct uvc_request_data resp;
	struct uvc_streaming_control sc;

	memset(&resp, 0, sizeof(resp));

	uint8_t type = req->bRequestType;
	uint8_t request = req->bRequest;
	uint8_t cs = req->wValue >> 8;
	uint8_t unit = req->wIndex >> 8;
	uint8_t intf = req->wIndex & 0xff;
	uint16_t wLength = req->wLength;
	LOGD("UVC SETUP type=%02x req=%02x value=%04x index=%04x length=%u",
	     type, request, req->wValue, req->wIndex, wLength);

	if (intf == UVC_INTF_CONTROL) {
		switch (unit) {
		case UVCD_CUSTOM_UNIT_ID:
			handle_custom_setup(g, req, &resp);
			break;
		case UVCD_PROCESSING_UNIT_ID:
			handle_control_setup(g, req, &resp);
			break;
		case UVCD_CAMERA_TERMINAL_ID:
			handle_camera_setup(g, req, &resp);
			break;
		default:
			/* Interface-level requests (e.g. the error code
			 * control) and the output terminal: nothing here. */
			resp.length = -1;
			break;
		}
		goto send;
	}

	if (intf != UVC_INTF_STREAMING ||
	    (cs != UVC_VS_PROBE_CONTROL && cs != UVC_VS_COMMIT_CONTROL)) {
		LOGW("UVC SETUP unsupported interface=%u cs=%u", intf, cs);
		resp.length = -1;
		goto send;
	}

	struct uvc_streaming_control *target =
		(cs == UVC_VS_PROBE_CONTROL) ? &g->probe : &g->commit;

	if (type & USB_DIR_IN) {
		uint16_t len = (wLength < sizeof(sc)) ? wLength : sizeof(sc);

		switch (request) {
		case UVC_GET_CUR:
			memcpy(resp.data, target, len);
			resp.length = len;
			break;
		case UVC_GET_MIN:
			fill_streaming_control(&sc, UVCD_FMT_MJPEG, UVCD_FRAME_360P,
					       UVCD_INTERVAL_30FPS);
			memcpy(resp.data, &sc, len);
			resp.length = len;
			break;
		case UVC_GET_MAX:
			fill_streaming_control(&sc, UVCD_FMT_H264, UVCD_FRAME_1080P,
					       UVCD_INTERVAL_5FPS);
			memcpy(resp.data, &sc, len);
			resp.length = len;
			break;
		case UVC_GET_DEF:
			fill_streaming_control(&sc, UVCD_FMT_MJPEG, UVCD_FRAME_1080P,
					       UVCD_INTERVAL_30FPS);
			memcpy(resp.data, &sc, len);
			resp.length = len;
			break;
		default:
			LOGW("UVC SETUP unsupported IN request=%02x", request);
			resp.length = -1;
			break;
		}
	} else {
		g->last_cs = cs;
		g->last_intf = intf;
		g->last_request = request;
		resp.length = wLength;
		LOGD("UVC OUT request=%02x cs=%u awaiting DATA", request, cs);
	}

send:
	LOGD("UVC RESPONSE length=%d", resp.length);
	if (ioctl(g->fd, UVCIOC_SEND_RESPONSE, &resp) < 0) {
		LOGW("UVCIOC_SEND_RESPONSE: %s", strerror(errno));
		return;
	}
	LOGD("UVC RESPONSE sent");
}

static void handle_data_event(gadget_t *g, const struct uvc_request_data *data)
{
	if (g->last_request == UVC_SET_CUR && g->last_intf == UVC_INTF_CONTROL) {
		if (g->last_unit == UVCD_CAMERA_TERMINAL_ID) {
			handle_camera_data(g, data);
			return;
		}
		bool xu = (g->last_unit == UVCD_CUSTOM_UNIT_ID);
		int32_t need = xu ? (int32_t)sizeof(int16_t) : (int32_t)control_length(g->last_cs);
		int16_t value = 0;

		if (data->length < need) {
			LOGW("short control data unit=%s selector=%u length=%d", xu ? "xu" : "pu",
			     g->last_cs, data->length);
			return;
		}
		if (need == 1)
			value = data->data[0];
		else
			memcpy(&value, data->data, sizeof(value));

		/* Actions: a non-zero write triggers them, nothing is stored. */
		if (xu && g->last_cs == UVCD_CUSTOM_RESET) {
			if (value != 0)
				uvcd_ctrl_reset(g);
			return;
		}
		if (xu && g->last_cs == UVCD_CUSTOM_H264_KEYFRAME) {
			if (value != 0 && uvcd_enc_request_keyframe(g->pipe) != 0)
				LOGW("keyframe request failed");
			return;
		}

		/* Settings: one path for both units -- clamp, apply, store,
		 * persist, and any encoder follow-up -- shared with the reset. */
		const struct uvcd_ctrl_def *def =
			xu ? custom_ctrl_def(g->last_cs) : standard_ctrl_def(g->last_cs);
		int applied;
		if (def == NULL) {
			LOGW("unknown control unit=%s selector=%u", xu ? "xu" : "pu", g->last_cs);
			return;
		}
		if (uvcd_ctrl_set(g, def, value, &applied) != 0) {
			LOGW("HAL rejected %s=%d", def->name, value);
			return;
		}
		LOGI("%s=%d", def->name, applied);
		return;
	}

	struct uvc_streaming_control proposed;
	struct uvc_streaming_control *target =
		(g->last_cs == UVC_VS_COMMIT_CONTROL) ? &g->commit : &g->probe;

	memcpy(&proposed, data->data, sizeof(proposed));
	LOGD("UVC DATA format=%u frame=%u interval=%u", proposed.bFormatIndex,
	     proposed.bFrameIndex, proposed.dwFrameInterval);
	fill_streaming_control(target, proposed.bFormatIndex, proposed.bFrameIndex,
			       proposed.dwFrameInterval);

	LOGD("%s fmt=%u frm=%u int=%u",
	     g->last_cs == UVC_VS_COMMIT_CONTROL ? "COMMIT" : "PROBE", target->bFormatIndex,
	     target->bFrameIndex, target->dwFrameInterval);

	if (g->last_cs == UVC_VS_COMMIT_CONTROL) {
		if (g->streaming)
			stop_streaming(g);
		LOGI("COMMIT received, bringing up pipeline + streaming");
		if (start_streaming(g) < 0)
			LOGE("failed to start streaming");
	}
}

static int start_streaming(gadget_t *g)
{
	struct v4l2_requestbuffers rb;
	struct v4l2_format fmt;
	int i;

	g->cur_format = g->commit.bFormatIndex;
	g->cur_frame = g->commit.bFrameIndex;
	g->cur_interval = g->commit.dwFrameInterval;

	/* Bring up the ISP/encoder pipeline for exactly what was negotiated */
	stream_guard_arm(g);
	if (uvcd_pipeline_start(g->pipe, g->cur_format, g->cur_frame, g->cur_interval) != 0) {
		LOGE("pipeline start failed for fmt=%u frame=%u", g->cur_format, g->cur_frame);
		_exit(1);
	}
	g->read_seq = g->pipe->frame.seq;

	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	fmt.fmt.pix.pixelformat =
		(g->cur_format == UVCD_FMT_H264) ? v4l2_fourcc('H', '2', '6', '4')
						  : V4L2_PIX_FMT_MJPEG;
	fmt.fmt.pix.width = uvcd_frames[g->cur_frame].width;
	fmt.fmt.pix.height = uvcd_frames[g->cur_frame].height;
	fmt.fmt.pix.sizeimage = uvcd_frames[g->cur_frame].max_size;
	fmt.fmt.pix.field = V4L2_FIELD_NONE;

	if (ioctl(g->fd, VIDIOC_S_FMT, &fmt) < 0)
		LOGW("VIDIOC_S_FMT: %s", strerror(errno));

	memset(&rb, 0, sizeof(rb));
	rb.count = g->buf_count;
	rb.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	rb.memory = V4L2_MEMORY_MMAP;

	if (ioctl(g->fd, VIDIOC_REQBUFS, &rb) < 0) {
		LOGE("VIDIOC_REQBUFS: %s", strerror(errno));
		uvcd_pipeline_stop(g->pipe);
		stream_guard_clear(g);
		return -1;
	}
	g->buf_count = rb.count;

	for (i = 0; i < g->buf_count; i++) {
		struct v4l2_buffer buf;

		memset(&buf, 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;

		if (ioctl(g->fd, VIDIOC_QUERYBUF, &buf) < 0) {
			LOGE("VIDIOC_QUERYBUF %d: %s", i, strerror(errno));
			goto err_unmap;
		}

		g->buffers[i].length = buf.length;
		g->buffers[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED,
					  g->fd, buf.m.offset);
		if (g->buffers[i].start == MAP_FAILED) {
			g->buffers[i].start = NULL;
			LOGE("mmap buffer %d: %s", i, strerror(errno));
			goto err_unmap;
		}

		/* Not queued: a buffer goes to the gadget only once it holds a
		 * real frame (see deliver_frame). Priming the queue with
		 * placeholder buffers sends the host that many garbage frames
		 * the moment streaming starts. STREAMON with an empty queue is
		 * fine -- the gadget's pump idles until the first QBUF. */
		g->buf_queued[i] = false;
	}

	int type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	if (ioctl(g->fd, VIDIOC_STREAMON, &type) < 0) {
		LOGE("VIDIOC_STREAMON: %s", strerror(errno));
		goto err_unmap;
	}

	g->stalled_since_ms = 0;
	g->streaming = true;
	g->last_frame_ms = monotonic_ms();
	g->watched_seq = 0;
	g->healthy_since_ms = 0;

	LOGI("streaming: %s %ux%u", g->cur_format == UVCD_FMT_H264 ? "H.264" : "MJPEG",
	     uvcd_frames[g->cur_frame].width, uvcd_frames[g->cur_frame].height);
	return 0;

err_unmap:
	for (int j = 0; j < i; j++) {
		if (g->buffers[j].start) {
			munmap(g->buffers[j].start, g->buffers[j].length);
			g->buffers[j].start = NULL;
		}
	}
	memset(&rb, 0, sizeof(rb));
	rb.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	rb.memory = V4L2_MEMORY_MMAP;
	ioctl(g->fd, VIDIOC_REQBUFS, &rb);
	uvcd_pipeline_stop(g->pipe);
	stream_guard_clear(g);
	return -1;
}

static void stop_streaming(gadget_t *g)
{
	if (!g->streaming)
		return;

	int type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	ioctl(g->fd, VIDIOC_STREAMOFF, &type);

	for (int i = 0; i < g->buf_count; i++) {
		if (g->buffers[i].start) {
			munmap(g->buffers[i].start, g->buffers[i].length);
			g->buffers[i].start = NULL;
		}
	}

	struct v4l2_requestbuffers rb = {0};
	rb.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	rb.memory = V4L2_MEMORY_MMAP;
	ioctl(g->fd, VIDIOC_REQBUFS, &rb);

	g->streaming = false;

	/* True on-demand: the channels go now, the sensor/ISP once
	 * uvcd_pipeline_tick() sees no stream for UVCD_HAL_LINGER_MS. */
	uvcd_pipeline_stop(g->pipe);
	/* Only once teardown is over: it can hang after an ISP failure. */
	stream_guard_clear(g);
	LOGI("streaming stopped, pipeline released");
}

static void process_events(gadget_t *g)
{
	struct v4l2_event ev;

	while (ioctl(g->fd, VIDIOC_DQEVENT, &ev) == 0) {
		struct uvc_event *uvc_ev = (struct uvc_event *)&ev.u.data;
		LOGD("UVC EVENT type=%u", ev.type);

		switch (ev.type) {
		case UVC_EVENT_CONNECT:
			LOGI("USB connected (speed %d)", uvc_ev->speed);
			break;
		case UVC_EVENT_DISCONNECT:
			LOGI("USB disconnected");
			stop_streaming(g);
			break;
		case UVC_EVENT_SETUP:
			handle_setup_event(g, &uvc_ev->req);
			break;
		case UVC_EVENT_DATA:
			handle_data_event(g, &uvc_ev->data);
			break;
		case UVC_EVENT_STREAMON:
			LOGI("STREAMON");
			if (start_streaming(g) < 0)
				LOGE("failed to start streaming");
			break;
		case UVC_EVENT_STREAMOFF:
			LOGI("STREAMOFF");
			stop_streaming(g);
			break;
		default:
			LOGW("UVC EVENT unknown type=%u", ev.type);
			break;
		}
	}
	/* An empty queue is ENOENT on this kernel (EAGAIN on newer ones). */
	if (errno != EAGAIN && errno != ENOENT)
		LOGW("VIDIOC_DQEVENT: %s", strerror(errno));
}

static int64_t monotonic_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* A buffer userspace owns and may fill, or -1 with errno set. Unqueued
 * buffers are used first; once every buffer is with the gadget, reclaim
 * whichever the host has finished with. */
static int acquire_buffer(gadget_t *g)
{
	struct v4l2_buffer buf;

	for (int i = 0; i < g->buf_count; i++) {
		if (!g->buf_queued[i])
			return i;
	}

	memset(&buf, 0, sizeof(buf));
	buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	buf.memory = V4L2_MEMORY_MMAP;
	if (ioctl(g->fd, VIDIOC_DQBUF, &buf) < 0)
		return -1;
	if (buf.index >= (unsigned)g->buf_count) {
		errno = EINVAL;
		return -1;
	}
	g->buf_queued[buf.index] = false;
	return (int)buf.index;
}

static void deliver_frame(gadget_t *g)
{
	struct v4l2_buffer buf;
	uint64_t wseq, skipped;
	uint32_t len;
	int64_t ts_us;
	int idx;

	pthread_mutex_lock(&g->pipe->frame.lock);
	wseq = g->pipe->frame.seq;
	pthread_mutex_unlock(&g->pipe->frame.lock);
	if (g->read_seq >= wseq)
		return;

	idx = acquire_buffer(g);
	if (idx < 0) {
		if (errno != EAGAIN) {
			g->healthy_since_ms = 0;
			return;
		}
		/* Keep encoding into the single latest-frame slot. A host that
		 * resumes reading can immediately receive a fresh frame. */
		if (!g->stalled_since_ms)
			g->stalled_since_ms = monotonic_ms();
		g->healthy_since_ms = 0;
		return;
	}
	if (g->stalled_since_ms && g->cur_format == UVCD_FMT_H264)
		uvcd_enc_request_keyframe(g->pipe);
	g->stalled_since_ms = 0;

	pthread_mutex_lock(&g->pipe->frame.lock);
	wseq = g->pipe->frame.seq;
	len = g->pipe->frame.len;
	ts_us = g->pipe->frame.ts_us;
	skipped = wseq - g->read_seq - 1;
	g->read_seq = wseq;
	if (len > g->buffers[idx].length) {
		g->healthy_since_ms = 0;
		/* Never truncate: a cut frame decodes as garbage from the cut
		 * onward, which is worse than the host not getting it. The
		 * buffer stays ours for the next frame. */
		pthread_mutex_unlock(&g->pipe->frame.lock);
		LOGW("frame of %u bytes exceeds the %zu-byte buffer, dropped", len,
		     g->buffers[idx].length);
		/* Every H.264 frame after this one references what the host
		 * never got; start a fresh chain now instead of at the GOP. */
		uvcd_enc_request_keyframe(g->pipe);
		return;
	}
	memcpy(g->buffers[idx].start, g->pipe->frame.data, len);
	pthread_mutex_unlock(&g->pipe->frame.lock);

	/* The handoff keeps only the newest frame, which is harmless for MJPEG
	 * but not for H.264: a skipped P-frame breaks the reference chain and
	 * the host shows errors until the next IDR. Say so rather than let it
	 * look like USB corruption. */
	if (skipped && g->cur_format == UVCD_FMT_H264)
		LOGW("H.264: %llu frame(s) skipped before seq %llu; expect decode errors until the next IDR",
		     (unsigned long long)skipped, (unsigned long long)wseq);

	LOGD("delivering frame seq=%llu len=%u format=%u frame=%u",
	     (unsigned long long)wseq, len, g->cur_format, g->cur_frame);

	memset(&buf, 0, sizeof(buf));
	buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	buf.memory = V4L2_MEMORY_MMAP;
	buf.index = (unsigned)idx;
	buf.bytesused = len;
	/* Capture time; the gadget sends it to the host as the frame's PTS. */
	buf.timestamp.tv_sec = (time_t)(ts_us / 1000000);
	buf.timestamp.tv_usec = (suseconds_t)(ts_us % 1000000);
	if (ioctl(g->fd, VIDIOC_QBUF, &buf) < 0) {
		g->healthy_since_ms = 0;
		LOGW("VIDIOC_QBUF: %s", strerror(errno));
		return;
	}
	g->buf_queued[idx] = true;
	if (!g->healthy_since_ms)
		g->healthy_since_ms = monotonic_ms();
}

static int gadget_open(gadget_t *g)
{
	struct v4l2_event_subscription sub;
	static const uint32_t events[] = {
		UVC_EVENT_CONNECT,   UVC_EVENT_DISCONNECT, UVC_EVENT_STREAMON,
		UVC_EVENT_STREAMOFF, UVC_EVENT_SETUP,	   UVC_EVENT_DATA,
	};

	g->fd = open(g->device, O_RDWR | O_NONBLOCK);
	if (g->fd < 0) {
		LOGE("open %s: %s", g->device, strerror(errno));
		return -1;
	}

	memset(&sub, 0, sizeof(sub));
	for (size_t i = 0; i < sizeof(events) / sizeof(events[0]); i++) {
		sub.type = events[i];
		LOGD("subscribing UVC event type=%u", sub.type);
		if (ioctl(g->fd, VIDIOC_SUBSCRIBE_EVENT, &sub) < 0) {
			LOGE("subscribe event %u: %s", events[i], strerror(errno));
			close(g->fd);
			g->fd = -1;
			return -1;
		}
	}
	LOGI("all UVC events subscribed on fd=%d", g->fd);

	fill_streaming_control(&g->probe, UVCD_FMT_MJPEG, UVCD_FRAME_1080P, UVCD_INTERVAL_30FPS);
	g->commit = g->probe;
	g->buf_count = UVCD_MAX_BUFFERS;

	FILE *ready = fopen("/var/run/uvcd.ready", "w");
	if (!ready) {
		close(g->fd);
		g->fd = -1;
		return -1;
	}
	fprintf(ready, "%ld\n", (long)getpid());
	if (fclose(ready) != 0) {
		close(g->fd);
		g->fd = -1;
		return -1;
	}
	LOGI("UVC device %s opened", g->device);
	return 0;
}

extern volatile sig_atomic_t uvcd_running;

int uvcd_gadget_run(uvcd_pipeline_t *p, const char *device)
{
	gadget_t g = {0};
	g.pipe = p;
	g.device = device;

	if (gadget_open(&g) < 0)
		return -1;
	int ctl_fd = uvcd_ctl_listen();
	if (ctl_fd < 0)
		LOGW("control socket %s: %s", UVCD_CTL_PATH, strerror(errno));

	while (uvcd_running) {
		struct pollfd pfd[2] = {
			{.fd = g.fd, .events = POLLPRI},
			{.fd = ctl_fd, .events = POLLIN},
		};
		int timeout = g.streaming ? 5 : 500;
		int n = poll(pfd, 2, timeout);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			LOGE("poll: %s", strerror(errno));
			break;
		}
		if (n > 0)
			LOGD("UVC poll revents=%04x", pfd[0].revents);

		if (pfd[0].revents & POLLPRI)
			process_events(&g);
		if (pfd[1].revents & POLLIN)
			uvcd_ctl_accept(&g, ctl_fd);

		if (g.streaming) {
			uint64_t seq;
			pthread_mutex_lock(&p->frame.lock);
			seq = p->frame.seq;
			pthread_mutex_unlock(&p->frame.lock);
			if (seq != g.watched_seq) {
				g.watched_seq = seq;
				g.last_frame_ms = monotonic_ms();
			}
			if (monotonic_ms() - g.last_frame_ms >= UVCD_FRAME_TIMEOUT_MS) {
				LOGE("no encoded frames for %d ms; restarting daemon",
				     UVCD_FRAME_TIMEOUT_MS);
				/* The SDK can hang during teardown after an ISP failure.
				 * Leave the dirty marker and let the supervisor disconnect. */
				_exit(1);
			}
			deliver_frame(&g);
		} else {
			g.healthy_since_ms = 0;
		}

		stream_guard_tick(&g);
		config_flush(&g, false);
		uvcd_pipeline_tick(p);
	}

	unlink("/var/run/uvcd.ready");
	uvcd_ctl_close(ctl_fd);
	stop_streaming(&g);
	config_flush(&g, true);
	if (g.fd >= 0)
		close(g.fd);
	return uvcd_running ? -1 : 0;
}
