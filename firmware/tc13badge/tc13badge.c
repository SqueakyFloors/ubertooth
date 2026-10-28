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

#include "tc13badge.h"
#include "cc2400.h"

static void spi_delay()
{
	int i;
	for (i = 13; i; i--)
		asm("nop");

}

static void wait()
{
	uint16_t i;
	for (i = 2197; i; i--)
		asm("nop");
}

void cc2400_init()
{
	/* CSN (slave select) is active low */
	CSN_SET;

	/* set up GPIO pins */
	prcr.prc2 = 1; // write enable pd0
	pd0.b5 = 1;    // CC3V3 output

	pd1.b0 = 1; // CC1V8 output
	pd1.b3 = 1; // CSN output
	/*
	pd1.b6 = 1;  // SCLK output
	pd1.b4 = 1;  // SI output
	pd1.b5 = 0;  // SO input
	pur0.b3 = 1; // enable SO pull-up
	*/

	/* activate 1V8 supply for CC2400 */
	CC1V8_SET;
	wait();

	/* activate 3V3 supply for CC2400 IO */
	CC3V3_SET;
}

void cc2400_off(void)
{
	CC3V3_CLR;
	CSN_CLR;
	CC1V8_CLR;
}

/*
 * One byte of a SPI transaction, with a bound on each of its two waits. The
 * caller must drop CSN.
 *
 * Every read and write the badge makes to the radio comes through here, so the
 * bare spins these replace were under all the bounded waits below: a UART that
 * stops raising its transmit-ready or receive-complete flag froze the badge
 * inside one poll, before any of those bounds could be reached. The flags are
 * the R8C's own peripheral rather than the radio, so what it takes is a UART
 * left disabled -- uart_off() switches both halves off, and sleep() and
 * r8c_takeover() call it -- and the radio reached after that without
 * uart_init().
 *
 * A byte that never arrived reads as zero. Zero rather than 0xff because the
 * status byte and the registers are read through here: with every bit clear,
 * the radio reads as not stable, not locked and in no state a wait is after,
 * so the waits below run their budgets and give up. All ones would have them
 * return at once as though the radio were ready.
 */
uint8_t cc2400_spi_byte(uint8_t data)
{
	uint16_t i;

	for (i = SPI_BYTE_POLLS; i; i--)
		if (u0c1.ti)
			break;
	if (!i)
		return 0;

	u0tbl = data;

	for (i = SPI_BYTE_POLLS; i; i--)
		if (u0c1.ri)
			break;
	if (!i)
		return 0;

	return u0rb;
}

/* alternative implementation, slower, without UART */
uint8_t cc2400_spi_byte_bitbang(uint8_t data)
{
	int len = 8;
	uint8_t msb = 1 << (len - 1);

	while (len--) {
		if (data & msb)
			SI_SET;
		else
			SI_CLR;
		data <<= 1;

		//spi_delay();

		SCLK_SET;
		if (SO)
			data |= 1;

		//spi_delay();

		SCLK_CLR;
	}

	return data;
}

/* read 16 bit value from a register */
uint16_t cc2400_get(uint8_t reg)
{
	uint8_t status;
	uint16_t val;

	/* start transaction by dropping CSN */
	//spi_delay();
	CSN_CLR;

	status = cc2400_spi_byte(reg | 0x80);
	val = cc2400_spi_byte(0);
	val <<= 8;
	val |= cc2400_spi_byte(0);

	/* end transaction by raising CSN */
	//spi_delay();
	CSN_SET;

	return val;
}

/* write 16 bit value to a register */
void cc2400_set(uint8_t reg, uint16_t val)
{
	uint8_t status;

	/* start transaction by dropping CSN */
	//spi_delay();
	CSN_CLR;

	status = cc2400_spi_byte(reg);
	cc2400_spi_byte((val >> 8) & 0xFF);
	cc2400_spi_byte(val & 0xFF);

	/* end transaction by raising CSN */
	//spi_delay();
	CSN_SET;
}

/* read 8 bit value from a register */
uint8_t cc2400_get8(uint8_t reg)
{
	uint8_t status;
	uint8_t val;

	/* start transaction by dropping CSN */
	//spi_delay();
	CSN_CLR;

	status = cc2400_spi_byte(reg | 0x80);
	val = cc2400_spi_byte(0);

	/* end transaction by raising CSN */
	//spi_delay();
	CSN_SET;

	return val;
}

/* write 8 bit value to a register */
void cc2400_set8(uint8_t reg, uint8_t val)
{
	uint8_t status;

	/* start transaction by dropping CSN */
	//spi_delay();
	CSN_CLR;

	status = cc2400_spi_byte(reg);
	cc2400_spi_byte(val);

	/* end transaction by raising CSN */
	//spi_delay();
	CSN_SET;
}

/* get the status */
uint8_t cc2400_status()
{
	return cc2400_strobe(0);
}

/* strobe register, return status */
uint8_t cc2400_strobe(uint8_t reg)
{
	uint8_t status;

	/* start transaction by dropping CSN */
	//spi_delay();
	CSN_CLR;

	status = cc2400_spi_byte(reg);

	/* end transaction by raising CSN */
	//spi_delay();
	CSN_SET;

	return status;
}

/*
 * Bounded waits on CC2400 state.
 *
 * Every loop below used to be a bare spin: a radio that stopped reporting the
 * state the loop was waiting for froze the badge with its lights stopped, and
 * only pulling the battery cleared it. Each of these gives up after a fixed
 * number of polls and returns, so the caller carries on and the badge comes
 * back by itself.
 *
 * It comes back slowly. None of these hands in a service callback, where the
 * Ubertooth's waits hand in their USB poll, because the badge has nothing to
 * service -- but it also means a wait that runs to its bound does nothing else
 * for its whole length. With a dead radio, one sweep of led_specan()'s
 * thirteen channels spends thirteen lock waits in a row with the LEDs off;
 * led_specan() makes thirteen of those sweeps before it returns, and main()
 * does not look at the button until it does. So what these buy is a badge that
 * recovers rather than one that has to be opened, not a badge that behaves
 * normally with a broken radio.
 *
 * Giving up does not make the radio work. What the caller reads from it next
 * is not a measurement, and a transmit strobed after a lock wait that gave up
 * goes out with the synthesizer in the wrong state. The bound is there to end
 * the loop, which is the same trade the Ubertooth firmware makes in
 * firmware/common/cc2400_wait.c.
 *
 * This is a second set of them rather than that file because the badge cannot
 * compile it: the badge is a Renesas R8C part with its own register map, its
 * own SPI through a UART and no watchdog driver, and that file is written
 * against the Ubertooth's LPC17xx. The names and the shape are kept the same
 * so the two read alike.
 *
 * None of these re-strobes the radio part-way through, where the Ubertooth's
 * waits do. That is for an interrupt handler putting the synthesizer back the
 * other way under the poll, and no handler on this part touches the radio.
 *
 * The power-on self-test keeps its own unbounded waits, in
 * cc2400_register_test() and in post(): see the comment on the first.
 */

/* Wait for the MAIN register to read back `value`, which is how a reset is
 * confirmed. Returns 1 if it did, 0 if the wait gave up. */
int cc2400_wait_main(uint16_t value)
{
	uint16_t i;

	for (i = MAIN_POLLS; i; i--)
		if (cc2400_get(MAIN) == value)
			return 1;
	return 0;
}

/* Wait for the CC2400 to report its 16 MHz crystal oscillator stable.
 * Returns 1 if it did, 0 if the wait gave up. */
int cc2400_wait_xosc_stable(void)
{
	uint16_t i;

	for (i = XOSC_STABLE_POLLS; i; i--)
		if (cc2400_status() & XOSC16M_STABLE)
			return 1;
	return 0;
}

/* Wait for the synthesizer to reach `locked`: 1 after a SFSON strobe, 0 after
 * SRFOFF. Returns 1 if it did, 0 if the wait gave up. */
int cc2400_wait_fs(int locked)
{
	uint16_t i;

	for (i = locked ? FS_LOCK_POLLS : FS_UNLOCK_POLLS; i; i--)
		if (!!(cc2400_status() & FS_LOCK) == !!locked)
			return 1;
	return 0;
}

/* cc2400_wait_fs(0), which is what most callers want. */
int cc2400_wait_fs_unlock(void)
{
	return cc2400_wait_fs(0);
}

/* Wait for the radio's state machine to report `state`, one of the STATE_*
 * values. Returns 1 if it did, 0 if the wait gave up. */
int cc2400_wait_fsm(uint8_t state)
{
	uint16_t i;

	for (i = FSM_POLLS; i; i--)
		/* FSMSTATE's low five bits are the state; the rest is
		 * breakpoint configuration. */
		if ((cc2400_get(FSMSTATE) & 0x1f) == state)
			return 1;
	return 0;
}

/* Wait for the radio to report that it has decoded a sync word, which is a
 * packet arriving. Returns 1 if one did, 0 if the wait gave up.
 *
 * The one wait here that counts in a uint32_t: its budget has to cover the
 * repeater's turnaround, which is more polls than a uint16_t holds. */
int cc2400_wait_sync_received(void)
{
	uint32_t i;

	for (i = SYNC_RECEIVED_POLLS; i; i--)
		if (cc2400_status() & SYNC_RECEIVED)
			return 1;
	return 0;
}

void cc2400_reset()
{
	cc2400_set(MAIN, 0x0000);
	cc2400_wait_main(0x0000);
	cc2400_set(MAIN, 0x8000);
	cc2400_wait_main(0x8000);
}

/* Activate the CC2400's 16 MHz oscillator. Returns 1 if it reported stable,
 * 0 if it did not: the one caller that has something to say about that is the
 * self-test. */
int cc2400_clock_start()
{
	cc2400_reset();
	cc2400_strobe(SXOSCON);
	return cc2400_wait_xosc_stable();
}

/* Check to see if we can read a register.
 *
 * This wait has no bound, on purpose. It is a step of the power-on self-test,
 * which counts itself off in LEDs: each step that passes pulses one more of
 * them (see pulse_leds()), and the badge then goes dark where it stops. Nine
 * steps have passed when this one runs, so a badge that stops here has shown
 * nine pulsing LEDs and nothing since, which says the radio does not answer a
 * register read -- and there is no host to keep answering for, nor anything
 * after this step a badge whose radio does not answer could do. */
void cc2400_register_test(void)
{
	while (cc2400_get(MANFIDL) != 0x133D) wait();
}

int cc2400_range_test(void)
{
	int i;
	int j;
	int attempt;
	int got_reply = 0;
	int success = 1;
	const uint8_t len = 22;
	const uint16_t channel = 2441;
	uint8_t pa = 0;
	uint8_t txbuf[22];
	uint8_t rxbuf[22];

	txbuf[0] = len - 1; // length of data (rest of payload)
	txbuf[1] = 0; // request
	txbuf[2] = 13;
	txbuf[3] = 13;
	txbuf[4] = 13;
	txbuf[5] = 13;
	txbuf[6] = 13;
	txbuf[7] = 13;
	txbuf[8] = 13;
	txbuf[9] = 13;
	txbuf[10] = 13;
	txbuf[11] = 13;
	txbuf[12] = 13;
	txbuf[13] = 13;
	txbuf[14] = 13;
	txbuf[15] = 13;
	txbuf[16] = 13;
	txbuf[17] = 13;
	txbuf[18] = pa; // request pa
	txbuf[19] = 0; // request number
	txbuf[20] = 0xff; // reply pa
	txbuf[21] = 0xff; // reply number

	// Bluetooth-like modulation
	cc2400_set(MANAND,  0x7fff);
	cc2400_set(LMTST,   0x2b22);
	cc2400_set(MDMTST0, 0x134b);
	cc2400_set(FSDIV,   channel);
	cc2400_set(SYNCH,   0xf9ae);
	cc2400_set(SYNCL,   0x1584);
	cc2400_set(FREND,   8 | pa);
	cc2400_set(MDMCTRL, 0x0029);
	cc2400_wait_xosc_stable();
	cc2400_strobe(SFSON);
	cc2400_wait_fs(1);
	for (pa = 0; pa < 5; pa++) { // could go to 7, but hard on battery
		cc2400_set(FREND, 8 | pa);
		txbuf[18] = pa;
		for (i = 0; i < 4; i++) {
			txbuf[19] = i;
			cc2400_wait_fsm(STATE_STROBE_FS_ON);
			// transmit a packet
			for (j = 0; j < len; j++)
				cc2400_set8(FIFOREG, txbuf[j]);
			cc2400_strobe(STX);
		}
	}
	// sent packet, now look for repeated packet
	cc2400_wait_fsm(STATE_STROBE_FS_ON);
	cc2400_strobe(SRFOFF);
	cc2400_wait_fs_unlock();
	cc2400_set(FSDIV, channel - 1);
	cc2400_wait_xosc_stable();
	cc2400_strobe(SFSON);
	cc2400_wait_fs(1);
	/*
	 * Listen for the repeated packet, going round again on one whose
	 * checksum did not check out.
	 *
	 * Only on that. A wait that found no packet at all ends the test,
	 * because the listening has to be one stretch: on the way back round,
	 * cc2400_wait_fsm() would wait out its whole budget with the radio
	 * still in receive -- the state machine is at STATE_STROBE_RX, not the
	 * STATE_STROBE_FS_ON it is after -- and a reply arriving in that gap
	 * would be missed. A retry after a bad checksum does not have that
	 * problem: the radio has received a packet and is back at
	 * STATE_STROBE_FS_ON.
	 *
	 * This was `while (1)`, which is what kept the badge here while a
	 * radio handed over bad packet after bad packet.
	 */
	for (attempt = BAD_CRC_RETRIES; attempt && !got_reply; attempt--) {
		cc2400_wait_fsm(STATE_STROBE_FS_ON);
		cc2400_strobe(SRX);
		if (!cc2400_wait_sync_received())
			break;
		for (j = 0; j < len; j++)
			rxbuf[j] = cc2400_get8(FIFOREG);
		if (cc2400_status() & STATUS_CRC_OK)
			got_reply = 1;
	}

	// done
	cc2400_strobe(SRFOFF);
	cc2400_wait_fs_unlock();

	/* No reply, no result. This is the only thing between a listen that
	 * heard nothing and a reported pass -- success is 1 until the
	 * comparison below clears it, and that comparison reads an rxbuf
	 * nothing has filled. */
	if (!got_reply) {
		success = 0;
		return success;
	}

	// make sure rx packet is as expected
	txbuf[1] = 1; // expected value in rxbuf
	for (i = 0; i < 18; i++)
		if (rxbuf[i] != txbuf[i])
			success = 0;

	return success;
}

void init_button(void)
{
	pd3.b3 = 0;  // set p3_3 as input
	pur0.b6 = 1; // enable p3_3 pull-up resistor
}

/*
 * Initialize INT3 interrupt for button SW1.  After calling this once,
 * stop_until_button() may be used repeatedly and an interrupt service
 * routinge may be set up for other uses of the button.
 */
void int3_init(void)
{
	DISABLE_INTERRUPTS;
	int3ic = 0x00;
	inten.int3en = 1;
	//intf.b = 0xC0; // filter INT3 with f32
	intf.b = 0x00; // filter disabled for stop mode compatibility
	int3ic = 0x01; // set INT3 interrupt priority
	FOUR_NOPS;
	ENABLE_INTERRUPTS;
}

/* set GPIO pins to outputs for 13 LEDs */
void init_leds(void)
{
	pd3.b5 = 1;
	pd3.b4 = 1;
	pd1.b1 = 1;
	pd1.b2 = 1;
	pd1.b7 = 1;
	pd2.b0 = 1;
	pd2.b1 = 1;
	pd2.b2 = 1;
	pd2.b3 = 1;
	pd2.b4 = 1;
	pd2.b5 = 1;
	pd2.b6 = 1;
	pd2.b7 = 1;
}

void set_led(uint8_t led, uint8_t state)
{
	if (state) {
		switch (led) {
		case 0:
			LED1_SET;
			break;
		case 1:
			LED2_SET;
			break;
		case 2:
			LED3_SET;
			break;
		case 3:
			LED4_SET;
			break;
		case 4:
			LED5_SET;
			break;
		case 5:
			LED6_SET;
			break;
		case 6:
			LED7_SET;
			break;
		case 7:
			LED8_SET;
			break;
		case 8:
			LED9_SET;
			break;
		case 9:
			LED10_SET;
			break;
		case 10:
			LED11_SET;
			break;
		case 11:
			LED12_SET;
			break;
		case 12:
			LED13_SET;
			break;
		}
	} else {
		switch (led) {
		case 0:
			LED1_CLR;
			break;
		case 1:
			LED2_CLR;
			break;
		case 2:
			LED3_CLR;
			break;
		case 3:
			LED4_CLR;
			break;
		case 4:
			LED5_CLR;
			break;
		case 5:
			LED6_CLR;
			break;
		case 6:
			LED7_CLR;
			break;
		case 7:
			LED8_CLR;
			break;
		case 8:
			LED9_CLR;
			break;
		case 9:
			LED10_CLR;
			break;
		case 10:
			LED11_CLR;
			break;
		case 11:
			LED12_CLR;
			break;
		case 12:
			LED13_CLR;
			break;
		}
	}
}

void all_leds_on(void)
{
	LED1_SET;
	LED2_SET;
	LED3_SET;
	LED4_SET;
	LED5_SET;
	LED6_SET;
	LED7_SET;
	LED8_SET;
	LED9_SET;
	LED10_SET;
	LED11_SET;
	LED12_SET;
	LED13_SET;
}

void all_leds_off(void)
{
	LED1_CLR;
	LED2_CLR;
	LED3_CLR;
	LED4_CLR;
	LED5_CLR;
	LED6_CLR;
	LED7_CLR;
	LED8_CLR;
	LED9_CLR;
	LED10_CLR;
	LED11_CLR;
	LED12_CLR;
	LED13_CLR;
}

/* set up uart0 for CC2400 communication */
void uart_init(void)
{
	pd1.b5 = 0;    // set p1_5 as input
	s0tic = 0x00;  // disable transmit interrupt
	s0ric = 0x00;  // disable receive interrupt
	u0c1.te = 0;   // disable transmit
	u0c1.re = 0;   // disable receive
	u0mr.b = 0x01; // activate synchronous serial mode
	u0c0.b = 0x80; // transfer MSB first
	u0c1.b = 0x00;
	u0brg = 3;     // fast clock rate
	u0c1.te = 1;   // enable transmit
	u0c1.re = 1;   // enable receive
}

void uart_off(void)
{
	u0c1.te = 0; // disable transmit
	u0c1.re = 0; // disable receive
	u0mr.b = 0;  // disable uart0
}

/*
 * Enter stop mode (low power, clocks stopped) until button SW1
 * is pressed.  See Renesas Application Note REJ05B1064-0102.
 *
 * This requires that int3_init() has been executed once.
 *
 * Also, if r8c_takeover_ini() has been called, a wake-up will
 * occur if the R8C_CTL line is toggled by the LPC175x.
 */
void stop_until_button(void)
{
	prcr.prc0 = 1; // disable control register protection
	cm1.b0 = 1; // enter stop mode
	//asm("jmp ?+");
	//asm("?:");
	FOUR_NOPS;
	prcr.prc0 = 1; // disable control register protection
	cm1.b6 = 0; // no CPU clock division
	cm1.b7 = 0;
	cm0.b6 = 0; // enable division change
	prcr.prc0 = 0; // enable control register protection
}

void r8c_takeover_init(void)
{
	/* R8C_ACK is active low */
	R8C_ACK_SET;

	/* set up GPIO pins */
	prcr.prc2 = 1; // write enable pd0
	pd0.b0 = 1;    // R8C_ACK output
	pd4.b5 = 0;    // R8C_CTL input
	pur1.b1 = 1;   // enable R8C_CTL (p4_5) pull-up resistor 

	/* set up INT0 interrupt (R8C_CTL) */
	DISABLE_INTERRUPTS;
	int0ic = 0x00;
	inten.int0en = 1;
	inten.int0pl = 1; // trigger on both edges
	intf.b = 0x00; // filter disabled for stop mode compatibility
	int0ic = 0x01; // set INT3 interrupt priority
	FOUR_NOPS;
	ENABLE_INTERRUPTS;
}

/* allow LPC175x to take control */
void r8c_takeover(void)
{
	all_leds_off();
	uart_off();
	cc2400_off();
	prcr.prc2 = 1; // write enable pd0
	pd0.b5 = 0;    // CC3V3 input
	pd1.b0 = 0;    // CC1V8 input
	pd1.b3 = 0;    // CSN input
	R8C_ACK_CLR;
}

/* take control back from LPC175x */
void r8c_take_back(void)
{
	R8C_ACK_SET;
	cc2400_init();
	uart_init();
	cc2400_clock_start();
}
