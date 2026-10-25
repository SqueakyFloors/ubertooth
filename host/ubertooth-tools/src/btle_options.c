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
			fprintf(stderr, "error: MAC mask must be 0-%d (got %s)\n",
			        MAC_MASK_MAX, mask_text);
			return 0;
		}
	}

	/* The mask has been split off by now, so these quote the address part
	 * of what the user wrote rather than the whole word. */
	if (strlen(text) != 6 * 2 + 5) {
		fprintf(stderr, "error: MAC address must be"
		        " xx:xx:xx:xx:xx:xx (got %s)\n", text);
		return 0;
	}
	if (opt_parse_bd_addr(text, address) < 0) {
		fprintf(stderr, "error: MAC address must be hex digits and"
		        " colons (got %s)\n", text);
		return 0;
	}

	*mask_out = (uint8_t)mask;
	return 1;
}
