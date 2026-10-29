/* Fault handlers
 *
 * Copyright 2019 Mike Ryan
 * Copyright 2020 Etienne Helluy-Lafont, Univ. Lille, CNRS.
 *
 * This file is part of Project Ubertooth.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#include <stdarg.h>
#include <ubertooth.h>
#include <ubtbr/debug.h>
#include <ubtbr/ubertooth_usb.h>
#include <ubtbr/system.h>
#include <watchdog.h>
#include "tinyprintf.h"

/*
 * Every path in this file ends in blink_fault_code() below, which flashes its
 * code and never returns. Nothing in that loop feeds the watchdog, so the
 * board resets itself a few seconds later and comes back usable; before the
 * watchdog it sat there answering nothing on USB until someone unplugged it.
 *
 * Each path therefore records where it was in the breadcrumb first, and
 * ubertooth-util -w reads that back once the board has reconnected. Without it
 * the record would still name whatever the firmware was doing before the fault
 * -- "not in a wait" from the main loop, or "servicing USB" for a fault inside
 * usb_work() -- so a fault read as something it was not.
 *
 * The site goes in before the dump, not after: the dump goes out over USB, and
 * usb_send_sync() marks its own site while it runs and puts this one back when
 * it returns.
 */

/*
 * The LED code a stopped board blinks, counted in blinks: a panic the firmware
 * raised itself, then one code per processor fault. The four faults use the
 * same numbers as the other image, so a code means the same thing whichever
 * image a board is running.
 *
 * The codes used to be a run of long blinks and then a run of short ones --
 * 2 and 25, 4 and 12, 8 and 6, 8 and 6 -- which is one code for a bus fault
 * and a usage fault both. None of them was readable anyway: they were blinked
 * through a sleep that did not sleep, and even with a working one they took
 * longer than the watchdog allows. The second run carried nothing the first
 * did not; the register dump above is where the detail is.
 */
#define FAULT_CODE_PANIC  1
#define FAULT_CODE_HARD   2
#define FAULT_CODE_MEM    3
#define FAULT_CODE_BUS    4
#define FAULT_CODE_USAGE  5

/*
 * How long one blink is, and the gap that separates one repeat of a code from
 * the next.
 *
 * The whole code has to be blinked inside the watchdog's timeout. Nothing
 * below feeds the watchdog, on purpose -- the board resets itself a few
 * seconds after the fault and comes back usable -- so a code that took longer
 * than that would be cut off part-way, and a cut-off count is a different
 * count. The longest code and its gap come to 2.1 of the 3 seconds the
 * watchdog allows; the dump above eats into that, so a later repeat may still
 * be cut, but the first one is not.
 */
#define FAULT_BLINK_MS 150
#define FAULT_GAP_MS   600

/* The longest code, which is the only one that can fail this. A code added
 * below goes here too; test_btbr_fault_blink.c checks every one of them. */
_Static_assert(FAULT_CODE_USAGE * 2 * FAULT_BLINK_MS + FAULT_GAP_MS <
               UBERTOOTH_WATCHDOG_TIMEOUT_MS,
               "a fault code must be blinked before the watchdog resets the board");

/* Blink one code forever, until the watchdog resets the board. */
static void __attribute__((noreturn)) blink_fault_code(int code)
{
	while (1) {
		flash_leds(code, FAULT_BLINK_MS);
		active_msleep(FAULT_GAP_MS);
	}
}

void __attribute__((noreturn)) die (char *fmt, ...)
{
	char buf[64] = {0};
	int len;
	va_list ap;
	irq_save_disable();

	watchdog_mark(WATCHDOG_SITE_PANIC);

	// This looks a bit like early_printf
	buf[0] = BTUSB_EARLY_PRINT;
	va_start(ap, fmt);
	len = tfp_vsnprintf(buf+1, sizeof(buf)-1, fmt, ap);
	va_end(ap);
	usb_send_sync((void*)buf, 1+len);

	blink_fault_code(FAULT_CODE_PANIC);
}

static void dump_debug_registers(void)
{
	int i;
	extern uint32_t _StackTop;
	volatile uint32_t _stk[1] = {0xdeadbeef}, *stk;
	early_printf("CFSR=%08x\n", SCB_CFSR);
	early_printf("HFSR=%08x\n", SCB_HFSR);
	early_printf("MMSR=%08x\n", SCB_MMSR);
	early_printf("BFSR=%08x\n", SCB_BFSR);
	early_printf("UFSR=%08x\n", SCB_UFSR);
	early_printf("BFAR=%08x\n", SCB_BFAR);

	early_printf("\nStack dump (~%p):\n", _stk);
	for (stk=(uint32_t*)((uint32_t)&_stk&~0xf);stk<&_StackTop;stk+=4)
	{
		early_printf("%p: %08x %08x %08x %08x\n",
			stk, stk[0], stk[1], stk[2], stk[3]);
	}
}

void HardFault_Handler(void)
{
	watchdog_mark(WATCHDOG_SITE_FAULT_HARD);
	/* The part records no address for this one. */
	watchdog_fault(SCB_HFSR, 0);
	early_printf("HardFault at 0x%x\n", get_lr());
	dump_debug_registers();
	blink_fault_code(FAULT_CODE_HARD);
}

void MemManagement_Handler(void)
{
	uint8_t mmsr = SCB_MMSR;

	watchdog_mark(WATCHDOG_SITE_FAULT_MEM);
	/* Bit 7 of the status says the address register holds this fault's
	 * address. The debug UART below prints the same registers, for a board
	 * someone has a serial console on; the breadcrumb is what a board
	 * plugged into a host alone can still report. */
	watchdog_fault(mmsr, (mmsr & (1 << 7)) ? SCB_MMFAR : 0);
	early_printf("MMFault at 0x%x\n", get_lr());
	dump_debug_registers();
	blink_fault_code(FAULT_CODE_MEM);
}

void BusFault_Handler(void)
{
	uint8_t bfsr = SCB_BFSR;

	watchdog_mark(WATCHDOG_SITE_FAULT_BUS);
	watchdog_fault(bfsr, (bfsr & (1 << 7)) ? SCB_BFAR : 0);
	early_printf("BusFault at %x\n", get_lr());
	dump_debug_registers();
	blink_fault_code(FAULT_CODE_BUS);
}

void UsageFault_Handler(void)
{
	watchdog_mark(WATCHDOG_SITE_FAULT_USAGE);
	/* The part records no address for this one. */
	watchdog_fault(SCB_UFSR, 0);
	early_printf("UsageFault at %x\n", get_lr());
	dump_debug_registers();
	blink_fault_code(FAULT_CODE_USAGE);
}
