/*
 * Copyright 2019 Mike Ryan
 *
 * This file is part of Project Ubertooth and is released under the
 * terms of the GPL. Refer to COPYING for more information.
 */

#include "ubertooth.h"
#include "fault_frame.h"
#include "watchdog.h"

// delay for about 30 ms (very rough)
static void delay_30ms(void) {
	volatile int i;
	for (i = 0; i < 3000000; ++i)
		;
}

// flash LEDs a certain number of times
static void flash_leds(int count) {
	int i;

	for (i = 0; i < count; ++i) {
		USRLED_SET;
		RXLED_SET;
		TXLED_SET;
		delay_30ms();
		USRLED_CLR;
		RXLED_CLR;
		TXLED_CLR;
		delay_30ms();
	}
	delay_30ms();
	delay_30ms();
	delay_30ms();
}

/*
 * These four blink a code forever and never service USB. Since the watchdog is
 * armed they now get about three seconds to blink before it resets the board,
 * so a fault that recurs shows its code in bursts separated by a reboot rather
 * than as one unbroken sequence. That is wanted: the board used to sit there
 * answering nothing until someone unplugged it, and it now comes back usable.
 *
 * Three seconds is not enough to blink a whole code: delay_30ms() is a rough
 * busy loop, nearer 150 ms than 30, so a burst is cut off part-way and counting
 * blinks no longer tells you which fault it was. Each handler therefore records
 * its own fault in the breadcrumb before it starts blinking, and
 * ubertooth-util -w reads it back together with the mode the board was in --
 * which counting blinks never gave you. A site each rather than one shared
 * "a fault" site, so -w says which of the four it was.
 *
 * Each handler also records the part's fault status register and one address:
 * the access the processor refused, where the part keeps a register for it,
 * and otherwise the instruction the processor was running, read from the
 * exception frame (fault_frame.h). The status says which of the two it is, and
 * ubertooth-util -w says so in words. Both are needed, because the one fault
 * this part can take without a memory protection unit -- an instruction fetch
 * from a region that cannot be executed, which is a branch through a corrupted
 * pointer or return address -- is one the part records no access address for,
 * and the address it refused to execute is the whole of what there is to go on.
 *
 * These run at a priority the USB interrupt cannot preempt -- HardFault above
 * everything, the other three at the same 0 as USB -- so the USB handler's feed
 * cannot keep a faulted board alive by accident.
 */
UBERTOOTH_FAULT_ENTRY(HardFault_Handler, hard_fault_at)

void hard_fault_at(uint32_t *frame) {
	uint32_t val = SCB_HFSR;

	watchdog_mark(WATCHDOG_SITE_FAULT_HARD);
	/* A forced hard fault is one of the other three escalated: it was taken
	 * where its own handler could not preempt what was running, which is
	 * anywhere inside an interrupt handler, since those run at the same
	 * priority. The hard fault status says only "forced", so what gets
	 * recorded is the configurable fault status, which says which of the
	 * three it was, and the address register of whichever of them has one
	 * worth reading.
	 *
	 * A board on the bench recorded exactly this during a BLE follow soak,
	 * and the status alone -- forced, with no address -- was as far as the
	 * record went. */
	if (val & SCB_HFSR_FORCED) {
		uint32_t cfsr = SCB_CFSR;

		if (cfsr & UBERTOOTH_FAULT_ADDR_VALID)
			watchdog_fault(cfsr, SCB_MMFAR);
		else if (cfsr & UBERTOOTH_FAULT_BUS_ADDR_VALID)
			watchdog_fault(cfsr, SCB_BFAR);
		else if (cfsr & (FAULT_FRAME_STACKING_ERROR_CFSR_MEM |
		                 FAULT_FRAME_STACKING_ERROR_CFSR_BUS))
			watchdog_fault(cfsr, 0);
		else
			watchdog_fault(cfsr, fault_frame_pc(frame));
	} else {
		/* A vector table read that failed, or a debug event: the part
		 * records no access address for either, so the instruction the
		 * processor was running is what gets reported. */
		watchdog_fault(val, fault_frame_pc(frame));
	}

	while (1) {
		flash_leds(2);

		if (val & (1 << 1)) {
			flash_leds(3); // vector table read
		} else if (val & (1 << 30)) {
			flash_leds(4); // forced (should never happen)
		}

		delay_30ms();
		delay_30ms();
		delay_30ms();
	}
}

UBERTOOTH_FAULT_ENTRY(MemManagement_Handler, mem_fault_at)

void mem_fault_at(uint32_t *frame) {
	int i;
	int offset[5] = { 7, 4, 3, 1, 0 };
	uint8_t mmsr = SCB_MMSR;

	watchdog_mark(WATCHDOG_SITE_FAULT_MEM);
	/* A write into the guard at the bottom of the stack arrives here, and so
	 * does an instruction fetch from an address the part refuses to execute
	 * -- a branch through a corrupted pointer or return address.
	 *
	 * Bit 7 of the status is the part's own "the address register holds this
	 * fault's address". It is set for the refused write, where that address
	 * is what the stack ran into, and zero is a real address there -- a null
	 * pointer dereference records it. It is clear for the instruction fetch,
	 * and there the address worth having is the one the processor tried to
	 * run: where the corrupted pointer or return address pointed. The
	 * pointer's own value has its lowest bit set to select the instruction
	 * set and this address does not. */
	if (mmsr & UBERTOOTH_FAULT_ADDR_VALID)
		watchdog_fault(mmsr, SCB_MMFAR);
	else if (mmsr & FAULT_FRAME_STACKING_ERROR)
		watchdog_fault(mmsr, 0);
	else
		watchdog_fault(mmsr, fault_frame_pc(frame));

	while (1) {
		flash_leds(3);

		for (i = 0; i < 5; ++i) {
			if (mmsr & (1 << offset[i])) {
				flash_leds(i+1);
				break;
			}
		}

		delay_30ms();
		delay_30ms();
		delay_30ms();
	}
}

UBERTOOTH_FAULT_ENTRY(BusFault_Handler, bus_fault_at)

void bus_fault_at(uint32_t *frame) {
	int i;
	int offset[6] = { 7, 4, 3, 2, 1, 0 };
	uint8_t bfsr = SCB_BFSR;

	watchdog_mark(WATCHDOG_SITE_FAULT_BUS);
	/* As above: bit 7 of the status says whether the address register holds
	 * this fault's address. It does not for an imprecise fault, where the
	 * processor had moved on before the bus reported the error, and there
	 * the instruction the frame names is near what made the access rather
	 * than the access itself. */
	if (bfsr & UBERTOOTH_FAULT_ADDR_VALID)
		watchdog_fault(bfsr, SCB_BFAR);
	else if (bfsr & FAULT_FRAME_STACKING_ERROR)
		watchdog_fault(bfsr, 0);
	else
		watchdog_fault(bfsr, fault_frame_pc(frame));

	while (1) {
		flash_leds(4);

		for (i = 0; i < 6; ++i) {
			if (bfsr & (1 << offset[i])) {
				flash_leds(i+1);
				break;
			}
		}

		delay_30ms();
		delay_30ms();
		delay_30ms();
	}
}

UBERTOOTH_FAULT_ENTRY(UsageFault_Handler, usage_fault_at)

void usage_fault_at(uint32_t *frame) {
	int i;
	int offset[6] = { 9, 8, 3, 2, 1, 0 };
	uint16_t ufsr = SCB_UFSR;

	watchdog_mark(WATCHDOG_SITE_FAULT_USAGE);
	/* The part keeps no address register for this fault, and bit 7 of its
	 * status is reserved and reads zero, so the instruction the frame names
	 * is the only locator there is -- and for this fault it is the right
	 * one, because what the processor refused is the instruction itself. */
	watchdog_fault(ufsr, fault_frame_pc(frame));

	while (1) {
		flash_leds(5);

		for (i = 0; i < 6; ++i) {
			if (ufsr & (1 << offset[i])) {
				flash_leds(i+1);
				break;
			}
		}

		delay_30ms();
		delay_30ms();
		delay_30ms();
	}
}
