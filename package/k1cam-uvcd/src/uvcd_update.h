/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * uvcd_update.h -- the daemon side of the firmware protocol (uvcd_firmware.h):
 * device information, flash dumps, A/B updates, and confirming a trial boot.
 *
 * The gadget hands it the UVC requests for selectors 26-28 from its event
 * loop and calls uvcd_update_tick() from there. Nothing here blocks that
 * loop on flash: snapshots, installs and environment writes run on one
 * worker thread, and STATUS is built from state that thread publishes.
 */
#ifndef UVCD_UPDATE_H
#define UVCD_UPDATE_H

#include <stdbool.h>
#include <stdint.h>

#include "fw_flash.h"

struct uvcd_update;

struct uvcd_update_config {
	struct fw_flash flash;
	const char *cmdline_path; /* /proc/cmdline: k1cam.slot, k1cam.boot */
	const char *meminfo_path; /* /proc/meminfo */
	const char *mounts_path;  /* /proc/mounts: which upper layer / runs on */
	const char *release_path; /* /usr/lib/k1cam/firmware-release */
	const char *serial_path;  /* g_webcam's iSerialNumber */
	const char *data_root;    /* where the data partition is mounted */
	const char *run_dir;      /* RAM: the trial's outcome for k1cam-trial-gate */
	/* Pause (true) or resume flash writers outside uvcd while a dump is
	 * copied to RAM. NULL: stop and continue the jffs2 GC threads. */
	void (*quiesce)(bool pause);
	/* Reboot; returns only on failure. NULL: exec /sbin/reboot. */
	int (*reboot)(void);
	int receive_timeout_ms; /* upload idle this long: abandoned */
	int dump_timeout_ms;    /* dump idle this long: released */
	uint32_t memory_reserve_kb; /* kept free besides any RAM copy */
};

void uvcd_update_default_config(struct uvcd_update_config *cfg);
struct uvcd_update *uvcd_update_create(const struct uvcd_update_config *cfg);
void uvcd_update_destroy(struct uvcd_update *u);

/* A SETUP request for one of the firmware selectors. in: device-to-host.
 * For GET requests out (60 bytes) receives the reply; returns its length,
 * or -1 to stall. For SET_CUR returns the data length to accept, or -1. */
int uvcd_update_setup(struct uvcd_update *u, uint8_t request, uint8_t selector, bool in,
		      uint16_t length, uint8_t *out);
/* The data stage of an accepted SET_CUR. */
void uvcd_update_data(struct uvcd_update *u, uint8_t selector, const uint8_t *data,
		      int32_t length);
/* Idle sessions, from the gadget loop. */
void uvcd_update_tick(struct uvcd_update *u);
/* USB went away: sessions waiting on the host end. */
void uvcd_update_disconnect(struct uvcd_update *u);

/* uvcd must not write its settings to flash while a dump snapshot is taken. */
bool uvcd_update_persist_paused(struct uvcd_update *u);

/* This boot is a trial of the running slot, and it has not been judged yet:
 * the gadget should prove the camera works and report it. */
bool uvcd_update_trial_wants_check(struct uvcd_update *u);
/* The camera's verdict. ok: confirm the slot (on the worker). Otherwise
 * the trial is left pending and k1cam-trial-gate reboots to count it. */
void uvcd_update_trial_health(struct uvcd_update *u, bool ok, const char *what);

/* Host tests: wait for the worker to finish its job. */
void uvcd_update_wait_idle(struct uvcd_update *u);

#endif /* UVCD_UPDATE_H */
