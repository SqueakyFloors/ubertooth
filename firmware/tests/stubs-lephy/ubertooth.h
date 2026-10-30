/*
 * Host-test stand-in for firmware/common/ubertooth.h, for the tests that
 * compile firmware/bluetooth_rxtx/le_phy.c.
 *
 * It is not a rewrite of that header: it is the real one, with the register
 * file underneath it replaced. The fake lpc17.h comes first and defines the
 * real header's include guard, so the LED macros, the pin numbers, the SSP
 * the radio's bytes arrive on and every function prototype below are the
 * firmware's own -- only the addresses they resolve to are ordinary memory.
 *
 * The one thing it adds is a declaration of the mode the main loop records,
 * which the application image owns (bluetooth_rxtx.c) next to the
 * requested_mode the real header already declares. le_phy.c reads neither
 * through this header today, but both are extern there, so the test supplies
 * them.
 */
#ifndef UBERTOOTH_FW_TESTS_STUB_LEPHY_UBERTOOTH_H
#define UBERTOOTH_FW_TESTS_STUB_LEPHY_UBERTOOTH_H

/*
 * The firmware's wait(u8 seconds) is a busy loop for the board's clock. The
 * host has a wait(int *) of its own in <sys/wait.h>, which <stdlib.h> pulls
 * in -- and le_phy.c includes <stdlib.h> -- so the two declarations meet.
 * Pulling the host's in first and renaming the firmware's for the length of
 * the real header keeps that header usable here, and leaves `wait` meaning
 * the host's everywhere else. Nothing compiled into these tests calls the
 * firmware's; one that did would fail to link, which is the right way to
 * find out.
 */
#include <stdlib.h>

#define wait ubertooth_wait_seconds
#include "lpc17.h"
#include "../../common/ubertooth.h"
#undef wait

/* The board's second RAM bank, which a host test has no linker script for and
 * whose section name is not even valid here. */
#undef AHB_SRAM
#define AHB_SRAM

extern volatile uint8_t mode;

#endif /* UBERTOOTH_FW_TESTS_STUB_LEPHY_UBERTOOTH_H */
