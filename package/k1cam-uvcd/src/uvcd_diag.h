/* Read-only diagnostics v1. Explicit little-endian bytes, never a C ABI. */
#ifndef UVCD_DIAG_H
#define UVCD_DIAG_H
#include <stdint.h>
#include <stddef.h>
#define UVCD_DIAG_VERSION 1
#define UVCD_DIAG_LATCH 29
#define UVCD_DIAG_PAGE 30
#define UVCD_DIAG_SIZE 745
#define UVCD_DIAG_PAGE_SIZE 60
#define UVCD_DIAG_PAYLOAD 56
#define UVCD_DIAG_PAGES ((UVCD_DIAG_SIZE + UVCD_DIAG_PAYLOAD - 1) / UVCD_DIAG_PAYLOAD)
#define UVCD_DIAG_TEXT_SIZE 8192
#define UVCD_DIAG_AE 1
#define UVCD_DIAG_EXPOSURE 2
#define UVCD_DIAG_AWB 4
#define UVCD_DIAG_VIEW 8
#define UVCD_DIAG_WEIGHTS 16
#define UVCD_DIAG_ZONES 32
#define UVCD_DIAG_HIST 64
#define UVCD_DIAG_COMP 128
#define UVCD_DIAG_GUARD 256
#define UVCD_DIAG_WEIGHTS_OFFSET 54
#define UVCD_DIAG_ZONES_OFFSET 167
#define UVCD_DIAG_HIST_OFFSET 617
static inline void uvcd_diag_put(uint8_t *p, uint64_t v, unsigned n)
{
	for (unsigned i = 0; i < n; i++)
		p[i] = (uint8_t)(v >> (i * 8));
}
static inline uint64_t uvcd_diag_get(const uint8_t *p, unsigned n)
{
	uint64_t v = 0;
	for (unsigned i = 0; i < n; i++)
		v |= (uint64_t)p[i] << (i * 8);
	return v;
}
/* Returns -1 on an unknown layout, declared size below v1, or short output. */
int uvcd_diag_format(const uint8_t *record, char *out, size_t size);
#endif
