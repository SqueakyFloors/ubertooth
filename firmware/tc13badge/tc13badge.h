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

#ifndef __TC13BADGE_H
#define __TC13BADGE_H

#include "r8c2l.h"

#define uint8_t unsigned char
#define uint16_t unsigned short
#define uint32_t unsigned long

#define MAX(a, b) ((a)>(b) ? (a) : (b))
#define FOUR_NOPS asm("nop"); asm("nop"); asm("nop"); asm("nop")
#define ENABLE_INTERRUPTS asm("fset i")
#define DISABLE_INTERRUPTS asm("fclr i")
#define WAIT asm("wait")

/* indicator LED control */
#define USRLED     p2.b7
#define USRLED_SET (p2.b7 = 1)
#define USRLED_CLR (p2.b7 = 0)
#define RXLED      p2.b2
#define RXLED_SET  (p2.b2 = 1)
#define RXLED_CLR  (p2.b2 = 0)
#define TXLED      p3.b5
#define TXLED_SET  (p3.b5 = 1)
#define TXLED_CLR  (p3.b5 = 0)
#define LED1       p3.b5
#define LED1_SET   (p3.b5 = 1)
#define LED1_CLR   (p3.b5 = 0)
#define LED2       p3.b4
#define LED2_SET   (p3.b4 = 1)
#define LED2_CLR   (p3.b4 = 0)
#define LED3       p1.b1
#define LED3_SET   (p1.b1 = 1)
#define LED3_CLR   (p1.b1 = 0)
#define LED4       p1.b2
#define LED4_SET   (p1.b2 = 1)
#define LED4_CLR   (p1.b2 = 0)
#define LED5       p1.b7
#define LED5_SET   (p1.b7 = 1)
#define LED5_CLR   (p1.b7 = 0)
#define LED6       p2.b0
#define LED6_SET   (p2.b0 = 1)
#define LED6_CLR   (p2.b0 = 0)
#define LED7       p2.b2
#define LED7_SET   (p2.b2 = 1)
#define LED7_CLR   (p2.b2 = 0)
#define LED8       p2.b1
#define LED8_SET   (p2.b1 = 1)
#define LED8_CLR   (p2.b1 = 0)
#define LED9       p2.b3
#define LED9_SET   (p2.b3 = 1)
#define LED9_CLR   (p2.b3 = 0)
#define LED10      p2.b4
#define LED10_SET  (p2.b4 = 1)
#define LED10_CLR  (p2.b4 = 0)
#define LED11      p2.b5
#define LED11_SET  (p2.b5 = 1)
#define LED11_CLR  (p2.b5 = 0)
#define LED12      p2.b6
#define LED12_SET  (p2.b6 = 1)
#define LED12_CLR  (p2.b6 = 0)
#define LED13      p2.b7
#define LED13_SET  (p2.b7 = 1)
#define LED13_CLR  (p2.b7 = 0)

/* SW1 button press */
#define SW1 (!(p3.b3))

/* 1V8 regulator control */
#define CC1V8      p1.b0
#define CC1V8_SET  (p1.b0 = 1)
#define CC1V8_CLR  (p1.b0 = 0)

/* CC2400 control */
#define CC3V3_SET  (p0.b5 = 1)
#define CC3V3_CLR  (p0.b5 = 0)
#define CSN_SET    (p1.b3 = 1)
#define CSN_CLR    (p1.b3 = 0)
#define SCLK_SET   (p1.b6 = 1)
#define SCLK_CLR   (p1.b6 = 0)
#define SI_SET     (p1.b4 = 1)
#define SI_CLR     (p1.b4 = 0)
#define SO         p1.b5

/* R8C takeover by LPC175x */
#define R8C_CTL     p4.b5
#define R8C_ACK_SET (p0.b0 = 1)
#define R8C_ACK_CLR (p0.b0 = 0)

/*
 * How many times each wait on CC2400 state polls the radio before it gives up.
 *
 * These are poll counts, not times: one poll is one SPI transaction through
 * the badge's UART, so how long a budget lasts depends on the UART's clock.
 * Each is sized to be out of reach of a radio that is working at all, which is
 * all it has to be -- the states being waited for are reached in well under a
 * millisecond.
 *
 * The wait for an unlock is deliberately the shortest, as it is in the
 * Ubertooth firmware: it is asked for on the way out of a measurement, where
 * carrying on costs nothing.
 *
 * All but the first are the Ubertooth's values, which were sized against its
 * own SPI and are more than this part needs; what a poll costs here has not
 * been measured. Raising one has a ceiling: the waits count in a uint16_t,
 * because a counter wider than a word costs this 16-bit part real
 * instructions, so a budget above 65535 is taken modulo 65536 -- a far shorter
 * wait than asked for, and no wait at all on an exact multiple.
 */
#define MAIN_POLLS        1000
#define XOSC_STABLE_POLLS 10000
#define FS_LOCK_POLLS     10000
#define FS_UNLOCK_POLLS   1000
#define FSM_POLLS         2000

/*
 * Waiting for a packet is the one wait that is not about the radio being late:
 * nothing is wrong when no packet comes, the other end has simply not
 * transmitted. The range test gives up on it all the same, because the badge
 * has no host to ask it to stop.
 *
 * 28561 polls, which is what the range test has always spent listening, in one
 * stretch. It has to be one stretch: the radio is receiving throughout it, and
 * a wait that gave up and started another would first have to wait out
 * FSM_POLLS with the radio still in receive, so a reply arriving in that gap
 * would be missed. A wait that finds no packet is therefore the end of the
 * test.
 *
 * Whether 28561 polls is long enough is a separate question, which needs a
 * badge to answer: the only peer this range test has is an Ubertooth running
 * cc2400_repeater(), which waits a second before it replies.
 */
#define SYNC_RECEIVED_POLLS 28561

/*
 * How many packets with a failed checksum the range test takes before it gives
 * up. It used to take them forever, so a radio handing over one bad packet
 * after another kept the badge in the test. Each retry starts from a radio
 * that has just received a packet, which is why a retry is worth making at
 * all: the state the wait above needs has been reached.
 *
 * A retry listens again from the full budget, so the worst case is this many
 * times SYNC_RECEIVED_POLLS. That is longer than the one shared count the test
 * used to spend across its retries, and the trade is deliberate: a retry only
 * happens after a packet actually arrived, and a listen that gets the whole
 * budget is the thing worth keeping.
 */
#define BAD_CRC_RETRIES 13

void cc2400_init(void);
void cc2400_off(void);
uint8_t cc2400_spi_byte(uint8_t data);
uint16_t cc2400_get(uint8_t reg);
void cc2400_set(uint8_t reg, uint16_t val);
uint8_t cc2400_get8(uint8_t reg);
void cc2400_set8(uint8_t reg, uint8_t val);
uint8_t cc2400_status(void);
uint8_t cc2400_strobe(uint8_t reg);
int cc2400_wait_main(uint16_t value);
int cc2400_wait_xosc_stable(void);
int cc2400_wait_fs(int locked);
int cc2400_wait_fs_unlock(void);
int cc2400_wait_fsm(uint8_t state);
int cc2400_wait_sync_received(void);
void cc2400_reset(void);
int cc2400_clock_start(void);
void cc2400_register_test(void);
int cc2400_range_test(void);
void init_button(void);
void int3_init(void);
void init_leds(void);
void set_led(uint8_t led, uint8_t state);
void all_leds_on(void);
void all_leds_off(void);
void uart_init(void);
void uart_off(void);
void stop_until_button(void);
void r8c_takeover_init(void);
void r8c_takeover(void);
void r8c_take_back(void);

#endif /* __TC13BADGE_H */
