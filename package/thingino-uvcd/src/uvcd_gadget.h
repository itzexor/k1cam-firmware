/*
 * uvcd_gadget.h -- control-table types shared between uvcd_gadget.c and
 * config persistence (uvcd_config.c). gadget_t itself stays opaque outside
 * uvcd_gadget.c; everything else needed to drive it lives behind the
 * accessors declared here.
 */
#ifndef UVCD_GADGET_H
#define UVCD_GADGET_H

#include <stddef.h>
#include <time.h>

#include "uvcd.h"

typedef struct gadget_s gadget_t;

/* --------------------------------------------------------------------------
 * Control table -- single source of truth for UVC request dispatch,
 * config load/save, and RESET. `selector` is only meaningful inside
 * uvcd_gadget.c (UVC_PU_* for UVCD_CTRL_STANDARD, UVCD_CUSTOM_* for
 * UVCD_CTRL_CUSTOM, UVC_CT_* for UVCD_CTRL_CAMERA); callers outside that
 * file never need to interpret it.
 */
typedef enum {
	UVCD_CTRL_STANDARD, /* UVC Processing Unit */
	UVCD_CTRL_CUSTOM,   /* UVC Extension Unit 4 */
	UVCD_CTRL_CAMERA,   /* UVC Camera Terminal */
} uvcd_ctrl_kind_t;

struct uvcd_ctrl_def {
	const char *name;
	uvcd_ctrl_kind_t kind;
	unsigned selector;
	int min, max, def;
	size_t state_offset; /* offsetof(struct uvcd_control_state, <field>) */
	int res;       /* GET_RES reply; 0 means 1 */
	unsigned part; /* which value within a multi-value UVC control (pan/tilt) */
};

static inline int *uvcd_ctrl_field(struct uvcd_control_state *s, const struct uvcd_ctrl_def *def)
{
	return (int *)((char *)s + def->state_offset);
}

size_t uvcd_ctrl_count(void);
const struct uvcd_ctrl_def *uvcd_ctrl_at(size_t i);
const struct uvcd_ctrl_def *uvcd_ctrl_find(const char *name);

/* Read the live shadow value for a control (no HAL round-trip). */
int uvcd_ctrl_get(gadget_t *g, const struct uvcd_ctrl_def *def);

/* Clamp to [def->min, def->max], push to the HAL (when it is up -- else the
 * value is only stored, for the next bring-up), and update the shadow on
 * success. Returns 0 with *out_applied set to the value actually applied,
 * or -1 if the HAL rejected it (shadow left unchanged). */
int uvcd_ctrl_set(gadget_t *g, const struct uvcd_ctrl_def *def, int value, int *out_applied);

/* Restore every control to its compiled-in factory default and persist the
 * result immediately. Reachable from the host as UVC XU selector 10. */
void uvcd_ctrl_reset(gadget_t *g);

#endif /* UVCD_GADGET_H */
