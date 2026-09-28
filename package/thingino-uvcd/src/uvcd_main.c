/*
 * uvcd_main.c -- entry point: signal handling, HAL/pipeline lifecycle
 */

#include <stdio.h>
#include <stdarg.h>
#include <signal.h>
#include <syslog.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

#include "uvcd.h"

volatile sig_atomic_t uvcd_running = 1;

/* Where log lines go once daemonized. The kernel ring buffer (/dev/kmsg)
 * rather than syslog: this image runs no syslogd, and kmsg needs no daemon
 * at all -- `dmesg` on the debug console shows uvcd's lines interleaved with
 * the gadget/ISP driver messages they relate to. kernel.printk keeps these
 * levels off the UART, so they cost nothing at boot. syslog is only the
 * fallback for a kernel without /dev/kmsg. */
static int uvcd_log_kmsg_fd = -1;
static bool uvcd_log_use_syslog = false;

/* Debug level is dropped unless -v: the per-frame and per-event lines live
 * there, and at 30fps they would otherwise be a steady CPU and ring-buffer
 * tax on a single-core SoC that is busy encoding. */
static bool uvcd_log_verbose = false;

void uvcd_log(int level, const char *fmt, ...)
{
	va_list ap;

	if (level >= UVCD_LOG_DBG && !uvcd_log_verbose)
		return;

	va_start(ap, fmt);
	if (uvcd_log_kmsg_fd >= 0) {
		char buf[512];
		int n = snprintf(buf, sizeof(buf), "<%d>uvcd: ", level);
		if (n > 0 && n < (int)sizeof(buf)) {
			int m = vsnprintf(buf + n, sizeof(buf) - (size_t)n, fmt, ap);
			size_t len = (size_t)n + (m > 0 ? (size_t)m : 0);
			/* Keep room for the newline: without it the kernel
			 * treats the next record as a continuation of this
			 * one and dmesg runs them together. */
			if (len > sizeof(buf) - 2)
				len = sizeof(buf) - 2;
			buf[len++] = '\n';
			ssize_t ignored = write(uvcd_log_kmsg_fd, buf, len);
			(void)ignored;
		}
	} else if (uvcd_log_use_syslog) {
		vsyslog(level, fmt, ap);
	} else {
		vfprintf(stderr, fmt, ap);
		fputc('\n', stderr);
	}
	va_end(ap);
}

static void on_signal(int sig)
{
	(void)sig;
	uvcd_running = 0;
}

/* Bridge HAL logging into our own logger */
static void hal_log_bridge(int level, const char *file, int line, const char *fmt, ...)
{
	(void)file;
	(void)line;
	static const int map[] = {UVCD_LOG_ERR, UVCD_LOG_ERR, UVCD_LOG_WARN, UVCD_LOG_INFO,
				  UVCD_LOG_DBG};
	int lvl = (level >= 0 && level <= 4) ? map[level] : UVCD_LOG_DBG;

	va_list ap;
	va_start(ap, fmt);
	char buf[512];
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	uvcd_log(lvl, "%s", buf);
}

int main(int argc, char **argv)
{
	const char *device = "/dev/video0";
	bool foreground = false;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
			device = argv[++i];
		else if (strcmp(argv[i], "-f") == 0)
			foreground = true;
		else if (strcmp(argv[i], "-v") == 0)
			uvcd_log_verbose = true;
	}

	if (!foreground) {
		/* Opened before daemon(): it only redirects fds 0-2, so this
		 * one survives into the daemon. */
		uvcd_log_kmsg_fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
		if (uvcd_log_kmsg_fd < 0) {
			uvcd_log_use_syslog = true;
			openlog("uvcd", LOG_PID, LOG_DAEMON);
		}
		if (daemon(0, 0) != 0) {
			LOGE("daemon() failed: %s", strerror(errno));
			return 1;
		}
	}

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	signal(SIGPIPE, SIG_IGN);

	rss_hal_set_log_func(hal_log_bridge);

	uvcd_pipeline_t pipe_state;
	if (uvcd_pipeline_init(&pipe_state) != 0) {
		LOGE("pipeline init failed");
		return 1;
	}

	LOGI("uvcd running, device=%s", device);
	int ret = uvcd_gadget_run(&pipe_state, device);

	uvcd_pipeline_deinit(&pipe_state);
	LOGI("uvcd shutting down");
	if (uvcd_log_use_syslog)
		closelog();
	return ret == 0 ? 0 : 1;
}
