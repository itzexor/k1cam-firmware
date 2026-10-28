/*
 * uvcd.h -- minimal fixed-function ISP -> UVC webcam daemon
 *
 * Single process, single sensor (GC2083 on T31), single active video
 * pipeline. No SHM rings, no IPC -- state transitions are driven only by
 * standard UVC PROBE/COMMIT/STREAMON/STREAMOFF requests from the USB host,
 * and settings arrive as UVC controls (or from the camera's own shell over
 * the local socket in uvcd_ctl.c). The daemon saves every accepted change
 * itself to UVCD_CONFIG_PATH and reloads it at startup (uvcd_config.c).
 *
 * Two layers. The camera (uvcd_camera.c) is the sensor, ISP and IMP system:
 * it comes up with the daemon and stays up, so auto-exposure, white balance
 * and metering are already settled when a host starts streaming, and every
 * control applies to it live. A stream (uvcd_pipeline.c) is the framesource
 * and encoder channels plus the thread that hands frames to the gadget; it
 * exists only while a host streams, and leases the camera for that time.
 */
#ifndef UVCD_H
#define UVCD_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <time.h>
#include <raptor_hal.h>
#include "uvcd_diag.h"

static inline int64_t uvcd_monotonic_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static inline int64_t uvcd_monotonic_ms(void)
{
	return uvcd_monotonic_us() / 1000;
}

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

/* With no stream, the camera runs the default output's view (the UVC
 * default is 1080p at 30 fps), so the most common stream starts on exactly
 * what the camera is already doing. It runs at UVCD_IDLE_FPS until exposure
 * has settled, then at UVCD_IDLE_SLOW_FPS until the scene changes, with
 * exposure held to what fits a UVCD_IDLE_FPS frame so that a stream at
 * that rate starts on the same exposure. */
#define UVCD_IDLE_FPS 30
#define UVCD_IDLE_SLOW_FPS 5
#define UVCD_IDLE_FRAME UVCD_FRAME_1080P

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
/* Center-weighted, plus whatever stands out bright against the rest of the
 * view (a print on the bed), with the highlights kept from clipping -- see
 * subject_metering_tick(). */
#define UVCD_METERING_SUBJECT 4

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
	uint32_t cap;      /* the running stream's limit, its max_size (<= size) */
	uint32_t too_large; /* frames in a row dropped for being over cap */
	uint32_t len;      /* valid bytes */
	uint64_t seq;      /* bumped on every publish; consumer tracks last seen */
	int64_t ts_us;     /* capture time, CLOCK_MONOTONIC microseconds */
	bool is_key;
};

/* --------------------------------------------------------------------------
 * Camera: sensor + ISP + IMP system -- uvcd_camera.c
 */
typedef struct {
	rss_hal_ctx_t *hal_ctx;
	const rss_hal_ops_t *ops;
	const rss_hal_caps_t *caps;

	/* Sensor identity, read from procfs once at init and handed to every
	 * bring-up. */
	rss_sensor_config_t sensor;
	int sensor_w, sensor_h;
	int isp_clk_hz; /* tx-isp's isp_clk parameter; 0 if unknown */

	/* The daemon's live controls (uvcd_pipeline_t.controls): what a
	 * bring-up applies and the periodic loops read. A change arrives
	 * through uvcd_camera_apply() before it is stored there. */
	const struct uvcd_control_state *controls;

	/* Sensor + ISP + IMP system initialized. Up from daemon start to exit,
	 * unless bring-up failed; then a stream start tries again. */
	bool up;
	bool brought_up_once;

	/* A stream holds the camera: its output size and negotiated rate. With
	 * none, the camera runs UVCD_IDLE_FRAME at UVCD_IDLE_FPS. */
	bool leased;
	uint8_t lease_frame;
	uint32_t lease_fps;

	/* The rate the sensor is actually running at, which exposure priority
	 * may hold below the negotiated one. 0 = not set since bring-up. */
	uint32_t sensor_fps;
	int64_t priority_checked_ms;

	/* Idle pacing (uvcd_camera_tick): settling at UVCD_IDLE_FPS until
	 * brightness and exposure hold still, then settled at
	 * UVCD_IDLE_SLOW_FPS until either moves away from what it settled on.
	 * it_max_idle is auto-exposure's integration limit at UVCD_IDLE_FPS,
	 * in sensor lines, applied while slow (it_capped); 0 = unknown, and
	 * idle then stays at UVCD_IDLE_FPS. it_max_slow is the SDK's own limit
	 * at UVCD_IDLE_SLOW_FPS, which a stream at that rate gets back. */
	bool idle_settled;
	bool it_capped;
	uint32_t it_max_idle, it_max_slow;
	int64_t settle_since_ms, idle_checked_ms;
	int idle_samples, idle_changed;
	float idle_l[3], idle_ev[3]; /* settling: the last three samples */
	float settled_l, settled_ev;

	/* The part of the sensor image the output shows (zoom/pan/tilt), in
	 * sensor pixels, for output size view_frame. Metering follows it. */
	uint8_t view_frame;
	struct {
		int x, y, w, h;
	} view;

	/* The metering mode whose weights are in the ISP for this view. Not
	 * valid after a bring-up or a view change, until they are written. */
	bool metering_valid;
	int metering_mode;

	/* The tuning file's own metering weights, read at the first bring-up,
	 * for UVCD_METERING_TUNING. */
	uint8_t ae_weight_ref[15][15];
	bool ae_weight_ref_valid;

	/* UVCD_METERING_SUBJECT: the weights it is easing toward the subject,
	 * the ones last written, and how much of the exposure-compensation
	 * control the highlight guard holds back (0 = none, so the control
	 * applies as set). */
	float subject_weight[15][15];
	uint8_t subject_written[15][15];
	float ae_comp_cut;
	bool ae_comp_valid, guard_valid;
	uint8_t ae_comp_applied, guard_top;
	float guard_level;
	uint16_t diag_sequence;
	int64_t subject_checked_ms, guard_checked_ms;
	int64_t metering_tried_ms;

	/* The ISP's factory gamma curve, read once after HAL init. Scaling
	 * this rather than synthesizing a curve from scratch keeps gamma=100
	 * bit-for-bit identical to the tuning the vendor shipped, and avoids
	 * having to guess the table's undocumented value range. */
	uint16_t gamma_ref[UVCD_GAMMA_POINTS];
	bool gamma_ref_valid;
} uvcd_camera_t;

/* Create the HAL context and read the sensor's identity. Does not bring the
 * camera up. `controls` stays the camera's view of the live controls. */
int uvcd_camera_init(uvcd_camera_t *cam, const struct uvcd_control_state *controls);
/* Take the camera down and release the HAL context. */
void uvcd_camera_deinit(uvcd_camera_t *cam);

/* Bring the sensor, ISP and IMP system up with every control applied, at
 * the lease's rate and view or the idle ones. Idempotent. */
int uvcd_camera_up(uvcd_camera_t *cam);
void uvcd_camera_down(uvcd_camera_t *cam);

/* A stream takes the camera for output size `frame` at `fps`: the sensor
 * rate and the view follow. Call before creating the stream's channels,
 * which take their crop from cam->view, then uvcd_camera_stream_on() once
 * they exist. Fails only if the camera is down. */
int uvcd_camera_acquire(uvcd_camera_t *cam, uint8_t frame, uint32_t fps);
/* The stream's channels exist: colour space for `format`, then exposure
 * for the rate and metering for the view. */
void uvcd_camera_stream_on(uvcd_camera_t *cam, uint8_t format);
/* The stream is gone: back to the idle rate and view. */
void uvcd_camera_release(uvcd_camera_t *cam);

/* Call periodically: subject metering whenever the camera is up, exposure
 * priority while a stream holds it. */
void uvcd_camera_tick(uvcd_camera_t *cam);

/* Apply one control, the field at `field` (offsetof in struct
 * uvcd_control_state), from the whole state `next`, before the caller
 * stores it. Leaving an automatic mode fills in the value the automatic
 * one was using (white-balance temperature, exposure time) in `next`.
 * With the camera down this applies nothing and succeeds: the value takes
 * effect at the next bring-up. Fields that are not the camera's (encoder
 * settings) succeed without effect. Returns 0 or a negative RSS_ERR_* /
 * errno value. */
int uvcd_camera_apply(uvcd_camera_t *cam, struct uvcd_control_state *next, size_t field);

/* Whether `c`'s zoom/pan/tilt would move the view of the current output. */
bool uvcd_camera_view_moves(const uvcd_camera_t *cam, const struct uvcd_control_state *c);

/* The exposure time auto-exposure is using now, 100 us units, or -1. */
int uvcd_camera_live_exposure(uvcd_camera_t *cam);

/* The colour temperature the ISP's auto white balance currently measures,
 * in Kelvin, or -1 when the camera is down or the ISP won't say. */
int uvcd_camera_awb_ct(uvcd_camera_t *cam);

/* Capture from the main loop only; no waits for a frame or writes to the ISP. */
void uvcd_camera_diag(uvcd_camera_t *cam, uint8_t record[UVCD_DIAG_SIZE]);

/* --------------------------------------------------------------------------
 * Pipeline: the camera plus the stream that leases it -- uvcd_pipeline.c
 */
typedef struct {
	uvcd_camera_t cam;

	/* The one runtime copy of every control -- loaded from UVCD_CONFIG_PATH
	 * (or compiled-in defaults) at init, changed in place by the gadget's
	 * control paths, read by the camera and when the encoder channel is
	 * created. */
	struct uvcd_control_state controls;
	/* The controls last proven by a healthy stream, as in
	 * UVCD_CONFIG_GOOD_PATH: what a crash falls back to. */
	struct uvcd_control_state good;
	/* Runs in a row that ended in a crash before proving their controls,
	 * from the crash guard files (uvcd_config.h); 0 once one proves them. */
	int unproven_streak;
	/* The camera-side crash guard (UVCD_CAMERA_GUARD_PATH): armed when
	 * controls not yet proven reach the camera outside a stream. */
	bool camera_guard_armed;
	int64_t camera_guard_since_ms;

	/* Currently configured/running stream (0 = torn down) */
	bool configured;
	bool running;
	uint8_t cur_format; /* UVCD_FMT_* */
	uint8_t cur_frame;  /* UVCD_FRAME_* */
	uint32_t cur_interval;
	uint32_t enc_fps; /* the rate the encoder was last told */
	/* Taken off mjpeg_quality for this stream only, after its frames
	 * outgrew the USB cap (uvcd_gadget.c); 0 again at the next stream or
	 * quality change. */
	int mjpeg_quality_cut;

	/* The SDK's frame timestamps were rebased onto CLOCK_MONOTONIC at
	 * start; if not, frames are stamped when the pump receives them. */
	bool ts_rebased;

	pthread_t pump_tid;
	volatile sig_atomic_t pump_run;

	struct uvcd_frame_buf frame;
} uvcd_pipeline_t;

/* Load the controls (falling back after a crash), set up the camera and
 * bring it up. Fails only if the sensor is not there at all; a camera that
 * would not come up is tried again at the first stream. */
int uvcd_pipeline_init(uvcd_pipeline_t *p);
void uvcd_pipeline_deinit(uvcd_pipeline_t *p);

/* Apply one control (as uvcd_camera_apply) before the caller stores it. A
 * view change while streaming re-creates the stream's channels on the new
 * crop, keeping the old view if that fails. */
int uvcd_pipeline_apply(uvcd_pipeline_t *p, struct uvcd_control_state *next, size_t field);

/* Arm the camera-side crash guard: a change is about to reach the camera
 * outside a stream. Proven once the camera has run UVCD_CONFIG_HEALTHY_MS
 * since, or released by a clean exit. */
void uvcd_pipeline_camera_guard_arm(uvcd_pipeline_t *p);

/* Encoder settings. Each returns 0 when the setting is stored for the next
 * channel creation and, where T31 allows it, already applied live. */
int uvcd_enc_apply_bitrate(uvcd_pipeline_t *p);
int uvcd_enc_request_keyframe(uvcd_pipeline_t *p);

/* Recreate the encoder channel with the current settings, keeping the
 * negotiated format/frame/interval, for settings T31 can only take at
 * channel creation. No-op unless running `format`. The frame sequence
 * restarts at 0; the caller must resync anything that tracks it. */
bool uvcd_pipeline_restart_encoder(uvcd_pipeline_t *p, uint8_t format);

/* Lease the camera (bringing it up if it is down) and create FS+encoder
 * for (format,frame) if not already configured for it, then enable the
 * channel and start the pump thread. Idempotent. */
int uvcd_pipeline_start(uvcd_pipeline_t *p, uint8_t format, uint8_t frame,
			uint32_t interval);

/* Stop the pump thread, fully tear down FS+encoder and release the camera
 * back to idle. */
void uvcd_pipeline_stop(uvcd_pipeline_t *p);

/* Call periodically from the main loop: the camera's loops, the encoder
 * following the sensor's rate, and the camera-side crash guard. */
void uvcd_pipeline_tick(uvcd_pipeline_t *p);

/* --------------------------------------------------------------------------
 * Gadget (UVC + kernel webcam device) -- uvcd_gadget.c
 */
int uvcd_gadget_run(uvcd_pipeline_t *p, const char *device);

#endif /* UVCD_H */
