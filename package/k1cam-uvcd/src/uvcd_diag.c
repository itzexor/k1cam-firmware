#include "uvcd_diag.h"
#include <stdio.h>
#include <stdarg.h>

static int append(char **out, size_t *left, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(*out, *left, fmt, ap);
	va_end(ap);
	if (n < 0 || (size_t)n >= *left)
		return -1;
	*out += n;
	*left -= n;
	return 0;
}

int uvcd_diag_format(const uint8_t *r, char *out, size_t size)
{
	if (r[0] != UVCD_DIAG_VERSION || uvcd_diag_get(r + 4, 2) < UVCD_DIAG_SIZE)
		return -1;
	static const struct {
		const char *name;
		unsigned offset, bytes;
	} fields[] = {
	    {"version", 0, 1},		 {"flags", 1, 1},	    {"sequence", 2, 2},
	    {"record_size", 4, 2},	 {"valid", 6, 2},	    {"monotonic_ms", 8, 8},
	    {"metering", 16, 1},	 {"comp_requested", 17, 1}, {"comp_applied", 18, 1},
	    {"guard_top_bin", 19, 1},	 {"ae_target", 20, 2},	    {"ae_mean", 22, 2},
	    {"exposure_us", 24, 4},	 {"gain_x256", 28, 4},	    {"awb_kelvin", 32, 2},
	    {"sensor_width", 34, 2},	 {"sensor_height", 36, 2},  {"view_x", 38, 2},
	    {"view_y", 40, 2},		 {"view_width", 42, 2},	    {"view_height", 44, 2},
	    {"zoom_x100", 46, 2},	 {"rotation", 48, 2},	    {"guard_level_x100", 50, 2},
	    {"guard_cut_x10000", 52, 2},
	};
	for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
		if (append(
			&out, &size, "%s=%llu\n", fields[i].name,
			(unsigned long long)uvcd_diag_get(r + fields[i].offset, fields[i].bytes)))
			return -1;
	for (unsigned row = 0; row < 15; row++) {
		if (append(&out, &size, "weights_%02u=", row))
			return -1;
		for (unsigned col = 0; col < 15; col++) {
			unsigned i = row * 15 + col;
			unsigned w = (r[UVCD_DIAG_WEIGHTS_OFFSET + i / 2] >> (4 * (i % 2))) & 15;
			if (append(&out, &size, "%s%u", col ? " " : "", w))
				return -1;
		}
		if (append(&out, &size, "\nlevels_%02u=", row))
			return -1;
		for (unsigned col = 0; col < 15; col++)
			if (append(&out, &size, "%s%u", col ? " " : "",
				   (unsigned)uvcd_diag_get(
				       r + UVCD_DIAG_ZONES_OFFSET + 2 * (row * 15 + col), 2)))
				return -1;
		if (append(&out, &size, "\n"))
			return -1;
	}
	if (append(&out, &size, "histogram="))
		return -1;
	for (unsigned i = 0; i < 64; i++)
		if (append(&out, &size, "%s%u", i ? " " : "",
			   (unsigned)uvcd_diag_get(r + UVCD_DIAG_HIST_OFFSET + i * 2, 2)))
			return -1;
	return append(&out, &size, "\nok\n");
}
