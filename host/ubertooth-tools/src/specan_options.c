/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "specan_options.h"
#include "opt_parse.h"

int specan_parse_frequency(const char *text, int *frequency)
{
	if (frequency == NULL)
		return -1;
	return opt_parse_int(text, SPECAN_FREQ_MIN, SPECAN_FREQ_MAX, frequency);
}

int specan_validate_frequency_range(int lower, int upper)
{
	return lower <= upper ? 0 : -1;
}

int specan_sample_in_range(int frequency, int lower, int upper)
{
	return frequency >= lower && frequency <= upper;
}
