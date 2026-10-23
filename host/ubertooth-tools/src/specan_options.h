#ifndef UBERTOOTH_SPECAN_OPTIONS_H
#define UBERTOOTH_SPECAN_OPTIONS_H

int specan_parse_frequency(const char *text, int *frequency);
int specan_validate_frequency_range(int lower, int upper);

#endif
