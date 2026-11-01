/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Exclusive mode options of ubertooth-util. See util_modes.h.
 */
#include "util_modes.h"

static const char *const mode_options[UTIL_MODE_COUNT] = {
	"-q", "-n", "-e", "-t", "-x", "-I", "-i",
};

const char *util_mode_option(enum util_mode mode)
{
	if ((int)mode < 0 || mode >= UTIL_MODE_COUNT)
		return NULL;
	return mode_options[mode];
}

int util_check_modes(const int requested[UTIL_MODE_COUNT], FILE *err)
{
	int count = 0;
	int m;

	for (m = 0; m < UTIL_MODE_COUNT; m++)
		count += requested[m] ? 1 : 0;
	if (count <= 1)
		return 0;

	fprintf(err, "error: options");
	for (m = 0; m < UTIL_MODE_COUNT; m++) {
		if (requested[m])
			fprintf(err, " %s", mode_options[m]);
	}
	fprintf(err, " cannot be combined; give only one of");
	for (m = 0; m < UTIL_MODE_COUNT; m++)
		fprintf(err, " %s", mode_options[m]);
	fprintf(err, "\n");
	return 1;
}
