#include "btle_options.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int convert_mac_address(char *text, uint8_t *address, uint8_t *mask_out)
{
	int i;
	char *mask_text;
	unsigned mask = 48;

	if (text == NULL || address == NULL || mask_out == NULL)
		return 0;
	if (strcmp(text, "none") == 0) {
		memset(address, 0, 6);
		*mask_out = 0;
		return 1;
	}

	mask_text = strchr(text, '/');
	if (mask_text != NULL) {
		*mask_text = '\0';
		++mask_text;
		mask = strtoul(mask_text, NULL, 10);
		if (mask > 48) {
			printf("Error: MAC mask must be between 0 and 48\n");
			return 0;
		}
	}
	*mask_out = mask;

	if (strlen(text) != 6 * 2 + 5) {
		printf("Error: MAC address is wrong length\n");
		return 0;
	}

	for (i = 0; i < 6 * 3; i += 3) {
		if (!isxdigit((unsigned char)text[i]) ||
		    !isxdigit((unsigned char)text[i + 1])) {
			printf("Error: MAC address contains invalid character(s)\n");
			return 0;
		}
		if (i < 5 * 3 && text[i + 2] != ':') {
			printf("Error: MAC address contains invalid character(s)\n");
			return 0;
		}
	}

	for (i = 0; i < 6; ++i) {
		unsigned byte;
		sscanf(&text[i * 3], "%02x", &byte);
		address[i] = byte;
	}

	return 1;
}

int parse_access_address(const char *text, uint32_t *address)
{
	uint32_t value = 0;
	size_t i, n;

	if (text == NULL || address == NULL)
		return 0;
	n = strlen(text);
	if (n == 0 || n > 8)
		return 0;
	for (i = 0; i < n; i++) {
		int c = (unsigned char)text[i];
		if (!isxdigit(c))
			return 0;
		value = (value << 4) |
		        (uint32_t)(isdigit(c) ? c - '0' : tolower(c) - 'a' + 10);
	}
	*address = value;
	return 1;
}
