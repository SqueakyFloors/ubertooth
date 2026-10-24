#include "btle_options.h"
#include "opt_parse.h"

#include <stdio.h>
#include <string.h>

/* The widest MAC mask: 6 bytes of 8 bits. */
#define MAC_MASK_MAX 48

int convert_mac_address(char *text, uint8_t *address, uint8_t *mask_out)
{
	char *mask_text;
	int mask = MAC_MASK_MAX;

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
		if (opt_parse_int(mask_text, 0, MAC_MASK_MAX, &mask) < 0) {
			printf("Error: MAC mask must be between 0 and %d\n",
			       MAC_MASK_MAX);
			return 0;
		}
	}

	if (strlen(text) != 6 * 2 + 5) {
		printf("Error: MAC address is wrong length\n");
		return 0;
	}
	if (opt_parse_bd_addr(text, address) < 0) {
		printf("Error: MAC address contains invalid character(s)\n");
		return 0;
	}

	*mask_out = (uint8_t)mask;
	return 1;
}
