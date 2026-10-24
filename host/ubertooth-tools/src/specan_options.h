#ifndef UBERTOOTH_SPECAN_OPTIONS_H
#define UBERTOOTH_SPECAN_OPTIONS_H

/* The sweep range in MHz: the 2.4 GHz ISM band the CC2400 tunes, the same as
 * BT_FREQ_MIN..BT_FREQ_MAX in ubertooth_interface.h (repeated here so the
 * option parsing can be unit tested without the library headers). */
#define SPECAN_FREQ_MIN 2402
#define SPECAN_FREQ_MAX 2480

/* A decimal frequency in MHz inside the sweep range. Returns 0 and sets
 * *frequency, or -1 and leaves it alone. */
int specan_parse_frequency(const char *text, int *frequency);
int specan_validate_frequency_range(int lower, int upper);

/* Nonzero if a reported sample's frequency is inside the requested sweep.
 * The firmware only sweeps lower..upper, so a sample outside it is left over
 * from an earlier run (queued on the device before this run started). */
int specan_sample_in_range(int frequency, int lower, int upper);

#endif
