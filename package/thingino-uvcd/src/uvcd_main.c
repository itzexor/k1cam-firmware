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

#include "uvcd.h"

volatile sig_atomic_t uvcd_running = 1;
static bool uvcd_log_use_syslog = false;

void uvcd_log(int level, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	if (uvcd_log_use_syslog) {
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
	}

	if (!foreground) {
		uvcd_log_use_syslog = true;
		openlog("uvcd", LOG_PID, LOG_DAEMON);
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
