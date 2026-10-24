/*
 * Copyright 2026 Project Ubertooth contributors
 *
 * This file is part of Project Ubertooth and is released under the
 * terms of the GPL. Refer to COPYING for more information.
 */

/*
 * Identify: blinks the USR, RX and TX LEDs together, about three on-off blinks
 * a second, so a board can be picked out of several. Modelled on xmas mode
 * (xmas.c), but on TIMER3 and with all three LEDs in step, at the rate
 * ubertooth-util used to drive over USB.
 *
 * Like xmas, this runs on the board: USB is serviced from its interrupt while
 * the mode runs, so a STOP or another mode request from the host still gets
 * through and ends it.
 */

#include "ubertooth.h"
#include "identify.h"
#include "identify_blink.h"

extern volatile uint8_t requested_mode;

#define NOW T3TC

/*
 * TIMER3 runs at cclk/4: clock_start() raises TIMER0-2 in PCLKSEL0/PCLKSEL1
 * and leaves TIMER3 at the reset divider. The prescaler below makes one timer
 * tick 1 ms, so the blink interval is in milliseconds.
 */
#ifdef TC13BADGE
#define IDENTIFY_PCLK_HZ (30000000 / 4)
#else
#define IDENTIFY_PCLK_HZ (100000000 / 4)
#endif
#define IDENTIFY_PRESCALE ((IDENTIFY_PCLK_HZ / 1000) - 1)
#define IDENTIFY_INTERVAL UBERTOOTH_IDENTIFY_TOGGLE_MS

/* Written by identify_main() before the match interrupt is enabled, and only
 * by TIMER3_IRQHandler() after that. */
static identify_blink_t blink;

static void identify_leds(uint8_t level)
{
	if (level) {
		USRLED_SET;
		RXLED_SET;
		TXLED_SET;
	} else {
		USRLED_CLR;
		RXLED_CLR;
		TXLED_CLR;
	}
}

static void timer3_start(void)
{
	PCONP |= PCONP_PCTIM3;
	T3TCR = TCR_Counter_Reset;
	T3PR = IDENTIFY_PRESCALE;
	/* A match left pending by an earlier run would otherwise fire a toggle
	 * before the first one is scheduled. */
	T3IR = TIR_MR0_Interrupt;
	T3TCR = TCR_Counter_Enable;

	// set up interrupt handler
	ISER0 = ISER0_ISE_TIMER3;
}

static void timer3_stop(void)
{
	T3MCR = 0;
	T3TCR = TCR_Counter_Reset;

	// clear interrupt handler
	ICER0 = ICER0_ICE_TIMER3;

	PCONP &= ~PCONP_PCTIM3;
}

void identify_main(void)
{
	// enable USB interrupts
	ISER0 = ISER0_ISE_USB;

	timer3_start();

	identify_blink_start(&blink, NOW, IDENTIFY_INTERVAL);
	identify_leds(blink.level);
	T3MR0 = blink.next;

	T3MCR |= TMCR_MR0I;

	while (requested_mode == MODE_IDENTIFY)
		asm("WFI"); // wait for interrupt

	timer3_stop();

	/* Leave the board as a reset leaves it: all three LEDs off, whatever the
	 * blink was in the middle of. */
	identify_leds(0);

	// disable USB interrupts
	ICER0 = ICER0_ICE_USB;
}

void TIMER3_IRQHandler(void)
{
	if (T3IR & TIR_MR0_Interrupt) {
		T3IR = TIR_MR0_Interrupt;
		identify_blink_toggle(&blink, NOW, IDENTIFY_INTERVAL);
		identify_leds(blink.level);
		T3MR0 = blink.next;
	}
}
