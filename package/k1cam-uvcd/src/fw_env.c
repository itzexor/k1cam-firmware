/* SPDX-License-Identifier: GPL-3.0-only */
#include "fw_env.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "fw_crc.h"

static const char *const copy_names[2] = {"env_a", "env_b"};

static uint32_t get_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static bool copy_valid(const uint8_t *copy)
{
	return fw_crc32(0, copy + FW_ENV_HEADER, FW_ENV_DATA_SIZE) == get_le32(copy);
}

int fw_env_choose(struct fw_env *env, const uint8_t *copy_a, const uint8_t *copy_b)
{
	const uint8_t *copies[2] = {copy_a, copy_b};

	for (int i = 0; i < 2; i++) {
		env->valid[i] = copies[i] && copy_valid(copies[i]);
		env->flags[i] = copies[i] ? copies[i][4] : 0;
	}
	if (env->valid[0] && env->valid[1]) {
		uint8_t a = env->flags[0], b = env->flags[1];
		if (a == 255 && b == 0)
			env->current = 1;
		else if (b == 255 && a == 0)
			env->current = 0;
		else
			env->current = b > a ? 1 : 0;
	} else if (env->valid[0] || env->valid[1]) {
		env->current = env->valid[0] ? 0 : 1;
	} else {
		env->current = -1;
		memset(env->data, 0, sizeof(env->data));
		return -1;
	}
	memcpy(env->data, copies[env->current] + FW_ENV_HEADER, FW_ENV_DATA_SIZE);
	return 0;
}

int fw_env_load(struct fw_flash *f, const struct fw_layout_map *map, struct fw_env *env)
{
	uint8_t *copies[2] = {malloc(FW_ENV_SIZE), malloc(FW_ENV_SIZE)};
	int ret = -1, err = ENOMEM;

	if (!copies[0] || !copies[1])
		goto out;
	for (int i = 0; i < 2; i++) {
		int h = fw_layout_open(f, map, copy_names[i], false, NULL);
		bool ok = h >= 0 && fw_flash_read_all(f, h, copies[i], FW_ENV_SIZE, 0) == 0;
		if (h >= 0)
			f->ops->close(f, h);
		/* A copy that can't be read counts as invalid, as in U-Boot. */
		if (!ok) {
			free(copies[i]);
			copies[i] = NULL;
		}
	}
	ret = fw_env_choose(env, copies[0], copies[1]);
	err = ENOMSG;
out:
	free(copies[0]);
	free(copies[1]);
	if (ret != 0)
		errno = err;
	return ret;
}

/* Entries are "name=value\0" up to an empty one; anything after that is
 * padding. Returns the length of the used part, the final NUL included
 * (so 1 for an empty environment), or 0 if the data is not terminated. */
static size_t used_length(const uint8_t *data)
{
	size_t at = 0;
	while (at < FW_ENV_DATA_SIZE) {
		size_t len = strnlen((const char *)data + at, FW_ENV_DATA_SIZE - at);
		if (at + len == FW_ENV_DATA_SIZE)
			return 0;
		at += len + 1;
		if (len == 0)
			return at;
	}
	return 0;
}

const char *fw_env_get(const struct fw_env *env, const char *name)
{
	size_t name_len = strlen(name);
	size_t end = used_length(env->data);

	for (size_t at = 0; at + 1 < end;) {
		const char *entry = (const char *)env->data + at;
		size_t len = strlen(entry);
		if (len > name_len && !strncmp(entry, name, name_len) && entry[name_len] == '=')
			return entry + name_len + 1;
		at += len + 1;
	}
	return NULL;
}

static bool valid_text(const char *s, bool name)
{
	if (name && !*s)
		return false;
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;
		if (c < 0x20 || c == 0x7f || (name && (c == '=' || c == ' ')))
			return false;
	}
	return true;
}

int fw_env_set(struct fw_env *env, const char *name, const char *value)
{
	if (!valid_text(name, true) || (value && !valid_text(value, false))) {
		errno = EINVAL;
		return -1;
	}
	size_t end = used_length(env->data);
	if (!end) {
		errno = EINVAL;
		return -1;
	}

	/* Rebuild the data area without name, then append the new entry. */
	uint8_t *out = calloc(1, FW_ENV_DATA_SIZE);
	if (!out)
		return -1;
	size_t name_len = strlen(name), used = 0;
	for (size_t at = 0; at + 1 < end;) {
		const char *entry = (const char *)env->data + at;
		size_t len = strlen(entry);
		if (!(len > name_len && !strncmp(entry, name, name_len) && entry[name_len] == '=')) {
			memcpy(out + used, entry, len + 1);
			used += len + 1;
		}
		at += len + 1;
	}
	if (value) {
		size_t need = name_len + 1 + strlen(value) + 1;
		/* Room for the entry and the terminating empty one. */
		if (used + need + 1 > FW_ENV_DATA_SIZE) {
			free(out);
			errno = ENOSPC;
			return -1;
		}
		memcpy(out + used, name, name_len);
		out[used + name_len] = '=';
		strcpy((char *)out + used + name_len + 1, value);
		used += need;
	}
	out[used] = '\0';
	memcpy(env->data, out, FW_ENV_DATA_SIZE);
	free(out);
	return 0;
}

void fw_env_image(const struct fw_env *env, uint8_t flags, uint8_t *out)
{
	uint32_t crc = fw_crc32(0, env->data, FW_ENV_DATA_SIZE);
	out[0] = (uint8_t)crc;
	out[1] = (uint8_t)(crc >> 8);
	out[2] = (uint8_t)(crc >> 16);
	out[3] = (uint8_t)(crc >> 24);
	out[4] = flags;
	memcpy(out + FW_ENV_HEADER, env->data, FW_ENV_DATA_SIZE);
}

int fw_env_save(struct fw_flash *f, const struct fw_layout_map *map, struct fw_env *env)
{
	if (env->current != 0 && env->current != 1) {
		/* Never write an environment that wasn't read from flash. */
		errno = ENOMSG;
		return -1;
	}
	int target = !env->current;
	uint8_t *image = malloc(FW_ENV_SIZE), *check = malloc(FW_ENV_SIZE);
	struct fw_mtd_geometry g;
	int h = -1, ret = -1, err = 0;

	if (!image || !check) {
		err = ENOMEM;
		goto out;
	}
	fw_env_image(env, FW_ENV_FLAG_ACTIVE, image);

	h = fw_layout_open(f, map, copy_names[target], true, &g);
	if (h < 0) {
		err = -h;
		goto out;
	}
	if (fw_flash_erase_range(f, h, g.erasesize, 0, FW_ENV_SIZE) != 0 ||
	    fw_flash_write_all(f, h, image, FW_ENV_SIZE, 0) != 0 ||
	    fw_flash_read_all(f, h, check, FW_ENV_SIZE, 0) != 0) {
		err = errno;
		goto out;
	}
	if (memcmp(image, check, FW_ENV_SIZE) != 0) {
		err = EIO;
		goto out;
	}
	f->ops->close(f, h);

	/* The new copy is complete and marked active. Retiring the old one
	 * is what makes the choice unambiguous; until then both are active
	 * and U-Boot picks env_a, which is one of the two complete states. */
	uint8_t obsolete = FW_ENV_FLAG_OBSOLETE, flag = 0xff;
	h = fw_layout_open(f, map, copy_names[env->current], true, NULL);
	if (h < 0) {
		err = -h;
		goto out;
	}
	if (fw_flash_write_all(f, h, &obsolete, 1, 4) != 0 ||
	    fw_flash_read_all(f, h, &flag, 1, 4) != 0) {
		err = errno;
		goto out;
	}
	if (flag != FW_ENV_FLAG_OBSOLETE) {
		err = EIO;
		goto out;
	}
	env->flags[env->current] = FW_ENV_FLAG_OBSOLETE;
	env->valid[target] = true;
	env->flags[target] = FW_ENV_FLAG_ACTIVE;
	env->current = target;
	ret = 0;
out:
	if (h >= 0)
		f->ops->close(f, h);
	free(image);
	free(check);
	if (ret != 0)
		errno = err ? err : EIO;
	return ret;
}
