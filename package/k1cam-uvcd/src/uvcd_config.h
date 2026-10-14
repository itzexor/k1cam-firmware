/*
 * uvcd_config.h -- flat key=value persistence for struct uvcd_control_state
 */
#ifndef UVCD_CONFIG_H
#define UVCD_CONFIG_H

#include "uvcd.h"

#ifndef UVCD_CONFIG_PATH /* the host tests point it elsewhere */
#define UVCD_CONFIG_PATH "/etc/uvcd.conf"
#endif
/* The controls last proven by a healthy stream (see the crash guard). */
#define UVCD_CONFIG_GOOD_PATH UVCD_CONFIG_PATH ".good"
#define UVCD_CONFIG_DIRTY_PATH UVCD_CONFIG_PATH ".dirty"
#ifndef UVCD_RUN_DIR /* RAM (tmpfs); the host tests point it elsewhere */
#define UVCD_RUN_DIR "/var/run"
#endif
#define UVCD_CAMERA_GUARD_PATH UVCD_RUN_DIR "/uvcd.unproven"

/* Seed *out with every control's compiled-in default. */
void uvcd_config_defaults(struct uvcd_control_state *out);

/* Seed *out with defaults, then override from path if it exists. A missing
 * file, or missing/unrecognized/out-of-range keys within it, are not
 * errors -- the corresponding field is just left at its default. */
void uvcd_config_load(const char *path, struct uvcd_control_state *out);

/* Atomically write *in to path as flat "name=value" lines (temp file +
 * fsync + rename), leaving out values equal to their default. Returns 0 on
 * success, -1 on failure (errno set).
 *
 * Controls are persistent by default, so there is no host-visible SAVE:
 * uvcd_gadget.c calls this shortly after any accepted change. */
int uvcd_config_save(const char *path, const struct uvcd_control_state *in);

/* Crash guard. Each file holds how many runs in a row ended in a crash
 * before proving their controls; a crash leaves it behind, and startup
 * takes the larger of the two. One such run sends the controls back to
 * UVCD_CONFIG_GOOD_PATH; at UVCD_CONFIG_UNPROVEN_LIMIT they go to factory.
 * Clean stops and exits leave no file.
 *
 * - UVCD_CONFIG_DIRTY_PATH, on flash (uvcd_gadget.c): from a stream's
 *   start, and from any change made while it runs, until it proves healthy
 *   by delivering frames for a while. It survives a kernel crash, so it
 *   also counts one; it cannot tell a pulled plug from that.
 * - UVCD_CAMERA_GUARD_PATH, in RAM (uvcd_pipeline.c): from the camera's
 *   bring-up at startup, and from any change made outside a stream, until
 *   the camera has run a while. It only sees the daemon crash or hang and
 *   be restarted in the same boot, so unplugging the camera, which is also
 *   its power, never counts. */
#define UVCD_CONFIG_UNPROVEN_LIMIT 2
/* How long a stream must deliver frames, or the camera run, on the latest
 * controls to prove them. */
#define UVCD_CONFIG_HEALTHY_MS 10000

/* 0 if there is no file. An empty or unreadable one counts as 1 (earlier
 * builds wrote it empty). */
int uvcd_config_streak_read(const char *path);

/* Write n, durably: it has to survive a hang or a pulled plug. Returns 0 on
 * success, -1 on failure (errno set). */
int uvcd_config_streak_write(const char *path, int n);

#endif /* UVCD_CONFIG_H */
