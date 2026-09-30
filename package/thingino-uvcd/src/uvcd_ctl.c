#include "uvcd_ctl.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int reply(char *out, size_t size, const char *text)
{
	return snprintf(out, size, "%s\n", text) < (int)size ? 0 : -1;
}

int uvcd_ctl_command(gadget_t *g, const char *line, char *out, size_t size)
{
	char command[32], name[64], arg[64];
	int n = sscanf(line, "%31s %63s %63s", command, name, arg);
	if (n < 1)
		return reply(out, size, "err empty command");
	if (!strcmp(command, "list")) {
		size_t used = 0;
		for (size_t i = 0; i < uvcd_ctrl_count(); i++) {
			const struct uvcd_ctrl_def *d = uvcd_ctrl_at(i);
			int w = snprintf(out + used, size - used, "%s %d\n", d->name, uvcd_ctrl_get(g, d));
			if (w < 0 || (size_t)w >= size - used)
				return -1;
			used += (size_t)w;
		}
		return snprintf(out + used, size - used, "ok\n") < (int)(size - used) ? 0 : -1;
	}
	if (!strcmp(command, "reset")) {
		uvcd_ctrl_reset(g);
		return reply(out, size, "ok");
	}
	if (!strcmp(command, "keyframe"))
		return reply(out, size, uvcd_ctrl_keyframe(g) == 0 ? "ok" : "err keyframe failed");
	const struct uvcd_ctrl_def *def = n >= 2 ? uvcd_ctrl_find(name) : NULL;
	if (!def)
		return reply(out, size, "err unknown control");
	if (!strcmp(command, "get") && n == 2) {
		snprintf(out, size, "%d\nok\n", uvcd_ctrl_get(g, def));
		return 0;
	}
	if (!strcmp(command, "set") && n == 3) {
		char *end;
		long value = strtol(arg, &end, 10);
		int applied;
		if (*arg == '\0' || *end != '\0')
			return reply(out, size, "err bad value");
		if (uvcd_ctrl_set(g, def, (int)value, &applied) != 0)
			return reply(out, size, "err apply failed");
		snprintf(out, size, "%d\nok\n", applied);
		return 0;
	}
	return reply(out, size, "err usage");
}

int uvcd_ctl_listen(void)
{
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	strncpy(addr.sun_path, UVCD_CTL_PATH, sizeof(addr.sun_path) - 1);
	unlink(UVCD_CTL_PATH);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) || listen(fd, 4)) {
		close(fd);
		return -1;
	}
	return fd;
}

void uvcd_ctl_close(int fd)
{
	if (fd >= 0)
		close(fd);
	unlink(UVCD_CTL_PATH);
}

void uvcd_ctl_accept(gadget_t *g, int fd)
{
	int client = accept4(fd, NULL, NULL, SOCK_CLOEXEC);
	if (client < 0)
		return;
	struct timeval timeout = {.tv_sec = 1};
	setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	char line[160] = {0}, out[4096];
	ssize_t n = read(client, line, sizeof(line) - 1);
	if (n > 0) {
		char *nl = strpbrk(line, "\r\n");
		if (nl)
			*nl = '\0';
		if (uvcd_ctl_command(g, line, out, sizeof(out)) != 0)
			strcpy(out, "err reply too large\n");
		write(client, out, strlen(out));
	}
	close(client);
}
