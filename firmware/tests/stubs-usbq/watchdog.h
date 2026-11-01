/*
 * Host-test stand-in for firmware/common/watchdog.h, for ubertooth_usb.c.
 *
 * handle_usb() records that it is servicing USB and feeds the watchdog when
 * the pass is done. Those are the two calls it makes into the watchdog, and
 * the test supplies both, so it can check that a completed pass is what feeds.
 */
#ifndef UBERTOOTH_FW_TESTS_STUB_USBQ_WATCHDOG_H
#define UBERTOOTH_FW_TESTS_STUB_USBQ_WATCHDOG_H

#include <stdint.h>

uint8_t watchdog_mark(uint8_t site);
uint8_t watchdog_mark_inner(uint8_t site);
void watchdog_unmark(uint8_t site);
void watchdog_usb_serviced(void);

/* The low-energy engine counts a failed capture transfer here. */
void watchdog_count_transfer_error(void);

#endif /* UBERTOOTH_FW_TESTS_STUB_USBQ_WATCHDOG_H */
