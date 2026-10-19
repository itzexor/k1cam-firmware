/* Exercise socket reply framing without a camera or a running daemon. */
#include <assert.h>
#include <stdarg.h>
#define main uvcdctl_program_main
#define socket test_socket
#define connect test_connect
#define read test_read
#define close test_close
#define dprintf test_dprintf
#define ioctl test_ioctl
#define open test_open
#include "../uvcdctl/uvcdctl.c"
#undef main

static const char **chunks;
static size_t chunk_index;

int test_socket(int domain, int type, int protocol)
{
	assert(domain == AF_UNIX && type == SOCK_STREAM && protocol == 0);
	return 7;
}
int test_connect(int fd, const struct sockaddr *addr, socklen_t len)
{
	(void)addr;
	(void)len;
	assert(fd == 7);
	return 0;
}
int test_close(int fd) { assert(fd == 7); return 0; }
int test_dprintf(int fd, const char *fmt, ...)
{
	(void)fmt;
	assert(fd == 7);
	return 1;
}
ssize_t test_read(int fd, void *buf, size_t size)
{
	assert(fd == 7);
	const char *chunk = chunks[chunk_index];
	if (!chunk)
		return 0;
	size_t n = strlen(chunk);
	assert(n <= size);
	memcpy(buf, chunk, n); /* deliberately no NUL terminator */
	chunk_index++;
	return (ssize_t)n;
}

static int current_qp = 30, get_error;
int test_open(const char *path, int flags, ...)
{
	(void)flags;
	assert(!strcmp(path, "/fake"));
	return 7;
}
int test_ioctl(int fd, unsigned long request, ...)
{
	assert(fd == 7);
	va_list ap;
	va_start(ap, request);
	void *arg = va_arg(ap, void *);
	va_end(ap);
	if (request == VIDIOC_QUERYCTRL) {
		struct v4l2_queryctrl *q = arg;
		q->minimum = -36000;
		q->maximum = 36000;
		q->step = 3600;
		return 0;
	}
	if (request == VIDIOC_S_CTRL || request == VIDIOC_G_CTRL) {
		((struct v4l2_control *)arg)->value = 0;
		return 0;
	}
	assert(request == UVCIOC_CTRL_QUERY);
	struct uvc_xu_control_query *q = arg;
	int v = 0;
	switch (q->query) {
	case UVC_GET_MIN: v = -1; break;
	case UVC_GET_MAX: v = 51; break;
	case UVC_GET_DEF: v = -1; break;
	case UVC_SET_CUR: return 0; /* USB ACK, even if the daemon rejects it */
	case UVC_GET_CUR:
		if (get_error) { errno = EIO; return -1; }
		v = current_qp;
		break;
	default: assert(false);
	}
	q->data[0] = (uint8_t)v;
	q->data[1] = (uint8_t)((uint16_t)v >> 8);
	return 0;
}

static void check(const char **reply_chunks, int expected)
{
	chunks = reply_chunks;
	chunk_index = 0;
	assert(socket_command("set brightness 128") == expected);
}

int main(void)
{
	const char *fragmented[] = {"128\n", "o", "k\n", NULL};
	const char *failed[] = {"err ", "apply failed\n", NULL};
	const char *incomplete[] = {"128\no", NULL};
	const char *embedded[] = {"notok\n", NULL};
	const char *trailing_error[] = {"ok\n", "err apply failed\n", NULL};
	check(fragmented, 0);
	check(failed, 1);
	check(incomplete, 1);
	check(embedded, 1);
	check(trailing_error, 1);
	char *args[] = {"uvcdctl", "--device", "/fake", "set", "h264-max-qp", "20", NULL};
	assert(uvcdctl_program_main(6, args) == 1); /* readback differs from requested */
	current_qp = 20;
	assert(uvcdctl_program_main(6, args) == 0);
	get_error = 1;
	assert(uvcdctl_program_main(6, args) == 1); /* no optimistic fallback */
	get_error = 0;
	args[4] = "pan";
	args[5] = "200";
	assert(uvcdctl_program_main(6, args) == 0); /* host step alignment to 0 */
	puts("uvcdctl regression tests passed");
	return 0;
}
