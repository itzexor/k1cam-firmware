#ifndef UVCD_CTL_H
#define UVCD_CTL_H

#include <stddef.h>
#include "uvcd_gadget.h"

#define UVCD_CTL_PATH "/var/run/uvcd.sock"

int uvcd_ctl_listen(void);
void uvcd_ctl_close(int fd);
void uvcd_ctl_accept(gadget_t *g, int fd);
int uvcd_ctl_command(gadget_t *g, const char *line, char *out, size_t out_size);

#endif
