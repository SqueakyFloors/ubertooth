#include "specan_options.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>

int specan_parse_frequency(const char *text, int *frequency)
{
	char *end;
	long value;

	if (text == NULL || *text == '\0' || frequency == NULL)
		return -1;
	errno = 0;
	value = strtol(text, &end, 10);
	if (errno != 0 || *end != '\0' || value < 2402 || value > 2480 ||
	    value > INT_MAX)
		return -1;
	*frequency = (int)value;
	return 0;
}

int specan_validate_frequency_range(int lower, int upper)
{
	return lower <= upper ? 0 : -1;
}
