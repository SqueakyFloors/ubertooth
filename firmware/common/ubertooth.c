/*
 * Copyright 2010, 2011 Michael Ossmann
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

#include <string.h>

#include "ubertooth.h"
#include "device_id.h"
#include "irq.h"

#define IAP_LOCATION 0x1FFF1FF1
const IAP_ENTRY iap_entry = (IAP_ENTRY)IAP_LOCATION;

/* delay a number of seconds while on internal oscillator (4 MHz) */
void wait(u8 seconds)
{
	wait_us(seconds * 1000000);
}

/* delay a number of milliseconds while on internal oscillator (4 MHz) */
void wait_ms(u32 ms)
{
	wait_us(ms * 1000);
}

/* efficiently reverse the bits of a 32-bit word */
u32 rbit(u32 value) {
  u32 result = 0;
  asm("rbit %0, %1" : "=r" (result) : "r" (value));
  return result;
}

/* delay a number of microseconds while on internal oscillator (4 MHz) */
/* we only have a resolution of 1000/400, so to the nearest 2.5        */
static volatile u32 wait_us_counter;
void wait_us(u32 us)
{
	/* This is binary multiply by ~0.3999, i.e, multiply by
	   0.011011011b. The loop also contains 6 instructions at -Os, so
	   why this factor works is not at all related to the comment
	   above ;-) */
	u32 count = (us>>2) + (us>>3) + (us>>6) + (us>>7) + (us>>10) + (us>>11);

	/* Fewer than four microseconds shift away to nothing, and the loop
	   below decrements before it tests: a count of zero would wrap and spin
	   through all 2^32 of them, over an hour, with nothing feeding the
	   watchdog. One pass is the shortest wait there is. */
	if (count == 0)
		count = 1;

	wait_us_counter = count;
	while(--wait_us_counter);
}


/*
 * This should be called very early by every firmware in order to ensure safe
 * operating conditions for the CC2400.
 */
void gpio_init()
{
	/* 
	 * Set all pins for GPIO.  This shouldn't be necessary after a reset, but
	 * we might get called at other times.
	 */
	all_pins_off();

	/* set certain pins as outputs, all others inputs */
#ifdef UBERTOOTH_ZERO
	FIO0DIR = PIN_USRLED;
	FIO1DIR = (PIN_CC3V3 | PIN_RX | PIN_TX | PIN_CSN |
			PIN_SCLK | PIN_MOSI | PIN_CC1V8 | PIN_BTGR);
	FIO2DIR = PIN_SSEL0;
	FIO3DIR = 0;
	FIO4DIR = (PIN_RXLED | PIN_TXLED);
#endif
#ifdef UBERTOOTH_ONE
	FIO0DIR = 0;
	FIO1DIR = (PIN_USRLED | PIN_RXLED | PIN_TXLED | PIN_CC3V3 |
			PIN_RX | PIN_CC1V8 | PIN_BTGR);
	FIO2DIR = (PIN_CSN | PIN_SCLK | PIN_MOSI | PIN_PAEN | PIN_HGM);
	FIO3DIR = 0;
	FIO4DIR = (PIN_TX | PIN_SSEL1);
#endif
#ifdef TC13BADGE
	/*
	 * Leave all the CC2400 pins configured as inputs until cc2400_init() is
	 * called so that we don't interfere with the R8C's control of the CC2400.
	 */
	FIO0DIR = 0;
	FIO1DIR = PIN_R8C_CTL;
	FIO2DIR = 0;
	FIO3DIR = 0;
	FIO4DIR = 0;
#endif

	/* set all outputs low */
	FIO0PIN = 0;
	FIO1PIN = 0;
	FIO2PIN = 0;
	FIO3PIN = 0;
	FIO4PIN = 0;

#ifdef TC13BADGE
	/* R8C_CTL is active low */
	R8C_CTL_SET;
#endif
}

void all_pins_off(void)
{
	/* configure all pins for GPIO */
	PINSEL0 = 0;
	PINSEL1 = 0;
	PINSEL2 = 0;
	PINSEL3 = 0;
	PINSEL4 = 0;
	PINSEL7 = 0;
	PINSEL9 = 0;
	PINSEL10 = 0;

	/* configure all pins as inputs */
	FIO0DIR = 0;
	FIO1DIR = 0;
	FIO2DIR = 0;
	FIO3DIR = 0;
	FIO4DIR = 0;

	/* pull-up on every pin */
	PINMODE0 = 0;
	PINMODE1 = 0;
	PINMODE2 = 0;
	PINMODE3 = 0;
	PINMODE4 = 0;
	PINMODE7 = 0;
	PINMODE9 = 0;

	/* set all outputs low */
	FIO0PIN = 0;
	FIO1PIN = 0;
	FIO2PIN = 0;
	FIO3PIN = 0;
	FIO4PIN = 0;
}

/*
 * Every application that uses the main oscillator (including any that use both
 * USB and the CC2400) should start with this.
 */
void ubertooth_init()
{
	gpio_init();
	cc2400_init();
	clock_start();
}

/* configure SSP for CC2400's secondary serial data interface */
void dio_ssp_init()
{
#ifdef UBERTOOTH_ZERO
	/* set P0.15 as SCK0 */
	PINSEL0 = (PINSEL0 & ~(3 << 30)) | (2 << 30);

	/* set P1.16 as SSEL0 */
	PINSEL1 = (PINSEL1 & ~(3 << 0)) | (2 << 0);

	/* set P1.17 as MISO0 */
	PINSEL1 = (PINSEL1 & ~(3 << 2)) | (2 << 2);

	/* set P1.18 as MOSI0 */
	PINSEL1 = (PINSEL1 & ~(3 << 4)) | (2 << 4);
#endif
#if defined UBERTOOTH_ONE || defined TC13BADGE
	/* set P0.7 as SCK1 */
	PINSEL0 = (PINSEL0 & ~(3 << 14)) | (2 << 14);

	/* set P0.6 as SSEL1 */
	PINSEL0 = (PINSEL0 & ~(3 << 12)) | (2 << 12);

	/* set P0.8 as MISO1 */
	PINSEL0 = (PINSEL0 & ~(3 << 16)) | (2 << 16);

	/* set P0.9 as MOSI1 */
	PINSEL0 = (PINSEL0 & ~(3 << 18)) | (2 << 18);
#endif

	/*
	 * DIO_SSEL is a GPIO output connected directly to the SSEL input on the
	 * microcontroller for the CC2400's un-buffered (DIO/DCLK) serial
	 * interface.  Since the CC2400 doesn't have a slave select output, we
	 * control it with this.  DIO_SSEL should already be configured by
	 * gpio_init().  We set it high by default because it is an active low
	 * signal.
	 */
	DIO_SSEL_SET;

	/* configure DIO_SSP */
	DIO_SSP_CR0 = (0x7 /* 8 bit transfer */ | SSPCR0_CPOL | SSPCR0_CPHA);
	DIO_SSP_CR1 = (SSPCR1_MS | SSPCR1_SOD);
}

void atest_init()
{
	/*
	 * ADC can optionally be configured for ATEST1 and ATEST2, but for now we
	 * set them as floating inputs.
	 */

	/* P0.25 is ATEST1, P0.26 is ATEST2 */
	PINSEL1 &= ~((0x3 << 20) | (0x3 << 18)); // set as GPIO
	FIO0DIR &= ~(0x3 << 25); // set as input
	PINMODE1 |= (0x5 << 19); // no pull-up/pull-down
	PINMODE1 &= ~(0x5 << 18); // no pull-up/pull-down
}

void cc2400_init()
{
#ifdef TC13BADGE
	/* request CC2400 control from the R8C */
	r8c_takeover();

	/* configure output pins for CC2400 control */
	FIO0DIR = 0;
	FIO1DIR = (PIN_CC3V3 | PIN_CC1V8 | PIN_CSN | PIN_SCLK | PIN_MOSI |
			PIN_R8C_CTL);
	FIO2DIR = (PIN_CSN | PIN_SCLK | PIN_MOSI);
	FIO3DIR = 0;
	FIO4DIR = PIN_SSEL1;

	/* set all outputs low */
	FIO0PIN = 0;
	FIO1PIN = 0; /* assuming we have already asserted R8C_CTL low */
	FIO2PIN = 0;
	FIO3PIN = 0;
	FIO4PIN = 0;
#else
	atest_init();
#endif

	/* activate 1V8 supply for CC2400 */
	CC1V8_SET;
	wait_us(50);

	/* CSN (slave select) is active low */
	CSN_SET;

	/* activate 3V3 supply for CC2400 IO */
	CC3V3_SET;

	/*
	 * No radio register is written here, because nothing written here
	 * survives. Every caller goes on to cc2400_reset() -- some of the bench
	 * images call it directly, the rest reach it through clock_start(), as
	 * ubertooth_init() does -- and that takes MAIN to 0 and back: a few
	 * microseconds after this returns, every register is at its reset value
	 * again.
	 *
	 * A `cc2400_set(MANAND, 0x7fff)` used to sit here, meant to hold the
	 * receive gain amplifier out of reset for the whole run. It was undone
	 * that way, so a board that had just booted read MANAND as 0xffff and
	 * no mode ever saw it. Each mode writes the registers it needs when it
	 * starts, MANAND included, and cc2400_idle() puts MANAND back when a
	 * mode ends.
	 */
}

/*
 * This is a single SPI transaction of variable length, usually 8 or 24 bits.
 * The CC2400 also supports longer transactions (e.g. for the FIFO), but we
 * haven't implemented anything longer than 32 bits.
 *
 * We're bit-banging because:
 *
 * 1. We're using one SPI peripheral for the CC2400's unbuffered data
 *    interace.
 * 2. We're saving the second SPI peripheral for an expansion port.
 * 3. The CC2400 needs CSN held low for the entire transaction which the
 *    LPC17xx SPI peripheral won't do without some workaround anyway.
 *
 * Interrupts are masked for the length of the transfer. CSN, SCLK and MOSI
 * are the only path to the radio, and the interrupt handlers that drive the
 * radio reach it through this same function: the LE PHY's DMA handler reads
 * RSSI and strobes the synthesizer, its timer handler retunes, and in the
 * modes that service USB from the USB interrupt the vendor request handler
 * writes radio registers. A handler that ran part-way through a transfer
 * would begin its own with CSN already low, and both would then read or write
 * a corrupt word, or lose a strobe.
 *
 * The cost is one transfer of interrupt latency. A bit of the loop below
 * compiles to about 19 cycles, so once the PLL is up and the CPU runs at
 * 100 MHz an 8-bit status read or strobe masks for roughly 1.6 microseconds
 * and the longest transfer, 24 bits, for roughly 4.6. The transfers that
 * cc2400_reset() and clock_start() make run before the PLL, on the 4 MHz
 * internal oscillator, and so take 25 times as long, but nothing is armed
 * that early in boot.
 *
 * Two things in the firmware feel that latency. The LE PHY accepts a measured
 * connection anchor only within 3 microseconds of its estimate, and it takes
 * that measurement from the clock read at the top of its DMA handler. A
 * register transfer elsewhere can delay that handler past the window, and the
 * PHY then keeps its estimate for that connection event instead of the
 * measurement. In LE follow mode the main loop touches the radio only for a
 * host request, but the PHY's own timer handler also drives it, so a request
 * in flight is not the only way to reach this.
 *
 * The BR/EDR receive loop pays it on every pass, with no host involved.
 * bt_stream_rx() polls RSSI while it waits for each DMA transfer to finish,
 * 50 to 80 times across roughly 400 microseconds, and each poll is a 24-bit
 * transfer. So for most of that wait interrupts are masked, and the
 * carrier-sense interrupt that opens the symbol capture --
 * EINT3_IRQHandler(), which drops DIO_SSEL -- runs up to 4.6 microseconds
 * late, moving the start of the capture window by a few symbol periods at
 * 1 megabit per second.
 *
 * Whether either costs a packet is not known: the capture rate has not been
 * measured on hardware before and after. What is certain is that a transfer
 * which comes out corrupt costs more.
 *
 * PRIMASK is blunter than the problem needs, because it also blocks the
 * handlers that never touch the bus -- the carrier-sense and clkn handlers,
 * the two most timing-sensitive ones in the BR/EDR path, pay the latency for
 * nothing. Masking only the handlers that drive the radio, through the
 * interrupt controller's enable registers, would avoid that, at the price of
 * a guard that has to be kept in step with which handlers those are.
 *
 * Each bit costs the same whether interrupts are masked or not, so the
 * transfer itself is no slower than before.
 */
u32 cc2400_spi(u8 len, u32 data)
{
	u32 msb = 1 << (len - 1);
	u32 primask = irq_save_disable();

	/* start transaction by dropping CSN */
	CSN_CLR;

	while (len--) {
		if (data & msb)
			MOSI_SET;
		else
			MOSI_CLR;
		data <<= 1;

		SCLK_SET;
		/* Save a few cycles by avoiding the compare/branch */
		data |= !!MISO;
		SCLK_CLR;
	}

	/* end transaction by raising CSN */
	CSN_SET;

	irq_restore(primask);

	return data;
}

/* read 16 bit value from a register */
u16 cc2400_get(u8 reg)
{
	u32 in;

	u32 out = (reg | 0x80) << 16;
	in = cc2400_spi(24, out);
	return in & 0xFFFF;
}

/* write 16 bit value to a register */
void cc2400_set(u8 reg, u16 val)
{
	u32 out = (reg << 16) | val;
	cc2400_spi(24, out);
}

/* read 8 bit value from a register */
u8 cc2400_get8(u8 reg)
{
	u16 in;

	u16 out = (reg | 0x80) << 8;
	in = cc2400_spi(16, out);
	return in & 0xFF;
}

/* write 8 bit value to a register */
void cc2400_set8(u8 reg, u8 val)
{
	u32 out = (reg << 8) | val;
	cc2400_spi(16, out);
}

/*
 * The hold the CC2400 needs at the end of a FIFO transfer, before CSN rises.
 * The datasheet calls it t_ps and asks for 300 nanoseconds, FIFO accesses
 * only; this is about 0.85 microseconds. delay_counter is volatile so that
 * the loop survives the optimizer.
 */
static volatile u32 delay_counter;
static void spi_delay() {
       delay_counter = 10;
       while (--delay_counter);
}


/*
 * The two FIFO transfers below drive the same CSN/SCLK/MOSI lines by hand
 * instead of going through cc2400_spi(), so they need the same guard, and now
 * they have it: interrupts are masked from before CSN drops until after it
 * rises. Without it a handler that drives the radio could begin its own
 * transfer with CSN already low, and both would then read or write a corrupt
 * word, or lose a strobe.
 *
 * The exposure that exists today is a control request arriving from the USB
 * interrupt while the main loop writes the FIFO: the vendor request handler
 * writes radio registers for SET_PALEVEL, SET_CHANNEL, SET_SQUELCH and
 * WRITE_REGISTER(S), and bt_slave_le() writes an advertising packet into the
 * FIFO from its loop with that interrupt enabled. Every FIFO transfer in this
 * image is made from a mode loop rather than from a handler, so the main loop
 * being cut into is the whole of it -- but the guard covers a handler reaching
 * a transfer too, which is what it would take for the LE PHY's handlers to
 * share the bus with one.
 *
 * These two hold the bus far longer than a register transfer, so the guard
 * costs more here than it does in cc2400_spi().
 *
 * The write has no delay loops, about 1 microsecond a byte: the longest write
 * any caller makes is one FIFO's worth, 32 bytes, so it masks for roughly 32
 * microseconds. That is ten times a register transfer and still well inside
 * the clkn timer interrupt's 312.5 microsecond period, so no tick is lost --
 * the handlers simply run that much later.
 *
 * The read waits out spi_delay() twice per bit, about 0.85 microseconds each,
 * so a bit costs about 1.8 microseconds and a 32-byte read runs to roughly
 * 460 microseconds. That is longer than one clkn period and shorter than two,
 * so a read costs the Bluetooth clock at most one tick. An interrupt raised
 * while masked is not lost: it is latched and runs as soon as the mask comes
 * off, so a span holding one timer match only delays the handler. A tick goes
 * only when two matches fall inside the span, because they coalesce into the
 * one pending bit -- which happens when the read begins within about 150
 * microseconds of a match, a little under half the time. The timer itself
 * reloads in hardware and keeps time; it is the count clkn keeps in software
 * that falls 312.5 microseconds behind when that happens.
 *
 * Who pays those ticks: only the two modes that read the FIFO, the generic
 * receive (rx_generic_sync()) and the range test. The generic receive stamps
 * each packet it enqueues with clkn, so its timestamps fall further behind
 * the more packets it captures, and no shipped tool asks for that mode. The
 * range test does ship -- `ubertooth-util -n` and `-e` -- and reads the FIFO
 * for every packet it exchanges, but it uses clkn only to time a one-second
 * turnaround, which a tick either way does not change. The capture modes
 * whose timestamps a user reads -- BR/EDR receive and the LE PHY -- take
 * their data over DMA and never touch the FIFO, so they are unaffected.
 *
 * The btbr image has its own bit-banged FIFO write, in
 * firmware/btbr/src/btphy/rf.c, which is not masked. There it runs only from
 * interrupt handlers -- the slot scheduler in the timer interrupt, and the
 * refill the GIO6 interrupt calls -- and a handler can be preempted only by
 * one of a higher priority. In both images every handler keeps the reset
 * priority of 0 except GPDMA, which dma_poweron() puts at the lowest, so
 * each of those writes runs to its end with nothing able to reach the bus in
 * the middle of it.
 */

/* write multiple bytes to SPI */
void cc2400_fifo_write(u8 len, u8 *data) {
	u8 msb = 1 << 7;
	u8 reg = FIFOREG;
	u8 i, j, temp;
	u32 primask = irq_save_disable();

	/* start transaction by dropping CSN */
	CSN_CLR;

	for (i = 0; i < 8; ++i) {
		if (reg & msb)
			MOSI_SET;
		else
			MOSI_CLR;
		reg <<= 1;
		SCLK_SET;
		SCLK_CLR;
	}

	for (i = 0; i < len; ++i) {
		temp = data[i];
		for (j = 0; j < 8; ++j) {
			if (temp & msb)
				MOSI_SET;
			else
				MOSI_CLR;
			temp <<= 1;
			SCLK_SET;
			SCLK_CLR;
		}
	}

	/*
	 * What used to be here was another eight clock pulses, with a comment
	 * saying they were necessary to clock in the last byte. They are not:
	 * the radio takes each byte as its eighth bit is clocked, and what it
	 * then needs is the hold below, not more clocks. The extra pulses
	 * clocked a whole extra byte into the FIFO -- whatever MOSI happened to
	 * be left holding -- so every packet written through here carried one
	 * stray byte on the end.
	 *
	 * In the modes that transmit with the radio's CRC off that byte is a
	 * harmless tail the receiver ignores, which is why it went unnoticed.
	 * With the CRC on, as the range test has it, the radio computes the CRC
	 * over the stray byte too, so the receiver -- which reads the number of
	 * bytes it expects and then asks the radio whether the next two make
	 * the CRC -- never sees a match, and the whole exchange fails.
	 */
	
	spi_delay();
	/* end transaction by raising CSN */
	CSN_SET;

	irq_restore(primask);
}

/* read multiple bytes from SPI */
void cc2400_fifo_read(u8 len, u8 *buf) {
	u8 msb = 1 << 7;
	u8 i, j, temp, reg;
	u32 primask;
	// Set first bit because it's a read
	reg = 0x80 | FIFOREG;

	primask = irq_save_disable();

	/* start transaction by dropping CSN */
	CSN_CLR;

	for (i = 0; i < 8; ++i) {
		if (reg & msb)
			MOSI_SET;
		else
			MOSI_CLR;
		reg <<= 1;
		SCLK_SET;
		SCLK_CLR;
	}

	for (i = 0; i < len; ++i) {
		temp = 0;
		for (j = 0; j < 8; ++j) {
			spi_delay();
			SCLK_SET;
			temp <<= 1;
			if (MISO)
				temp |= 1;
			spi_delay();
			SCLK_CLR;
		}
		buf[i] = temp;
	}

	/* end transaction by raising CSN */
	spi_delay();
	CSN_SET;

	irq_restore(primask);
}

/* get the status */
u8 cc2400_status()
{
	return cc2400_spi(8, 0);
}

/* strobe register, return status */
u8 cc2400_strobe(u8 reg)
{
	return cc2400_spi(8, reg);
}

/*
 * Warning: This should only be called when running on the internal oscillator.
 * Otherwise use clock_start().
 */
void cc2400_reset()
{
	/* These two waits, and the one for the oscillator in clock_start()
	 * below, are the radio waits in this tree left unbounded on purpose.
	 *
	 * Every caller is boot code running on the internal oscillator, before
	 * the USB controller is brought up, and nothing is armed that early, so
	 * the contention that can stall a wait in a running mode cannot happen
	 * here. The only way the readback never arrives is a radio that is not
	 * answering at all -- and then there is nothing to carry on to. The
	 * 16 MHz clock the rest of clock_start() needs comes from the CC2400,
	 * so a bound here would only move the hang a few lines down, to the
	 * wait for the main oscillator. Either way the board never enumerates,
	 * which is a different failure from the one bounding fixes: a board
	 * that enumerates and then answers nothing. */
	cc2400_set(MAIN, 0x0000);
	while (cc2400_get(MAIN) != 0x0000);
	cc2400_set(MAIN, 0x8000);
	while (cc2400_get(MAIN) != 0x8000);
}

/* activate the CC2400's 16 MHz oscillator and sync LPC175x to it */
void clock_start()
{
	/* configure flash accelerator for higher clock rate */
	FLASHCFG = (0x03A | (FLASHTIM << 12));

	/* switch to the internal oscillator if necessary */
	CLKSRCSEL = 0;

	/* disconnect PLL0 */
	PLL0CON &= ~PLL0CON_PLLC0;
	PLL0FEED_SEQUENCE;
	while (PLL0STAT & PLL0STAT_PLLC0_STAT);

	/* turn off PLL0 */
	PLL0CON &= ~PLL0CON_PLLE0;
	PLL0FEED_SEQUENCE;
	while (PLL0STAT & PLL0STAT_PLLE0_STAT);

	/* temporarily set CPU clock divider to 1 */
	CCLKCFG = 0;

	/* configure CC2400 oscillator, output carrier sense on GIO6 */
	cc2400_reset();
	cc2400_set(IOCFG, (GIO_CARRIER_SENSE_N << 9) | (GIO_CLK_16M << 3));
	cc2400_strobe(SXOSCON);
	/* Unbounded on purpose, for the reasons given at cc2400_reset() above:
	 * this runs during boot, before the USB controller is brought up, and
	 * everything after it needs the 16 MHz clock the CC2400 provides. */
	while (!(cc2400_status() & XOSC16M_STABLE));

	/* activate main oscillator */
	SCS = SCS_OSCEN;
	while (!(SCS & SCS_OSCSTAT));

	/*
	 * errata sheet says we must select peripheral clock before enabling and
	 * connecting PLL0
 	 */
#ifdef TC13BADGE
	PCLKSEL0  = (1 << 2); /* TIMER0 at cclk (30 MHz) */
	PCLKSEL1  = 0;
#else
        // XXX here
	PCLKSEL0  = (2 << 2) | (2 << 4); /* TIMER0 and TIMER1 at cclk/2 (50 MHz) */
	PCLKSEL1  = (2 << 12); /* TIMER2 at cclk/2 (50 MHz) */
#endif

	/* switch to main oscillator */
	CLKSRCSEL = 1;

	/* configure PLL0 */
	PLL0CFG = (MSEL0 << 0) | (NSEL0 << 16);
	PLL0FEED_SEQUENCE;

	/* turn on PLL0 */
	PLL0CON |= PLL0CON_PLLE0;
	PLL0FEED_SEQUENCE;
	while (!(PLL0STAT & PLL0STAT_PLLE0_STAT));

	/* set CPU clock divider */
	CCLKCFG = CCLKSEL;

	/* connect PLL0 */
	PLL0CON |= PLL0CON_PLLC0;
	PLL0FEED_SEQUENCE;
	while (!(PLL0STAT & PLL0STAT_PLLC0_STAT));

	/* configure PLL1 */
	PLL1CFG = (MSEL1 << 0) | (PSEL1 << 5);
	PLL1FEED_SEQUENCE;

	/* turn on PLL1 */
	PLL1CON |= PLL1CON_PLLE1;
	PLL1FEED_SEQUENCE;
	while (!(PLL1STAT & PLL1STAT_PLLE1_STAT));
	while (!(PLL1STAT & PLL1STAT_PLOCK1));

	/* connect PLL1 */
	PLL1CON |= PLL1CON_PLLC1;
	PLL1FEED_SEQUENCE;
	while (!(PLL1STAT & PLL1STAT_PLLC1_STAT));
}

/* reset the LPC17xx, the cc2400 will be handled by the boot code */
void reset()
{
	all_pins_off();

	/*
	 * A watchdog time-out is this part's chip reset, so the way to reset the
	 * board is to let the watchdog run out. The timeout is set to the
	 * smallest the counter accepts, 255 counts of the watchdog clock, which
	 * is 255 microseconds -- and it is set here rather than left alone
	 * because the application arms the watchdog with a three-second timeout
	 * while it runs (watchdog.c). Without this write, a reset would wait out
	 * that timeout and this function would return first, leaving the caller
	 * running on a board that was supposed to have reset. Writing WDTC while
	 * the watchdog is already running and feeding it afterwards is what makes
	 * the new value take effect.
	 *
	 * The reset clears the enable bits again, so the bootloader this hands
	 * over to runs with the watchdog off and no feed of its own to write.
	 */
	USRLED_CLR;
	WDTC = 0xFF;
	WDMOD |= WDMOD_WDEN | WDMOD_WDRESET;
	WDFEED_SEQUENCE;

	/* Longer than the timeout above, by a wide margin, so this never
	 * returns. */
	wait(1);
}

/* take control of R8C microcontroller and CC2400 on the ToorCon 13 badge */
#ifdef TC13BADGE
void r8c_takeover(void)
{
	/* wait until user presses SW1 while USB is connected */
	while (SW1 || !VBUS) {
		/* turn off power to all peripherals except GPIO */
		PCONP = PCONP_PCGPIO;

		/* enable interrupt on falling edge of P2.8 (SW1) */
		IO2IntEnF |= (0x1 << 8);
		ISER0 |= ISER0_ISE_EINT3;

		/* go into power-down mode and wait for interrupt */
		SCB_SCR = SCB_SCR_SLEEPDEEP;
		PCON = (PCON_PM0 | PCON_BOGD);
		asm("WFI");

		/* disable interrupt */
		IO2IntEnF &= ~(0x1 << 8);
		ICER0 |= ICER0_ICE_EINT3;

		/* turn power back on for peripherals we use */
		PCONP = (PCONP_PCTIM0 | PCONP_PCSSP1 | PCONP_PCGPIO | PCONP_PCSSP0);
	}

	/* drop R8C_CTL to let the R8C know we are taking over */
	R8C_CTL_CLR;

	/* wait for the R8C to acknowledge the takeover */
	while (R8C_ACK);
}

/* interrupt handler for SW1 */
void EINT3_IRQHandler(void)
{
	/* clear interrupt */
	IO2IntClr |= (0x1 << 8);
}
#endif

/*
 * The part number and the device serial number, as the boot ROM reported them
 * at startup: a status byte followed by the words the ROM returned, least
 * significant byte of each word first.
 *
 * Reading either one means calling the part's In-Application Programming entry
 * point in the boot ROM, and this firmware answers control requests from inside
 * whichever mode loop is running. A boot-ROM call that does not return promptly
 * therefore stops that loop: a live capture asked for its serial number has
 * been seen to answer nothing at all for the watchdog's whole 3000 ms and then
 * be reset. Neither number can change while the part is powered, so each image
 * reads them once at startup and answers every request from these copies, which
 * leaves no path from a request to the boot ROM.
 *
 * The status byte starts at 0xff, which no boot-ROM status uses, so an image
 * that never made the call reports a failed read rather than a serial number of
 * sixteen zeros.
 */
static uint8_t part_num_reply[5] = { 0xff };
static uint8_t device_serial_reply[17] = { 0xff };

/*
 * Read both numbers from the boot ROM. Every image calls this from main(),
 * after ubertooth_init() and before the USB stack starts, which is where the
 * USB serial-number descriptor has always read the serial from.
 */
void device_id_init(void)
{
	u32 command[5];
	u32 result[5];
	u32 flags;

	/*
	 * Interrupts off for the two calls, as in set_isp() below and for the
	 * same reason: the boot ROM neither uses nor masks interrupts, so a
	 * handler that fires while it is running vectors back into the image.
	 * Nothing is running yet at startup, so this costs nothing there.
	 */
	flags = irq_save_disable();

	command[0] = 54; /* read part number */
	iap_entry(command, result);
	device_id_pack(part_num_reply, result, 1);

	command[0] = 58; /* read device serial number */
	iap_entry(command, result);
	device_id_pack(device_serial_reply, result, 4);

	irq_restore(flags);
}

void get_part_num(uint8_t *buffer, int *len)
{
	memcpy(buffer, part_num_reply, sizeof(part_num_reply));
	*len = sizeof(part_num_reply);
}

void get_device_serial(uint8_t *buffer, int *len)
{
	memcpy(buffer, device_serial_reply, sizeof(device_serial_reply));
	*len = sizeof(device_serial_reply);
}

void set_isp(void)
{
	u32 command[5];
	u32 result[5];
	/*
	 * Interrupts off for the hand-over, and this is the one place that can
	 * do it for both images: the guard it uses, defined earlier in this
	 * file, is file-local, and btbr has its own copy under a different
	 * header.
	 *
	 * The programmer lives in ROM and neither uses nor masks interrupts, so
	 * a handler that fires while it starts up vectors back into the image
	 * that was running. In the btbr image that happens every 312.5
	 * microseconds, its slot timer, and the hand-over then did not take at
	 * all on a board: this call came back and the firmware carried on
	 * answering the host as if nothing had been asked. The call used to be
	 * made from inside the USB interrupt handler, which masked the other
	 * handlers for free; now that each image answers the host first and
	 * hands over from its own loop, the mask has to be here.
	 *
	 * The bluetooth_rxtx image happens to be quiet by the time it hands over
	 * -- its dispatch stops the clock and the mode that was running -- but
	 * that is a property of fifteen mode functions, not of this path, so it
	 * is masked here too rather than relied on.
	 */
	u32 flags = irq_save_disable();
	/*
	 * The one hand-over that inherits this firmware's memory map: the
	 * programmer starts without a chip reset, so the stack's guard region
	 * would still be in force while ROM code it knows nothing about runs.
	 * Entering the bootloader goes through a reset instead, which clears
	 * the unit, and the bootloader starts an application at its reset
	 * vector, where it arms a guard of its own.
	 *
	 * Saved rather than just cleared, because this call can come back: see
	 * below.
	 */
	int has_mpu = MPU_TYPE_DREGION(MPU_TYPE) != 0;
	u32 mpu_ctrl = has_mpu ? MPU_CTRL : 0;

	if (has_mpu) {
		MPU_CTRL = 0;
		__asm__ volatile ("dsb" ::: "memory");
		__asm__ volatile ("isb");
	}

	command[0] = 57;
	iap_entry(command, result);

	/* Not reached, unless the programmer refuses: then the board must not be
	 * left running with every interrupt masked, nor with the rest of this
	 * boot spent without a stack guard. */
	if (has_mpu) {
		MPU_CTRL = mpu_ctrl;
		__asm__ volatile ("dsb" ::: "memory");
		__asm__ volatile ("isb");
	}
	irq_restore(flags);
}

//FIXME ssp
//FIXME tx/rx
