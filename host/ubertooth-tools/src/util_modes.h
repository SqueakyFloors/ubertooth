/*
 * Exclusive mode options of ubertooth-util.
 *
 * Each of these options ends the command by putting the device into a
 * continuous mode (or into a programming mode). Only one can run, so asking
 * for several in one command is a usage error, detected before any device is
 * opened. Hardware-independent so it can be tested without an Ubertooth.
 */
#ifndef UBERTOOTH_UTIL_MODES_H
#define UBERTOOTH_UTIL_MODES_H

#include <stdio.h>

enum util_mode {
	UTIL_MODE_LED_SPECAN, /* -q */
	UTIL_MODE_RANGE_TEST, /* -n */
	UTIL_MODE_REPEATER,   /* -e */
	UTIL_MODE_TX_TEST,    /* -t */
	UTIL_MODE_XMAS,       /* -x */
	UTIL_MODE_IDENTIFY,   /* -I */
	UTIL_MODE_ISP,        /* -i */
	UTIL_MODE_COUNT
};

/* The command-line option for a mode, for example "-q"; NULL if out of range. */
const char *util_mode_option(enum util_mode mode);

/*
 * Checks that at most one mode is requested (requested[mode] nonzero).
 * Returns 0 when zero or one mode is requested. Otherwise writes one
 * "error: ..." line naming the conflicting options to err and returns 1.
 */
int util_check_modes(const int requested[UTIL_MODE_COUNT], FILE *err);

#endif /* UBERTOOTH_UTIL_MODES_H */
