#ifndef UBERTOOTH_SPECAN_OPTIONS_H
#define UBERTOOTH_SPECAN_OPTIONS_H

int specan_parse_frequency(const char *text, int *frequency);
int specan_validate_frequency_range(int lower, int upper);

/* Nonzero if a reported sample's frequency is inside the requested sweep.
 * The firmware only sweeps lower..upper, so a sample outside it is left over
 * from an earlier run (queued on the device before this run started). */
int specan_sample_in_range(int frequency, int lower, int upper);

#endif
