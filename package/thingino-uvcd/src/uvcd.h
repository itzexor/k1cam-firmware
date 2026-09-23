/*
 * uvcd.h -- minimal fixed-function ISP -> UVC webcam daemon
 *
 * Single process, single sensor (GC2083 on T31), single active video
 * pipeline. No SHM rings, no IPC, no config file, no control socket --
 * state transitions are driven only by standard UVC PROBE/COMMIT/
 * STREAMON/STREAMOFF requests from the USB host. When no host is
 * attached, the framesource and encoder channels are fully torn down
 * (not just idle): no ISP/encoder cycles are spent with nobody reading.
 */
#ifndef UVCD_H
#define UVCD_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <signal.h>
#include <raptor_hal.h>

/* --------------------------------------------------------------------------
 * Logging -- syslog only, no daemon framework
 */
void uvcd_log(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#define UVCD_LOG_ERR  3
#define UVCD_LOG_WARN 4
#define UVCD_LOG_INFO 6
#define UVCD_LOG_DBG  7
#define LOGE(...) uvcd_log(UVCD_LOG_ERR, __VA_ARGS__)
#define LOGW(...) uvcd_log(UVCD_LOG_WARN, __VA_ARGS__)
#define LOGI(...) uvcd_log(UVCD_LOG_INFO, __VA_ARGS__)
#define LOGD(...) uvcd_log(UVCD_LOG_DBG, __VA_ARGS__)

/* --------------------------------------------------------------------------
 * UVC format table -- one entry per negotiable (format, frame) pair
 */
#define UVCD_FMT_MJPEG 1
#define UVCD_FMT_H264  2
#define UVCD_NUM_FORMATS 2

#define UVCD_FRAME_1080P 1
#define UVCD_FRAME_1280X960 2
#define UVCD_FRAME_720P  3
#define UVCD_FRAME_800X600 4
#define UVCD_FRAME_640X480 5
#define UVCD_FRAME_360P  6
#define UVCD_NUM_FRAMES  6

struct uvcd_frame_info {
	uint16_t width;
	uint16_t height;
	uint32_t max_size;    /* worst-case compressed frame size */
	uint32_t h264_bitrate; /* bps, only used when format == H264 */
};

extern const struct uvcd_frame_info uvcd_frames[UVCD_NUM_FRAMES + 1];

#define UVCD_INTERVAL_30FPS 333333
#define UVCD_INTERVAL_25FPS 400000
#define UVCD_INTERVAL_15FPS 666666

#define UVCD_MAX_BUFFERS 4
#define UVCD_ENC_CHN 0
#define UVCD_FS_CHN  0

/* --------------------------------------------------------------------------
 * Live ISP/UVC control values -- the single runtime state behind both UVC
 * control paths (Processing Unit for the standard controls, Extension Unit
 * 4 for the custom ISP ones), and the persisted/default value shape for
 * uvcd_config.c. Declared here (not in uvcd_gadget.h) because
 * uvcd_pipeline_t below embeds one: config load has to happen before the
 * gadget (or even the HAL) exists.
 */
struct uvcd_control_state {
	int brightness;
	int contrast;
	int saturation;
	int sharpness;
	int hue;
	int backlight;
	int power_line_frequency;
	int gamma;    /* gamma * 100, UVC/V4L2 convention (100 == 1.0) */
	int wb_temp;  /* white balance, Kelvin */
	int wb_auto;  /* 0 = manual (use wb_temp), 1 = auto white balance */
	int hflip;
	int vflip;
	int max_again;
	int max_dgain;
	int ae_comp;
	int sinter;
	int temper;
	int dpc;
	int drc;
	int defog;
	int highlight;
};

/* Gamma is a 129-point curve in the HAL, but a single scalar in UVC/V4L2.
 * The ISP's factory curve is read once at init and used as the reference
 * that the scalar re-shapes -- see uvcd_apply_gamma(). */
#define UVCD_GAMMA_POINTS 129
#define UVCD_GAMMA_MIN 40
#define UVCD_GAMMA_MAX 500
#define UVCD_GAMMA_DEF 100 /* 1.0 -- writes the factory curve back unchanged */

/* White balance is a mode + RGB gains in the HAL, but a colour temperature
 * in UVC/V4L2. The sensor only offers discrete presets, so a requested
 * temperature snaps to the nearest one -- see uvcd_apply_wb(). */
#define UVCD_WB_TEMP_MIN 2800
#define UVCD_WB_TEMP_MAX 7500
#define UVCD_WB_TEMP_DEF 5500
#define UVCD_WB_AUTO_DEF 1

/* --------------------------------------------------------------------------
 * Encoded-frame handoff between the encoder pump thread and the gadget
 * thread. Single slot, double-buffered by a lock -- there is exactly one
 * producer (pump thread) and one consumer (gadget thread), and a slow
 * consumer only ever loses its own frame, never the pipeline's throughput.
 */
struct uvcd_frame_buf {
	pthread_mutex_t lock;
	uint8_t *data;
	uint32_t size;     /* allocated capacity */
	uint32_t len;      /* valid bytes */
	uint64_t seq;      /* bumped on every publish; consumer tracks last seen */
	bool is_key;
};

/* --------------------------------------------------------------------------
 * Pipeline state (sensor + FS + encoder channel currently configured)
 */
typedef struct {
	rss_hal_ctx_t *hal_ctx;
	const rss_hal_ops_t *ops;
	const rss_hal_caps_t *caps;

	int sensor_w, sensor_h;

	/* ISP control values in effect -- loaded from UVCD_CONFIG_PATH (or
	 * compiled-in defaults) before the HAL is initialized, and re-used by
	 * gadget_open() to seed the UVC/ACM live shadow. */
	struct uvcd_control_state controls;

	/* The ISP's factory gamma curve, read once after HAL init. Scaling
	 * this rather than synthesizing a curve from scratch keeps gamma=100
	 * bit-for-bit identical to the tuning the vendor shipped, and avoids
	 * having to guess the table's undocumented value range. */
	uint16_t gamma_ref[UVCD_GAMMA_POINTS];
	bool gamma_ref_valid;

	/* Currently configured/running pipeline (0 = torn down) */
	bool configured;
	bool running;
	uint8_t cur_format; /* UVCD_FMT_* */
	uint8_t cur_frame;  /* UVCD_FRAME_* */
	uint32_t cur_interval;

	pthread_t pump_tid;
	volatile sig_atomic_t pump_run;

	struct uvcd_frame_buf frame;
} uvcd_pipeline_t;

int uvcd_pipeline_init(uvcd_pipeline_t *p);
void uvcd_pipeline_deinit(uvcd_pipeline_t *p);

/* Translate the two UVC controls whose HAL shape is not a plain scalar.
 * Both live here rather than in uvcd_gadget.c because both the gadget's
 * control path and pipeline bring-up need them, and both are HAL-specific.
 * Return 0 on success, or a negative RSS_ERR_* / errno value. */
int uvcd_apply_gamma(uvcd_pipeline_t *p, int gamma_x100);
int uvcd_apply_wb(uvcd_pipeline_t *p, int temp_kelvin, int auto_on);

/* Bring up FS+encoder for (format,frame) if not already configured for it,
 * then enable the channel and start the pump thread. Idempotent. */
int uvcd_pipeline_start(uvcd_pipeline_t *p, uint8_t format, uint8_t frame,
			uint32_t interval);

/* Stop the pump thread and fully tear down FS+encoder (true on-demand --
 * zero ISP/encoder cycles spent while no USB host is attached). */
void uvcd_pipeline_stop(uvcd_pipeline_t *p);

/* --------------------------------------------------------------------------
 * Gadget (UVC + kernel webcam device) -- uvcd_gadget.c
 */
int uvcd_gadget_run(uvcd_pipeline_t *p, const char *device);

#endif /* UVCD_H */
