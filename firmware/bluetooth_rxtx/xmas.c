/*
 * Copyright 2020 Mike Ryan
 *
 * This file is part of Project Ubertooth and is released under the
 * terms of the GPL. Refer to COPYING for more information.
 */

#include <stdlib.h>

#include "ubertooth.h"
#include "watchdog.h"

extern volatile uint8_t requested_mode;

#define NOW T2TC

static void timer2_start(void) {
	PCONP |= PCONP_PCTIM2;
	T2TCR = TCR_Counter_Reset;
	T2PR = 50000-1; // 1 ms
	/* No match is armed yet, and a match an earlier run of this mode latched
	 * is cleared, so nothing of timer2 reaches the interrupt controller when
	 * the line below is enabled -- not until xmas_main() arms the three
	 * matches. Without this the handler could run while xmas_main() is still
	 * choosing the first three blink times, and the two would be inside
	 * rand() together: it keeps one sequence for every caller. A toggle
	 * would also be taken before the first one was scheduled.
	 *
	 * The identify mode clears its flags the same way (identify.c), and the
	 * low-energy engine clears both registers when it borrows timer2 for its
	 * own blink (le_phy.c), naming this mode as what may have left them
	 * set. */
	T2MCR = 0;
	T2IR = 0xff; // write-1-to-clear: every match and capture flag
	/* A pending interrupt the controller itself latched needs no clearing:
	 * the handler acts only on a flag in T2IR, which is now empty. */
	T2TCR = TCR_Counter_Enable;

	// set up interrupt handler
	ISER0 = ISER0_ISE_TIMER2;
}

static void timer2_stop(void) {
	/* Disarmed before the counter stops, so no match is left armed for
	 * whatever powers timer2 up next. */
	T2MCR = 0;
	T2TCR = TCR_Counter_Reset;

	// clear interrupt handler
	ICER0 = ICER0_ICE_TIMER2;

	PCONP &= ~PCONP_PCTIM2;
}

#define BLINK (NOW + 400 + (rand() % 1200))

void xmas_main(void) {
	// enable USB interrupts
	ISER0 = ISER0_ISE_USB;

	timer2_start();

	T2MR0 = BLINK;
	T2MR1 = BLINK;
	T2MR2 = BLINK;

	T2MCR |= TMCR_MR0I | TMCR_MR1I | TMCR_MR2I;

	while (requested_mode == MODE_XMAS) {
		/* Nothing here services USB -- the USB interrupt does, and it only
		 * fires when the host asks something, which in this mode it may
		 * never do. Reaching this line is what the watchdog gets instead:
		 * it runs below every interrupt handler, so no handler is stuck and
		 * a request would be answered. */
		watchdog_usb_ready();
		asm("WFI"); // wait for interrupt
	}

	timer2_stop();

	// disable USB interrupts
	ICER0 = ICER0_ICE_USB;
}

void TIMER2_IRQHandler(void) {
	if (T2IR & TIR_MR0_Interrupt) {
		T2IR = TIR_MR0_Interrupt;
		if (USRLED) USRLED_CLR; else USRLED_SET;
		T2MR0 = BLINK;
	}
	if (T2IR & TIR_MR1_Interrupt) {
		T2IR = TIR_MR1_Interrupt;
		if (RXLED) RXLED_CLR; else RXLED_SET;
		T2MR1 = BLINK;
	}
	if (T2IR & TIR_MR2_Interrupt) {
		T2IR = TIR_MR2_Interrupt;
		if (TXLED) TXLED_CLR; else TXLED_SET;
		T2MR2 = BLINK;
	}
}
