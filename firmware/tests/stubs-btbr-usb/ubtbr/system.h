/*
 * Host-test stand-in for firmware/btbr/include/ubtbr/system.h.
 *
 * The real header masks interrupts with the Cortex-M PRIMASK register through
 * inline assembly. The btbr queue helpers use it, and nothing in this test
 * runs concurrently, so here it does nothing.
 */
#ifndef UBERTOOTH_TEST_BTBR_SYSTEM_STUB_H
#define UBERTOOTH_TEST_BTBR_SYSTEM_STUB_H

#include <stdint.h>

static inline uint32_t irq_save_disable(void)
{
	return 0;
}

static inline void irq_restore(uint32_t primask)
{
	(void)primask;
}

#endif /* UBERTOOTH_TEST_BTBR_SYSTEM_STUB_H */
