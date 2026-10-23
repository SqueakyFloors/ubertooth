#include "ego_options.h"

#include <errno.h>
#include <stdlib.h>

int ego_parse_packet_length(const char *text, uint8_t *length)
{
	char *end;
	long value;

	if (text == NULL || *text == '\0' || length == NULL)
		return -1;
	errno = 0;
	value = strtol(text, &end, 10);
	if (errno != 0 || *end != '\0' || value < 1 || value > 48)
		return -1;
	*length = (uint8_t)value;
	return 0;
}
