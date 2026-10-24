#ifndef UBERTOOTH_BTLE_OPTIONS_H
#define UBERTOOTH_BTLE_OPTIONS_H

#include <stdint.h>

int convert_mac_address(char *text, uint8_t *address, uint8_t *mask_out);

/* Parse an access address given as 1 to 8 hex digits (fewer than 8 means
 * leading zeros: "12345" is 0x00012345). Returns 1 and sets *address, or 0
 * for anything else (empty, a non-hex character, more than 8 digits). */
int parse_access_address(const char *text, uint32_t *address);

#endif
