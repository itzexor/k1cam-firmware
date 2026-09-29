/*
 * uvcd_config.h -- flat key=value persistence for struct uvcd_control_state
 */
#ifndef UVCD_CONFIG_H
#define UVCD_CONFIG_H

#include "uvcd.h"

#define UVCD_CONFIG_PATH "/etc/uvcd.conf"
#define UVCD_CONFIG_DIRTY_PATH UVCD_CONFIG_PATH ".dirty"

/* Seed *out with every control's compiled-in default. */
void uvcd_config_defaults(struct uvcd_control_state *out);

/* Seed *out with defaults, then override from path if it exists. A missing
 * file, or missing/unrecognized/out-of-range keys within it, are not
 * errors -- the corresponding field is just left at its default. */
void uvcd_config_load(const char *path, struct uvcd_control_state *out);

/* Atomically write *in to path as flat "name=value" lines (temp file +
 * fsync + rename). Returns 0 on success, -1 on failure (errno set).
 *
 * Controls are persistent by default, so there is no host-visible SAVE:
 * uvcd_gadget.c calls this after accepted changes have survived ten
 * seconds of healthy streaming. Idle changes remain in RAM. */
int uvcd_config_save(const char *path, const struct uvcd_control_state *in);

/* Crash guard (uvcd_gadget.c): while a stream has not yet proven healthy,
 * UVCD_CONFIG_DIRTY_PATH holds how many streams in a row started without
 * getting there. A crash leaves it behind; once it reaches
 * UVCD_CONFIG_UNPROVEN_LIMIT at startup, the saved controls are set aside
 * for factory ones. Idle time and clean stops leave no file. */
#define UVCD_CONFIG_UNPROVEN_LIMIT 2

/* 0 if there is no file. An empty or unreadable one counts as 1 (earlier
 * builds wrote it empty). */
int uvcd_config_streak_read(const char *path);

/* Write n, durably: it has to survive a hang or a pulled plug. Returns 0 on
 * success, -1 on failure (errno set). */
int uvcd_config_streak_write(const char *path, int n);

#endif /* UVCD_CONFIG_H */
