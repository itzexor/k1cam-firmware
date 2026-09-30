/* Host regression tests for the config and crash guard files. Link with
 * --gc-sections to discard the control-table paths this doesn't exercise. */
#include <assert.h>
#include <stdarg.h>
#include <sys/stat.h>
#include "../src/uvcd_config.c"

static const struct uvcd_ctrl_def defs[] = {
	{.name = "brightness", .min = 0, .max = 255, .def = 128,
	 .state_offset = offsetof(struct uvcd_control_state, brightness)},
	{.name = "contrast", .min = 0, .max = 255, .def = 128,
	 .state_offset = offsetof(struct uvcd_control_state, contrast)},
};

size_t uvcd_ctrl_count(void) { return sizeof(defs) / sizeof(defs[0]); }
const struct uvcd_ctrl_def *uvcd_ctrl_at(size_t i) { return i < uvcd_ctrl_count() ? &defs[i] : NULL; }
const struct uvcd_ctrl_def *uvcd_ctrl_find(const char *name)
{
	for (size_t i = 0; i < uvcd_ctrl_count(); i++)
		if (!strcmp(name, defs[i].name))
			return &defs[i];
	return NULL;
}

void uvcd_log(int level, const char *fmt, ...)
{
	(void)level;
	(void)fmt;
}

static void write_raw(const char *path, const char *text)
{
	FILE *f = fopen(path, "w");
	assert(f);
	fputs(text, f);
	fclose(f);
}

static bool file_is(const char *path, const char *text)
{
	char buf[256] = {0};
	FILE *f = fopen(path, "r");
	assert(f);
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	return n == strlen(text) && memcmp(buf, text, n) == 0;
}

int main(void)
{
	char dir[] = "/tmp/uvcd-config-test-XXXXXX";
	assert(mkdtemp(dir));
	char path[64];
	snprintf(path, sizeof(path), "%s/uvcd.conf.dirty", dir);

	assert(uvcd_config_streak_read(path) == 0); /* no file: nothing died */
	assert(uvcd_config_streak_write(path, 1) == 0);
	assert(uvcd_config_streak_read(path) == 1);
	assert(uvcd_config_streak_write(path, UVCD_CONFIG_UNPROVEN_LIMIT) == 0);
	assert(uvcd_config_streak_read(path) == UVCD_CONFIG_UNPROVEN_LIMIT);
	write_raw(path, ""); /* what earlier builds left behind */
	assert(uvcd_config_streak_read(path) == 1);
	write_raw(path, "junk\n");
	assert(uvcd_config_streak_read(path) == 1);
	write_raw(path, "-3\n");
	assert(uvcd_config_streak_read(path) == 1);

	/* Only what differs from the default is written. */
	struct uvcd_control_state saved, loaded;
	uvcd_config_defaults(&saved);
	saved.brightness = 150;
	assert(uvcd_config_save(path, &saved) == 0);
	assert(file_is(path, "brightness=150\n"));
	uvcd_config_load(path, &loaded);
	assert(loaded.brightness == 150 && loaded.contrast == 128);
	saved.brightness = 128;
	assert(uvcd_config_save(path, &saved) == 0);
	assert(file_is(path, ""));

	/* Out-of-range values clamp; unknown keys (like the apply-on-boot flag
	 * of the retired explicit save model) are skipped. */
	write_raw(path, "apply-on-boot=0\nbrightness=200\ncontrast=999\n");
	uvcd_config_load(path, &loaded);
	assert(loaded.brightness == 200 && loaded.contrast == 255);

	unlink(path);
	rmdir(dir);
	puts("config regression tests passed");
	return 0;
}
