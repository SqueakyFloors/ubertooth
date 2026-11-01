/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef UBERTOOTH_BTLE_OPTIONS_H
#define UBERTOOTH_BTLE_OPTIONS_H

#include <stdint.h>

/* Parse a target option's value: a BD_ADDR, optionally followed by "/<bits>"
 * to match only the first <bits> of it, or "none" for the all-zero address
 * and a zero mask. Returns 1 and sets *address and *mask_out, or prints why
 * not and returns 0. <text> is modified in place.
 *
 * <what> names the address in the refusals, which read "error: <what>
 * address must be ..." and "error: <what> mask must be ...". */
int convert_mac_address(char *text, const char *what, uint8_t *address,
                        uint8_t *mask_out);

/* Parse an address option's value that is one whole address and nothing
 * else: no "/<bits>" mask, no "none". A faux peripheral advertises from one
 * address, so it has nothing for a mask to narrow or for "none" to clear;
 * both spellings used to parse here and then be dropped. Returns 1 and sets
 * *address, or prints why not and returns 0.
 *
 * <what> names the address in the refusal, which reads "error: <what>
 * address must be xx:xx:xx:xx:xx:xx (got <text>)" -- the same line any other
 * value that is not an address gets. */
int convert_plain_mac_address(const char *text, const char *what,
                              uint8_t *address);

#endif
