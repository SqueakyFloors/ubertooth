#include "ego_options.h"
#include "opt_parse.h"

#include <string.h>

int ego_parse_packet_length(const char *text, uint8_t *length)
{
	int value;

	if (length == NULL ||
	    opt_parse_int(text, EGO_PACKET_LENGTH_MIN, EGO_PACKET_LENGTH_MAX,
	                  &value) < 0)
		return -1;
	*length = (uint8_t)value;
	return 0;
}

int ego_parse_access_code(const char *text, uint8_t *code, int *length)
{
	uint8_t bytes[EGO_ACCESS_CODE_BYTES];
	size_t i, n;

	if (text == NULL || code == NULL || length == NULL)
		return -1;
	n = strlen(text);
	/* whole bytes only: 2 hex digits each, 1 to EGO_ACCESS_CODE_BYTES of them */
	if (n < 2 || n > EGO_ACCESS_CODE_BYTES * 2 || n % 2 != 0)
		return -1;
	for (i = 0; i < n / 2; i++) {
		char pair[3];
		uint32_t byte;

		pair[0] = text[i * 2];
		pair[1] = text[i * 2 + 1];
		pair[2] = '\0';
		if (opt_parse_hex(pair, 2, 2, &byte) < 0)
			return -1;
		bytes[i] = (uint8_t)byte;
	}
	memcpy(code, bytes, n / 2);
	*length = (int)(n / 2);
	return 0;
}
