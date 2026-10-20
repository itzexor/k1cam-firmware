/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * fw_env.h -- the U-Boot environment, redundant copies in env_a and env_b.
 *
 * Each copy is U-Boot's env_t for CONFIG_ENV_REDUNDANT: a CRC-32 of the
 * data area (little-endian, as U-Boot stores it on this CPU), a flags byte,
 * then "name=value\0" entries ending in an empty one. Reading picks the
 * copy U-Boot's env_check_redund() would; saving does what env_sf_save()
 * does: write the whole other copy marked active (1), check it, then mark
 * the old one obsolete (0) by programming its flags byte. A power cut at
 * any point leaves one complete copy that U-Boot will choose, either the
 * old environment or the new one, never a mixture.
 */
#ifndef FW_ENV_H
#define FW_ENV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fw_flash.h"

#define FW_ENV_HEADER 5 /* crc32 + flags */
#define FW_ENV_DATA_SIZE (FW_ENV_SIZE - FW_ENV_HEADER)
#define FW_ENV_FLAG_ACTIVE 1
#define FW_ENV_FLAG_OBSOLETE 0

struct fw_env {
	uint8_t data[FW_ENV_DATA_SIZE]; /* the chosen copy's data area */
	int current;                    /* 0: env_a, 1: env_b, -1: neither valid */
	bool valid[2];                  /* CRC matched */
	uint8_t flags[2];
};

/* Choose like env_check_redund(): only valid copies count; of two, the
 * larger flags byte wins, 255 before 0 counting as wrapped, and equal
 * flags choose env_a. Returns 0, -1 with valid[] both false if neither
 * copy is valid. */
int fw_env_choose(struct fw_env *env, const uint8_t *copy_a, const uint8_t *copy_b);

/* Read both copies from map's env_a and env_b. Returns 0, or -1 with errno
 * set (ENOMSG when neither copy is valid). */
int fw_env_load(struct fw_flash *f, const struct fw_layout_map *map, struct fw_env *env);

/* The value of name, or NULL. Points into env->data. */
const char *fw_env_get(const struct fw_env *env, const char *name);

/* Set name to value, or remove it for value NULL. Returns 0, or -1 with
 * errno: EINVAL for a bad name or value, ENOSPC if it would not fit. */
int fw_env_set(struct fw_env *env, const char *name, const char *value);

/* Write env to the copy that is not current and make it current, as
 * described above. The written copy is read back and checked before the
 * old one is retired. Returns 0, or -1 with errno set; on failure the
 * flash still holds a complete environment (the old one, unless the
 * failure was in retiring it, after which either is complete). */
int fw_env_save(struct fw_flash *f, const struct fw_layout_map *map, struct fw_env *env);

/* Build one copy's bytes (crc, flags, data) into out (FW_ENV_SIZE bytes). */
void fw_env_image(const struct fw_env *env, uint8_t flags, uint8_t *out);

#endif /* FW_ENV_H */
