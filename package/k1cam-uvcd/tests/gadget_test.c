#include <assert.h>
#include <stdarg.h>
#define ioctl test_ioctl
#define unlink test_unlink
#include "../src/uvcd_gadget.c"
#include "../src/uvcd_ctl.c"

static int saves, good_saves, queued, idrs, restarts;
static int restart_errors, start_errors, start_calls, bitrate_errors, bitrate_calls;
static struct uvcd_control_state restarted_controls;
static struct uvcd_control_state last_saved, last_good;
static int unlinks, streak_writes, last_streak, save_errors;

int test_unlink(const char *path)
{
	(void)path;
	unlinks++;
	return 0;
}

int uvcd_config_streak_write(const char *path, int n)
{
	(void)path;
	streak_writes++;
	last_streak = n;
	return 0;
}
static bool host_reading;

void uvcd_log(int level, const char *fmt, ...)
{
	(void)level;
	(void)fmt;
}

void uvcd_config_defaults(struct uvcd_control_state *out)
{
	memset(out, 0, sizeof(*out));
	for (size_t i = 0; i < UVCD_CTRL_DEF_COUNT; i++)
		*uvcd_ctrl_field(out, &uvcd_ctrl_defs[i]) = uvcd_ctrl_defs[i].def;
}

int uvcd_config_save(const char *path, const struct uvcd_control_state *c)
{
	if (save_errors) { save_errors--; errno = ENOSPC; return -1; }
	if (!strcmp(path, UVCD_CONFIG_GOOD_PATH)) {
		good_saves++;
		last_good = *c;
	} else {
		assert(!strcmp(path, UVCD_CONFIG_PATH));
		saves++;
		last_saved = *c;
	}
	return 0;
}

bool uvcd_pipeline_restart_encoder(uvcd_pipeline_t *p, uint8_t format)
{
	(void)format;
	restarts++;
	p->frame.seq = 0;
	if (restart_errors) {
		restart_errors--;
		p->running = false;
		return false;
	}
	return true;
}

int uvcd_pipeline_start(uvcd_pipeline_t *p, uint8_t format, uint8_t frame, uint32_t interval)
{
	assert(format == p->cur_format && frame == p->cur_frame && interval == p->cur_interval);
	start_calls++;
	restarted_controls = p->controls;
	p->frame.seq = 0;
	if (start_errors) {
		start_errors--;
		return -EIO;
	}
	p->running = true;
	return 0;
}

int uvcd_enc_request_keyframe(uvcd_pipeline_t *p)
{
	(void)p;
	idrs++;
	return 0;
}

int uvcd_camera_live_exposure(uvcd_camera_t *cam) { (void)cam; return -1; }
int uvcd_enc_apply_bitrate(uvcd_pipeline_t *p)
{
	bitrate_calls++;
	restarted_controls = p->controls;
	if (bitrate_errors) {
		bitrate_errors--;
		return -EIO;
	}
	return 0;
}

/* The camera, as the gadget sees it: it takes or rejects a change, and may
 * fill in a dependent value (leaving auto white balance keeps the measured
 * temperature). */
static int applies, apply_error, camera_guard_arms;
static bool apply_resets_sequence;
static size_t last_field;
int uvcd_pipeline_apply(uvcd_pipeline_t *p, struct uvcd_control_state *next, size_t field)
{
	if (apply_resets_sequence)
		p->frame.seq = 0; /* restored stream after a rejected view change */
	applies++;
	last_field = field;
	if (apply_error)
		return apply_error;
	if (field == offsetof(struct uvcd_control_state, wb_auto) && !next->wb_auto)
		next->wb_temp = 3300;
	return 0;
}

void uvcd_pipeline_camera_guard_arm(uvcd_pipeline_t *p)
{
	(void)p;
	camera_guard_arms++;
}

int test_ioctl(int fd, unsigned long request, ...)
{
	(void)fd;
	va_list ap;
	va_start(ap, request);
	struct v4l2_buffer *buf = va_arg(ap, struct v4l2_buffer *);
	va_end(ap);
	if (request == VIDIOC_DQBUF) {
		if (!host_reading) {
			errno = EAGAIN;
			return -1;
		}
		buf->index = 0;
		return 0;
	}
	assert(request == VIDIOC_QBUF);
	queued++;
	return 0;
}

static void test_rejected_view(void)
{
	static const char *names[] = {"zoom", "pan", "tilt"};
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		uvcd_pipeline_t p = {0};
		pthread_mutex_init(&p.frame.lock, NULL);
		uvcd_config_defaults(&p.controls);
		struct uvcd_control_state old = p.controls;
		uint8_t data[] = {1, 2, 3, 4}, output[4] = {0};
		p.frame.data = data;
		p.frame.len = sizeof(data);
		p.frame.seq = 1000;
		gadget_t g = {.pipe = &p, .streaming = true, .read_seq = 1000,
			     .watched_seq = 1000, .healthy_since_ms = 1,
			     .cur_format = UVCD_FMT_MJPEG, .buf_count = 1};
		g.buffers[0].start = output;
		g.buffers[0].length = sizeof(output);
		apply_error = -EIO;
		apply_resets_sequence = true;
		int applied = -123;
		assert(uvcd_ctrl_set(&g, uvcd_ctrl_find(names[i]), 200, &applied) == -1);
		assert(applied == -123 && !g.config_dirty);
		assert(memcmp(&old, &p.controls, sizeof(old)) == 0);
		assert(g.read_seq == 0 && g.watched_seq == 0 && g.healthy_since_ms == 0);
		assert(g.last_frame_ms > 0);
		/* The restored stream's very first frame can be delivered. */
		p.frame.seq = 1;
		int before = queued;
		deliver_frame(&g);
		assert(queued == before + 1 && g.read_seq == 1);
		assert(memcmp(data, output, sizeof(data)) == 0);
		pthread_mutex_destroy(&p.frame.lock);
	}
	apply_error = 0;
	apply_resets_sequence = false;
}

static void test_gain_headroom(void)
{
	uvcd_pipeline_t p = {0};
	gadget_t g = {.pipe = &p};
	uvcd_config_defaults(&p.controls);
	const struct uvcd_ctrl_def *again = uvcd_ctrl_find("max-analog-gain");
	const struct uvcd_ctrl_def *dgain = uvcd_ctrl_find("max-digital-gain");
	assert(custom_control_max(UVCD_CUSTOM_MAX_AGAIN) == 224);
	assert(custom_control_def(UVCD_CUSTOM_MAX_AGAIN) == 160);
	assert(custom_control_max(UVCD_CUSTOM_MAX_DGAIN) == 160);
	assert(p.controls.max_again == 160 && p.controls.max_dgain == 80);
	int applied;
	assert(uvcd_ctrl_set(&g, again, 224, &applied) == 0 && applied == 224);
	assert(p.controls.max_again == 224);
	g.config_dirty = false;
	apply_error = -EIO;
	assert(uvcd_ctrl_set(&g, again, 192, &applied) == -1);
	assert(p.controls.max_again == 224 && !g.config_dirty);
	apply_error = 0;
	assert(uvcd_ctrl_set(&g, again, 255, &applied) == 0 && applied == 224);
	assert(uvcd_ctrl_set(&g, dgain, 224, &applied) == 0 && applied == 160);
}

static void test_encoder_failures(void)
{
	static const char *names[] = {"h264-rate-control", "h264-min-qp", "h264-max-qp",
		"h264-profile", "mjpeg-quality"};
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		uvcd_pipeline_t p = {.running = true, .cur_format = i == 4 ? UVCD_FMT_MJPEG : UVCD_FMT_H264,
			.cur_frame = UVCD_FRAME_720P, .cur_interval = 333333};
		uvcd_config_defaults(&p.controls);
		struct uvcd_control_state old = p.controls;
		gadget_t g = {.pipe = &p, .streaming = true, .read_seq = 1000};
		restart_errors = 1;
		int starts = start_calls, applied = -123;
		assert(uvcd_ctrl_set(&g, uvcd_ctrl_find(names[i]), i == 4 ? 60 : 1, &applied) == -1);
		assert(p.running && start_calls == starts + 1 && !g.config_dirty);
		assert(applied == -123 && g.read_seq == 0);
		assert(memcmp(&old, &p.controls, sizeof(old)) == 0);
		assert(memcmp(&old, &restarted_controls, sizeof(old)) == 0);
	}
	uvcd_pipeline_t p = {.running = true, .cur_format = UVCD_FMT_H264};
	uvcd_config_defaults(&p.controls);
	gadget_t g = {.pipe = &p, .streaming = true};
	int calls = bitrate_calls, applied;
	bitrate_errors = 1;
	assert(uvcd_ctrl_set(&g, uvcd_ctrl_find("h264-bitrate-kbps"), 2000, &applied) == -1);
	assert(bitrate_calls == calls + 2 && restarted_controls.h264_bitrate_kbps == 0);
	assert(p.controls.h264_bitrate_kbps == 0 && !g.config_dirty);
	/* An inactive format is staged, and an unchanged value never restarts. */
	calls = restarts;
	p.cur_format = UVCD_FMT_MJPEG;
	assert(uvcd_ctrl_set(&g, uvcd_ctrl_find("h264-profile"), 0, &applied) == 0);
	assert(restarts == calls && p.controls.h264_profile == 0);
	assert(uvcd_ctrl_set(&g, uvcd_ctrl_find("mjpeg-quality"), 80, &applied) == 0);
	assert(restarts == calls);
	/* Crossed bounds are rejected before they reach the pipeline. */
	p.controls.h264_min_qp = 30;
	calls = applies;
	assert(uvcd_ctrl_set(&g, uvcd_ctrl_find("h264-max-qp"), 20, &applied) == -1);
	assert(applies == calls && p.controls.h264_max_qp == -1);

	char reply_buf[128];
	assert(uvcd_ctl_command(&g, "set brightness 9999999999999999999999999999", reply_buf, sizeof(reply_buf)) == 0);
	assert(!strcmp(reply_buf, "err bad value\n"));
	int before = good_saves;
	apply_error = -EIO;
	assert(uvcd_ctl_command(&g, "reset", reply_buf, sizeof(reply_buf)) == 0);
	assert(!strcmp(reply_buf, "err reset incomplete\n") && good_saves == before);
	apply_error = 0;
	p.running = false;
	save_errors = 1;
	assert(uvcd_ctrl_reset(&g) == -1 && good_saves == before);
}

int main(void)
{
	uvcd_pipeline_t p = {0};
	pthread_mutex_init(&p.frame.lock, NULL);
	gadget_t g = {.pipe = &p};

	/* Sharpness stops where frames still fit the USB cap. Backlight and
	 * defog are gone, and their requests stall like any unknown one. */
	assert(standard_ctrl_def(UVC_PU_SHARPNESS_CONTROL)->max == 180);
	assert(standard_ctrl_def(UVC_PU_BACKLIGHT_COMPENSATION_CONTROL) == NULL);
	assert(control_value(&g, UVC_PU_BACKLIGHT_COMPENSATION_CONTROL) == NULL);
	assert(custom_ctrl_def(8) == NULL && custom_control_value(&g, 8) == NULL);
	uvcd_config_defaults(&p.controls);
	p.good = p.controls;
	int applied;
	const struct uvcd_ctrl_def *brightness = uvcd_ctrl_find("brightness");

	/* Every change persists, idle ones too, once a burst has settled. */
	assert(uvcd_ctrl_set(&g, brightness, 140, &applied) == 0 && applied == 140);
	assert(uvcd_ctrl_set(&g, brightness, 150, &applied) == 0 && applied == 150);
	config_flush(&g, false);
	assert(saves == 0 && g.config_dirty);
	g.config_dirty_at -= UVCD_CONFIG_SETTLE_MS;
	config_flush(&g, false);
	assert(saves == 1 && last_saved.brightness == 150 && !g.config_dirty);
	config_flush(&g, true);
	assert(saves == 1); /* nothing pending */
	/* Rewriting a value it already has changes nothing. */
	assert(uvcd_ctrl_set(&g, brightness, 150, &applied) == 0 && applied == 150);
	assert(!g.config_dirty);
	/* Idle changes go to the camera-side guard in RAM, never the stream's
	 * on flash: an idle camera unplugged is not a crash. */
	assert(streak_writes == 0 && unlinks == 0);
	assert(camera_guard_arms == 2);

	/* A change the camera rejects is neither stored nor persisted. */
	int was_applies = applies;
	apply_error = -EIO;
	assert(uvcd_ctrl_set(&g, brightness, 99, &applied) == -1);
	apply_error = 0;
	assert(applies == was_applies + 1 && last_field == brightness->state_offset);
	assert(p.controls.brightness == 150 && !g.config_dirty);
	/* It was counted before it was applied: a crash while applying it is
	 * caught too. */
	assert(camera_guard_arms == 3);
	/* What the camera filled in is stored with the change. */
	const struct uvcd_ctrl_def *wb_auto = uvcd_ctrl_find("white-balance-auto");
	assert(uvcd_ctrl_set(&g, wb_auto, 0, &applied) == 0 && applied == 0);
	assert(p.controls.wb_auto == 0 && p.controls.wb_temp == 3300 && g.config_dirty);
	g.config_dirty_at -= UVCD_CONFIG_SETTLE_MS;
	config_flush(&g, false);
	assert(last_saved.wb_temp == 3300 && last_saved.wb_auto == 0);
	int arms = camera_guard_arms;

	/* A stream is counted in the crash guard from its start, once, on top
	 * of the streams before it that died unproven. */
	p.unproven_streak = 1;
	stream_guard_arm(&g);
	stream_guard_arm(&g);
	assert(streak_writes == 1 && last_streak == 2 && g.guard_armed);

	g.streaming = true;
	g.healthy_since_ms = uvcd_monotonic_ms() - 9000;
	g.last_frame_ms = uvcd_monotonic_ms();
	stream_guard_tick(&g);
	assert(good_saves == 0 && g.guard_armed && unlinks == 0);
	g.healthy_since_ms -= 2000;
	stream_guard_tick(&g);
	/* Proven healthy: the running controls become what a crash falls
	 * back to, the guard file goes and the streak resets. */
	assert(good_saves == 1 && last_good.brightness == 150 && p.good.brightness == 150);
	assert(!g.guard_armed && p.unproven_streak == 0 && unlinks == 1);
	stream_guard_clear(&g);
	assert(unlinks == 1); /* nothing armed, nothing to clear */
	stream_guard_tick(&g);
	assert(good_saves == 1); /* proven once is enough */

	/* A change mid-stream has to prove itself again before it is what a
	 * crash falls back to; a rewrite of the same value does not. */
	assert(uvcd_ctrl_set(&g, brightness, 150, &applied) == 0);
	assert(!g.guard_armed && streak_writes == 1);
	assert(uvcd_ctrl_set(&g, brightness, 160, &applied) == 0);
	assert(g.guard_armed && streak_writes == 2 && last_streak == 1);
	assert(camera_guard_arms == arms);
	assert(g.healthy_since_ms == 0 && p.good.brightness == 150);
	g.healthy_since_ms = uvcd_monotonic_ms() - UVCD_CONFIG_HEALTHY_MS;
	stream_guard_tick(&g);
	assert(good_saves == 2 && p.good.brightness == 160 && !g.guard_armed);
	/* ...and a clean stop before then does not prove it. */
	assert(uvcd_ctrl_set(&g, brightness, 170, &applied) == 0 && g.guard_armed);
	stream_guard_clear(&g);
	assert(good_saves == 2 && p.good.brightness == 160);
	g.config_dirty_at -= UVCD_CONFIG_SETTLE_MS;
	config_flush(&g, false);
	assert(last_saved.brightness == 170);

	uint8_t data[] = {1, 2, 3, 4}, output[4] = {0};
	p.frame.data = data;
	p.frame.len = sizeof(data);
	p.frame.seq = 1;
	g.cur_format = UVCD_FMT_H264;
	g.buf_count = 1;
	g.buf_queued[0] = true;
	g.buffers[0].start = output;
	g.buffers[0].length = sizeof(output);
	g.stalled_since_ms = uvcd_monotonic_ms() - 10000;
	deliver_frame(&g);
	assert(g.streaming && queued == 0 && g.read_seq == 0);
	host_reading = true;
	deliver_frame(&g);
	assert(g.streaming && queued == 1 && g.read_seq == 1 && idrs == 1);
	assert(memcmp(data, output, sizeof(data)) == 0);

	/* A frame bigger than the gadget buffer is dropped whole: never
	 * truncated, and never answered by lowering the quality. */
	uint8_t big[8] = {0};
	g.cur_format = UVCD_FMT_MJPEG;
	p.controls.mjpeg_quality = 80;
	p.frame.data = big;
	p.frame.len = sizeof(big);
	p.frame.seq = 2;
	int was_queued = queued, was_idrs = idrs;
	deliver_frame(&g);
	assert(queued == was_queued && idrs == was_idrs + 1 && restarts == 0);
	assert(g.read_seq == 2 && p.controls.mjpeg_quality == 80);
	assert(g.healthy_since_ms == 0);
	/* The next frame that fits goes out on the buffer the drop kept. */
	p.frame.data = data;
	p.frame.len = sizeof(data);
	p.frame.seq = 3;
	deliver_frame(&g);
	assert(queued == was_queued + 1 && g.read_seq == 3);
	assert(uvcd_ctrl_find("power-line-frequency")->def == 0);
	assert(uvcd_ctrl_find("spatial-denoise")->def == 0);

	/* Factory reset is written at once, and replaces the proven controls
	 * so a later crash cannot bring back what it threw away. Even when
	 * nothing differs from factory, it leaves the file at factory. */
	g.streaming = false;
	int was_saves = saves, was_good = good_saves;
	uvcd_ctrl_reset(&g);
	assert(saves == was_saves + 1 && last_saved.brightness == 128 && !g.config_dirty);
	assert(good_saves == was_good + 1 && last_good.brightness == 128);
	assert(p.good.brightness == 128 && p.controls.brightness == 128);
	uvcd_ctrl_reset(&g);
	assert(saves == was_saves + 2 && good_saves == was_good + 2);

	/* The retired explicit-save and GOP selectors answer like any unknown
	 * one, and a saved GOP no longer names a control. */
	for (unsigned sel = 23; sel <= 25; sel++)
		assert(custom_ctrl_def(sel) == NULL && custom_control_value(&g, sel) == NULL);
	assert(custom_ctrl_def(15) == NULL && custom_control_value(&g, 15) == NULL);
	assert(uvcd_ctrl_find("h264-gop-frames") == NULL);
	test_rejected_view();
	test_gain_headroom();
	test_encoder_failures();
	puts("gadget regression tests passed");
	pthread_mutex_destroy(&p.frame.lock);
	return 0;
}
