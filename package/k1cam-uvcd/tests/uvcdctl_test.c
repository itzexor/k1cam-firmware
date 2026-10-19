/* Exercise socket framing, USB writes and listings without a camera. */
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
#define access test_access
#include "../uvcdctl/uvcdctl.c"
#undef main

static const char **chunks;
static size_t chunk_index;
static char last_request[160];
static bool local_socket;

int test_access(const char *path, int mode)
{
	assert(!strcmp(path, UVCD_SOCKET_PATH) && mode == F_OK);
	return local_socket ? 0 : -1;
}

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
	assert(fd == 7);
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(last_request, sizeof(last_request), fmt, ap);
	va_end(ap);
	return n;
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

static int current_qp = 30, get_error, range_error;
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
	case UVC_GET_MIN:
		if (range_error) { errno = range_error; return -1; }
		v = -1;
		break;
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

static int capture_command(int argc, char **argv, FILE *stream, char *out, size_t size)
{
	FILE *capture = tmpfile();
	assert(capture);
	fflush(stream);
	int target = fileno(stream);
	int saved = dup(target);
	assert(saved >= 0 && dup2(fileno(capture), target) >= 0);
	int rc = uvcdctl_program_main(argc, argv);
	fflush(stream);
	assert(dup2(saved, target) >= 0);
	FILE *saved_stream = fdopen(saved, "w");
	assert(saved_stream);
	fclose(saved_stream);
	rewind(capture);
	size_t n = fread(out, 1, size - 1, capture);
	assert(feof(capture));
	out[n] = '\0';
	fclose(capture);
	return rc;
}

static int capture_list(const char *command, const char *key, char *out, size_t size)
{
	char *args[] = {"uvcdctl", "--device", "/fake", (char *)command, (char *)key, NULL};
	char *local_args[] = {"uvcdctl", (char *)command, (char *)key, NULL};
	return local_socket ? capture_command(key ? 3 : 2, local_args, stdout, out, size) :
		capture_command(key ? 5 : 4, args, stdout, out, size);
}

static void check_help(void)
{
	char out[4096];
	/* Any attempted device open would fail the /fake assertion in test_open. */
	char *args[] = {"uvcdctl", "--device", "/must-not-open", "get", NULL, NULL, NULL};
	assert(capture_command(4, args, stderr, out, sizeof(out)) == 2);
	assert(strstr(out, "usage: uvcdctl [--device PATH] get <key>\n"));
	assert(!strstr(out, "commands:") && !strstr(out, "no k1cam camera"));
	args[3] = "set";
	assert(capture_command(4, args, stderr, out, sizeof(out)) == 2);
	assert(strstr(out, "usage: uvcdctl [--device PATH] set <key> <value>\n"));
	assert(!strstr(out, "commands:"));
	args[4] = "brightness";
	assert(capture_command(5, args, stderr, out, sizeof(out)) == 2);
	assert(strstr(out, "set <key> <value>"));
	args[3] = "get";
	args[5] = "extra";
	assert(capture_command(6, args, stderr, out, sizeof(out)) == 2);
	assert(strstr(out, "get <key>") && !strstr(out, "commands:"));

	args[3] = "help";
	args[4] = NULL;
	assert(capture_command(4, args, stdout, out, sizeof(out)) == 0);
	assert(strstr(out, "commands:") && strstr(out, "help [command]"));
	const char *topics[] = {"get", "set", "list", "describe", "reset", "keyframe", "help"};
	for (size_t i = 0; i < sizeof(topics) / sizeof(topics[0]); i++) {
		char expected[96], help[4096];
		args[3] = "help";
		args[4] = (char *)topics[i];
		assert(capture_command(5, args, stdout, help, sizeof(help)) == 0);
		snprintf(expected, sizeof(expected), "usage: uvcdctl [--device PATH] %s", topics[i]);
		assert(strstr(help, expected) && strstr(help, "Example"));
		assert(!strstr(help, "commands:"));
		args[3] = (char *)topics[i];
		args[4] = "--help";
		assert(capture_command(5, args, stdout, out, sizeof(out)) == 0);
		assert(!strcmp(out, help));
		args[4] = "-h";
		assert(capture_command(5, args, stdout, out, sizeof(out)) == 0);
		assert(!strcmp(out, help));
	}
	args[3] = "help";
	args[4] = "unknown";
	assert(capture_command(5, args, stderr, out, sizeof(out)) == 2);
	assert(strstr(out, "unknown command 'unknown'") && strstr(out, "help [command]"));
	args[4] = "get";
	assert(capture_command(6, args, stderr, out, sizeof(out)) == 2);
	assert(strstr(out, "help [command]") && !strstr(out, "commands:"));
}

static void check_listings(void)
{
	char brief[16384], detailed[16384], numeric[16384];
	assert(capture_list("list", "h264-max-qp", brief, sizeof(brief)) == 0);
	assert(capture_list("describe", "h264-max-qp", detailed, sizeof(detailed)) == 0);
	assert(capture_list("describe", "44", numeric, sizeof(numeric)) == 0);
	assert(!strcmp(detailed, numeric));
	assert(strstr(brief, "h264-max-qp") && !strstr(brief, "brightness"));
	assert(strstr(brief, "DEFAULT") && !strstr(brief, "DESCRIPTION"));
	assert(!strstr(brief, find_ctrl("44")->help));
	assert(strstr(detailed, "DESCRIPTION") && strstr(detailed, find_ctrl("44")->help));
	assert(!strchr(detailed, '\033')); /* redirected output has no colour */
	/* The detailed row is the brief row with the help text appended. */
	const char *brief_row = strchr(brief, '\n') + 1;
	const char *detailed_row = strchr(detailed, '\n') + 1;
	assert(!strncmp(brief_row, detailed_row, strcspn(brief_row, "\n")));
	assert(capture_list("list", NULL, brief, sizeof(brief)) == 0);
	assert(capture_list("describe", NULL, detailed, sizeof(detailed)) == 0);
	for (size_t i = 0; i < CTRL_COUNT; i++) {
		assert(strstr(brief, ctrl_defs[i].name));
		assert(strstr(detailed, ctrl_defs[i].help));
	}
	range_error = ENOENT;
	assert(capture_list("describe", "44", detailed, sizeof(detailed)) == 1);
	assert(strstr(detailed, "unsupported") && strstr(detailed, find_ctrl("44")->help));
	range_error = 0;
	get_error = 1;
	assert(capture_list("describe", "44", detailed, sizeof(detailed)) == 1);
	assert(strstr(detailed, "error") && strstr(detailed, find_ctrl("44")->help));
	get_error = 0;
	assert(capture_list("list", "unknown", brief, sizeof(brief)) == 2);
	assert(!*brief);
	char *extra[] = {"uvcdctl", "list", "44", "extra", NULL};
	assert(uvcdctl_program_main(4, extra) == 2);
	extra[1] = "describe";
	assert(uvcdctl_program_main(4, extra) == 2);

	int value, min, max, dv;
	const char *info[] = {"20 -1 ", "51 -1\nok\n", NULL};
	chunks = info;
	chunk_index = 0;
	assert(list_read(-1, find_ctrl("44"), &value, &min, &max, &dv) == 0);
	assert(value == 20 && min == -1 && max == 51 && dv == -1);
	assert(!strcmp(last_request, "info h264-max-qp\n"));
	/* Both transports pass their data through exactly the same formatter. */
	assert(capture_list("describe", "44", detailed, sizeof(detailed)) == 0);
	assert(capture_list("list", "44", brief, sizeof(brief)) == 0);
	local_socket = true;
	chunks = info;
	chunk_index = 0;
	assert(capture_list("describe", "44", numeric, sizeof(numeric)) == 0);
	assert(!strcmp(detailed, numeric));
	chunk_index = 0;
	assert(capture_list("list", "44", numeric, sizeof(numeric)) == 0);
	assert(!strcmp(brief, numeric));
	local_socket = false;
	const char *ae[] = {"8 1 8 8\nok\n", NULL};
	chunks = ae;
	chunk_index = 0;
	assert(list_read(-1, find_ctrl("auto-exposure"), &value, &min, &max, &dv) == 0);
	assert(value == 3 && min == 1 && max == 3 && dv == 3);
	const char *bad_info[] = {"err unknown control\n", NULL};
	chunks = bad_info;
	chunk_index = 0;
	assert(list_read(-1, find_ctrl("44"), &value, &min, &max, &dv) == -1);
	const char *malformed[] = {"20 -1\nok\n", NULL};
	chunks = malformed;
	chunk_index = 0;
	assert(list_read(-1, find_ctrl("44"), &value, &min, &max, &dv) == -1);
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
	check_listings();
	check_help();
	puts("uvcdctl regression tests passed");
	return 0;
}
