/*
 * Host-test stand-in for firmware/common/ubertooth.h, for the tests that ask
 * which GPIO port the GIO6 macros reach for on each board.
 *
 * It is not a rewrite of that header: it is the real one, with the register
 * file underneath it replaced. The fake lpc17.h comes first and defines the
 * real register header's include guard, so the pin numbers and the GIO6
 * macros below are the firmware's own -- only the addresses they resolve to
 * are ordinary memory.
 *
 * This file is part of Project Ubertooth and is released under the terms of
 * the GPL. Refer to COPYING for more information.
 */
#ifndef UBERTOOTH_FW_TESTS_STUB_GIO6_UBERTOOTH_H
#define UBERTOOTH_FW_TESTS_STUB_GIO6_UBERTOOTH_H

/*
 * The firmware's wait(u8 seconds) is a busy loop for the board's clock. The
 * host has a wait(int *) of its own in <sys/wait.h>, which <stdlib.h> pulls
 * in, so the two declarations meet. Renaming the firmware's for the length of
 * the real header keeps that header usable here, and leaves `wait` meaning the
 * host's everywhere else. Nothing compiled into these tests calls the
 * firmware's; one that did would fail to link, which is the right way to find
 * out.
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

#endif /* UBERTOOTH_FW_TESTS_STUB_GIO6_UBERTOOTH_H */
