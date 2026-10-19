/* Host regression tests. Link with --gc-sections to discard hardware paths. */
#include <assert.h>
#include <stdarg.h>
#include "../src/uvcd_camera.c"
#include "../src/uvcd_pipeline.c"

/* Smallest sensor crop measured to stream at each output size. One step
 * past these made the ISP fail. At 100 MHz the last working zoom was 131,
 * 250, 342, then 400 for the rest; at 200 MHz, 300 for 1080p and 400 for
 * the rest. */
struct crop {
	int w, h;
};
static const struct crop proven_slow[UVCD_NUM_FRAMES + 1] = {
	[UVCD_FRAME_1080P] = {1464, 824},
	[UVCD_FRAME_1280X960] = {576, 432},
	[UVCD_FRAME_720P] = {560, 314},
	[UVCD_FRAME_800X600] = {360, 270},
	[UVCD_FRAME_640X480] = {360, 270},
	[UVCD_FRAME_360P] = {480, 270},
};
static const struct crop proven_fast[UVCD_NUM_FRAMES + 1] = {
	[UVCD_FRAME_1080P] = {640, 360},
	[UVCD_FRAME_1280X960] = {360, 270},
	[UVCD_FRAME_720P] = {480, 270},
	[UVCD_FRAME_800X600] = {360, 270},
	[UVCD_FRAME_640X480] = {360, 270},
	[UVCD_FRAME_360P] = {480, 270},
};

static rss_fs_config_t fs;
static rss_video_config_t enc;
static uint32_t stream_size;
static int stream_count, buffer_error, encoder_calls, destroyed;

static char log_text[8192];
void uvcd_log(int level, const char *fmt, ...)
{
	(void)level;
	size_t used = strlen(log_text);
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(log_text + used, sizeof(log_text) - used, fmt, ap);
	va_end(ap);
}

static int channel_ok(void *ctx, int chn)
{
	(void)ctx;
	(void)chn;
	return 0;
}

static int destroy(void *ctx, int chn)
{
	destroyed++;
	return channel_ok(ctx, chn);
}

static int depth_ok(void *ctx, int chn, int depth)
{
	(void)depth;
	return channel_ok(ctx, chn);
}

static int create_fs(void *ctx, int chn, const rss_fs_config_t *cfg)
{
	fs = *cfg;
	return channel_ok(ctx, chn);
}

static int set_size(void *ctx, int chn, uint32_t bytes)
{
	(void)ctx;
	(void)chn;
	stream_size = bytes;
	return buffer_error;
}

static int set_count(void *ctx, int chn, int count)
{
	stream_count = count;
	return channel_ok(ctx, chn);
}

static int create_enc(void *ctx, int chn, const rss_video_config_t *cfg)
{
	(void)ctx;
	(void)chn;
	assert(stream_size > 0 && stream_count > 0);
	enc = *cfg;
	encoder_calls++;
	return -EIO; /* Stop after capturing the complete configuration. */
}

/* Subject metering: fake ISP statistics in, weights and compensation out. */
static uint32_t fake_zone[15][15], fake_hist[256];
static uint8_t written_weight[15][15];
static int weight_writes, written_comp = -1, weight_error, comp_error;

static int get_zone(void *ctx, uint32_t zone[15][15])
{
	(void)ctx;
	memcpy(zone, fake_zone, sizeof(fake_zone));
	return 0;
}

static int get_hist(void *ctx, uint32_t bins[256])
{
	(void)ctx;
	memcpy(bins, fake_hist, sizeof(fake_hist));
	return 0;
}

static int set_weight(void *ctx, const uint8_t weight[15][15])
{
	(void)ctx;
	weight_writes++;
	if (weight_error)
		return weight_error; /* a rejected write leaves the ISP as it was */
	memcpy(written_weight, weight, sizeof(written_weight));
	return 0;
}

static int set_comp(void *ctx, int comp)
{
	(void)ctx;
	if (comp_error)
		return comp_error;
	written_comp = comp;
	return 0;
}

static bool ae_stable;
static int get_ae_state(void *ctx, void *state)
{
	(void)ctx;
	((struct uvcd_imp_ae_state *)state)->stable = ae_stable;
	return 0;
}

/* Clipped pixels pile up in one bin: 247..250 on the K1 near unity gain. */
#define FAKE_WHITE 249

/* A dark bed (zones at 40) with a bright print over a 3x3 block left of
 * centre, and a histogram whose brightest 1% sits at `top_bin`: spread up
 * from there like the falloff of a highlight, or from FAKE_WHITE on all
 * piled up in it, clipped. */
static void fake_scene(uint32_t print, int top_bin)
{
	for (int r = 0; r < 15; r++)
		for (int col = 0; col < 15; col++)
			fake_zone[r][col] = r >= 8 && r <= 10 && col >= 3 && col <= 5 ? print : 40;
	memset(fake_hist, 0, sizeof(fake_hist));
	fake_hist[60] = 9900;
	if (top_bin > FAKE_WHITE)
		top_bin = FAKE_WHITE;
	fake_hist[FAKE_WHITE] = 100;
	for (int i = top_bin; i < top_bin + 10 && i < FAKE_WHITE; i++) {
		fake_hist[i] += 10;
		fake_hist[FAKE_WHITE] -= 10;
	}
}

/* The camera's answer to a compensation, measured on the K1 (gc2083,
 * hyb_D blob) by sweeping it over a fixed scene: AE's locked target at each
 * compensation, while the brightest 1% stayed proportional to it until it
 * saturated. */
static const int measured_comp[] = {0, 32, 48, 64, 96, 128, 176, 224, 255};
static const int measured_target[] = {0, 7, 10, 14, 21, 27, 48, 72, 86};

static float measured_ae_target(int comp)
{
	int i = 1;

	while (i < 8 && comp > measured_comp[i])
		i++;
	return (float)measured_target[i - 1] +
	       (float)(measured_target[i] - measured_target[i - 1]) *
		       (float)(comp - measured_comp[i - 1]) /
		       (float)(measured_comp[i] - measured_comp[i - 1]);
}

/* A scene whose brightest 1% would sit at `level` (past 255 if the sensor
 * could go there) at compensation `at`, shown at the compensation the
 * guard last wrote. */
static void measured_scene(float level, int at)
{
	float top = level * measured_ae_target(written_comp) / measured_ae_target(at);

	fake_scene(400, (int)(top + 0.5f));
}

/* Run the guard on a measured scene until it holds still; the steps it took. */
static int guard_settle(uvcd_camera_t *cam, float level, int at)
{
	for (int steps = 0; steps < 20; steps++) {
		int comp = written_comp;

		measured_scene(level, at);
		highlight_guard(cam);
		if (written_comp == comp)
			return steps;
	}
	return -1;
}

static void test_subject_metering(void)
{
	rss_hal_ops_t ops = {
		.isp_get_ae_zone = get_zone,
		.isp_get_ae_hist_origin = get_hist,
		.isp_set_ae_weight = set_weight,
		.isp_set_ae_comp = set_comp,
	};
	struct uvcd_control_state controls = {0};
	uvcd_camera_t cam = {.ops = &ops, .sensor_w = 1920, .sensor_h = 1080, .up = true,
			     .controls = &controls};
	cam.view.w = 1920;
	cam.view.h = 1080;
	controls.ae_mode = UVCD_AE_APERTURE_PRIORITY;
	controls.ae_comp = 128;
	controls.metering = UVCD_METERING_SUBJECT;

	/* Starts out center-weighted. */
	uint8_t center[15][15];
	metering_weights(&cam, UVCD_METERING_CENTER, center);
	assert(apply_metering(&cam, &controls) == 0);
	assert(memcmp(written_weight, center, sizeof(center)) == 0);
	assert(center[9][4] < 8 && center[7][7] == 8 && center[0][0] == 2);
	assert(cam.metering_valid && cam.metering_mode == UVCD_METERING_SUBJECT);

	/* The print eases up to full weight; the bed keeps center weights. */
	fake_scene(400, 175);
	for (int i = 0; i < 8; i++)
		subject_weights(&cam);
	assert(written_weight[9][4] == 8 && written_weight[8][3] == 8);
	assert(written_weight[0][0] == center[0][0] && written_weight[12][12] == center[12][12]);
	/* Unchanged statistics write nothing new. */
	int writes = weight_writes;
	subject_weights(&cam);
	assert(weight_writes == writes);
	/* A zone just a little brighter than the bed is not a subject. */
	fake_scene(56, 200);
	for (int i = 0; i < 8; i++)
		subject_weights(&cam);
	assert(memcmp(written_weight, center, sizeof(center)) == 0);
	/* Nor is near-black noise: 3 against a median of 1 used to be a
	 * full-weight subject. */
	for (int r = 0; r < 15; r++)
		for (int col = 0; col < 15; col++)
			fake_zone[r][col] = 1;
	fake_zone[9][4] = 3;
	for (int i = 0; i < 8; i++)
		subject_weights(&cam);
	assert(memcmp(written_weight, center, sizeof(center)) == 0);

	/* Weights the ISP rejected are not taken as written: with the print's
	 * weights still in the ISP, a rejected reset to center-weighted must
	 * be retried, not skipped as already in place. */
	fake_scene(400, 175);
	for (int i = 0; i < 8; i++)
		subject_weights(&cam);
	assert(written_weight[9][4] == 8);
	cam.metering_valid = false; /* as after a view change */
	weight_error = -EIO;
	assert(apply_metering(&cam, &controls) == -EIO && !cam.metering_valid);
	weight_error = 0;
	fake_scene(56, 175);
	for (int i = 0; i < 8; i++)
		subject_weights(&cam);
	assert(memcmp(written_weight, center, sizeof(center)) == 0);
	/* ...and the tick retries the rejected reset itself. */
	assert(!cam.metering_valid);
	cam.metering_tried_ms = 0;
	writes = weight_writes;
	uvcd_camera_tick(&cam);
	assert(cam.metering_valid && weight_writes == writes + 1);

	/* A rejected compensation write leaves the guard where it was. */
	fake_scene(400, 255);
	comp_error = -EIO;
	highlight_guard(&cam);
	assert(cam.ae_comp_cut == 0.0f);
	comp_error = 0;
	/* A small light source may clip without holding the scene back. */
	memset(fake_hist, 0, sizeof(fake_hist));
	fake_hist[60] = 992;
	fake_hist[255] = 8; /* 0.8% */
	written_comp = -1;
	highlight_guard(&cam);
	assert(written_comp == -1 && cam.ae_comp_cut == 0.0f);

	/* Clipping while auto-exposure still adjusts is the light having just
	 * gone up, not the scene: the guard waits for AE to lock. */
	ops.isp_get_ae_state = get_ae_state;
	ae_stable = false;
	fake_scene(400, 255);
	written_comp = -1;
	cam.guard_checked_ms = cam.subject_checked_ms = 0;
	subject_metering_tick(&cam);
	assert(written_comp == -1 && cam.ae_comp_cut == 0.0f && cam.guard_checked_ms == 0);
	ae_stable = true;
	cam.subject_checked_ms = 0;
	subject_metering_tick(&cam);
	assert(written_comp == 64 && cam.guard_checked_ms != 0);
	ops.isp_get_ae_state = NULL;
	cam.ae_comp_cut = 0.0f;
	written_comp = 128;

	/* The top of an LED hotspot, as the K1 measured it (in 0.01% of the
	 * picture): 0.67% piled up at the white level, the falloff below it.
	 * The brightest 1% reaches down into the falloff, so it is a real
	 * pixel at 231, not a saturated one. */
	static const uint32_t hotspot[] = {14, 14, 14, 14, 14, 14, 14, 14, /* 231.. */
					   10, 10, 0, 10, 10, 20, 20, 40, 50, 50, 670};
	memset(fake_hist, 0, sizeof(fake_hist));
	fake_hist[60] = 100000;
	for (int i = 0; i < 19; i++) {
		fake_hist[231 + i] = hotspot[i];
		fake_hist[60] -= hotspot[i];
	}
	highlight_guard(&cam);
	assert(written_comp == 94); /* 128 * 170 / 231 */
	cam.ae_comp_cut = 0.0f;
	written_comp = 128;
	/* A saturated top halves the compensation: its true level is unknown. */
	assert(guard_settle(&cam, 600.0f, 128) == 1 && written_comp == 64);
	/* Never below half, however bright. */
	assert(guard_settle(&cam, 2000.0f, 128) == 0 && written_comp == 64);
	/* The light off again: given back in a step, and the last unit in a
	 * second (the 64 -> 127 step is a square root, 1.99). */
	assert(guard_settle(&cam, 83.0f, 128) == 2 && written_comp == 128);
	assert(cam.ae_comp_cut == 0.0f);
	/* Clipping but not saturated: one step, into the band. */
	assert(guard_settle(&cam, 230.0f, 128) == 1 && written_comp == 95);
	/* Between the thresholds it holds still. */
	fake_scene(400, 175);
	written_comp = -1;
	highlight_guard(&cam);
	assert(written_comp == -1);
	/* Well below clipping, it gives the compensation back, never more. */
	written_comp = 95;
	fake_scene(400, 120);
	for (int i = 0; i < 30; i++)
		highlight_guard(&cam);
	assert(written_comp == 128 && cam.ae_comp_cut == 0.0f);
	/* Above 128 compensation moves AE more than in proportion, and giving
	 * the full ratio back hunted 128 <-> 230 here. Now it settles in the
	 * band, wherever the scene sits. */
	controls.ae_comp = 255;
	for (float level = 150.0f; level < 900.0f; level += 10.0f) {
		cam.ae_comp_cut = 0.0f;
		written_comp = 255;
		int steps = guard_settle(&cam, level, 255), top = 255;

		while (top > 0 && fake_hist[top] == 0)
			top--;
		assert(steps >= 0 && steps <= 4);
		/* In the band, or pinned at either end of the cut. */
		assert((top >= 150 && top < 200) || (top >= 200 && written_comp == 128) ||
		       (top < 150 && written_comp == 255));
	}
	controls.ae_comp = 128;
	cam.ae_comp_cut = 0.0f;

	/* The control itself still applies under the guard ... */
	fake_scene(400, 255);
	highlight_guard(&cam);
	struct uvcd_control_state c = controls;
	c.ae_comp = 200;
	assert(apply_ae_comp(&cam, &c) == 0 && written_comp == 100);
	/* ... and leaving subject metering gives back what it held. */
	c.metering = UVCD_METERING_CENTER;
	assert(apply_metering(&cam, &c) == 0);
	assert(written_comp == 200 && cam.ae_comp_cut == 0.0f);
	assert(memcmp(written_weight, center, sizeof(center)) == 0);

	/* Nothing to do in manual exposure or other modes. */
	writes = weight_writes;
	cam.subject_checked_ms = 0;
	controls.metering = UVCD_METERING_CENTER;
	subject_metering_tick(&cam);
	controls.metering = UVCD_METERING_SUBJECT;
	controls.ae_mode = UVCD_AE_MANUAL;
	subject_metering_tick(&cam);
	assert(weight_writes == writes);
}


/* Both ISP writes must succeed before a metering mode is accepted. */
static void test_metering_exit_failure(void)
{
	rss_hal_ops_t ops = {.isp_set_ae_weight = set_weight, .isp_set_ae_comp = set_comp};
	for (int mode = UVCD_METERING_TUNING; mode < UVCD_METERING_SUBJECT; mode++) {
		struct uvcd_control_state controls = {.ae_comp = 128,
			.ae_mode = UVCD_AE_APERTURE_PRIORITY, .metering = UVCD_METERING_SUBJECT};
		uvcd_camera_t cam = {.ops = &ops, .controls = &controls, .up = true,
			.sensor_w = 1920, .sensor_h = 1080, .ae_weight_ref_valid = true};
		cam.view.w = 1920;
		cam.view.h = 1080;
		memset(cam.ae_weight_ref, 3, sizeof(cam.ae_weight_ref));
		assert(apply_metering(&cam, &controls) == 0);
		uint8_t old_weight[15][15];
		memcpy(old_weight, written_weight, sizeof(old_weight));
		cam.ae_comp_cut = 0.5f;
		assert(apply_ae_comp(&cam, &controls) == 0 && written_comp == 64);
		struct uvcd_control_state next = controls;
		next.metering = mode;

		/* Rejected weights leave compensation and its cut untouched. */
		weight_error = -EIO;
		assert(apply_metering(&cam, &next) == -EIO);
		assert(written_comp == 64 && cam.ae_comp_cut == 0.5f);
		assert(memcmp(old_weight, written_weight, sizeof(old_weight)) == 0);
		weight_error = 0;

		/* Accepted weights, rejected compensation: fail the change, keep
		 * track of the actual weights, and retry the stored mode on tick. */
		comp_error = -EIO;
		assert(apply_metering(&cam, &next) == -EIO);
		assert(!cam.metering_valid && cam.ae_comp_cut == 0.5f && written_comp == 64);
		assert(memcmp(weights_in_isp(&cam), written_weight, sizeof(written_weight)) == 0);
		comp_error = 0;
		cam.metering_tried_ms = 0;
		uvcd_camera_tick(&cam);
		assert(cam.metering_valid && cam.metering_mode == UVCD_METERING_SUBJECT);
		assert(cam.ae_comp_cut == 0.5f && written_comp == 64);
		assert(memcmp(old_weight, written_weight, sizeof(old_weight)) == 0);

		assert(apply_metering(&cam, &next) == 0);
		assert(cam.metering_valid && cam.metering_mode == mode);
		assert(cam.ae_comp_cut == 0.0f && written_comp == 128);
	}
}

/* The camera's lease: rate, view, exposure and metering follow whoever
 * holds the camera, and the camera stays up throughout. */
int rss_hal_get_imp_version(char *buf, int size)
{
	(void)buf;
	(void)size;
	return -1;
}

static uint32_t sensor_rate, encoder_rate;
static int csc_mode, expr_writes, inits;
static int rate_errors, expr_errors, vflip_errors, hal_hflip, hal_vflip, isp_flip;
static struct uvcd_imp_expr last_expr;
static rss_wb_config_t last_wb;

static int hal_init(void *ctx, const rss_multi_sensor_config_t *cfg)
{
	(void)ctx;
	(void)cfg;
	inits++;
	return 0;
}

static int set_sensor_rate(void *ctx, uint32_t num, uint32_t den)
{
	(void)ctx;
	assert(den == 1);
	if (rate_errors) {
		rate_errors--;
		return -EIO;
	}
	sensor_rate = num;
	return 0;
}

static int set_encoder_rate(void *ctx, int chn, uint32_t num, uint32_t den)
{
	(void)ctx;
	(void)chn;
	assert(den == 1);
	encoder_rate = num;
	return 0;
}

static int get_csc(void *ctx, void *attr)
{
	(void)ctx;
	((struct uvcd_imp_csc *)attr)->mode = csc_mode;
	return 0;
}

static int set_csc(void *ctx, const void *attr)
{
	(void)ctx;
	csc_mode = ((const struct uvcd_imp_csc *)attr)->mode;
	return 0;
}

static int set_expr(void *ctx, const void *attr)
{
	(void)ctx;
	expr_writes++;
	if (expr_errors) {
		expr_errors--;
		return -EIO;
	}
	last_expr = *(const struct uvcd_imp_expr *)attr;
	return 0;
}

static int get_exposure(void *ctx, rss_exposure_t *e)
{
	(void)ctx;
	memset(e, 0, sizeof(*e));
	e->exposure_time = 12345; /* us */
	e->total_gain = 256;
	return 0;
}

static int get_ct(void *ctx, void *ct)
{
	(void)ctx;
	*(unsigned int *)ct = 9000; /* above the control's range */
	return 0;
}

static int set_wb(void *ctx, const rss_wb_config_t *cfg)
{
	(void)ctx;
	last_wb = *cfg;
	return 0;
}

static int set_level(void *ctx, int value);
static int set_isp_mode(void *ctx, rss_isp_mode_t mode) { return set_level(ctx, mode); }
static int set_flicker(void *ctx, rss_antiflicker_t mode) { return set_level(ctx, mode); }
static int hal_deinit(void *ctx) { (void)ctx; return 0; }
/* The T31 HAL (raptor-hal hal_isp.c) keeps its own copy of each flip bit,
 * updates it before calling, and writes both bits as one
 * IMP_ISP_Tuning_SetHVFLIP mode (bit 0 horizontal, bit 1 vertical); its
 * getter reads the ISP. On the K1 with a rejected vflip injected into
 * libimp, restoring hflip alone wrote SetHVFLIP 1, then 3 (rejected), then
 * 2, and isp-m0 showed the picture flipped vertically. */
static int set_hvflip(void)
{
	isp_flip = hal_hflip | hal_vflip << 1;
	return 0;
}
static int set_hflip(void *ctx, int value) { (void)ctx; hal_hflip = !!value; return set_hvflip(); }
static int set_vflip(void *ctx, int value)
{
	(void)ctx;
	hal_vflip = !!value;
	if (vflip_errors) { vflip_errors--; return -EIO; }
	return set_hvflip();
}
static int get_flip(void *ctx, int *h, int *v)
{
	(void)ctx;
	*h = isp_flip & 1;
	*v = isp_flip >> 1;
	return 0;
}
static int reject_level(void *ctx, int value) { (void)ctx; (void)value; return -EIO; }

/* A real T31 supplies these setters; focused tests override individual ones. */
static void startup_ops(rss_hal_ops_t *ops)
{
	ops->isp_set_brightness = set_level;
	ops->isp_set_contrast = set_level;
	ops->isp_set_saturation = set_level;
	ops->isp_set_sharpness = set_level;
	ops->isp_set_sinter_strength = set_level;
	ops->isp_set_temper_strength = set_level;
	ops->isp_set_hue = set_level;
	ops->isp_set_max_again = set_level;
	ops->isp_set_max_dgain = set_level;
	ops->isp_set_dpc_strength = set_level;
	ops->isp_set_drc_strength = set_level;
	ops->isp_set_backlight_comp = set_level;
	ops->isp_set_highlight_depress = set_level;
	ops->isp_set_running_mode = set_isp_mode;
	ops->isp_set_bypass = set_level;
	ops->isp_set_antiflicker = set_flicker;
	ops->isp_set_hflip = set_hflip;
	ops->isp_set_vflip = set_vflip;
	ops->isp_get_hvflip = get_flip;
	ops->isp_set_wb = set_wb;
	ops->deinit = hal_deinit;
}

static void test_control_failures(void)
{
	rss_hal_ops_t ops = {.init = hal_init, .isp_set_sensor_fps = set_sensor_rate,
		.isp_set_expr = set_expr, .isp_set_ae_comp = set_comp, .isp_set_ae_weight = set_weight};
	startup_ops(&ops);
	struct uvcd_control_state c = {.zoom = 100, .gamma = 100, .wb_auto = 1,
		.ae_mode = UVCD_AE_APERTURE_PRIORITY, .metering = UVCD_METERING_CENTER};
	uvcd_camera_t cam = {.ops = &ops, .controls = &c};
	int before;
	/* Rejected startup writes are named in the log; the camera comes up. */
	log_text[0] = '\0';
	ops.isp_set_brightness = reject_level;
	assert(uvcd_camera_up(&cam) == 0 && cam.up);
	assert(strstr(log_text, "startup brightness rejected (-5)"));
	uvcd_camera_down(&cam);
	ops.isp_set_brightness = set_level;
	log_text[0] = '\0';
	c.gamma = 120; /* a non-default curve cannot be applied without a reference */
	assert(uvcd_camera_up(&cam) == 0 && cam.up);
	assert(strstr(log_text, "startup gamma rejected"));
	uvcd_camera_down(&cam);
	log_text[0] = '\0';
	c.gamma = 100;
	assert(uvcd_camera_up(&cam) == 0 && !strstr(log_text, "startup gamma"));
	uvcd_camera_down(&cam);
	log_text[0] = '\0';
	rate_errors = 1;
	assert(uvcd_camera_up(&cam) == 0 && cam.up);
	assert(strstr(log_text, "startup exposure rejected (-5)"));
	uvcd_camera_down(&cam);
	assert(uvcd_camera_up(&cam) == 0 && cam.up && cam.sensor_fps == 30);

	/* A rejected stream rate still streams, at the sensor's current rate. */
	log_text[0] = '\0';
	rate_errors = 1;
	assert(uvcd_camera_acquire(&cam, UVCD_FRAME_720P, 15) == 0);
	assert(cam.leased && cam.sensor_fps == 30);
	assert(strstr(log_text, "sensor fps 15 rejected (-5)"));
	uvcd_camera_release(&cam);

	struct uvcd_control_state next = c;
	next.rotation = 180;
	hal_hflip = hal_vflip = isp_flip = 0;
	vflip_errors = 1;
	assert(uvcd_camera_apply(&cam, &next, offsetof(struct uvcd_control_state, rotation)) == -EIO);
	assert(isp_flip == 0 && hal_hflip == 0 && hal_vflip == 0);
	assert(uvcd_camera_apply(&cam, &next, offsetof(struct uvcd_control_state, rotation)) == 0);
	assert(isp_flip == 3);
	next.rotation = 0; /* and back, from both bits set */
	vflip_errors = 1;
	assert(uvcd_camera_apply(&cam, &next, offsetof(struct uvcd_control_state, rotation)) == -EIO);
	assert(isp_flip == 3 && hal_hflip == 1 && hal_vflip == 1);

	c.ae_mode = UVCD_AE_MANUAL;
	c.exposure_time = 100;
	cam.leased = true;
	cam.lease_fps = 30;
	assert(apply_exposure(&cam, &c) == 0);
	next = c;
	next.ae_priority = 1;
	next.exposure_time = 500; /* needs 20 fps */
	rate_errors = 1;
	before = expr_writes;
	assert(apply_exposure(&cam, &next) == -EIO && expr_writes == before);
	assert(cam.sensor_fps == 30);
	expr_errors = 1;
	assert(apply_exposure(&cam, &next) == -EIO);
	assert(cam.sensor_fps == 30 && sensor_rate == 30 && last_expr.time == 10000);
	assert(apply_exposure(&cam, &next) == 0 && cam.sensor_fps == 20 && last_expr.time == 50000);

	next = c;
	next.metering = UVCD_METERING_TUNING;
	assert(apply_metering(&cam, &next) == -ENOTSUP);
	uvcd_camera_down(&cam);
}

static void test_camera_lease(void)
{
	rss_hal_ops_t ops = {
		.init = hal_init,
		.isp_set_sensor_fps = set_sensor_rate,
		.isp_set_ae_weight = set_weight,
		.isp_get_ae_zone = get_zone,
		.isp_get_ae_hist_origin = get_hist,
		.isp_set_ae_comp = set_comp,
		.isp_get_csc_attr = get_csc,
		.isp_set_csc_attr = set_csc,
		.isp_set_expr = set_expr,
		.isp_get_exposure = get_exposure,
		.isp_get_awb_ct_attr = get_ct,
		.isp_set_wb = set_wb,
		.enc_set_fps = set_encoder_rate,
	};
	uvcd_pipeline_t p = {.cam = {.ops = &ops, .sensor_w = 1920, .sensor_h = 1080}};
	startup_ops(&ops);
	p.cam.controls = &p.controls;
	p.controls.gamma = 100;
	p.controls.zoom = 100;
	p.controls.ae_mode = UVCD_AE_APERTURE_PRIORITY;
	p.controls.ae_comp = 128;
	p.controls.wb_auto = 1;
	p.controls.metering = UVCD_METERING_SUBJECT;
	weight_error = comp_error = 0;

	/* A control changed while the camera is down is only stored, for the
	 * bring-up to apply. */
	struct uvcd_control_state next = p.controls;
	next.metering = UVCD_METERING_CENTER;
	int writes = weight_writes;
	assert(uvcd_pipeline_apply(&p, &next, offsetof(struct uvcd_control_state, metering)) == 0);
	assert(weight_writes == writes);

	/* Up: idle rate and the default output's view, metered at once. */
	assert(uvcd_camera_up(&p.cam) == 0 && p.cam.up && inits == 1);
	assert(uvcd_camera_up(&p.cam) == 0 && inits == 1);
	assert(sensor_rate == UVCD_IDLE_FPS && p.cam.view_frame == UVCD_IDLE_FRAME);
	assert(p.cam.view.w == 1920 && p.cam.view.h == 1080);
	assert(p.cam.metering_valid && weight_writes == writes + 1);

	/* Subject metering runs with no stream: the idle camera meters the
	 * scene the next stream will start on. */
	fake_scene(400, 175);
	for (int i = 0; i < 8; i++) {
		p.cam.subject_checked_ms = 0;
		uvcd_pipeline_tick(&p);
	}
	assert(written_weight[9][4] == 8);
	uint8_t learned[15][15];
	memcpy(learned, written_weight, sizeof(learned));

	/* A 720p stream at 15 fps sees the same 16:9 view: the rate follows
	 * the lease, and the learned weights stay. */
	writes = weight_writes;
	assert(uvcd_camera_acquire(&p.cam, UVCD_FRAME_720P, 15) == 0);
	assert(sensor_rate == 15 && p.cam.metering_valid);
	uvcd_camera_stream_on(&p.cam, UVCD_FMT_H264);
	assert(csc_mode == UVCD_CSC_BT709_CLIP);
	assert(weight_writes == writes && memcmp(written_weight, learned, sizeof(learned)) == 0);
	/* The view of a running stream is its channel's crop: only the stream
	 * may move it. */
	next = p.controls;
	next.zoom = 200;
	assert(uvcd_camera_apply(&p.cam, &next, offsetof(struct uvcd_control_state, zoom)) ==
	       -EBUSY);

	/* Exposure priority slows a stream that runs out of frame time, and
	 * the encoder follows the sensor. */
	p.configured = true;
	p.enc_fps = 15;
	p.controls.ae_mode = UVCD_AE_MANUAL;
	p.controls.ae_priority = 1;
	p.controls.exposure_time = 900; /* 90 ms: no listed rate fits, floor 15 */
	uvcd_camera_stream_on(&p.cam, UVCD_FMT_H264);
	assert(sensor_rate == 15 && last_expr.mode == 1 && last_expr.time == 65535);
	p.cam.lease_fps = 30;
	uvcd_camera_stream_on(&p.cam, UVCD_FMT_H264);
	assert(sensor_rate == 15); /* 30 -> 25 -> 20 -> 15, the floor */
	p.enc_fps = 30;
	uvcd_pipeline_tick(&p);
	assert(encoder_rate == 15 && p.enc_fps == 15);
	p.configured = false;
	p.controls.ae_mode = UVCD_AE_APERTURE_PRIORITY;
	p.controls.ae_priority = 0;

	/* Released, the camera goes back to the idle rate. */
	uvcd_camera_release(&p.cam);
	assert(!p.cam.leased && sensor_rate == UVCD_IDLE_FPS);

	/* A 4:3 stream crops the sides: a new view, metered afresh. */
	writes = weight_writes;
	assert(uvcd_camera_acquire(&p.cam, UVCD_FRAME_640X480, 30) == 0);
	assert(p.cam.view.w == 1440 && p.cam.view.x == 240 && !p.cam.metering_valid);
	uvcd_camera_stream_on(&p.cam, UVCD_FMT_MJPEG);
	assert(csc_mode == UVCD_CSC_BT601_FULL && p.cam.metering_valid);
	assert(weight_writes == writes + 1);

	/* Released: back to the idle rate and view. */
	writes = weight_writes;
	uvcd_camera_release(&p.cam);
	assert(!p.cam.leased && sensor_rate == UVCD_IDLE_FPS);
	assert(p.cam.view.w == 1920 && p.cam.metering_valid && weight_writes == writes + 1);
	uvcd_camera_release(&p.cam); /* nothing held: nothing happens */
	assert(weight_writes == writes + 1);

	/* Idle zoom moves the camera's own view, and metering with it (at the
	 * K1's ISP clock, where 1080p takes 2x). */
	p.cam.isp_clk_hz = UVCD_ISP_FAST_HZ;
	next = p.controls;
	next.zoom = 200;
	writes = weight_writes;
	assert(uvcd_pipeline_apply(&p, &next, offsetof(struct uvcd_control_state, zoom)) == 0);
	assert(p.cam.view.w == 960 && p.cam.view.x == 480 && weight_writes == writes + 1);
	p.controls = next;
	/* A zoom that leaves the view where it is changes nothing. */
	next.pan = 0;
	writes = weight_writes;
	assert(uvcd_pipeline_apply(&p, &next, offsetof(struct uvcd_control_state, pan)) == 0);
	assert(weight_writes == writes);

	/* Leaving an automatic mode keeps what it measured, clamped to the
	 * control's range. */
	next = p.controls;
	next.wb_auto = 0;
	assert(uvcd_camera_apply(&p.cam, &next, offsetof(struct uvcd_control_state, wb_auto)) == 0);
	assert(next.wb_temp == UVCD_WB_TEMP_MAX && last_wb.mode == RSS_WB_MANUAL);
	next = p.controls;
	next.ae_mode = UVCD_AE_MANUAL;
	assert(uvcd_camera_apply(&p.cam, &next, offsetof(struct uvcd_control_state, ae_mode)) == 0);
	assert(next.exposure_time == 123 && last_expr.mode == 1 && last_expr.time == 12300);
	/* Idle runs the idle rate even for a long manual exposure. */
	next.exposure_time = 500;
	next.ae_priority = 1;
	assert(uvcd_camera_apply(&p.cam, &next,
				 offsetof(struct uvcd_control_state, exposure_time)) == 0);
	assert(sensor_rate == UVCD_IDLE_FPS && last_expr.time == 1000000 / UVCD_IDLE_FPS);
	/* Encoder settings are not the camera's. */
	int exprs = expr_writes;
	next.mjpeg_quality = 50;
	assert(uvcd_camera_apply(&p.cam, &next,
				 offsetof(struct uvcd_control_state, mjpeg_quality)) == 0);
	assert(expr_writes == exprs);
}


/* Idle pacing: full rate until brightness and exposure hold still, slow
 * after, full again when the scene changes. The fake ISP reports what a
 * scene and an exposure would give. */
static uint32_t it_limit_set, it_limit_now, fake_time = 30000, fake_gain = 2048;
static int it_limit_error, rate_sets;

static int count_rate(void *ctx, uint32_t num, uint32_t den)
{
	rate_sets++;
	/* Only a new rate brings the SDK's limit for it (1125 lines a frame at
	 * 30 fps on the GC2083, less 8). */
	if (num != sensor_rate)
		it_limit_now = 1125 * 30 / num - 8;
	return set_sensor_rate(ctx, num, den);
}

/* The SDK's own limit follows the rate it was last set to; idle's cap
 * stays until the rate changes. */
static int get_it_limit(void *ctx, uint32_t *limit)
{
	(void)ctx;
	*limit = it_limit_now;
	return 0;
}

static int set_it_limit(void *ctx, uint32_t limit)
{
	(void)ctx;
	if (it_limit_error)
		return it_limit_error;
	it_limit_set = it_limit_now = limit;
	return 0;
}

static int get_scene_exposure(void *ctx, rss_exposure_t *e)
{
	(void)ctx;
	memset(e, 0, sizeof(*e));
	e->exposure_time = fake_time;
	e->total_gain = fake_gain;
	return 0;
}

static int set_level(void *ctx, int value)
{
	(void)ctx;
	(void)value;
	return 0;
}

static void flat_scene(uint32_t level)
{
	for (int r = 0; r < 15; r++)
		for (int col = 0; col < 15; col++)
			fake_zone[r][col] = level;
}

/* `n` idle ticks, each one sampling period apart. */
static void idle_ticks(uvcd_camera_t *cam, int n)
{
	for (int i = 0; i < n; i++) {
		cam->idle_checked_ms -= UVCD_IDLE_PERIOD_MS;
		cam->settle_since_ms -= UVCD_IDLE_PERIOD_MS;
		uvcd_camera_tick(cam);
	}
}

static void test_idle_pacing(void)
{
	rss_hal_ops_t ops = {
		.init = hal_init,
		.isp_set_sensor_fps = count_rate,
		.isp_get_ae_it_max = get_it_limit,
		.isp_set_ae_it_max = set_it_limit,
		.isp_set_ae_weight = set_weight,
		.isp_get_ae_zone = get_zone,
		.isp_set_expr = set_expr,
		.isp_get_exposure = get_scene_exposure,
		.isp_set_ae_comp = set_comp,
		.isp_set_brightness = set_level,
	};
	startup_ops(&ops);
	struct uvcd_control_state controls = {0};
	controls.gamma = 100;
	controls.wb_auto = 1;
	controls.zoom = 100;
	controls.ae_mode = UVCD_AE_APERTURE_PRIORITY;
	controls.ae_comp = 128;
	controls.metering = UVCD_METERING_CENTER;
	uvcd_camera_t cam = {.ops = &ops, .sensor_w = 1920, .sensor_h = 1080,
			     .controls = &controls};
	weight_error = comp_error = it_limit_error = 0;
	flat_scene(40);
	sensor_rate = 25; /* the GC2083 driver boots at 25 fps */
	it_limit_now = 1125 * 30 / 25 - 8;

	/* Bring-up runs full rate and reads the limit that rate gives. */
	assert(uvcd_camera_up(&cam) == 0);
	assert(sensor_rate == UVCD_IDLE_FPS && cam.it_max_idle == 1117 && !cam.idle_settled);
	/* Not before AE has run ... */
	fake_time = 0;
	idle_ticks(&cam, 6);
	assert(!cam.idle_settled && sensor_rate == UVCD_IDLE_FPS);
	/* ... and not while the exposure still moves. */
	fake_time = 30000;
	for (int i = 0; i < 6; i++) {
		fake_gain = 2048 + (uint32_t)i * 200;
		idle_ticks(&cam, 1);
	}
	assert(!cam.idle_settled);
	/* Held still: slow, with the full-rate exposure limit. */
	idle_ticks(&cam, 3);
	assert(cam.idle_settled && sensor_rate == UVCD_IDLE_SLOW_FPS && it_limit_set == 1117);
	assert(cam.it_capped && cam.it_max_slow == 6742);

	/* One flickering frame is not a scene change: the rate never moves. */
	int flicker_sets = rate_sets;
	flat_scene(80);
	idle_ticks(&cam, 1);
	flat_scene(40);
	idle_ticks(&cam, 4);
	assert(cam.idle_settled && sensor_rate == UVCD_IDLE_SLOW_FPS && rate_sets == flicker_sets);
	/* The lights going off is: full rate, the SDK's own limit back. */
	flat_scene(4);
	idle_ticks(&cam, 4);
	assert(!cam.idle_settled && sensor_rate == UVCD_IDLE_FPS && !cam.it_capped);
	/* In the dark AE pins at its limits and the exposure stays put: that
	 * is settled too, not a reason to stay at full rate. */
	idle_ticks(&cam, 4);
	assert(cam.idle_settled && sensor_rate == UVCD_IDLE_SLOW_FPS);
	int sets = rate_sets;
	idle_ticks(&cam, 20);
	assert(cam.idle_settled && rate_sets == sets);
	/* AE moving on its own counts as a change too. */
	fake_gain = 4096;
	idle_ticks(&cam, 4);
	assert(!cam.idle_settled);
	idle_ticks(&cam, 4);
	assert(cam.idle_settled);

	/* A control that changes what AE does settles again at once. */
	struct uvcd_control_state next = controls;
	next.ae_comp = 200;
	assert(uvcd_camera_apply(&cam, &next, offsetof(struct uvcd_control_state, ae_comp)) == 0);
	assert(!cam.idle_settled && sensor_rate == UVCD_IDLE_FPS);
	controls = next;
	/* One that doesn't, doesn't. */
	idle_ticks(&cam, 4);
	assert(cam.idle_settled);
	next.brightness = 90;
	assert(uvcd_camera_apply(&cam, &next, offsetof(struct uvcd_control_state, brightness)) == 0);
	assert(cam.idle_settled && sensor_rate == UVCD_IDLE_SLOW_FPS);

	/* A 5 fps stream after slow idle gets the SDK's limit for 5 fps back,
	 * not idle's, although the rate stays what it was. */
	sets = rate_sets;
	assert(uvcd_camera_acquire(&cam, UVCD_FRAME_1080P, UVCD_IDLE_SLOW_FPS) == 0);
	assert(rate_sets == sets && !cam.it_capped && it_limit_now == 6742);
	idle_ticks(&cam, 10); /* streams aren't paced */
	assert(rate_sets == sets);
	/* A stream at another rate gets it from the rate change. */
	uvcd_camera_release(&cam);
	idle_ticks(&cam, 4);
	assert(cam.it_capped && it_limit_now == 1117);
	assert(uvcd_camera_acquire(&cam, UVCD_FRAME_1080P, 15) == 0);
	assert(!cam.it_capped && it_limit_now == 2242);
	/* Released: settle at full rate again. */
	uvcd_camera_release(&cam);
	assert(!cam.idle_settled && sensor_rate == UVCD_IDLE_FPS);

	/* Manual exposure has nothing to settle. */
	controls.ae_mode = UVCD_AE_MANUAL;
	controls.exposure_time = 100;
	uvcd_camera_tick(&cam);
	assert(cam.idle_settled && sensor_rate == UVCD_IDLE_SLOW_FPS);
	controls.ae_mode = UVCD_AE_APERTURE_PRIORITY;
	uvcd_camera_release(&cam);
	cam.idle_settled = false;
	apply_exposure(&cam, &controls);

	/* An ISP that refuses the limit keeps idle at full rate for good. */
	it_limit_error = -EIO;
	idle_ticks(&cam, 4);
	assert(!cam.it_max_idle && sensor_rate == UVCD_IDLE_FPS);
	idle_ticks(&cam, 10);
	assert(sensor_rate == UVCD_IDLE_FPS);
	it_limit_error = 0;
}

/* The stream's channel takes its crop from the camera's view, which a
 * lease sets for the output size. */
static int configure(uvcd_pipeline_t *p, uint8_t format, uint8_t frame, uint32_t interval)
{
	assert(uvcd_camera_acquire(&p->cam, frame, uvcd_interval_fps(interval)) == 0);
	return configure_channel(p, format, frame, interval);
}

int main(void)
{
	rss_hal_ops_t ops = {
		.isp_set_sensor_fps = set_sensor_rate,
		.fs_create_channel = create_fs,
		.fs_destroy_channel = destroy,
		.fs_set_fifo = depth_ok,
		.fs_set_frame_depth = depth_ok,
		.enc_create_group = channel_ok,
		.enc_destroy_group = destroy,
		.enc_set_stream_buf_size = set_size,
		.enc_set_max_stream_cnt = set_count,
		.enc_create_channel = create_enc,
	};
	uvcd_pipeline_t p = {.cam = {.ops = &ops, .sensor_w = 1920, .sensor_h = 1080, .up = true}};
	p.cam.controls = &p.controls;
	p.controls.mjpeg_quality = 80;
	p.controls.h264_min_qp = p.controls.h264_max_qp = -1;
	for (int frame = 1; frame <= UVCD_NUM_FRAMES; frame++) {
		const struct uvcd_frame_info *fi = &uvcd_frames[frame];
		/* The pump's copy holds any frame the gadget can send, and the
		 * gadget takes about a byte per pixel. */
		assert(fi->max_size <= UVCD_FRAME_BUF_CAP);
		assert(fi->max_size >= (uint32_t)fi->width * fi->height);
		assert(fi->max_zoom >= UVCD_ZOOM_MIN && fi->max_zoom <= UVCD_ZOOM_MAX);
		assert(fi->max_zoom_slow >= UVCD_ZOOM_MIN && fi->max_zoom_slow <= fi->max_zoom);
	}
	/* 0: the clock couldn't be read, which must get the slow caps. */
	static const int clocks[] = {0, 100000000, UVCD_ISP_FAST_HZ, 250000000};
	for (size_t ci = 0; ci < sizeof(clocks) / sizeof(clocks[0]); ci++) {
		p.cam.isp_clk_hz = clocks[ci];
		const struct crop *proven = p.cam.isp_clk_hz >= UVCD_ISP_FAST_HZ ? proven_fast : proven_slow;
		int smallest_w = 1920;
		for (int frame = 1; frame <= UVCD_NUM_FRAMES; frame++) {
			for (int zoom = 100; zoom <= 400; zoom += 25) {
				for (int pan = -UVCD_PANTILT_MAX; pan <= UVCD_PANTILT_MAX;
				     pan += UVCD_PANTILT_MAX) {
					p.controls.zoom = zoom;
					p.controls.pan = pan;
					p.controls.tilt = -pan;
					assert(configure(&p, UVCD_FMT_MJPEG, frame, 333333) == -EIO);
					assert(!fs.crop.enable);
					assert(fs.width == uvcd_frames[frame].width);
					assert(fs.height == uvcd_frames[frame].height);
					if (fs.fcrop.enable) {
						assert(fs.fcrop.w > 0 && fs.fcrop.h > 0);
						assert(fs.fcrop.x + fs.fcrop.w <= 1920);
						assert(fs.fcrop.y + fs.fcrop.h <= 1080);
					}
					/* Never a smaller crop, i.e. more upscale, than was
					 * seen streaming on the K1 at this output size. */
					int cw = fs.fcrop.enable ? fs.fcrop.w : 1920;
					int ch = fs.fcrop.enable ? fs.fcrop.h : 1080;
					assert(cw >= proven[frame].w && ch >= proven[frame].h);
					if (frame == UVCD_FRAME_1080P && cw < smallest_w)
						smallest_w = cw;
					assert(stream_size >= fs.width * fs.height * 4);
					assert(stream_count == 1);
					assert(enc.init_qp == 80);
				}
			}
		}
		/* The fast clock really does zoom further at 1080p. */
		assert(p.cam.isp_clk_hz >= UVCD_ISP_FAST_HZ ? smallest_w < 700 : smallest_w > 1400);
	}
	p.cam.isp_clk_hz = 0;
	p.controls.zoom = 100;
	p.controls.pan = p.controls.tilt = 0;
	assert(configure(&p, UVCD_FMT_MJPEG, UVCD_FRAME_1080P, 333333) == -EIO);
	assert(!fs.fcrop.enable && !fs.scaler.enable); /* Preserve all 1080 rows. */
	assert(configure(&p, UVCD_FMT_MJPEG, UVCD_FRAME_1280X960, 333333) == -EIO);
	assert(fs.fcrop.x == 240 && fs.fcrop.y == 0);
	assert(fs.fcrop.w == 1440 && fs.fcrop.h == 1080);
	/* The requested JPEG quality goes to the encoder as is. */
	p.controls.mjpeg_quality = 100;
	assert(configure(&p, UVCD_FMT_MJPEG, UVCD_FRAME_720P, 333333) == -EIO);
	assert(enc.init_qp == 100);
	p.controls.mjpeg_quality = 80;
	assert(configure(&p, UVCD_FMT_H264, UVCD_FRAME_1080P, 333333) == -EIO);
	assert(enc.gop_length == 30 && enc.max_same_scene_cnt == 1);
	assert(stream_size == uvcd_frames[UVCD_FRAME_1080P].max_size && stream_count == 2);
	/* An IDR every second at whatever rate was negotiated. */
	assert(configure(&p, UVCD_FMT_H264, UVCD_FRAME_720P, 666666) == -EIO);
	assert(enc.gop_length == 15 && enc.max_same_scene_cnt == 1);
	assert(configure(&p, UVCD_FMT_H264, UVCD_FRAME_720P, 2000000) == -EIO);
	assert(enc.gop_length == 5 && enc.max_same_scene_cnt == 1);
	assert(configure(&p, UVCD_FMT_H264, UVCD_FRAME_720P, 333333) == -EIO);
	/* Automatic min QP: 20, unless an explicit max QP is below it. */
	assert(enc.min_qp == UVCD_H264_AUTO_MIN_QP && enc.max_qp == -1);
	p.controls.h264_max_qp = 12;
	assert(configure(&p, UVCD_FMT_H264, UVCD_FRAME_720P, 333333) == -EIO);
	assert(enc.min_qp == 12 && enc.max_qp == 12);
	p.controls.h264_min_qp = 30;
	p.controls.h264_max_qp = 40;
	assert(configure(&p, UVCD_FMT_H264, UVCD_FRAME_720P, 333333) == -EIO);
	assert(enc.min_qp == 30 && enc.max_qp == 40);
	p.controls.h264_min_qp = p.controls.h264_max_qp = -1;
	int calls = encoder_calls;
	destroyed = 0;
	buffer_error = -ENOMEM;
	assert(configure(&p, UVCD_FMT_MJPEG, UVCD_FRAME_720P, 333333) == -ENOMEM);
	assert(encoder_calls == calls && destroyed == 2);
	for (uint32_t ceiling = 5; ceiling <= 30; ceiling += 5) {
		uint32_t fps = ceiling;
		for (int i = 0; i < 10; i++)
			fps = fps_step(fps, -1, ceiling);
		assert(fps == (ceiling < 15 ? ceiling : 15));
	}
	test_subject_metering();
	test_metering_exit_failure();
	test_camera_lease();
	test_idle_pacing();
	test_control_failures();
	puts("pipeline regression tests passed");
	return 0;
}
