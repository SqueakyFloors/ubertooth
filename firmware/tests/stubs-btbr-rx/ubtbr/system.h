/*
 * Host-test stand-in for firmware/btbr/include/ubtbr/system.h.
 *
 * The real header masks interrupts with the Cortex-M PRIMASK register through
 * inline assembly. The message queue helpers the receive task's headers pull in
 * use it, and nothing in this test runs concurrently, so here it does nothing.
 *
 * This file is part of Project Ubertooth and is released under the terms of
 * the GPL. Refer to COPYING for more information.
 */
#ifndef UBERTOOTH_TEST_BTBR_RX_SYSTEM_STUB_H
#define UBERTOOTH_TEST_BTBR_RX_SYSTEM_STUB_H

#include <stdint.h>

static inline uint32_t irq_save_disable(void)
{
	return 0;
}

static inline void irq_restore(uint32_t primask)
{
	(void)primask;
}

#endif /* UBERTOOTH_TEST_BTBR_RX_SYSTEM_STUB_H */
