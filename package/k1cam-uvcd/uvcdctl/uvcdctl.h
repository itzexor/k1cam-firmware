/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * uvcdctl.h -- what uvcdctl.c shares with the firmware commands (firmware.c).
 */
#ifndef UVCDCTL_H
#define UVCDCTL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../src/uvcd_firmware.h"

#define UVCD_XU_UNIT_ID 4

/* ioctl, retried on EINTR. */
int xioctl(int fd, unsigned long request, void *arg);
/* One UVCIOC_CTRL_QUERY on Extension Unit 4. */
int xu_query_bytes(int fd, uint8_t selector, uint8_t query, void *data, uint16_t size);
/* A uvcvideo node that answers on Extension Unit 4. */
bool is_our_camera(int fd);
/* explicit_path, or the first k1cam camera; prints why on failure. */
int open_device(const char *explicit_path, char *chosen, size_t chosen_len);

/* uvcdctl firmware ...: argv[0] is the subcommand. */
int cmd_firmware(int argc, char **argv, const char *device);

#endif /* UVCDCTL_H */
