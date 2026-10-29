/*
 * Stand-in for firmware/common/irq.h, for the host unit tests.
 *
 * The real pair is ARM assembly that sets and restores PRIMASK, so it cannot
 * run here. This one records how deeply the code under test is nested inside
 * a guarded section, and gives a test one hook: a function run as the
 * outermost section ends.
 *
 * That hook is how a test stands in for an interrupt. On the board, an
 * interrupt raised while PRIMASK is set is not lost -- the controller latches
 * it and takes it the instant the mask comes off -- so running the hook where
 * the outermost section ends is exactly where the hardware would run the
 * handler. A test sets irq_pending_hook to the work a handler would do, calls
 * the function under test, and asserts that the handler's effect and the
 * guarded section's effect both survived.
 *
 * It is also what makes the guard's absence visible. Code that does its
 * read-modify-write without the guard never ends a section, so the hook never
 * runs, and a test that expects the handler's effect fails.
 *
 * Both variables are defined by the test (test_le_phy_logic.c), because only
 * one test compiles firmware code that uses this header.
 */

#ifndef __IRQ_H
#define __IRQ_H

/* For uint32_t and NULL, as the real header does for its own types. */
#include <stddef.h>
#include <stdint.h>

/* How many guarded sections the code under test is currently inside. */
extern unsigned irq_mask_depth;

/* Run as the outermost guarded section ends, if set: what a handler latched
 * while interrupts were masked would do. */
extern void (*irq_pending_hook)(void);

static inline uint32_t irq_save_disable(void)
{
	return irq_mask_depth++;
}

static inline void irq_restore(uint32_t primask)
{
	irq_mask_depth = primask;

	if (irq_mask_depth == 0 && irq_pending_hook != NULL) {
		void (*hook)(void) = irq_pending_hook;

		/* Cleared before the call, so a hook that reaches guarded code
		 * itself does not run again inside its own call. */
		irq_pending_hook = NULL;
		hook();
	}
}

#endif /* __IRQ_H */
