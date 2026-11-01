/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "btle_options.h"
#include "opt_parse.h"

#include <stdio.h>
#include <string.h>

/* The widest MAC mask: every bit of the address. */
#define MAC_MASK_MAX (OPT_BD_ADDR_BYTES * 8)

int convert_mac_address(char *text, const char *what, uint8_t *address,
                        uint8_t *mask_out)
{
	/* The word as the user wrote it, for the diagnostics: splitting the
	 * mask off below writes into text, which would otherwise make an
	 * error quote only the part before the '/'. */
	char given[80];
	/* "<what> address", the name the refusals call this value. */
	char label[48];
	char *mask_text;
	int mask = MAC_MASK_MAX;

	if (text == NULL || address == NULL || mask_out == NULL)
		return 0;
	snprintf(given, sizeof(given), "%s", text);
	snprintf(label, sizeof(label), "%s address", what);
	if (strcmp(text, "none") == 0) {
		memset(address, 0, OPT_BD_ADDR_BYTES);
		*mask_out = 0;
		return 1;
	}

	mask_text = strchr(text, '/');
	if (mask_text != NULL) {
		*mask_text = '\0';
		++mask_text;
		if (opt_parse_int(mask_text, 0, MAC_MASK_MAX, &mask) < 0) {
			fprintf(stderr, "error: %s mask must be 0-%d (got %s)\n",
			        what, MAC_MASK_MAX, mask_text);
			return 0;
		}
	}

	if (opt_require_bd_addr(text, label, given, address) < 0)
		return 0;

	*mask_out = (uint8_t)mask;
	return 1;
}

int convert_plain_mac_address(const char *text, const char *what,
                              uint8_t *address)
{
	/* "<what> address", the name the refusal calls this value. */
	char label[48];

	if (text == NULL || address == NULL)
		return 0;
	snprintf(label, sizeof(label), "%s address", what);
	/* No mask is split off and "none" is not a word here, so the whole
	 * value goes to the address parser -- a mask or "none" is refused as
	 * the address it is not -- and there is no separate word to quote. */
	if (opt_require_bd_addr(text, label, NULL, address) < 0)
		return 0;
	return 1;
}
