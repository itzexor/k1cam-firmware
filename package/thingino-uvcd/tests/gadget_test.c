#include <assert.h>
#include <stdarg.h>
#define ioctl test_ioctl
#define unlink test_unlink
#include "../src/uvcd_gadget.c"

static int saves, queued, idrs, restarts;

int test_unlink(const char *path)
{
	(void)path;
	return 0;
}
static bool host_reading;

void uvcd_log(int level, const char *fmt, ...)
{
	(void)level;
	(void)fmt;
}

int uvcd_config_save(const char *path, const struct uvcd_control_state *c)
{
	(void)path;
	(void)c;
	saves++;
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
	config_mark_dirty(&g);
	g.config_dirty_at -= 20000;
	config_flush(&g);
	assert(saves == 0 && g.config_dirty); /* Idle values never persist. */
	g.streaming = true;
	g.healthy_since_ms = monotonic_ms() - 9000;
	g.last_frame_ms = monotonic_ms();
	config_flush(&g);
	assert(saves == 0);
	g.healthy_since_ms -= 2000;
	config_flush(&g);
	assert(saves == 1 && !g.config_dirty);
	config_mark_dirty(&g);
	assert(g.healthy_since_ms == 0);
	config_flush(&g);
	assert(saves == 1); /* A change invalidates the previous health window. */

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

	g.cur_format = UVCD_FMT_MJPEG;
	p.controls.mjpeg_quality = 80;
	p.frame.jpeg_near_limit = true;
	deliver_frame(&g);
	assert(restarts == 1 && p.mjpeg_quality_limit == 75);
	assert(p.controls.mjpeg_quality == 80 && g.read_seq == 0);
	assert(g.healthy_since_ms == 0);
	assert(uvcd_ctrl_find("power-line-frequency")->def == 0);
	assert(uvcd_ctrl_find("spatial-denoise")->def == 192);
	puts("gadget regression tests passed");
	pthread_mutex_destroy(&p.frame.lock);
	return 0;
}
