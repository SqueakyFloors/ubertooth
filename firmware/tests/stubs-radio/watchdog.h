/*
 * Host-test stand-in for firmware/common/watchdog.h.
 *
 * The bounded radio waits in cc2400_wait.c record which wait they are in, so
 * a watchdog reset says where the board was stuck. Only that one call reaches
 * this header, and the test supplies it, so a test can check the site the wait
 * recorded and the site it put back.
 *
 * This file is part of Project Ubertooth and is released under the terms of
 * the GPL. Refer to COPYING for more information.
 */
#ifndef UBERTOOTH_FW_TESTS_STUB_RADIO_WATCHDOG_H
#define UBERTOOTH_FW_TESTS_STUB_RADIO_WATCHDOG_H

#include <stdint.h>

uint8_t watchdog_mark(uint8_t site);
uint8_t watchdog_mark_wait(uint8_t site);
void watchdog_leave_wait(uint8_t site);

#endif /* UBERTOOTH_FW_TESTS_STUB_RADIO_WATCHDOG_H */
