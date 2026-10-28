#ifndef UBERTOOTH_BTLE_OPTIONS_H
#define UBERTOOTH_BTLE_OPTIONS_H

#include <stdint.h>

/* Parse an address option's value: a BD_ADDR, optionally followed by
 * "/<bits>" to match only the first <bits> of it, or "none" for the all-zero
 * address and a zero mask. Returns 1 and sets *address and *mask_out, or
 * prints why not and returns 0. <text> is modified in place.
 *
 * <what> names the address in the refusals ("target", "faux slave"), which
 * read "error: <what> address must be ..." and "error: <what> mask must
 * be ...". -s and -t each pass their own address and mask: they are two
 * different things, and sharing one pair made the option written last set
 * both. */
int convert_mac_address(char *text, const char *what, uint8_t *address,
                        uint8_t *mask_out);

#endif
