/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef __UBERTOOTH_FAULT_FRAME_H
#define __UBERTOOTH_FAULT_FRAME_H

#include <stdint.h>

/*
 * Where a processor fault happened.
 *
 * None of the four fault status registers holds the address of the instruction
 * that faulted, and for two of the four faults the part records no address at
 * all. What does hold the instruction's address is the exception frame: on
 * exception entry the processor pushes eight words to the stack that was in
 * use -- r0 to r3, r12, the link register, the program counter of the
 * instruction being run, and the program status. This firmware runs everything
 * on the main stack, so the frame is at the main stack pointer as the handler
 * is entered.
 *
 * "As the handler is entered" is the catch. A compiled function moves the
 * stack pointer in its first instructions, by however much its locals need, so
 * once C code is running the pointer no longer says where the frame is. A
 * handler that wants the frame therefore has to be entered with no prologue,
 * which is what UBERTOOTH_FAULT_ENTRY() gives it: the compiler emits the entry
 * point literally, and its only instructions read the main stack pointer into
 * the first argument register and jump to the handler proper.
 *
 * The handler proper takes the frame as its argument and must never return,
 * which every fault handler in this firmware already satisfies: each blinks a
 * code until the watchdog resets the board.
 */
#define UBERTOOTH_FAULT_ENTRY(entry, handler)				\
	void handler(uint32_t *frame) __attribute__((noreturn));	\
	void entry(void) __attribute__((naked));			\
	void entry(void)						\
	{								\
		__asm__ __volatile__("mrs r0, msp\n\t"			\
				     "b " #handler "\n");		\
	}

/* How many words the processor pushes, and which of them is the program
 * counter. Core registers only: this part has no floating point unit, so
 * there is no longer frame to allow for. */
#define FAULT_FRAME_WORDS 8
#define FAULT_FRAME_PC_WORD 6

/*
 * "There is no frame to read": bit 4 of the eight-bit memory management and
 * bus fault statuses says the processor failed part-way through pushing the
 * frame, so the stack pointer has moved but what it points at was never
 * written. The words there are whatever the stack already held, and a handler
 * must not report them as an address. In the configurable fault status, which
 * a forced hard fault records, those two bytes sit at bits 0 and 8.
 */
#define FAULT_FRAME_STACKING_ERROR          0x0010u  /* in MMFSR or BFSR */
#define FAULT_FRAME_STACKING_ERROR_CFSR_MEM 0x0010u  /* in CFSR */
#define FAULT_FRAME_STACKING_ERROR_CFSR_BUS 0x1000u  /* in CFSR */

/*
 * The program counter in a frame that lies within [bottom, top), or zero if it
 * does not.
 *
 * The bounds check is not ceremony. A fault taken while the stack pointer is
 * wrong is one of the things this record exists to catch, and reading through a
 * wrong pointer here would fault again inside the handler and leave no record
 * at all. Zero doubles as "nothing to report". The one fault that could name
 * address zero is a branch through a null function pointer, which asks for the
 * other instruction set and so faults on the state rather than the fetch; that
 * one loses its address, and the fault status still says what happened.
 *
 * Written with the bounds as arguments, as stack_check.h does, so that a host
 * test can run it over an array of its own; the arithmetic is the part worth
 * pinning.
 */
static inline uint32_t fault_frame_pc_between(const uint32_t *frame,
                                              uintptr_t bottom, uintptr_t top)
{
	const uintptr_t at = (uintptr_t)frame;
	const uintptr_t bytes = FAULT_FRAME_WORDS * sizeof(uint32_t);

	/* The architecture keeps the stack pointer on a four-byte boundary. */
	if ((at & 3u) != 0)
		return 0;
	/* In the region at all, and then with the whole frame inside it. The
	 * two are separate so that neither subtraction can run below zero. */
	if (at < bottom || at >= top)
		return 0;
	if (top - at < bytes)
		return 0;

	return frame[FAULT_FRAME_PC_WORD];
}

/*
 * The stack the frame has to be in, from sections.ld: the lowest address the
 * stack may use, and the top of RAM.
 *
 * _stack_bottom and not _stack_limit, which is lower. Between the two sits the
 * guard the memory protection unit is told to refuse, so that a stack running
 * out of room faults instead of writing over what is below it
 * (stack_check.h). Reading a frame there would be refused as well, and the
 * second fault would be taken inside this handler and lose the record
 * altogether -- which is the one thing the bounds check exists to prevent.
 */
extern unsigned long _stack_bottom;
extern unsigned long _StackTop;

/* The same, over the stack this firmware runs on. */
static inline uint32_t fault_frame_pc(const uint32_t *frame)
{
	return fault_frame_pc_between(frame, (uintptr_t)&_stack_bottom,
	                              (uintptr_t)&_StackTop);
}

#endif /* __UBERTOOTH_FAULT_FRAME_H */
