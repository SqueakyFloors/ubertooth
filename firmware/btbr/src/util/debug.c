/* Debug
 *
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
#include <string.h>
#include <ubertooth.h>
#include "tinyprintf.h"
#include <ubtbr/ubertooth_usb.h>
#include <ubtbr/console.h>
#include <ubtbr/system.h>

static void a_putc(void *arg, char c)
{
	console_putc(c);
}

void cprintf(char *fmt, ...)
{
	va_list ap;
	void *ret;

	va_start(ap, fmt);
	tfp_format(NULL, a_putc, fmt, ap);
	va_end(ap);
}

void early_printf(char *fmt, ...)
{
	uint32_t flags = irq_save_disable();
	char buf[64] = {0};
	int len;
	va_list ap;
	void *ret;
	
	buf[0] = BTUSB_EARLY_PRINT;
	va_start(ap, fmt);
	len = tfp_vsnprintf(buf+1, sizeof(buf)-1, fmt, ap);
	va_end(ap);
	usb_send_sync((void*)buf, 1+len);

	irq_restore(flags);
}

void print_hex(uint8_t *pkt, unsigned size)
{
	unsigned i;
	for (i=0;i<size;i++)
		cprintf("%02x ", pkt[i]);
}

/* The system timer's reload value is 24 bits wide. */
_Static_assert(CCLK_HZ / 1000 - 1 <= 0xFFFFFFu,
               "a millisecond of core clocks must fit the system timer reload");

/*
 * Sleep for msec milliseconds by counting core clocks on the system timer.
 *
 * This used to be an empty loop over an ordinary counter,
 * `for (i = msec/2; i; i--);`. A 500 ms request became 250 decrements of a
 * register -- well under a microsecond -- and a compiler is free to delete a
 * loop like that outright. Everything that calls this blinks an LED code to
 * say which fault stopped the board, and the codes are told apart by their
 * timing, so none of them was readable.
 *
 * The system timer is used rather than a loop with a calibration constant
 * because it counts the clock itself: one reload is one millisecond however
 * the compiler arranges the code. It is read by polling, not by its
 * interrupt, because every caller runs on a fault path, where an interrupt
 * may never be taken. Setting it up here is safe for the same reason: no
 * caller returns, so nothing else needs the timer afterwards.
 *
 * The one case this gets wrong is a fault before clock_start() has run, when
 * the CPU is still on the 4 MHz internal oscillator and every wait is 25
 * times too long. Nothing can be done about that from here, and the watchdog
 * is not armed that early either.
 */
void active_msleep(unsigned msec)
{
	/* One reload of the counter is one millisecond of core clocks. */
	STRELOAD = CCLK_HZ / 1000 - 1;
	STCTRL = STCTRL_CLKSOURCE | STCTRL_ENABLE;

	while (msec--) {
		/* Writing the current value restarts the count and clears the
		 * flag the loop below waits for. */
		STCURR = 0;
		while (!(STCTRL & STCTRL_COUNTFLAG))
			;
	}

	STCTRL = 0;
}

// flash LEDs a certain number of times
void flash_leds(int count, int delay) {
	int i;

	for (i = 0; i < count; ++i) {
		USRLED_SET;
		RXLED_SET;
		TXLED_SET;
		active_msleep(delay);
		USRLED_CLR;
		RXLED_CLR;
		TXLED_CLR;
		active_msleep(delay);
	}
}
