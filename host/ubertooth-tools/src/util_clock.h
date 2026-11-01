/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Monotonic clock and sleep for the tools that wait or time something
 * (ubertooth-util's reset wait, ubertooth-specan's -v summary lines), kept in
 * their own file so the main() tests can link a fake clock instead.
 */
#ifndef UBERTOOTH_UTIL_CLOCK_H
#define UBERTOOTH_UTIL_CLOCK_H

#include <stdint.h>

/* Milliseconds from an arbitrary origin; never goes backwards. */
uint64_t util_clock_now_ms(void);

void util_clock_sleep_ms(unsigned ms);

#endif /* UBERTOOTH_UTIL_CLOCK_H */
