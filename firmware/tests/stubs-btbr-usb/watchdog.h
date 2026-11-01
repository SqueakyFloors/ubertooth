/*
 * Host-test stand-in for firmware/common/watchdog.h, for btbr's
 * ubertooth_usb.c.
 *
 * Its two service paths mark that they are servicing USB and feed the watchdog
 * once the controller has had a pass. The test supplies both, and counts the
 * feeds, so it can check that the bounded wait feeds while it waits rather than
 * leaving the watchdog to run out on a board that is answering the host.
 */
#ifndef UBERTOOTH_FW_TESTS_STUB_BTBR_USB_WATCHDOG_H
#define UBERTOOTH_FW_TESTS_STUB_BTBR_USB_WATCHDOG_H

#include <stdint.h>

/* The site values this file's callers use. The real enum lives in the host
 * interface header, which this test does not include. */
#define WATCHDOG_SITE_NONE 0
#define WATCHDOG_SITE_USB  1

uint8_t watchdog_mark(uint8_t site);
void watchdog_usb_serviced(void);

#endif /* UBERTOOTH_FW_TESTS_STUB_BTBR_USB_WATCHDOG_H */
