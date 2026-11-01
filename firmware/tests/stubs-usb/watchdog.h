/*
 * Host-test stand-in for firmware/common/watchdog.h, for the USB hardware
 * layer and the control transfer handler.
 *
 * Those two files mark the steps of a USB service pass for the breadcrumb.
 * The marks are plain stores on the board and have no bearing on what this
 * test checks -- that every wait in there terminates -- so here they do
 * nothing. The real header reaches the part's registers through
 * ubertooth.h, which this test does not stub.
 */
#ifndef UBERTOOTH_FW_TESTS_STUB_USB_WATCHDOG_H
#define UBERTOOTH_FW_TESTS_STUB_USB_WATCHDOG_H

#include <stdint.h>

/* The site values, which are shared with the host. */
#include "ubertooth_interface.h"

static inline uint8_t watchdog_mark_inner(uint8_t site)
{
	(void)site;
	return WATCHDOG_SITE_NONE;
}

static inline void watchdog_unmark(uint8_t site)
{
	(void)site;
}

#endif /* UBERTOOTH_FW_TESTS_STUB_USB_WATCHDOG_H */
