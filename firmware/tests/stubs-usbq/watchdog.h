/*
 * Host-test stand-in for firmware/common/watchdog.h, for ubertooth_usb.c.
 *
 * handle_usb() records that it is servicing USB and feeds the watchdog when
 * the pass is done. Those are the two calls it makes into the watchdog, and
 * the test supplies both, so it can check that a completed pass is what feeds.
 *
 * This file is part of Project Ubertooth and is released under the terms of
 * the GPL. Refer to COPYING for more information.
 */
#ifndef UBERTOOTH_FW_TESTS_STUB_USBQ_WATCHDOG_H
#define UBERTOOTH_FW_TESTS_STUB_USBQ_WATCHDOG_H

#include <stdint.h>

uint8_t watchdog_mark(uint8_t site);
void watchdog_usb_serviced(void);

#endif /* UBERTOOTH_FW_TESTS_STUB_USBQ_WATCHDOG_H */
