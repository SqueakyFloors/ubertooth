#ifndef UBERTOOTH_BTLE_OPTIONS_H
#define UBERTOOTH_BTLE_OPTIONS_H

#include <stdint.h>

/* Parse a connection-following target: a BD_ADDR, optionally followed by
 * "/<bits>" to match only the first <bits> of it, or "none" to clear the
 * target. Returns 1 and sets *address and *mask_out, or prints why not and
 * returns 0. <text> is modified in place.
 *
 * The faux slave address (-s) is one address and nothing else, so it is read
 * with opt_require_bd_addr() instead: a mask and "none" belong to the target. */
int btle_parse_target(char *text, uint8_t *address, uint8_t *mask_out);

#endif
