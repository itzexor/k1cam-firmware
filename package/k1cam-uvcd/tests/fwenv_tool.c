/*
 * fwenv_tool.c -- uvcd's environment code (fw_env.c) on a flash image file,
 * for the U-Boot sandbox boot-script test (board/k1cam/tests/boot-test.sh):
 *
 *   fwenv_tool IMAGE get NAME...             print NAME=VALUE (or NAME absent)
 *   fwenv_tool IMAGE set NAME=VALUE...       one transaction; empty VALUE deletes
 *   fwenv_tool IMAGE copies                  which copies are valid, their flags
 *
 * IMAGE is a whole 8 MiB flash with the A/B layout.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/fw_crc.c"
#include "../src/fw_env.c"
#include "../src/fw_flash.c"
#include "fake_flash.h"

int main(int argc, char **argv)
{
	struct fake_flash ff = {0};
	struct fw_layout_map map;
	struct fw_env env;

	if (argc < 3) {
		fprintf(stderr, "usage: fwenv_tool IMAGE get|set|copies [args]\n");
		return 2;
	}
	fake_flash_init(&ff, true);
	FILE *f = fopen(argv[1], "rb");
	if (!f || fread(ff.mem, 1, FW_FLASH_SIZE, f) != FW_FLASH_SIZE) {
		fprintf(stderr, "fwenv_tool: %s: not an 8 MiB image\n", argv[1]);
		return 1;
	}
	fclose(f);
	assert(fw_layout_resolve(&ff.flash, &map) == 0 && map.kind == FW_LAYOUT_AB);
	int loaded = fw_env_load(&ff.flash, &map, &env);

	if (!strcmp(argv[2], "copies")) {
		printf("env_a=%s,%u env_b=%s,%u current=%s\n", env.valid[0] ? "valid" : "invalid",
		       env.flags[0], env.valid[1] ? "valid" : "invalid", env.flags[1],
		       env.current == 0 ? "env_a" : env.current == 1 ? "env_b" : "none");
		return 0;
	}
	if (loaded != 0) {
		fprintf(stderr, "fwenv_tool: no valid environment\n");
		return 1;
	}
	if (!strcmp(argv[2], "get")) {
		for (int i = 3; i < argc; i++) {
			const char *v = fw_env_get(&env, argv[i]);
			if (v)
				printf("%s=%s\n", argv[i], v);
			else
				printf("%s absent\n", argv[i]);
		}
		return 0;
	}
	if (strcmp(argv[2], "set") != 0)
		return 2;
	for (int i = 3; i < argc; i++) {
		char *eq = strchr(argv[i], '=');
		if (!eq)
			return 2;
		*eq = '\0';
		if (fw_env_set(&env, argv[i], eq[1] ? eq + 1 : NULL) != 0) {
			perror("fwenv_tool: set");
			return 1;
		}
	}
	if (fw_env_save(&ff.flash, &map, &env) != 0) {
		perror("fwenv_tool: save");
		return 1;
	}
	f = fopen(argv[1], "r+b");
	if (!f || fwrite(ff.mem, 1, FW_FLASH_SIZE, f) != FW_FLASH_SIZE || fclose(f) != 0) {
		perror(argv[1]);
		return 1;
	}
	return 0;
}
