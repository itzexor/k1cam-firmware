/* Host regression tests for the crash guard file. Link with --gc-sections
 * to discard the control-table paths this doesn't exercise. */
#include <assert.h>
#include <stdarg.h>
#include <sys/stat.h>
#include "../src/uvcd_config.c"

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

	unlink(path);
	rmdir(dir);
	puts("config regression tests passed");
	return 0;
}
