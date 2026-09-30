#include <assert.h>
#include <stdarg.h>
#define ioctl test_ioctl
#define unlink test_unlink
#include "../src/uvcd_gadget.c"

static int saves, good_saves, queued, idrs, restarts;
static struct uvcd_control_state last_saved, last_good;
static int unlinks, streak_writes, last_streak;

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
	return true;
}

int uvcd_enc_request_keyframe(uvcd_pipeline_t *p)
{
	(void)p;
	idrs++;
	return 0;
}

#define STUB_APPLY(name) int name(uvcd_pipeline_t *p, const struct uvcd_control_state *c) \
	{ (void)p; (void)c; return 0; }
int uvcd_apply_gamma(uvcd_pipeline_t *p, int v) { (void)p; (void)v; return 0; }
int uvcd_apply_wb(uvcd_pipeline_t *p, int v, int a) { (void)p; (void)v; (void)a; return 0; }
int uvcd_awb_current_ct(uvcd_pipeline_t *p) { (void)p; return -1; }
int uvcd_live_exposure(uvcd_pipeline_t *p) { (void)p; return -1; }
int uvcd_enc_apply_bitrate(uvcd_pipeline_t *p) { (void)p; return 0; }
int uvcd_enc_apply_gop(uvcd_pipeline_t *p) { (void)p; return 0; }
STUB_APPLY(uvcd_apply_metering)
STUB_APPLY(uvcd_apply_rotation)
STUB_APPLY(uvcd_apply_exposure)
STUB_APPLY(uvcd_apply_view)

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
	assert(streak_writes == 0 && unlinks == 0); /* ...and idle is never a crash. */

	/* A stream is counted in the crash guard from its start, once, on top
	 * of the streams before it that died unproven. */
	p.unproven_streak = 1;
	stream_guard_arm(&g);
	stream_guard_arm(&g);
	assert(streak_writes == 1 && last_streak == 2 && g.guard_armed);

	g.streaming = true;
	g.healthy_since_ms = monotonic_ms() - 9000;
	g.last_frame_ms = monotonic_ms();
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
	 * crash falls back to. */
	assert(uvcd_ctrl_set(&g, brightness, 160, &applied) == 0);
	assert(g.guard_armed && streak_writes == 2 && last_streak == 1);
	assert(g.healthy_since_ms == 0 && p.good.brightness == 150);
	g.healthy_since_ms = monotonic_ms() - UVCD_CONFIG_HEALTHY_MS;
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
	g.stalled_since_ms = monotonic_ms() - 10000;
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
	assert(uvcd_ctrl_find("spatial-denoise")->def == 192);

	/* Factory reset is written at once, and replaces the proven controls
	 * so a later crash cannot bring back what it threw away. */
	g.streaming = false;
	int was_saves = saves, was_good = good_saves;
	uvcd_ctrl_reset(&g);
	assert(saves == was_saves + 1 && last_saved.brightness == 128 && !g.config_dirty);
	assert(good_saves == was_good + 1 && last_good.brightness == 128);
	assert(p.good.brightness == 128 && p.controls.brightness == 128);

	/* The retired explicit-save selectors answer like any unknown one. */
	for (unsigned sel = 23; sel <= 25; sel++)
		assert(custom_ctrl_def(sel) == NULL && custom_control_value(&g, sel) == NULL);
	puts("gadget regression tests passed");
	pthread_mutex_destroy(&p.frame.lock);
	return 0;
}
