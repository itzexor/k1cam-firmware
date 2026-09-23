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
#include <linux/videodev2.h>
#include <linux/usb/ch9.h>
#include <linux/usb/video.h>

#include "uvcd_gadget.h"
#include "uvcd_config.h"

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
#define UVCD_CUSTOM_UNIT_ID 4
#define UVCD_CUSTOM_MAX_AGAIN 1
#define UVCD_CUSTOM_MAX_DGAIN 2
#define UVCD_CUSTOM_AE_COMP 3
#define UVCD_CUSTOM_SINTER 4
#define UVCD_CUSTOM_TEMPER 5
#define UVCD_CUSTOM_DPC 6
#define UVCD_CUSTOM_DRC 7
#define UVCD_CUSTOM_DEFOG 8
#define UVCD_CUSTOM_HIGHLIGHT 9
/* Action control, not a setting: SET_CUR of a non-zero value restores every
 * control (standard and custom alike) to its compiled-in factory default.
 * GET_CUR always reads back 0 -- there is no state here to report. */
#define UVCD_CUSTOM_RESET 10
/* Standard V4L2 controls that UVC never standardized -- the Processing Unit
 * is modeled on an analog proc-amp, and image orientation isn't a proc-amp
 * function -- so like every other UVC vendor we carry them in the XU. */
#define UVCD_CUSTOM_HFLIP 11
#define UVCD_CUSTOM_VFLIP 12
#define UVCD_CUSTOM_GAIN_MIN 0
#define UVCD_CUSTOM_GAIN_MAX 160
#define UVCD_FLIP_MIN 0
#define UVCD_FLIP_MAX 1
#define UVCD_FLIP_DEF 0

#define UVCD_CONTROL_MIN 0
#define UVCD_CONTROL_MAX 255
#define UVCD_CONTROL_DEF 128
#define UVCD_HUE_MIN -128
#define UVCD_HUE_MAX 127
#define UVCD_HUE_DEF 0
#define UVCD_BACKLIGHT_MIN 0
#define UVCD_BACKLIGHT_MAX 10
#define UVCD_BACKLIGHT_DEF 0
#define UVCD_POWER_LINE_MIN 0
#define UVCD_POWER_LINE_MAX 2
#define UVCD_POWER_LINE_DEF 1

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
	bool streaming;
	int eagain_count;

	uint8_t cur_format;
	uint8_t cur_frame;
	uint32_t cur_interval;
	struct uvc_streaming_control probe;
	struct uvc_streaming_control commit;
	struct uvcd_control_state controls;
	uint8_t last_cs;
	uint8_t last_intf;
	uint8_t last_request;

	/* Controls are persistent by default -- there is no explicit SAVE.
	 * Every accepted change marks the shadow dirty; the poll loop writes
	 * it out once writes have settled, so dragging a slider costs one
	 * flash write per burst rather than one per tick. */
	bool config_dirty;
	time_t config_dirty_at;

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

	if (interval <= UVCD_INTERVAL_30FPS)
		interval = UVCD_INTERVAL_30FPS;
	else if (interval <= UVCD_INTERVAL_25FPS)
		interval = UVCD_INTERVAL_25FPS;
	else if (interval <= UVCD_INTERVAL_15FPS)
		interval = UVCD_INTERVAL_15FPS;
	else
		interval = UVCD_INTERVAL_15FPS;
	ctrl->dwFrameInterval = interval;

	ctrl->dwMaxVideoFrameSize = uvcd_frames[frame_idx].max_size;
	ctrl->dwMaxPayloadTransferSize = 64 * 1024;
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
		return &g->controls.hue;
	case UVC_PU_BRIGHTNESS_CONTROL:
		return &g->controls.brightness;
	case UVC_PU_CONTRAST_CONTROL:
		return &g->controls.contrast;
	case UVC_PU_SATURATION_CONTROL:
		return &g->controls.saturation;
	case UVC_PU_SHARPNESS_CONTROL:
		return &g->controls.sharpness;
	case UVC_PU_BACKLIGHT_COMPENSATION_CONTROL:
		return &g->controls.backlight;
	case UVC_PU_POWER_LINE_FREQUENCY_CONTROL:
		return &g->controls.power_line_frequency;
	case UVC_PU_GAMMA_CONTROL:
		return &g->controls.gamma;
	case UVC_PU_WHITE_BALANCE_TEMPERATURE_CONTROL:
		return &g->controls.wb_temp;
	case UVC_PU_WHITE_BALANCE_TEMPERATURE_AUTO_CONTROL:
		return &g->controls.wb_auto;
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
		if (value > UVCD_CONTROL_MAX)
			value = UVCD_CONTROL_MAX;
		return RSS_HAL_CALL(g->pipe->ops, isp_set_sharpness, g->pipe->hal_ctx, value);
	case UVC_PU_BACKLIGHT_COMPENSATION_CONTROL:
		if (value < UVCD_BACKLIGHT_MIN)
			value = UVCD_BACKLIGHT_MIN;
		if (value > UVCD_BACKLIGHT_MAX)
			value = UVCD_BACKLIGHT_MAX;
		return RSS_HAL_CALL(g->pipe->ops, isp_set_backlight_comp, g->pipe->hal_ctx, value);
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
		return uvcd_apply_wb(g->pipe, value, g->controls.wb_auto);
	case UVC_PU_WHITE_BALANCE_TEMPERATURE_AUTO_CONTROL:
		value = value ? 1 : 0;
		return uvcd_apply_wb(g->pipe, g->controls.wb_temp, value);
	default:
		return -EINVAL;
	}
}

static int *custom_control_value(gadget_t *g, uint8_t selector)
{
	switch (selector) {
	case UVCD_CUSTOM_MAX_AGAIN:
		return &g->controls.max_again;
	case UVCD_CUSTOM_MAX_DGAIN:
		return &g->controls.max_dgain;
	case UVCD_CUSTOM_AE_COMP:
		return &g->controls.ae_comp;
	case UVCD_CUSTOM_SINTER:
		return &g->controls.sinter;
	case UVCD_CUSTOM_TEMPER:
		return &g->controls.temper;
	case UVCD_CUSTOM_DPC:
		return &g->controls.dpc;
	case UVCD_CUSTOM_DRC:
		return &g->controls.drc;
	case UVCD_CUSTOM_DEFOG:
		return &g->controls.defog;
	case UVCD_CUSTOM_HIGHLIGHT:
		return &g->controls.highlight;
	case UVCD_CUSTOM_HFLIP:
		return &g->controls.hflip;
	case UVCD_CUSTOM_VFLIP:
		return &g->controls.vflip;
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
	case UVCD_CUSTOM_DEFOG:
		return RSS_HAL_CALL(g->pipe->ops, isp_set_defog_strength, g->pipe->hal_ctx, value);
	case UVCD_CUSTOM_HIGHLIGHT:
		return RSS_HAL_CALL(g->pipe->ops, isp_set_highlight_depress, g->pipe->hal_ctx, value);
	case UVCD_CUSTOM_HFLIP:
		return RSS_HAL_CALL(g->pipe->ops, isp_set_hflip, g->pipe->hal_ctx, value ? 1 : 0);
	case UVCD_CUSTOM_VFLIP:
		return RSS_HAL_CALL(g->pipe->ops, isp_set_vflip, g->pipe->hal_ctx, value ? 1 : 0);
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
static const struct uvcd_ctrl_def uvcd_ctrl_defs[] = {
	{"brightness", UVCD_CTRL_STANDARD, UVC_PU_BRIGHTNESS_CONTROL, UVCD_CONTROL_MIN,
	 UVCD_CONTROL_MAX, UVCD_CONTROL_DEF, offsetof(struct uvcd_control_state, brightness)},
	{"contrast", UVCD_CTRL_STANDARD, UVC_PU_CONTRAST_CONTROL, UVCD_CONTROL_MIN,
	 UVCD_CONTROL_MAX, UVCD_CONTROL_DEF, offsetof(struct uvcd_control_state, contrast)},
	{"saturation", UVCD_CTRL_STANDARD, UVC_PU_SATURATION_CONTROL, UVCD_CONTROL_MIN,
	 UVCD_CONTROL_MAX, UVCD_CONTROL_DEF, offsetof(struct uvcd_control_state, saturation)},
	{"sharpness", UVCD_CTRL_STANDARD, UVC_PU_SHARPNESS_CONTROL, UVCD_CONTROL_MIN,
	 UVCD_CONTROL_MAX, UVCD_CONTROL_DEF, offsetof(struct uvcd_control_state, sharpness)},
	{"hue", UVCD_CTRL_STANDARD, UVC_PU_HUE_CONTROL, UVCD_HUE_MIN, UVCD_HUE_MAX, UVCD_HUE_DEF,
	 offsetof(struct uvcd_control_state, hue)},
	{"backlight", UVCD_CTRL_STANDARD, UVC_PU_BACKLIGHT_COMPENSATION_CONTROL,
	 UVCD_BACKLIGHT_MIN, UVCD_BACKLIGHT_MAX, UVCD_BACKLIGHT_DEF,
	 offsetof(struct uvcd_control_state, backlight)},
	{"power-line-frequency", UVCD_CTRL_STANDARD, UVC_PU_POWER_LINE_FREQUENCY_CONTROL,
	 UVCD_POWER_LINE_MIN, UVCD_POWER_LINE_MAX, UVCD_POWER_LINE_DEF,
	 offsetof(struct uvcd_control_state, power_line_frequency)},
	{"gamma", UVCD_CTRL_STANDARD, UVC_PU_GAMMA_CONTROL, UVCD_GAMMA_MIN, UVCD_GAMMA_MAX,
	 UVCD_GAMMA_DEF, offsetof(struct uvcd_control_state, gamma)},
	{"white-balance-temperature", UVCD_CTRL_STANDARD,
	 UVC_PU_WHITE_BALANCE_TEMPERATURE_CONTROL, UVCD_WB_TEMP_MIN, UVCD_WB_TEMP_MAX,
	 UVCD_WB_TEMP_DEF, offsetof(struct uvcd_control_state, wb_temp)},
	{"white-balance-auto", UVCD_CTRL_STANDARD,
	 UVC_PU_WHITE_BALANCE_TEMPERATURE_AUTO_CONTROL, 0, 1, UVCD_WB_AUTO_DEF,
	 offsetof(struct uvcd_control_state, wb_auto)},
	{"hflip", UVCD_CTRL_CUSTOM, UVCD_CUSTOM_HFLIP, UVCD_FLIP_MIN, UVCD_FLIP_MAX,
	 UVCD_FLIP_DEF, offsetof(struct uvcd_control_state, hflip)},
	{"vflip", UVCD_CTRL_CUSTOM, UVCD_CUSTOM_VFLIP, UVCD_FLIP_MIN, UVCD_FLIP_MAX,
	 UVCD_FLIP_DEF, offsetof(struct uvcd_control_state, vflip)},
	{"max-again", UVCD_CTRL_CUSTOM, UVCD_CUSTOM_MAX_AGAIN, UVCD_CUSTOM_GAIN_MIN,
	 UVCD_CUSTOM_GAIN_MAX, 160, offsetof(struct uvcd_control_state, max_again)},
	{"max-dgain", UVCD_CTRL_CUSTOM, UVCD_CUSTOM_MAX_DGAIN, UVCD_CUSTOM_GAIN_MIN,
	 UVCD_CUSTOM_GAIN_MAX, 80, offsetof(struct uvcd_control_state, max_dgain)},
	{"ae-comp", UVCD_CTRL_CUSTOM, UVCD_CUSTOM_AE_COMP, 0, 255, 128,
	 offsetof(struct uvcd_control_state, ae_comp)},
	{"sinter", UVCD_CTRL_CUSTOM, UVCD_CUSTOM_SINTER, 0, 255, 128,
	 offsetof(struct uvcd_control_state, sinter)},
	{"temper", UVCD_CTRL_CUSTOM, UVCD_CUSTOM_TEMPER, 0, 255, 128,
	 offsetof(struct uvcd_control_state, temper)},
	{"dpc", UVCD_CTRL_CUSTOM, UVCD_CUSTOM_DPC, 0, 255, 128,
	 offsetof(struct uvcd_control_state, dpc)},
	{"drc", UVCD_CTRL_CUSTOM, UVCD_CUSTOM_DRC, 0, 255, 128,
	 offsetof(struct uvcd_control_state, drc)},
	{"defog", UVCD_CTRL_CUSTOM, UVCD_CUSTOM_DEFOG, 0, 255, 128,
	 offsetof(struct uvcd_control_state, defog)},
	{"highlight", UVCD_CTRL_CUSTOM, UVCD_CUSTOM_HIGHLIGHT, 0, 255, 128,
	 offsetof(struct uvcd_control_state, highlight)},
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

static const struct uvcd_ctrl_def *ctrl_def_by_selector(uvcd_ctrl_kind_t kind, uint8_t selector)
{
	for (size_t i = 0; i < UVCD_CTRL_DEF_COUNT; i++) {
		if (uvcd_ctrl_defs[i].kind == kind && uvcd_ctrl_defs[i].selector == selector)
			return &uvcd_ctrl_defs[i];
	}
	return NULL;
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
 * reboot, so the write-back is the daemon's job, not the caller's.
 */
#define UVCD_CONFIG_SETTLE_SECS 2

static void config_mark_dirty(gadget_t *g)
{
	g->config_dirty = true;
	g->config_dirty_at = time(NULL);
}

/* Write the shadow out once no further change has arrived for a moment.
 * force=true flushes immediately regardless (daemon shutdown). */
static void config_flush(gadget_t *g, bool force)
{
	if (!g->config_dirty)
		return;
	if (!force && time(NULL) - g->config_dirty_at < UVCD_CONFIG_SETTLE_SECS)
		return;

	if (uvcd_config_save(UVCD_CONFIG_PATH, &g->controls) != 0) {
		LOGW("persist %s: %s", UVCD_CONFIG_PATH, strerror(errno));
		/* Don't spin retrying a write that keeps failing (read-only
		 * rootfs, full flash): drop the flag and let the next accepted
		 * control change arm another attempt. */
	} else {
		LOGI("controls persisted to %s", UVCD_CONFIG_PATH);
	}
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
	return *uvcd_ctrl_field(&g->controls, def);
}

int uvcd_ctrl_set(gadget_t *g, const struct uvcd_ctrl_def *def, int value, int *out_applied)
{
	if (value < def->min)
		value = def->min;
	if (value > def->max)
		value = def->max;

	int ret = (def->kind == UVCD_CTRL_STANDARD) ? apply_control(g, def->selector, value)
						     : apply_custom_control(g, def->selector, value);
	if (ret != 0)
		return -1;

	*uvcd_ctrl_field(&g->controls, def) = value;
	config_mark_dirty(g);
	if (out_applied)
		*out_applied = value;
	return 0;
}

void uvcd_ctrl_reset(gadget_t *g)
{
	for (size_t i = 0; i < UVCD_CTRL_DEF_COUNT; i++) {
		int applied;
		if (uvcd_ctrl_set(g, &uvcd_ctrl_defs[i], uvcd_ctrl_defs[i].def, &applied) != 0)
			LOGW("reset: HAL rejected default for %s", uvcd_ctrl_defs[i].name);
	}
	/* A factory reset is deliberate and rare -- don't let it sit in the
	 * settle window where a power cut could lose it. */
	config_flush(g, true);
	LOGI("factory reset: all controls restored to defaults");
}

static void handle_custom_setup(gadget_t *g, const struct usb_ctrlrequest *req,
				struct uvc_request_data *resp)
{
	uint8_t selector = req->wValue >> 8;
	int16_t value;
	uint16_t len;
	bool is_action = (selector == UVCD_CUSTOM_RESET);
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
		g->last_intf = UVCD_CUSTOM_UNIT_ID;
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
				req->bRequest == UVC_GET_DEF ? custom_control_def(selector) : 1;
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
	LOGI("UVC SETUP type=%02x req=%02x value=%04x index=%04x length=%u",
	     type, request, req->wValue, req->wIndex, wLength);

	if (intf == UVC_INTF_CONTROL && unit == UVCD_CUSTOM_UNIT_ID) {
		handle_custom_setup(g, req, &resp);
		goto send;
	}

	if (intf == UVC_INTF_CONTROL) {
		handle_control_setup(g, req, &resp);
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
					       UVCD_INTERVAL_15FPS);
			memcpy(resp.data, &sc, len);
			resp.length = len;
			break;
		case UVC_GET_MAX:
			fill_streaming_control(&sc, UVCD_FMT_H264, UVCD_FRAME_1080P,
					       UVCD_INTERVAL_30FPS);
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
		LOGI("UVC OUT request=%02x cs=%u awaiting DATA", request, cs);
	}

send:
	LOGI("UVC RESPONSE length=%d", resp.length);
	if (ioctl(g->fd, UVCIOC_SEND_RESPONSE, &resp) < 0) {
		LOGW("UVCIOC_SEND_RESPONSE: %s", strerror(errno));
		return;
	}
	LOGI("UVC RESPONSE sent");
}

static void handle_data_event(gadget_t *g, const struct uvc_request_data *data)
{
	if (g->last_intf == UVCD_CUSTOM_UNIT_ID && g->last_request == UVC_SET_CUR &&
	    g->last_cs == UVCD_CUSTOM_RESET) {
		int16_t value = 0;

		if (data->length >= (int32_t)sizeof(value))
			memcpy(&value, data->data, sizeof(value));
		else if (data->length >= 1)
			value = data->data[0];

		if (value == 0) {
			LOGI("factory reset requested with value 0, ignoring");
			return;
		}
		uvcd_ctrl_reset(g);
		return;
	}

	if (g->last_intf == UVCD_CUSTOM_UNIT_ID && g->last_request == UVC_SET_CUR) {
		int16_t value;
		int *current = custom_control_value(g, g->last_cs);

		if (current == NULL || data->length < (int32_t)sizeof(value)) {
			LOGW("invalid custom control selector=%u length=%d", g->last_cs,
			     data->length);
			return;
		}
		memcpy(&value, data->data, sizeof(value));
		if (apply_custom_control(g, g->last_cs, value) != 0) {
			LOGW("HAL rejected custom control selector=%u value=%d", g->last_cs, value);
			return;
		}
		if (value < custom_control_min(g->last_cs))
			value = custom_control_min(g->last_cs);
		if (value > custom_control_max(g->last_cs))
			value = custom_control_max(g->last_cs);
		*current = value;
		config_mark_dirty(g);
		LOGI("custom control selector=%u value=%d", g->last_cs, value);
		return;
	}

	if (g->last_intf == UVC_INTF_CONTROL && g->last_request == UVC_SET_CUR) {
		int16_t value;
		int *current = control_value(g, g->last_cs);
		const struct uvcd_ctrl_def *cdef = standard_ctrl_def(g->last_cs);
		int32_t need = (int32_t)control_length(g->last_cs);

		if (current == NULL || cdef == NULL || data->length < need) {
			LOGW("invalid UVC control data selector=%u length=%d", g->last_cs,
			     data->length);
			return;
		}
		if (need == 1)
			value = data->data[0];
		else
			memcpy(&value, data->data, sizeof(value));

		if (apply_control(g, g->last_cs, value) != 0) {
			LOGW("HAL rejected UVC control selector=%u value=%d", g->last_cs, value);
			return;
		}
		if (value < cdef->min)
			value = cdef->min;
		if (value > cdef->max)
			value = cdef->max;
		*current = value;
		config_mark_dirty(g);
		LOGI("UVC control selector=%u value=%d", g->last_cs, value);
		return;
	}

	struct uvc_streaming_control proposed;
	struct uvc_streaming_control *target =
		(g->last_cs == UVC_VS_COMMIT_CONTROL) ? &g->commit : &g->probe;

	memcpy(&proposed, data->data, sizeof(proposed));
	LOGI("UVC DATA format=%u frame=%u interval=%u", proposed.bFormatIndex,
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
	if (uvcd_pipeline_start(g->pipe, g->cur_format, g->cur_frame, g->cur_interval) != 0) {
		LOGE("pipeline start failed for fmt=%u frame=%u", g->cur_format, g->cur_frame);
		return -1;
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

		buf.bytesused = 1;
		if (ioctl(g->fd, VIDIOC_QBUF, &buf) < 0) {
			LOGE("VIDIOC_QBUF %d: %s", i, strerror(errno));
			goto err_unmap;
		}
	}

	int type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	if (ioctl(g->fd, VIDIOC_STREAMON, &type) < 0) {
		LOGE("VIDIOC_STREAMON: %s", strerror(errno));
		goto err_unmap;
	}

	g->eagain_count = 0;
	g->streaming = true;

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

	/* True on-demand: no host means no ISP/encoder work at all */
	uvcd_pipeline_stop(g->pipe);
	LOGI("streaming stopped, pipeline released");
}

static void process_events(gadget_t *g)
{
	struct v4l2_event ev;

	while (ioctl(g->fd, VIDIOC_DQEVENT, &ev) == 0) {
		struct uvc_event *uvc_ev = (struct uvc_event *)&ev.u.data;
		LOGI("UVC EVENT type=%u", ev.type);

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
	if (errno != EAGAIN)
		LOGW("VIDIOC_DQEVENT: %s", strerror(errno));
}

static void deliver_frame(gadget_t *g)
{
	struct v4l2_buffer buf;
	uint64_t wseq;
	uint32_t len;

	pthread_mutex_lock(&g->pipe->frame.lock);
	wseq = g->pipe->frame.seq;
	if (g->read_seq >= wseq) {
		pthread_mutex_unlock(&g->pipe->frame.lock);
		return;
	}
	pthread_mutex_unlock(&g->pipe->frame.lock);

	memset(&buf, 0, sizeof(buf));
	buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	buf.memory = V4L2_MEMORY_MMAP;

	if (ioctl(g->fd, VIDIOC_DQBUF, &buf) < 0) {
		if (errno == EAGAIN) {
			if (++g->eagain_count >= 100) {
				LOGI("no host consumer, stopping streaming");
				stop_streaming(g);
			}
		}
		return;
	}
	g->eagain_count = 0;

	if (buf.index >= (unsigned)g->buf_count) {
		ioctl(g->fd, VIDIOC_QBUF, &buf);
		return;
	}

	pthread_mutex_lock(&g->pipe->frame.lock);
	wseq = g->pipe->frame.seq;
	if (g->read_seq >= wseq) {
		pthread_mutex_unlock(&g->pipe->frame.lock);
		LOGW("frame disappeared after DQBUF index=%u", buf.index);
		ioctl(g->fd, VIDIOC_QBUF, &buf);
		return;
	}

	len = g->pipe->frame.len;
	LOGI("delivering frame seq=%llu len=%u format=%u frame=%u",
	     (unsigned long long)wseq, len, g->cur_format, g->cur_frame);
	if (len > g->buffers[buf.index].length)
		len = (uint32_t)g->buffers[buf.index].length;
	memcpy(g->buffers[buf.index].start, g->pipe->frame.data, len);
	g->read_seq = wseq;
	pthread_mutex_unlock(&g->pipe->frame.lock);

	buf.bytesused = len;
	if (ioctl(g->fd, VIDIOC_QBUF, &buf) < 0)
		LOGW("VIDIOC_QBUF: %s", strerror(errno));
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
		LOGI("subscribing UVC event type=%u", sub.type);
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
	/* uvcd_pipeline_init() already loaded these from UVCD_CONFIG_PATH (or
	 * compiled-in defaults) and pushed them to the HAL -- reuse the same
	 * values here instead of a second hardcoded copy. */
	g->controls = g->pipe->controls;
	g->buf_count = UVCD_MAX_BUFFERS;

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

	while (uvcd_running) {
		struct pollfd pfd;
		pfd = (struct pollfd){.fd = g.fd, .events = POLLPRI};
		int timeout = g.streaming ? 5 : 500;
		int n = poll(&pfd, 1, timeout);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			LOGE("poll: %s", strerror(errno));
			break;
		}
		if (n > 0)
			LOGI("UVC poll revents=%04x", pfd.revents);

		if (pfd.revents & POLLPRI)
			process_events(&g);

		if (g.streaming)
			deliver_frame(&g);

		config_flush(&g, false);
	}

	stop_streaming(&g);
	config_flush(&g, true);
	if (g.fd >= 0)
		close(g.fd);
	return 0;
}
