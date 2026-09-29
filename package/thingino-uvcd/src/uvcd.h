/*
 * uvcd.h -- minimal fixed-function ISP -> UVC webcam daemon
 *
 * Single process, single sensor (GC2083 on T31), single active video
 * pipeline. No SHM rings, no IPC, no control socket -- state transitions
 * are driven only by standard UVC PROBE/COMMIT/STREAMON/STREAMOFF requests
 * from the USB host, and settings arrive only as UVC controls. The daemon
 * saves those itself to UVCD_CONFIG_PATH and reloads them at startup
 * (uvcd_config.c). When no host is streaming, the framesource and encoder
 * channels are torn down at once, and the sensor, ISP and IMP system follow
 * after UVCD_HAL_LINGER_MS: an idle camera does no imaging work at all.
 */
#ifndef UVCD_H
#define UVCD_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <signal.h>
#include <raptor_hal.h>

/* --------------------------------------------------------------------------
 * Logging -- kernel ring buffer (/dev/kmsg) when daemonized, stderr in the
 * foreground; see uvcd_main.c. No daemon framework.
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
	/* Highest zoom (x100) this output size can take. Zoom upscales a
	 * sensor crop to the output, and past a per-size ratio the ISP can't
	 * keep up: errors on every frame (or a silent stall), no encoded
	 * frames. The ratio depends on the ISP clock, so there is one cap for
	 * UVCD_ISP_FAST_HZ and up (what the K1 firmware runs) and one for the
	 * module's 100 MHz default. Measured on the K1, with a margin. */
	uint16_t max_zoom;
	uint16_t max_zoom_slow;
};

#define UVCD_ISP_FAST_HZ 200000000

extern const struct uvcd_frame_info uvcd_frames[UVCD_NUM_FRAMES + 1];

/* Frame intervals (100 ns units) offered in every frame descriptor of the
 * gadget (kernel webcam.c), fastest first. The two lists must match: the
 * host picks from the descriptor, the daemon snaps to this table. Any rate
 * the GC2083 driver accepts (5-30 fps) could go here. */
#define UVCD_NUM_INTERVALS 6
extern const uint32_t uvcd_intervals[UVCD_NUM_INTERVALS];
#define UVCD_INTERVAL_30FPS 333333
#define UVCD_INTERVAL_5FPS 2000000

/* The listed interval closest to `interval`, and the whole frame rate an
 * interval stands for. */
uint32_t uvcd_snap_interval(uint32_t interval);
uint32_t uvcd_interval_fps(uint32_t interval);

/* How long the sensor/ISP stay up after the last stream stops. Bringing
 * them back costs about half a second before the first frame, and hosts
 * routinely close and reopen a stream within a second or two (players
 * seeking, browsers enumerating, Windows' frame server), so each of those
 * would otherwise pay it. */
#define UVCD_HAL_LINGER_MS 5000

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
	int power_line_frequency;
	int gamma;    /* gamma * 100, UVC/V4L2 convention (100 == 1.0) */
	int wb_temp;  /* white balance, Kelvin */
	int wb_auto;  /* 0 = manual (use wb_temp), 1 = auto white balance */

	/* Camera Terminal */
	int ae_mode;       /* UVCD_AE_* -- the UVC bitmap value, not V4L2's menu index */
	int ae_priority;   /* 0 = frame rate constant, 1 = may slow down for exposure */
	int exposure_time; /* manual exposure, 100 us units (UVC convention) */
	int zoom;          /* 100 = full view, 400 = 4x digital zoom */
	int pan;           /* arc-seconds, + = right; moves the zoomed view */
	int tilt;          /* arc-seconds, + = up */

	int rotation; /* 0 or 180 degrees */
	int metering; /* UVCD_METERING_* */
	int max_again;
	int max_dgain;
	int ae_comp;
	int sinter;
	int temper;
	int dpc;
	int drc;
	int highlight;

	/* Encoder. Read by the pipeline when it creates the encoder channel;
	 * the ones T31 can change at runtime are also applied live. */
	int h264_bitrate_kbps; /* 0 = per-resolution default (uvcd_frames) */
	int h264_rate_control; /* UVCD_H264_RC_* */
	int h264_gop_frames;   /* 0 = one second at the negotiated frame rate */
	int h264_min_qp;       /* -1 = UVCD_H264_AUTO_MIN_QP */
	int h264_max_qp;       /* -1 = SDK default */
	int h264_profile;      /* 0 = baseline, 1 = main, 2 = high */
	int mjpeg_quality;     /* 1..100, higher is better */
};

/* Auto-exposure modes, as UVC's CT_AE_MODE bitmap. There is no iris, so
 * "aperture priority" -- exposure time automatic -- is what auto means here,
 * as on most webcams. */
#define UVCD_AE_MANUAL 1
#define UVCD_AE_APERTURE_PRIORITY 8
#define UVCD_AE_MODES (UVCD_AE_MANUAL | UVCD_AE_APERTURE_PRIORITY) /* GET_RES */

/* Manual exposure time, 100 us units. The ISP takes microseconds in a 16-bit
 * field, which caps it at 65.5 ms. */
#define UVCD_EXPOSURE_MIN 1
#define UVCD_EXPOSURE_MAX 655
#define UVCD_EXPOSURE_DEF 333

/* Digital zoom and pan/tilt, done by cropping the sensor image before the
 * scaler. Pan/tilt move the crop within whatever the zoom leaves spare; at
 * zoom 100 there is nothing to move. */
#define UVCD_ZOOM_MIN 100
#define UVCD_ZOOM_MAX 400
#define UVCD_ZOOM_DEF 100
#define UVCD_PANTILT_MAX 36000 /* +-10 degrees, spanning the whole spare range */
#define UVCD_PANTILT_RES 3600

/* Which part of the (visible) picture auto-exposure meters on. */
#define UVCD_METERING_TUNING 0 /* the sensor tuning file's own weights */
#define UVCD_METERING_AVERAGE 1
#define UVCD_METERING_CENTER 2
#define UVCD_METERING_SPOT 3

#define UVCD_H264_RC_CBR 0
#define UVCD_H264_RC_VBR 1
#define UVCD_H264_RC_CAPPED_VBR 2
#define UVCD_H264_AUTO_MIN_QP 20 /* h264_min_qp -1 */

/* Gamma is a 129-point curve in the HAL, but a single scalar in UVC/V4L2.
 * The ISP's factory curve is read once at init and used as the reference
 * that the scalar re-shapes -- see uvcd_apply_gamma(). */
#define UVCD_GAMMA_POINTS 129
#define UVCD_GAMMA_MIN 40
#define UVCD_GAMMA_MAX 500
#define UVCD_GAMMA_DEF 100 /* 1.0 -- writes the factory curve back unchanged */

/* White balance is a mode + RGB gains in the HAL, but a colour temperature
 * in UVC/V4L2. Manual mode computes the gains from a model of the sensor's
 * white locus -- see uvcd_apply_wb(). */
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
	int64_t ts_us;     /* capture time, CLOCK_MONOTONIC microseconds */
	bool is_key;
};

/* --------------------------------------------------------------------------
 * Pipeline state (sensor + FS + encoder channel currently configured)
 */
typedef struct {
	rss_hal_ctx_t *hal_ctx;
	const rss_hal_ops_t *ops;
	const rss_hal_caps_t *caps;

	/* Sensor identity, read from procfs once at init and handed to every
	 * HAL bring-up. */
	rss_sensor_config_t sensor;
	int sensor_w, sensor_h;
	int isp_clk_hz; /* tx-isp's isp_clk parameter; 0 if unknown */

	/* Sensor + ISP + IMP system initialized (hal init done). Brought up by
	 * uvcd_pipeline_start(), taken down by uvcd_pipeline_idle() once no
	 * channel has been configured for UVCD_HAL_LINGER_MS. */
	bool hal_up;
	int64_t idle_since_ms; /* 0 = not counting */

	/* The rate the sensor is actually running at, which exposure priority
	 * may hold below the negotiated one. 0 = not set since bring-up. */
	uint32_t sensor_fps;
	int64_t priority_checked_ms;

	/* The part of the sensor image the channel shows (zoom/pan/tilt),
	 * sensor pixels. Metering weights follow it. */
	struct {
		int x, y, w, h;
	} view;

	/* The tuning file's own metering weights, read at the first bring-up,
	 * for UVCD_METERING_TUNING. */
	uint8_t ae_weight_ref[15][15];
	bool ae_weight_ref_valid;

	/* Live controls start at factory defaults. Saved controls are a sparse
	 * overlay, applied at startup only when explicitly enabled. */
	struct uvcd_control_state controls;
	struct uvcd_control_state saved;
	uint64_t saved_mask;
	bool apply_on_boot;
	/* Streams in a row that started without proving healthy, from the
	 * crash guard file (uvcd_config.h); 0 once one does. */
	int unproven_streak;

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

	/* The SDK's frame timestamps were rebased onto CLOCK_MONOTONIC at
	 * start; if not, frames are stamped when the pump receives them. */
	bool ts_rebased;

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

/* Camera Terminal and orientation controls. Each takes the full control
 * state rather than one value, so a caller can apply a change before
 * storing it (uvcd_ctrl_set stores only what the ISP accepted). Exposure
 * and the view also need a configured channel for the frame rate and the
 * output size; without one they apply what they can and the rest follows
 * at the next stream start. */
int uvcd_apply_exposure(uvcd_pipeline_t *p, const struct uvcd_control_state *c);
int uvcd_apply_view(uvcd_pipeline_t *p, const struct uvcd_control_state *c);
int uvcd_apply_metering(uvcd_pipeline_t *p, const struct uvcd_control_state *c);
int uvcd_apply_rotation(uvcd_pipeline_t *p, const struct uvcd_control_state *c);

/* The exposure time auto-exposure is using now, 100 us units, or -1. */
int uvcd_live_exposure(uvcd_pipeline_t *p);

/* The colour temperature the ISP's auto white balance currently measures,
 * in Kelvin, or -1 when the HAL is down or the ISP won't say. */
int uvcd_awb_current_ct(uvcd_pipeline_t *p);

/* Encoder settings. Each returns 0 when the setting is stored for the next
 * channel creation and, where T31 allows it, already applied live. */
int uvcd_enc_apply_bitrate(uvcd_pipeline_t *p);
int uvcd_enc_apply_gop(uvcd_pipeline_t *p);
int uvcd_enc_request_keyframe(uvcd_pipeline_t *p);

/* Recreate the encoder channel with the current settings, keeping the
 * negotiated format/frame/interval, for settings T31 can only take at
 * channel creation. No-op unless running `format`. The frame sequence
 * restarts at 0; the caller must resync anything that tracks it. */
bool uvcd_pipeline_restart_encoder(uvcd_pipeline_t *p, uint8_t format);

/* Bring up the sensor/ISP if they are down, then FS+encoder for
 * (format,frame) if not already configured for it, then enable the channel
 * and start the pump thread. Idempotent. */
int uvcd_pipeline_start(uvcd_pipeline_t *p, uint8_t format, uint8_t frame,
			uint32_t interval);

/* Stop the pump thread and fully tear down FS+encoder. The sensor/ISP stay
 * up until uvcd_pipeline_idle() lets them go. */
void uvcd_pipeline_stop(uvcd_pipeline_t *p);

/* Call periodically from the main loop: runs exposure priority while
 * streaming, and takes the sensor, ISP and IMP system down once no channel
 * has been configured for UVCD_HAL_LINGER_MS. */
void uvcd_pipeline_tick(uvcd_pipeline_t *p);

/* True while the HAL is initialized. ISP calls are only valid then; while it
 * is down, controls are stored and applied at the next bring-up. */
static inline bool uvcd_pipeline_hal_up(const uvcd_pipeline_t *p)
{
	return p->hal_up;
}

/* --------------------------------------------------------------------------
 * Gadget (UVC + kernel webcam device) -- uvcd_gadget.c
 */
int uvcd_gadget_run(uvcd_pipeline_t *p, const char *device);

#endif /* UVCD_H */
