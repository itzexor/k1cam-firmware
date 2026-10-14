/* Host regression tests for which controls uvcd starts with: the saved
 * ones, or after a crash the last proven or factory ones. Link with
 * --gc-sections to discard hardware paths. */
#define UVCD_CONFIG_DIR "/tmp/uvcd-startup-test"
#define UVCD_CONFIG_PATH UVCD_CONFIG_DIR "/uvcd.conf"
#define UVCD_RUN_DIR UVCD_CONFIG_DIR
#include <assert.h>
#include <stdarg.h>
#include <sys/stat.h>
#include "../src/uvcd_config.c"
#include "../src/uvcd_pipeline.c"

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

static bool exists(const char *path)
{
	return access(path, F_OK) == 0;
}

static void clean(void)
{
	unlink(UVCD_CONFIG_PATH);
	unlink(UVCD_CONFIG_GOOD_PATH);
	unlink(UVCD_CONFIG_DIRTY_PATH);
	unlink(UVCD_CONFIG_PATH ".rejected");
	unlink(UVCD_CAMERA_GUARD_PATH);
}

int main(void)
{
	uvcd_pipeline_t p;
	mkdir(UVCD_CONFIG_DIR, 0755);
	clean();

	/* Nothing saved: factory controls. */
	memset(&p, 0, sizeof(p));
	assert(load_controls(&p) == 0);
	assert(p.controls.brightness == 128 && p.good.brightness == 128);

	/* No crash: the latest controls, even ones never streamed. */
	write_raw(UVCD_CONFIG_PATH, "brightness=150\n");
	write_raw(UVCD_CONFIG_GOOD_PATH, "brightness=140\n");
	memset(&p, 0, sizeof(p));
	assert(load_controls(&p) == 0);
	assert(p.controls.brightness == 150 && p.good.brightness == 140);
	assert(p.unproven_streak == 0);

	/* One stream died unproven: back to what the last healthy one proved,
	 * written back so the next start does not reload the suspect values.
	 * The streak stays until a stream proves healthy. */
	write_raw(UVCD_CONFIG_DIRTY_PATH, "1\n");
	memset(&p, 0, sizeof(p));
	assert(load_controls(&p) == 0);
	assert(p.controls.brightness == 140 && p.unproven_streak == 1);
	struct uvcd_control_state reloaded;
	uvcd_config_load(UVCD_CONFIG_PATH, &reloaded);
	assert(reloaded.brightness == 140);
	assert(exists(UVCD_CONFIG_DIRTY_PATH));

	/* Two in a row: factory, with the rejected file kept for diagnosis and
	 * nothing left that would bring the old values back. */
	write_raw(UVCD_CONFIG_PATH, "brightness=150\ncontrast=90\n");
	write_raw(UVCD_CONFIG_DIRTY_PATH, "2\n");
	memset(&p, 0, sizeof(p));
	assert(load_controls(&p) == 0);
	assert(p.controls.brightness == 128 && p.controls.contrast == 128);
	assert(p.good.brightness == 128 && p.unproven_streak == 0);
	assert(!exists(UVCD_CONFIG_PATH) && exists(UVCD_CONFIG_PATH ".rejected"));
	assert(!exists(UVCD_CONFIG_GOOD_PATH) && !exists(UVCD_CONFIG_DIRTY_PATH));

	/* The camera-side guard counts the same way, from RAM: a daemon that
	 * crashed outside a stream falls back too ... */
	clean();
	write_raw(UVCD_CONFIG_PATH, "brightness=150\n");
	write_raw(UVCD_CONFIG_GOOD_PATH, "brightness=140\n");
	write_raw(UVCD_CAMERA_GUARD_PATH, "1\n");
	memset(&p, 0, sizeof(p));
	assert(load_controls(&p) == 0);
	assert(p.controls.brightness == 140 && p.unproven_streak == 1);
	/* ... a crash both guards saw is one run, not two ... */
	write_raw(UVCD_CONFIG_PATH, "brightness=150\n");
	write_raw(UVCD_CONFIG_DIRTY_PATH, "1\n");
	memset(&p, 0, sizeof(p));
	assert(load_controls(&p) == 0);
	assert(p.controls.brightness == 140 && p.unproven_streak == 1);
	/* ... and a run of them goes to factory, clearing both. */
	write_raw(UVCD_CAMERA_GUARD_PATH, "2\n");
	memset(&p, 0, sizeof(p));
	assert(load_controls(&p) == 0);
	assert(p.controls.brightness == 128 && p.unproven_streak == 0);
	assert(!exists(UVCD_CAMERA_GUARD_PATH) && !exists(UVCD_CONFIG_DIRTY_PATH));

	/* Armed on top of the runs before it, once; a camera that is down
	 * takes nothing in, so there is nothing to guard. */
	clean();
	memset(&p, 0, sizeof(p));
	p.unproven_streak = 1;
	uvcd_pipeline_camera_guard_arm(&p);
	assert(!p.camera_guard_armed && !exists(UVCD_CAMERA_GUARD_PATH));
	p.cam.up = true;
	uvcd_pipeline_camera_guard_arm(&p);
	assert(p.camera_guard_armed && uvcd_config_streak_read(UVCD_CAMERA_GUARD_PATH) == 2);
	/* A clean exit is no crash: the file goes, the streak stays. */
	camera_guard_clear(&p, false);
	assert(!exists(UVCD_CAMERA_GUARD_PATH) && p.unproven_streak == 1);
	/* Proven by the camera running on what it got; each change restarts
	 * the wait. */
	uvcd_pipeline_camera_guard_arm(&p);
	p.camera_guard_since_ms -= UVCD_CONFIG_HEALTHY_MS - 1000;
	camera_guard_tick(&p);
	assert(p.camera_guard_armed);
	uvcd_pipeline_camera_guard_arm(&p);
	p.camera_guard_since_ms -= UVCD_CONFIG_HEALTHY_MS - 1000;
	camera_guard_tick(&p);
	assert(p.camera_guard_armed);
	p.camera_guard_since_ms -= 1000;
	camera_guard_tick(&p);
	assert(!p.camera_guard_armed && p.unproven_streak == 0);
	assert(!exists(UVCD_CAMERA_GUARD_PATH));

	clean();
	rmdir(UVCD_CONFIG_DIR);
	puts("startup regression tests passed");
	return 0;
}
