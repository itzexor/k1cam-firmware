/*
 * uvcd_config.c -- load/save /etc/uvcd.conf
 *
 * Flat "name=value" lines, one per control, no sections -- uvcd has a
 * single sensor and a single set of controls, so the INI sections
 * raptor.conf needs don't apply here. Field access goes through
 * uvcd_ctrl_field()/uvcd_ctrl_find() (uvcd_gadget.h) so this file never
 * needs to know the struct uvcd_control_state layout by name.
 */

#include "uvcd_config.h"
#include "uvcd_gadget.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void uvcd_config_defaults(struct uvcd_control_state *out)
{
	memset(out, 0, sizeof(*out));
	for (size_t i = 0; i < uvcd_ctrl_count(); i++) {
		const struct uvcd_ctrl_def *def = uvcd_ctrl_at(i);
		*uvcd_ctrl_field(out, def) = def->def;
	}
}

void uvcd_config_load(const char *path, struct uvcd_control_state *saved,
		      uint64_t *saved_mask, bool *apply_on_boot)
{
	uvcd_config_defaults(saved);
	*saved_mask = 0;
	*apply_on_boot = false;

	FILE *f = fopen(path, "r");
	if (!f)
		return;

	char line[128];
	while (fgets(line, sizeof(line), f)) {
		char *nl = strchr(line, '\n');
		if (nl)
			*nl = '\0';

		char *eq = strchr(line, '=');
		if (!eq)
			continue;
		*eq = '\0';
		const char *key = line;
		const char *val = eq + 1;

		if (strcmp(key, "apply-on-boot") == 0) {
			*apply_on_boot = atoi(val) != 0;
			continue;
		}
		const struct uvcd_ctrl_def *def = uvcd_ctrl_find(key);
		if (!def) {
			LOGW("%s: unknown key '%s', ignoring", path, key);
			continue;
		}

		char *end;
		long v = strtol(val, &end, 10);
		if (end == val) {
			LOGW("%s: bad value for '%s', ignoring", path, key);
			continue;
		}
		if (v < def->min)
			v = def->min;
		if (v > def->max)
			v = def->max;
		*uvcd_ctrl_field(saved, def) = (int)v;
		*saved_mask |= UINT64_C(1) << (size_t)(def - uvcd_ctrl_at(0));
	}
	fclose(f);
}

/* Best-effort: make a create or rename in path's directory durable too. */
static void fsync_dir(const char *path)
{
	char dir_path[280];
	strncpy(dir_path, path, sizeof(dir_path) - 1);
	dir_path[sizeof(dir_path) - 1] = '\0';
	char *slash = strrchr(dir_path, '/');
	if (slash)
		*slash = '\0';
	else
		strcpy(dir_path, ".");
	int dir_fd = open(dir_path, O_RDONLY | O_DIRECTORY);
	if (dir_fd >= 0) {
		fsync(dir_fd);
		close(dir_fd);
	}
}

int uvcd_config_save(const char *path, const struct uvcd_control_state *saved,
		     uint64_t saved_mask, bool apply_on_boot)
{
	char tmp_path[280];
	int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
	if (n < 0 || (size_t)n >= sizeof(tmp_path)) {
		errno = ENAMETOOLONG;
		return -1;
	}

	int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644);
	if (fd < 0)
		return -1;

	FILE *f = fdopen(fd, "w");
	if (!f) {
		close(fd);
		unlink(tmp_path);
		return -1;
	}

	int werr = 0;
	if (fprintf(f, "apply-on-boot=%d\n", apply_on_boot ? 1 : 0) < 0)
		werr = 1;
	for (size_t i = 0; !werr && i < uvcd_ctrl_count(); i++) {
		if (!(saved_mask & (UINT64_C(1) << i)))
			continue;
		const struct uvcd_ctrl_def *def = uvcd_ctrl_at(i);
		int value = *uvcd_ctrl_field((struct uvcd_control_state *)saved, def);
		if (fprintf(f, "%s=%d\n", def->name, value) < 0) {
			werr = 1;
			break;
		}
	}

	if (werr || fflush(f) != 0 || fsync(fileno(f)) != 0) {
		fclose(f);
		unlink(tmp_path);
		return -1;
	}
	fclose(f);

	if (rename(tmp_path, path) != 0) {
		unlink(tmp_path);
		return -1;
	}
	fsync_dir(path);
	return 0;
}

int uvcd_config_streak_read(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return 0;
	int n;
	if (fscanf(f, "%d", &n) != 1 || n < 1)
		n = 1;
	fclose(f);
	return n;
}

int uvcd_config_streak_write(const char *path, int n)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644);
	if (fd < 0)
		return -1;
	char buf[16];
	int len = snprintf(buf, sizeof(buf), "%d\n", n);
	if (write(fd, buf, (size_t)len) != len || fsync(fd) != 0) {
		int err = errno;
		close(fd);
		errno = err;
		return -1;
	}
	close(fd);
	fsync_dir(path);
	return 0;
}
