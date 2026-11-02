/*
 * Host-test stand-in for firmware/common/ubertooth.h, for the test that
 * compiles the wait before a FIFO read.
 *
 * It is the shared hardware-free stub plus the one piece of hardware that wait
 * reads: timer 0's counter. A function call rather than a variable, so the
 * test can move it on every read -- the wait only reads it, and a loop polling
 * a counter that never moves would run to its cap every time.
 */
#ifndef UBERTOOTH_TEST_RXLEAD_STUB_H
#define UBERTOOTH_TEST_RXLEAD_STUB_H

#include "../stubs/ubertooth.h"

uint32_t fake_clock_read(void);
#define T0TC fake_clock_read()

#endif /* UBERTOOTH_TEST_RXLEAD_STUB_H */
