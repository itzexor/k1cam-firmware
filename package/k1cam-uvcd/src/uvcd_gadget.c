/*
 * uvcd_gadget.c -- UVC gadget event handling and frame delivery
 *
 * Adapted from raptor's rwc daemon, stripped of audio/ring-IPC: frames
 * come straight from the stream's shared buffer (uvcd_pipeline.c), which
 * only fills while a host has actually committed to a format. Controls go
 * to the camera live, streaming or not (uvcd_ctrl_set).
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
#include "uvcd_firmware.h"
#include "uvcd_update.h"

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
/* It travels in v4l2_event.u.data, 64 bytes; the kernel caps requests at the
 * 60 left after the length (UVC_MAX_REQUEST_SIZE), the firmware frame size. */
_Static_assert(sizeof(struct uvc_request_data) == 64, "uvc_request_data is 64 bytes");
_Static_assert(UVCD_FW_FRAME_SIZE == sizeof(((struct uvc_request_data *)0)->data),
	       "firmware frames fill a request");

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
/* Request Error Code Control (UVC 1.5 4.2.1.2), on the control interface
 * itself: why the last request stalled, 0 once any request succeeds.
 * uvcvideo reads it after every stall to pick an errno; when that read
 * stalled too, the host logged "Failed to query ... UVC error code control"
 * for each refused request. Linux maps 5-7 to EIO. */
#define UVCD_REQUEST_ERROR_NONE 0x00
#define UVCD_REQUEST_ERROR_INVALID_UNIT 0x05
#define UVCD_REQUEST_ERROR_INVALID_CONTROL 0x06
#define UVCD_REQUEST_ERROR_INVALID_REQUEST 0x07
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
/* Encoder settings. Bitrate applies live; the rest can only be set when the
 * encoder channel is created, so changing them mid-stream re-creates it in
 * place (a short gap in frames, no renegotiation with the host). */
#define UVCD_CUSTOM_H264_BITRATE 13
#define UVCD_CUSTOM_H264_RATE_CONTROL 14
/* 15 was the GOP length. The T31 encoder only makes whole-second GOPs, and
 * libimp rounded a live change a second further than the same value at
 * stream start, so the GOP is fixed at one second. Retired, not reused. */
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
/* T31 SDK units: 32 per doubling (160 = 32x). libimp silently ignores a
 * ceiling above the sensor's max_again (GC2083: 125.4375x, 223.07 units),
 * so 224 kept the previous ceiling while reading back as set. The sensor
 * rounds a gain down to its step table, whose top steps are 106.7x (215.6)
 * and 125.4x: any ceiling from 216 to 223 stops at 106.7x, and 125.4x is
 * out of reach. Measured on the K1 at a 100 us exposure. */
#define UVCD_CUSTOM_AGAIN_MAX 223
#define UVCD_CUSTOM_DGAIN_MAX 160

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
	uint8_t request_error; /* UVCD_REQUEST_ERROR_*, for the last request */

	/* Zoom (0) and pan/tilt (1) writes held back by the view throttle
	 * (handle_camera_data), and when a view change last finished. */
	struct uvc_request_data view_pending[2];
	bool view_pending_set[2];
	int64_t view_applied_ms;

	/* Firmware info, dumps and updates (selectors 26-28); NULL if it
	 * could not be set up, and those selectors then stall. */
	struct uvcd_update *update;
	uint8_t diag[UVCD_DIAG_SIZE];
	bool diag_latched;
	uint8_t diag_page;
	/* A trial boot's proof that this release runs the camera. */
	struct {
		int step; /* SELFTEST_* */
		int64_t since_ms;
		uint64_t seen_seq;
		int frames;
		bool key;
		int tries;
	} selftest;

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

void uvcd_ctrl_diag(gadget_t *g, uint8_t r[UVCD_DIAG_SIZE]) { uvcd_camera_diag(&g->pipe->cam, r); }

static bool diagnostic_selector(uint8_t s) { return s == UVCD_DIAG_LATCH || s == UVCD_DIAG_PAGE; }

static bool firmware_selector(uint8_t selector)
{
	return selector == UVCD_FW_COMMAND_SELECTOR || selector == UVCD_FW_DATA_SELECTOR ||
	       selector == UVCD_FW_STATUS_SELECTOR;
}

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

/* --------------------------------------------------------------------------
 * Control table -- name<->selector mapping shared with config persistence
 * (uvcd_config.c), the source of the XU GET_MIN/GET_MAX/GET_DEF replies,
 * and the one place a value is clamped (uvcd_ctrl_set) before the camera
 * applies it.
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
		      UVCD_CUSTOM_GAIN_MIN, UVCD_CUSTOM_AGAIN_MAX, 160, max_again),
	UVCD_CTRL_ROW("max-digital-gain", UVCD_CTRL_CUSTOM, UVCD_CUSTOM_MAX_DGAIN,
		      UVCD_CUSTOM_GAIN_MIN, UVCD_CUSTOM_DGAIN_MAX, 80, max_dgain),
	/* Off: in the printer chamber the sensor runs at no gain, so noise is
	 * already low, and denoise erases the fine texture being watched.
	 * Measured there on time-averaged frames, 96/128/192 cost a textured bed
	 * 31/49/63% of its detail and printed line art 6/13/21%, for noise
	 * 1.32 -> 1.09/0.94/0.80. */
	UVCD_CTRL_ROW("spatial-denoise", UVCD_CTRL_CUSTOM, UVCD_CUSTOM_SINTER,
		      0, 255, 0, sinter),
	UVCD_CTRL_ROW("temporal-denoise", UVCD_CTRL_CUSTOM, UVCD_CUSTOM_TEMPER,
		      0, 255, 64, temper),
	UVCD_CTRL_ROW("defective-pixel-correction", UVCD_CTRL_CUSTOM, UVCD_CUSTOM_DPC,
		      0, 255, 128, dpc),
	/* No effect with the shipped sensor tuning (hyb_D), which bypasses
	 * ADR: frames at 0 and 255 measure the same. With ADR on, above 128 it
	 * lifts shadows and the black floor with them. Kept for other
	 * tunings. */
	UVCD_CTRL_ROW("dynamic-range-compression", UVCD_CTRL_CUSTOM, UVCD_CUSTOM_DRC,
		      0, 255, 128, drc),
	/* Not a 128-centred ratio like the knobs above: the SDK takes 0-10,
	 * 0 = off. At the old 0-255/128 it ran flat out, exposing for the
	 * brightest spot and leaving the rest of the picture black. */
	UVCD_CTRL_ROW("highlight-suppression", UVCD_CTRL_CUSTOM, UVCD_CUSTOM_HIGHLIGHT,
		      0, 10, 0, highlight),
	/* Center-weighted: the camera looks at a lit print in a dark chamber.
	 * The tuning's own weights expose for the chamber and blew a white
	 * label in the middle of the view out (37% clipped; 0.9% centred). */
	UVCD_CTRL_ROW("metering", UVCD_CTRL_CUSTOM, UVCD_CUSTOM_METERING,
		      UVCD_METERING_TUNING, UVCD_METERING_SUBJECT, UVCD_METERING_CENTER, metering),
	UVCD_CTRL_ROW("rotation", UVCD_CTRL_CUSTOM, UVCD_CUSTOM_ROTATION,
		      0, 180, 0, rotation, .res = 180),

	/* Encoder. Ranges fit the XU's 16-bit value, hence kbps. */
	UVCD_CTRL_ROW("h264-bitrate-kbps", UVCD_CTRL_CUSTOM, UVCD_CUSTOM_H264_BITRATE,
		      0, 16000, 0, h264_bitrate_kbps),
	UVCD_CTRL_ROW("h264-rate-control", UVCD_CTRL_CUSTOM, UVCD_CUSTOM_H264_RATE_CONTROL,
		      UVCD_H264_RC_CBR, UVCD_H264_RC_CAPPED_VBR, UVCD_H264_RC_CBR, h264_rate_control),
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
#define UVCD_FRAME_TIMEOUT_MS 5000

/* Stream crash guard (uvcd_config.h). The encoder is where a bad setting
 * takes the daemon (or the kernel) down, so a stream is counted in
 * UVCD_CONFIG_DIRTY_PATH from its start, and again from any change made
 * while it runs, until it proves healthy or stops cleanly. A crash leaves
 * the count behind for uvcd_pipeline_init(), which then goes back to the
 * controls the last healthy stream proved (UVCD_CONFIG_GOOD_PATH). Changes
 * outside a stream are the camera-side guard's, which lives in RAM, so an
 * idle camera writes nothing to flash and unplugging it is never taken for
 * a crash. */
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
	    uvcd_monotonic_ms() - g->healthy_since_ms < UVCD_CONFIG_HEALTHY_MS ||
	    uvcd_monotonic_ms() - g->last_frame_ms >= 1000)
		return;
	if (memcmp(&g->pipe->good, &g->pipe->controls, sizeof(g->pipe->good)) != 0) {
		if (uvcd_config_save(UVCD_CONFIG_GOOD_PATH, &g->pipe->controls) != 0)
			LOGW("persist %s: %s", UVCD_CONFIG_GOOD_PATH, strerror(errno));
		else
			g->pipe->good = g->pipe->controls;
	}
	stream_guard_clear(g);
}

/* A change is about to reach the camera: counted before it is applied, so
 * a crash in applying it counts too. Mid-stream it has to prove itself in
 * the stream again; outside one, the camera-side guard watches. */
static void guard_change(gadget_t *g)
{
	if (g->streaming) {
		g->healthy_since_ms = 0;
		stream_guard_arm(g);
	} else {
		uvcd_pipeline_camera_guard_arm(g->pipe);
	}
}

static void config_mark_dirty(gadget_t *g)
{
	g->config_dirty = true;
	g->config_dirty_at = uvcd_monotonic_ms();
}

/* Write pending changes once they have settled, or now if force. */
static int config_flush(gadget_t *g, bool force)
{
	if (!g->config_dirty ||
	    (!force && uvcd_monotonic_ms() - g->config_dirty_at < UVCD_CONFIG_SETTLE_MS))
		return 0;
	/* A dump is copying the flash: keep the change until it is done. */
	if (!force && g->update && uvcd_update_persist_paused(g->update))
		return 0;
	int ret = uvcd_config_save(UVCD_CONFIG_PATH, &g->pipe->controls);
	if (ret != 0)
		/* Don't spin retrying a write that keeps failing (read-only
		 * rootfs, full flash): drop the flag and let the next accepted
		 * control change arm another attempt. */
		LOGW("persist %s: %s", UVCD_CONFIG_PATH, strerror(errno));
	else
		LOGI("controls persisted to %s", UVCD_CONFIG_PATH);
	g->config_dirty = false;
	return ret;
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
static void resync_reader(gadget_t *g)
{
	g->read_seq = 0;
	g->watched_seq = 0;
	g->last_frame_ms = uvcd_monotonic_ms();
	g->healthy_since_ms = 0;
}

static int encoder_restart(gadget_t *g, uint8_t format)
{
	if (g->defer_encoder_restart) {
		if (format == UVCD_FMT_H264)
			g->restart_h264 = true;
		else
			g->restart_mjpeg = true;
		return 0;
	}
	if (!g->pipe->running || g->pipe->cur_format != format)
		return 0; /* inactive format: staged for its next stream */
	bool ok = uvcd_pipeline_restart_encoder(g->pipe, format);
	/* A failed restart can also reset the sequence during recovery. */
	resync_reader(g);
	return ok ? 0 : -EIO;
}

static int encoder_after_store(gadget_t *g, uint8_t selector)
{
	int ret = 0;

	switch (selector) {
	case UVCD_CUSTOM_H264_BITRATE:
		ret = uvcd_enc_apply_bitrate(g->pipe);
		break;
	case UVCD_CUSTOM_H264_RATE_CONTROL:
	case UVCD_CUSTOM_H264_MIN_QP:
	case UVCD_CUSTOM_H264_MAX_QP:
	case UVCD_CUSTOM_H264_PROFILE:
		ret = encoder_restart(g, UVCD_FMT_H264);
		break;
	case UVCD_CUSTOM_MJPEG_QUALITY:
		g->pipe->mjpeg_quality_cut = 0;
		ret = encoder_restart(g, UVCD_FMT_MJPEG);
		break;
	default:
		break;
	}
	return ret;
}

/* MJPEG frames the USB cap can't carry. JPEG size follows sensor noise, so
 * a quality and sharpness that fit in daylight can outgrow the cap at night
 * (1080p at quality 100, sharpness 180, 19x gain: 2.17 MB frames against
 * 2 MB). Every frame is then dropped, the frame watchdog restarts the
 * daemon, and the crash guard falls back to proven controls that may well
 * be the same ones, then to factory settings. Lower this stream's quality
 * instead, 10 at a time; the stored setting stays as it is. */
#define UVCD_TOO_LARGE_RUN 3
#define UVCD_MJPEG_QUALITY_STEP 10
#define UVCD_MJPEG_QUALITY_FLOOR 10

static void frame_size_tick(gadget_t *g)
{
	uvcd_pipeline_t *p = g->pipe;

	if (!g->streaming || !p->running || p->cur_format != UVCD_FMT_MJPEG)
		return;
	pthread_mutex_lock(&p->frame.lock);
	uint32_t run = p->frame.too_large, cap = p->frame.cap;
	pthread_mutex_unlock(&p->frame.lock);
	if (run < UVCD_TOO_LARGE_RUN)
		return;
	int from = p->controls.mjpeg_quality - p->mjpeg_quality_cut;
	if (from <= UVCD_MJPEG_QUALITY_FLOOR)
		return; /* nothing left to give: the frame watchdog takes over */
	int to = from - UVCD_MJPEG_QUALITY_STEP;
	if (to < UVCD_MJPEG_QUALITY_FLOOR)
		to = UVCD_MJPEG_QUALITY_FLOOR;
	p->mjpeg_quality_cut += from - to;
	LOGW("MJPEG frames outgrow the %u-byte cap at quality %d; this stream continues at %d",
	     cap, from, to);
	if (encoder_restart(g, UVCD_FMT_MJPEG) != 0)
		LOGW("encoder restart at quality %d failed", to);
}

int uvcd_ctrl_set(gadget_t *g, const struct uvcd_ctrl_def *def, int value, int *out_applied)
{
	value = uvcd_ctrl_normalize(def, value);

	/* Hosts rewrite values they already have (uvcvideo probes
	 * power-line-frequency on every connect): nothing to persist or
	 * re-prove then. */
	struct uvcd_control_state next = g->pipe->controls;
	int *field = uvcd_ctrl_field(&next, def);
	bool changed = *field != value;
	*field = value;
	if (!uvcd_ctrl_qp_valid(&next)) {
		LOGW("%s=%d would cross H.264 QP bounds", def->name, value);
		return -1;
	}
	if (changed)
		guard_change(g);

	/* Applied before it is stored, so the stored state is only ever what
	 * the camera took. The camera may fill in a dependent value (leaving
	 * an automatic mode keeps what it measured). */
	int ret = uvcd_pipeline_apply(g->pipe, &next, def->state_offset);
	/* A zoom/pan/tilt change re-creates a stream's channels, whose frame
	 * sequence starts again at 0, also when a rejected change restores the
	 * previous view. */
	if (changed && def->kind == UVCD_CTRL_CAMERA &&
	    (def->selector == UVC_CT_ZOOM_ABSOLUTE_CONTROL ||
	     def->selector == UVC_CT_PANTILT_ABSOLUTE_CONTROL)) {
		resync_reader(g);
	}
	if (ret != 0) {
		LOGW("%s=%d rejected (%d)", def->name, value, ret);
		return -1;
	}
	if (memcmp(&next, &g->pipe->controls, sizeof(next)) != 0) {
		struct uvcd_control_state old = g->pipe->controls;
		uint8_t format = g->pipe->cur_format, frame = g->pipe->cur_frame;
		uint32_t interval = g->pipe->cur_interval;
		g->pipe->controls = next;
		ret = def->kind == UVCD_CTRL_CUSTOM
			? encoder_after_store(g, (uint8_t)def->selector) : 0;
		if (ret != 0) {
			g->pipe->controls = old;
			int restore = def->selector == UVCD_CUSTOM_H264_BITRATE
				? uvcd_enc_apply_bitrate(g->pipe)
				: uvcd_pipeline_start(g->pipe, format, frame, interval);
			LOGW("%s=%d rejected by encoder (%d); restoring previous settings (%d)",
			     def->name, value, ret, restore);
			return -1;
		}
		config_mark_dirty(g);
	}
	if (out_applied)
		*out_applied = value;
	return 0;
}

int uvcd_ctrl_keyframe(gadget_t *g)
{
	return uvcd_enc_request_keyframe(g->pipe);
}

int uvcd_ctrl_reset(gadget_t *g)
{
	int ret = 0;
	/* The reset's view wins over one still held back. */
	g->view_pending_set[0] = g->view_pending_set[1] = false;
	g->defer_encoder_restart = true;
	g->restart_h264 = g->restart_mjpeg = false;
	for (size_t i = 0; i < UVCD_CTRL_DEF_COUNT; i++) {
		int applied;
		if (uvcd_ctrl_set(g, &uvcd_ctrl_defs[i], uvcd_ctrl_defs[i].def, &applied) != 0) {
			LOGW("reset: HAL rejected default for %s", uvcd_ctrl_defs[i].name);
			ret = -1;
		}
	}
	g->defer_encoder_restart = false;
	if (g->restart_h264 && encoder_restart(g, UVCD_FMT_H264) != 0)
		ret = -1;
	if (g->restart_mjpeg && encoder_restart(g, UVCD_FMT_MJPEG) != 0)
		ret = -1;

	/* Written at once, even if nothing changed, and the old proven
	 * controls go with it: a crash after a reset must not bring back what
	 * the reset threw away. */
	g->config_dirty = true;
	if (config_flush(g, true) != 0)
		ret = -1;
	if (ret != 0) {
		LOGW("factory reset incomplete; retaining last proven controls");
		return ret;
	}
	if (uvcd_config_save(UVCD_CONFIG_GOOD_PATH, &g->pipe->controls) != 0) {
		LOGW("persist %s: %s", UVCD_CONFIG_GOOD_PATH, strerror(errno));
		return -1;
	}
	g->pipe->good = g->pipe->controls;
	LOGI("factory reset: all controls restored to defaults");
	return 0;
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

/* GET_CUR of a Processing Unit control. Under auto white balance the
 * temperature is the one auto has measured, the way exposure-time reads
 * under auto-exposure; the stored one while the camera has no estimate.
 * Leaving auto starts manual from that value. */
static int current_value(gadget_t *g, uint8_t selector)
{
	int ct;

	if (selector != UVC_PU_WHITE_BALANCE_TEMPERATURE_CONTROL || !g->pipe->controls.wb_auto ||
	    (ct = uvcd_camera_awb_ct(&g->pipe->cam)) <= 0)
		return *control_value(g, selector);
	return ct < UVCD_WB_TEMP_MIN ? UVCD_WB_TEMP_MIN : ct > UVCD_WB_TEMP_MAX ? UVCD_WB_TEMP_MAX : ct;
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
		value = (req->bRequest == UVC_GET_CUR) ? current_value(g, selector) :
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
		/* Without it, uvcvideo reads the value once and caches it. */
		if (selector == UVC_PU_WHITE_BALANCE_TEMPERATURE_CONTROL)
			info |= UVC_CONTROL_CAP_AUTOUPDATE;
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
					int t = uvcd_camera_live_exposure(&g->pipe->cam);
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

static void apply_camera_data(gadget_t *g, uint8_t selector, const struct uvc_request_data *data)
{
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

/* Zoom, pan and tilt re-create a running stream's channels (apply_view):
 * a gap and a visible glitch each, and pan/tilt is two of them. A slider
 * sends a burst of writes, so while a stream runs take at most one view
 * change per UVCD_VIEW_INTERVAL_MS: the first at once, the last one asked
 * for when the interval is up (view_tick). */
#define UVCD_VIEW_INTERVAL_MS 150

static int view_slot(uint8_t selector)
{
	return selector == UVC_CT_ZOOM_ABSOLUTE_CONTROL ? 0
	       : selector == UVC_CT_PANTILT_ABSOLUTE_CONTROL ? 1
							      : -1;
}

static void handle_camera_data(gadget_t *g, const struct uvc_request_data *data)
{
	uint8_t selector = g->last_cs;
	int slot = view_slot(selector);

	if (slot < 0) {
		apply_camera_data(g, selector, data);
		return;
	}
	if (g->streaming && (g->view_pending_set[0] || g->view_pending_set[1] ||
			     uvcd_monotonic_ms() - g->view_applied_ms < UVCD_VIEW_INTERVAL_MS)) {
		g->view_pending[slot] = *data;
		g->view_pending_set[slot] = true;
		return;
	}
	apply_camera_data(g, selector, data);
	g->view_applied_ms = uvcd_monotonic_ms();
}

static void view_tick(gadget_t *g)
{
	static const uint8_t selectors[2] = {UVC_CT_ZOOM_ABSOLUTE_CONTROL,
					     UVC_CT_PANTILT_ABSOLUTE_CONTROL};

	if (!g->view_pending_set[0] && !g->view_pending_set[1])
		return;
	if (g->streaming && uvcd_monotonic_ms() - g->view_applied_ms < UVCD_VIEW_INTERVAL_MS)
		return;
	for (int i = 0; i < 2; i++) {
		if (!g->view_pending_set[i])
			continue;
		g->view_pending_set[i] = false;
		apply_camera_data(g, selectors[i], &g->view_pending[i]);
	}
	g->view_applied_ms = uvcd_monotonic_ms();
}

/* The control interface's own control: only the request error code. */
static void handle_interface_setup(gadget_t *g, const struct usb_ctrlrequest *req,
				   struct uvc_request_data *resp)
{
	uint8_t selector = req->wValue >> 8;
	uint16_t len = req->wLength < 1 ? req->wLength : 1;

	resp->length = -1;
	if (selector != UVC_VC_REQUEST_ERROR_CODE_CONTROL || !(req->bRequestType & USB_DIR_IN))
		return;
	if (req->bRequest == UVC_GET_CUR)
		resp->data[0] = g->request_error;
	else if (req->bRequest == UVC_GET_INFO)
		resp->data[0] = UVC_CONTROL_CAP_GET;
	else
		return;
	resp->length = len;
}

/* Why a request stalled: its unit, its control, or the request itself.
 * The handlers above stall only for those, using the same lookups. */
static uint8_t stall_reason(gadget_t *g, const struct usb_ctrlrequest *req)
{
	uint8_t cs = req->wValue >> 8;
	bool known;

	if ((req->wIndex & 0xff) != UVC_INTF_CONTROL)
		known = cs == UVC_VS_PROBE_CONTROL || cs == UVC_VS_COMMIT_CONTROL;
	else
		switch (req->wIndex >> 8) {
		case 0:
			known = cs == UVC_VC_REQUEST_ERROR_CODE_CONTROL;
			break;
		case UVCD_CUSTOM_UNIT_ID:
			known = diagnostic_selector(cs) ? true
				: firmware_selector(cs)
				    ? g->update != NULL
				    : cs == UVCD_CUSTOM_RESET || cs == UVCD_CUSTOM_H264_KEYFRAME ||
					  custom_control_value(g, cs);
			break;
		case UVCD_PROCESSING_UNIT_ID:
			known = standard_ctrl_def(cs) && control_value(g, cs);
			break;
		case UVCD_CAMERA_TERMINAL_ID:
			known = camera_control_length(cs) && ctrl_def_part(UVCD_CTRL_CAMERA, cs, 0);
			break;
		default:
			return UVCD_REQUEST_ERROR_INVALID_UNIT;
		}
	return known ? UVCD_REQUEST_ERROR_INVALID_REQUEST : UVCD_REQUEST_ERROR_INVALID_CONTROL;
}

static void handle_diag_setup(gadget_t *g, const struct usb_ctrlrequest *req,
			      struct uvc_request_data *resp)
{
	unsigned s = req->wValue >> 8;
	bool latch = s == UVCD_DIAG_LATCH;
	resp->length = -1;
	if (!(req->bRequestType & USB_DIR_IN)) {
		if (latch && req->bRequest == UVC_SET_CUR && req->wLength == 1) {
			g->last_cs = s;
			g->last_unit = UVCD_CUSTOM_UNIT_ID;
			g->last_intf = UVC_INTF_CONTROL;
			g->last_request = UVC_SET_CUR;
			resp->length = 1;
		}
		return;
	}
	memset(resp->data, 0, sizeof(resp->data));
	switch (req->bRequest) {
	case UVC_GET_INFO:
		resp->data[0] = UVC_CONTROL_CAP_GET | (latch ? UVC_CONTROL_CAP_SET : 0);
		resp->length = req->wLength < 1 ? req->wLength : 1;
		break;
	case UVC_GET_LEN:
		resp->data[0] = latch ? 1 : UVCD_DIAG_PAGE_SIZE;
		resp->length = req->wLength < 2 ? req->wLength : 2;
		break;
	case UVC_GET_CUR:
		/* A short page must never advance the cursor. */
		if (req->wLength != (latch ? 1 : UVCD_DIAG_PAGE_SIZE))
			return;
		resp->length = req->wLength;
		if (latch) {
			resp->data[0] = UVCD_DIAG_VERSION;
		} else if (g->diag_latched) {
			unsigned offset = g->diag_page * UVCD_DIAG_PAYLOAD;
			unsigned n = UVCD_DIAG_SIZE - offset;
			if (n > UVCD_DIAG_PAYLOAD)
				n = UVCD_DIAG_PAYLOAD;
			resp->data[0] = g->diag_page;
			resp->data[1] = UVCD_DIAG_PAGES;
			memcpy(resp->data + 2, g->diag + 2, 2);
			memcpy(resp->data + 4, g->diag + offset, n);
			g->diag_page = (g->diag_page + 1) % UVCD_DIAG_PAGES;
		}
		break;
	}
}

static void handle_firmware_setup(gadget_t *g, const struct usb_ctrlrequest *req,
				  struct uvc_request_data *resp)
{
	uint8_t selector = req->wValue >> 8;
	bool in = req->bRequestType & USB_DIR_IN;

	resp->length = g->update ? uvcd_update_setup(g->update, req->bRequest, selector, in,
						     req->wLength, resp->data)
				 : -1;
	if (!in && resp->length >= 0) {
		g->last_cs = selector;
		g->last_intf = UVC_INTF_CONTROL;
		g->last_unit = UVCD_CUSTOM_UNIT_ID;
		g->last_request = req->bRequest;
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
			if (diagnostic_selector(cs))
				handle_diag_setup(g, req, &resp);
			else if (firmware_selector(cs))
				handle_firmware_setup(g, req, &resp);
			else
				handle_custom_setup(g, req, &resp);
			break;
		case UVCD_PROCESSING_UNIT_ID:
			handle_control_setup(g, req, &resp);
			break;
		case UVCD_CAMERA_TERMINAL_ID:
			handle_camera_setup(g, req, &resp);
			break;
		case 0:
			handle_interface_setup(g, req, &resp);
			break;
		default:
			/* The output terminal: nothing here. */
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
	/* After the reply is built: reading the error code clears it too. */
	g->request_error = resp.length < 0 ? stall_reason(g, req) : UVCD_REQUEST_ERROR_NONE;
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
		if (g->last_unit == UVCD_CUSTOM_UNIT_ID && diagnostic_selector(g->last_cs)) {
			if (g->last_cs == UVCD_DIAG_LATCH && data->length == 1) {
				uvcd_ctrl_diag(g, g->diag);
				g->diag_latched = true;
				g->diag_page = 0;
			}
			return;
		}
		if (g->last_unit == UVCD_CUSTOM_UNIT_ID && firmware_selector(g->last_cs)) {
			if (g->update)
				uvcd_update_data(g->update, g->last_cs, data->data, data->length);
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
		LOGI("COMMIT received, starting the stream");
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

	/* Lease the camera and create the stream for exactly what was
	 * negotiated. */
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
	g->last_frame_ms = uvcd_monotonic_ms();
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

	/* The stream's channels go now; the camera stays up, back at idle. */
	uvcd_pipeline_stop(g->pipe);
	g->pipe->mjpeg_quality_cut = 0;
	/* Only once teardown is over: it can hang after an ISP failure. */
	stream_guard_clear(g);
	LOGI("streaming stopped");
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
			if (g->update)
				uvcd_update_disconnect(g->update);
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
			g->stalled_since_ms = uvcd_monotonic_ms();
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
		g->healthy_since_ms = uvcd_monotonic_ms();
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

/* --------------------------------------------------------------------------
 * Trial self-test. A trial boot of a new release is confirmed only once it
 * has shown it can run the camera: ISP frames through the H.264 and then
 * the MJPEG encoder, at a small size, with no host involved. A host stream
 * that runs healthily for a while counts as the same proof. Failing that,
 * the trial stays pending and k1cam-trial-gate reboots, which counts it.
 */
#define SELFTEST_OFF 0
#define SELFTEST_H264 1
#define SELFTEST_MJPEG 2
#define SELFTEST_FRAMES 5
#define SELFTEST_STEP_MS 5000
#define SELFTEST_TRIES 3
#define SELFTEST_HOST_MS 3000

static bool selftest_frame_valid(int step, const uint8_t *d, uint32_t len)
{
	if (step == SELFTEST_H264)
		return len > 4 && d[0] == 0 && d[1] == 0 &&
		       (d[2] == 1 || (d[2] == 0 && d[3] == 1));
	return len > 4 && d[0] == 0xff && d[1] == 0xd8 && d[len - 2] == 0xff && d[len - 1] == 0xd9;
}

static void selftest_start(gadget_t *g, int step)
{
	g->selftest.step = step;
	g->selftest.since_ms = uvcd_monotonic_ms();
	g->selftest.frames = 0;
	g->selftest.key = false;
	g->selftest.seen_seq = 0;
	if (uvcd_pipeline_start(g->pipe, step == SELFTEST_H264 ? UVCD_FMT_H264 : UVCD_FMT_MJPEG,
				UVCD_FRAME_360P, UVCD_INTERVAL_30FPS) != 0)
		LOGW("trial self-test: %s stream did not start",
		     step == SELFTEST_H264 ? "H.264" : "MJPEG");
}

static void selftest_tick(gadget_t *g)
{
	int64_t now = uvcd_monotonic_ms();

	if (!g->update)
		return;
	if (g->streaming) {
		/* The host has the camera (start_streaming replaced any test
		 * stream). Its frames are the proof, or the test resumes after. */
		g->selftest.step = SELFTEST_OFF;
		if (g->healthy_since_ms && now - g->healthy_since_ms >= SELFTEST_HOST_MS &&
		    uvcd_update_trial_wants_check(g->update))
			uvcd_update_trial_health(g->update, true, "host stream delivered frames");
		return;
	}
	if (g->selftest.step == SELFTEST_OFF) {
		if (uvcd_update_trial_wants_check(g->update)) {
			LOGI("trial boot: testing the camera before confirming this release");
			g->selftest.tries = 0;
			selftest_start(g, SELFTEST_H264);
		}
		return;
	}

	uvcd_pipeline_t *p = g->pipe;
	pthread_mutex_lock(&p->frame.lock);
	if (p->frame.seq != g->selftest.seen_seq) {
		g->selftest.seen_seq = p->frame.seq;
		if (selftest_frame_valid(g->selftest.step, p->frame.data, p->frame.len)) {
			g->selftest.frames++;
			g->selftest.key |= p->frame.is_key;
		}
	}
	pthread_mutex_unlock(&p->frame.lock);

	if (g->selftest.frames >= SELFTEST_FRAMES &&
	    (g->selftest.step != SELFTEST_H264 || g->selftest.key)) {
		uvcd_pipeline_stop(p);
		if (g->selftest.step == SELFTEST_H264) {
			selftest_start(g, SELFTEST_MJPEG);
		} else {
			g->selftest.step = SELFTEST_OFF;
			uvcd_update_trial_health(g->update, true, "H.264 and MJPEG frames encoded");
		}
		return;
	}
	if (now - g->selftest.since_ms < SELFTEST_STEP_MS)
		return;
	uvcd_pipeline_stop(p);
	if (++g->selftest.tries < SELFTEST_TRIES) {
		selftest_start(g, g->selftest.step);
		return;
	}
	g->selftest.step = SELFTEST_OFF;
	uvcd_update_trial_health(g->update, false,
				 g->selftest.frames ? "the encoder produced too few valid frames"
						    : "no encoded frames from the camera");
}

extern volatile sig_atomic_t uvcd_running;

int uvcd_gadget_run(uvcd_pipeline_t *p, const char *device)
{
	gadget_t g = {0};
	g.pipe = p;
	g.device = device;

	if (gadget_open(&g) < 0)
		return -1;
	g.update = uvcd_update_create(NULL);
	if (!g.update)
		LOGW("firmware updates unavailable: out of memory");
	int ctl_fd = uvcd_ctl_listen();
	if (ctl_fd < 0)
		LOGW("control socket %s: %s", UVCD_CTL_PATH, strerror(errno));

	while (uvcd_running) {
		struct pollfd pfd[2] = {
			{.fd = g.fd, .events = POLLPRI},
			{.fd = ctl_fd, .events = POLLIN},
		};
		int timeout = g.streaming ? 5 : g.selftest.step != SELFTEST_OFF ? 50 : 500;
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
				g.last_frame_ms = uvcd_monotonic_ms();
			}
			if (uvcd_monotonic_ms() - g.last_frame_ms >= UVCD_FRAME_TIMEOUT_MS) {
				LOGE("no encoded frames for %d ms; restarting daemon",
				     UVCD_FRAME_TIMEOUT_MS);
				/* The SDK can hang during teardown after an ISP failure.
				 * Leave the dirty marker and let the supervisor disconnect. */
				_exit(1);
			}
			deliver_frame(&g);
			frame_size_tick(&g);
		} else {
			g.healthy_since_ms = 0;
		}

		view_tick(&g);
		stream_guard_tick(&g);
		config_flush(&g, false);
		uvcd_pipeline_tick(p);
		selftest_tick(&g);
		if (g.update)
			uvcd_update_tick(g.update);
	}

	unlink("/var/run/uvcd.ready");
	uvcd_ctl_close(ctl_fd);
	stop_streaming(&g);
	if (g.selftest.step != SELFTEST_OFF)
		uvcd_pipeline_stop(p);
	uvcd_update_destroy(g.update);
	config_flush(&g, true);
	if (g.fd >= 0)
		close(g.fd);
	return uvcd_running ? -1 : 0;
}
