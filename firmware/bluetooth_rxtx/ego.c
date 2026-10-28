/*
 * Copyright 2015 Mike Ryan
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

#include "ego.h"
#include "watchdog.h"
#include "cc2400_wait.h"
#include "ubertooth_clock.h"
#include "ubertooth_usb.h"

/*
 * This code performs several functions related to the Yuneec E-GO electric
 * skateboard:
 *
 *  - connection following
 *  - continuous RX on a single channel
 *  - jamming
 */

int enqueue_with_ts(u8 type, u8 *buf, u32 ts);
/* CLK100NS comes from ubertooth_clock.h, included above. */
extern volatile u32 clkn; // TODO: replace with timer1
/* requested_mode comes from ubertooth.h, with the mode values. */
extern volatile u16 channel;
/* Which E-GO mode to run, and the request handler's word that a new
 * UBERTOOTH_EGO request has arrived. Both live in bluetooth_rxtx.c. */
extern volatile uint8_t ego_mode;
extern volatile uint8_t ego_restart;

static const uint16_t channels[4] = { 2408, 2418, 2423, 2469 };

// global mutable state
// FIXME: put this in a struct and reset it properly
volatile uint32_t access_code = EGO_DEFAULT_ACCESS_CODE;
/* Stored for rf_on()'s TODO below: the radio is told a 32-bit sync word
 * whatever length the host asked for, so nothing reads this yet. */
volatile int access_code_len = EGO_DEFAULT_ACCESS_CODE_LEN;

/*
 * Put the access code back to the one ubertooth-ego(1) documents.
 *
 * The mode dispatch calls this as a mode starts, for the reason it forgets the
 * AFH map there: the code belongs to the capture that asked for it. It used to
 * be set once at boot and kept until the board was reset, so an
 * `ubertooth-ego -a 11223344` left 11223344 on the board and the next run
 * captured with it while reporting nothing -- a capture that looks like a
 * default-access-code run and is not one.
 */
void ego_access_code_reset(void)
{
	access_code = EGO_DEFAULT_ACCESS_CODE;
	access_code_len = EGO_DEFAULT_ACCESS_CODE_LEN;
}

typedef enum _ego_state_t {
	EGO_ST_INIT = 0,
	EGO_ST_START_RX,
	EGO_ST_CAP,
	EGO_ST_SLEEP,
	EGO_ST_START_JAMMING,
	EGO_ST_JAMMING,
} ego_state_t;

typedef struct _ego_fsm_state_t {
	ego_state_t state;
	int channel_index;
	u32 sleep_start;
	u32 sleep_duration;
	int timer_active;

	// used by jamming
	int packet_observed;
	u32 anchor;
} ego_fsm_state_t;

typedef void (*ego_st_handler)(ego_fsm_state_t *);

uint8_t packet_len = 18;
/* The longest capture the host may ask for, in bytes. ubertooth-ego refuses
 * anything outside 1 to this (EGO_PACKET_LENGTH_MIN/MAX in the host's
 * ego_options.h); rfcat_subcommand() below refuses it too, because any
 * program can talk to the board over USB. */
#define EGO_MAX_PACKET_LEN 48
/* The receive buffer is a record's data field, not a capture's worth of
 * bytes: every e-go packet is handed to enqueue_with_ts(), which copies
 * DMA_SIZE (50) bytes out of it. A buffer of EGO_MAX_PACKET_LEN would be two
 * bytes short of that, so the copy read past its end -- and what followed was
 * the low half of rxtime, which then went out in the data field of the
 * record carrying it. */
typedef struct _ego_packet_t {
	u8 rxbuf[DMA_SIZE];
	u32 rxtime;
} ego_packet_t;
_Static_assert(EGO_MAX_PACKET_LEN <= DMA_SIZE,
               "the longest e-go capture must fit in a record's data field");

static void ssp_start(void) {
	// make sure the (active low) slave select signal is not active
	DIO_SSEL_SET;

	// enable SSP
	DIO_SSP_CR1 |= SSPCR1_SSE;

	// activate slave select pin
	DIO_SSEL_CLR;
}

static void ssp_stop() {
	// disable CC2400's output (active low)
	DIO_SSEL_SET;

	// disable SSP
	DIO_SSP_CR1 &= ~SSPCR1_SSE;
}

static void ego_init(void) {
	// enable USB interrupts
	ISER0 = ISER0_ISE_USB;

	dio_ssp_init();
}

/*
 * Stop the radio and put the front end back to where a state machine starts
 * from: the mode ending comes through here, and so does a restart under the
 * same mode, which has to configure the radio from a stopped state rather than
 * while it receives or transmits. The amplifier and the transmit LED are for
 * the restart -- interference leaves both on, and only the dispatch's teardown
 * would otherwise clear them.
 */
static void ego_radio_off(void) {
	cc2400_strobe(SRFOFF);
	ssp_stop(); // TODO disable SSP
#ifdef UBERTOOTH_ONE
	PAEN_CLR;
#endif
	RXLED_CLR;
	TXLED_CLR;
}

static void ego_deinit(void) {
	ego_radio_off();
	ICER0 = ICER0_ICE_USB;
}

/* What the last rf_on() tuned to and programmed as the sync word. Continuous
 * receive tunes once, so this is what lets it notice the host changing either
 * of them while it runs; see continuous_cap_state(). */
static uint16_t rf_on_channel;
static uint32_t rf_on_access_code;

static void rf_on(void) {
	/* Both are read once here, because the host can change either while
	 * this runs: what goes into the registers below has to be what is
	 * remembered as programmed, or a change that arrived mid-setup would
	 * never be noticed. */
	uint16_t tuned_channel = channel;
	uint32_t code = access_code;

	cc2400_set(MANAND,  0x7fff);
	cc2400_set(LMTST,   0x2b22);
	cc2400_set(MDMTST0, 0x134b); // without PRNG
	cc2400_set(GRMDM,   0x04e0); // un-buffered mode, 2FSK
	// 0 00 00 1 001 11 0 00 0 0
	//      |  | |   |  +--------> CRC off
	//      |  | |   +-----------> sync word: 32 MSB bits of SYNC_WORD
	//      |  | +---------------> 1 byte of 01010101
	//      |  +-----------------> packet mode
	//      +--------------------> un-buffered mode
	cc2400_set(FSDIV,   tuned_channel - 1); // 1 MHz IF
	cc2400_set(MDMCTRL, 0x0040); // 250 kHz frequency deviation
	cc2400_set(GRDEC,  3);       // 250 kbit

        // TODO handle access code length above
	cc2400_set(SYNCH, (code >> 16) & 0xffff);
	cc2400_set(SYNCL, (code >>  0) & 0xffff);

	rf_on_channel = tuned_channel;
	rf_on_access_code = code;

	/* Bounded, so a radio that stops answering cannot hold this mode for
	 * good: ego_main()'s loop is then reached again and sees that the host
	 * has asked for another mode. No service callback is needed -- this mode
	 * runs USB off its interrupt, which ego_main() enables on the way in. */
	cc2400_wait_xosc_stable(NULL);

	ssp_start();

	cc2400_strobe(SFSON);
	cc2400_wait_fs(1, NULL);
	cc2400_wait_fsm(STATE_STROBE_FS_ON, NULL);

	cc2400_strobe(SRX);
}

static void do_rx(ego_packet_t *packet) {
	int i;
	/* packet is a local of the state handler, so a capture length larger
	 * than the buffer would write down the stack, over rxtime and then over
	 * the handler's return address. rfcat_subcommand() refuses such a
	 * length, and this clamp keeps the loop inside the buffer whatever set
	 * packet_len -- the same belt and braces the generic transmit path uses
	 * between its request handler and tx_generic(). */
	int len = packet_len > EGO_MAX_PACKET_LEN ? EGO_MAX_PACKET_LEN
	                                          : (int)packet_len;

	for (i = 0; i < len; i++) {
		// make sure there are bytes ready
		while (!(SSP1SR & SSPSR_RNE)) ;
		packet->rxbuf[i] = (u8)DIO_SSP_DR;
	}
}

static inline int sync_received(void) {
	return cc2400_status() & SYNC_RECEIVED;
}

// sleep for some milliseconds
static void sleep_ms(ego_fsm_state_t *state, u32 duration) {
	state->sleep_start = CLK100NS;
	state->sleep_duration = duration * 1000*10;
}

// sleep for some milliseconds relative to the current anchor point
static void sleep_ms_anchor(ego_fsm_state_t *state, u32 duration) {
	state->sleep_start = state->anchor;
	state->sleep_duration = duration * 1000*10;
}

static inline int sleep_elapsed(ego_fsm_state_t *state) {
	u32 now = CLK100NS;
	if (now < state->sleep_start)
		now += CLK100NS_PERIOD;
	return (now - state->sleep_start) >= state->sleep_duration;
}


/////////////
// states

// do nothing
static void nop_state(ego_fsm_state_t *state) {
}

// used in follow and jam mode, override the channel supplied by user
static void init_state(ego_fsm_state_t *state) {
	state->channel_index = 0;
	channel = channels[state->channel_index];
	state->state = EGO_ST_START_RX;
}

static void start_rf_state(ego_fsm_state_t *state) {
	rf_on();
	state->state = EGO_ST_CAP;
}

static void cap_state(ego_fsm_state_t *state) {
	ego_packet_t packet = {
		.rxtime = CLK100NS,
	};

	if (sleep_elapsed(state)) {
		sleep_ms(state, 4);
		state->state = EGO_ST_SLEEP;
	}

	if (sync_received()) {
		RXLED_SET;
		do_rx(&packet);
		enqueue_with_ts(EGO_PACKET, packet.rxbuf, packet.rxtime);
		RXLED_CLR;

		sleep_ms(state, 6);
		state->state = EGO_ST_SLEEP;
	}

	// kill RF on state change
	if (state->state != EGO_ST_CAP) {
		cc2400_strobe(SRFOFF);
		ssp_stop();
		state->timer_active = 1;
	}
}

static void sleep_state(ego_fsm_state_t *state) {
	if (sleep_elapsed(state)) {
		// change channel
		state->channel_index = (state->channel_index + 1) % 4;
		channel = channels[state->channel_index];

		// set 7 ms timeout for RX
		sleep_ms(state, 7);
		state->timer_active = 1;

		state->state = EGO_ST_START_RX;
	}
}

// continuous cap states (reuses START_RX state)
static void continuous_init_state(ego_fsm_state_t *state) {
	state->state = EGO_ST_START_RX;
}

static void continuous_cap_state(ego_fsm_state_t *state) {
	ego_packet_t packet = {
		.rxtime = CLK100NS,
	};

	/*
	 * A tool states its channel and its access code after the request that
	 * starts this mode, because a mode start puts both back to their
	 * defaults. Either can therefore arrive once the mode is running, and
	 * this is the only E-GO mode that tunes once: follow and jam run
	 * rf_on() again on every hop, so they pick a change up within a hop.
	 * Go back through rf_on() when one of them changes, so the capture is
	 * on the channel and the sync word the user asked for.
	 */
	if (channel != rf_on_channel || access_code != rf_on_access_code) {
		cc2400_strobe(SRFOFF);
		ssp_stop();
		/* Whatever the radio had already handed over belongs to the
		 * channel and the sync word that are being left behind, as it
		 * does after a packet below: drop it, or the first packet of
		 * the new capture would be built partly out of it. */
		while (SSP1SR & SSPSR_RNE) {
			uint8_t stale = (uint8_t)DIO_SSP_DR;

			(void)stale;
		}
		state->state = EGO_ST_START_RX;
		return;
	}

	if (sync_received()) {
		RXLED_SET;
		do_rx(&packet);
		enqueue_with_ts(EGO_PACKET, packet.rxbuf, packet.rxtime);
		RXLED_CLR;

		ssp_stop();
		// restart cap with radio warm
		cc2400_strobe(SFSON);
		/* Bounded, as in rf_on(). */
		cc2400_wait_fs(1, NULL);
		cc2400_wait_fsm(STATE_STROBE_FS_ON, NULL);

		while (SSP1SR & SSPSR_RNE) {
			uint8_t tmp = (uint8_t)DIO_SSP_DR;
		}
		ssp_start();

		cc2400_strobe(SRX);
	}
}

#ifdef TX_ENABLE
// jammer states
static void jam_cap_state(ego_fsm_state_t *state) {
	if (sync_received()) {
		state->state = EGO_ST_START_JAMMING;
		state->packet_observed = 1;
		state->anchor = CLK100NS;
	}
	if (state->timer_active && sleep_elapsed(state)) {
		state->state = EGO_ST_START_JAMMING;
		state->packet_observed = 0;
		sleep_ms(state, 11); // 11 ms hop interval
	}

	// state changed, kill radio
	if (state->state != EGO_ST_CAP) {
		cc2400_strobe(SRFOFF);
		ssp_stop();
	}
}

static void start_jamming_state(ego_fsm_state_t *state) {
	cc2400_set(MANAND,  0x7fff);
	cc2400_set(LMTST,   0x2b22);
	cc2400_set(MDMTST0, 0x334b); // with PRNG
	// cc2400_set(GRMDM,   0x04e0); // un-buffered mode, 2FSK
	cc2400_set(GRMDM,   0x04c0); // un-buffered mode, 2FSK
	// 0 00 00 1 001 10 0 00 0 0
	//      |  | |   |  +--------> CRC off
	//      |  | |   +-----------> sync word: 24 MSB bits of SYNC_WORD
	//      |  | +---------------> 1 byte of 01010101
	//      |  +-----------------> packet mode
	//      +--------------------> un-buffered mode
	cc2400_set(FSDIV,   channel); // no IF for TX
	cc2400_set(MDMCTRL, 0x0026); // 150 kHz frequency deviation
	cc2400_set(GRDEC,  3);       // 250 kbit
	// jam at the amplifier level set with `ubertooth-util -a` (7 at power-on,
	// which is the level this used to force)
	cc2400_set(FREND, frend_keep_pa_level(cc2400_get(FREND)));

	/* Bounded, as in rf_on(). */
	cc2400_wait_xosc_stable(NULL);

	cc2400_strobe(SFSON);
	cc2400_wait_fs(1, NULL);
	cc2400_wait_fsm(STATE_STROBE_FS_ON, NULL);

#ifdef UBERTOOTH_ONE
	PAEN_SET;
#endif

	cc2400_strobe(STX);
	TXLED_SET;

	state->state = EGO_ST_JAMMING;
	sleep_ms_anchor(state, 2);
}

void jamming_state(ego_fsm_state_t *state) {
	if (sleep_elapsed(state)) {
		cc2400_strobe(SRFOFF);
#ifdef UBERTOOTH_ONE
		PAEN_CLR;
#endif
		TXLED_CLR;

		// change channel
		state->channel_index = (state->channel_index + 1) % 4;
		channel = channels[state->channel_index];

		state->state = EGO_ST_SLEEP;
		sleep_ms_anchor(state, 6);
	}
}

static void jam_sleep_state(ego_fsm_state_t *state) {
	if (sleep_elapsed(state)) {
		state->state = EGO_ST_START_RX;
		state->timer_active = 1;
		sleep_ms_anchor(state, 11);
	}
}
#endif

// hopping connection following
static const ego_st_handler follow_handler[] = {
	init_state,
	start_rf_state,
	cap_state,
	sleep_state,
	nop_state,
	nop_state,
	nop_state,
};

// continuous rx on a single channel
static const ego_st_handler continuous_rx_handler[] = {
	continuous_init_state, // do not override user channel
	start_rf_state,
	continuous_cap_state,
	nop_state,
	nop_state,
	nop_state,
};

#ifdef TX_ENABLE
// jamming
static const ego_st_handler jam_handler[] = {
	init_state,
	start_rf_state,
	jam_cap_state,
	jam_sleep_state,
	start_jamming_state,
	jamming_state,
};
#endif

/* The state machine for an E-GO mode, or NULL for a mode this firmware does
 * not have -- which the request handler refuses, so only a hand-driven board
 * could ask for one. */
static const ego_st_handler *ego_handler_for(uint8_t mode) {
	switch (mode) {
		case EGO_FOLLOW:
			return follow_handler;
		case EGO_CONTINUOUS_RX:
			return continuous_rx_handler;
#ifdef TX_ENABLE
		case EGO_JAM:
			return jam_handler;
#endif
	}
	return NULL;
}

/* Start a state machine from nothing, with the radio stopped: what a mode
 * start leaves for it. */
static void ego_fsm_start(ego_fsm_state_t *state) {
	u32 usb_irq;

	ego_radio_off();

	/*
	 * Drop what the run that is ending had queued and the host never
	 * collected. Those packets were captured in that run's mode, and the
	 * next poll of the queue comes from the next run, which would read them
	 * as its own. The mode dispatch does this for every mode start it sees;
	 * a restart it does not see.
	 *
	 * Held against the USB interrupt, because the queue is read from there,
	 * by the poll request: this mode services USB from that interrupt while
	 * its loop runs.
	 */
	usb_irq = usb_irq_hold();
	usb_queue_init();
	usb_irq_release(usb_irq);

	memset(state, 0, sizeof(*state));
	state->state = EGO_ST_INIT;
}

void ego_main(void) {
	const ego_st_handler *handler = ego_handler_for(ego_mode);
	ego_fsm_state_t state = { .state = EGO_ST_INIT };

	if (handler == NULL) {
		requested_mode = MODE_IDLE;
		return;
	}

	clkn_start(); // FIXME replace with a different timer

	ego_init();

	while (1) {
		watchdog_usb_ready();
		if (requested_mode != MODE_EGO)
			break;

		/*
		 * A second UBERTOOTH_EGO request, which asks for the mode this
		 * loop is already running and so never reaches the mode
		 * dispatch. A killed ubertooth-ego leaves the board here, so
		 * that is the request the next run sends: without this, the
		 * state machine the first run entered carried on and the mode
		 * the request stored was never read, and an `ubertooth-ego -r`
		 * after a killed `-f` captured in follow mode.
		 */
		if (ego_restart) {
			const ego_st_handler *restarted;

			/*
			 * Cleared before the mode is read, and the request
			 * handler stores the mode before setting this: a
			 * request arriving in between is then served by one
			 * more restart rather than lost.
			 */
			ego_restart = 0;

			restarted = ego_handler_for(ego_mode);
			if (restarted == NULL) {
				/* Going idle leaves the board in the state it
				 * is actually in. */
				requested_mode = MODE_IDLE;
				continue;
			}
			handler = restarted;
			ego_fsm_start(&state);
		}

		handler[state.state](&state);
	}

	ego_deinit();
}

/**
 * Process a subcommand for rfcat-like functionality.
 * Returns 1 on success, 0 on failure.
 */
int rfcat_subcommand(uint16_t cmd, uint8_t *body, int body_len) {
	int i;
	switch (cmd) {
		/*
		 * Set the access code. Its bytes are the most significant of
		 * the 32-bit sync word, so a code shorter than four bytes
		 * leaves the rest zero. The loop has to be bounded by the
		 * length of this body: the request buffer is shared between
		 * requests, so reading further would build the code partly out
		 * of whatever the previous request left there.
		 */
		case RFCAT_SET_AA:
			if (body_len < 1 || body_len > 4)
				return 0;
			for (i = 0; i < 4; ++i) {
				access_code <<= 8;
				if (i < body_len)
					access_code |= body[i];
			}
			access_code_len = body_len;
			break;

			/*
			 * Set the capture length. do_rx() reads this many
			 * bytes into a buffer that is a local of the state
			 * handler, so a length past EGO_MAX_PACKET_LEN has to
			 * be refused rather than written down the stack, over
			 * the timestamp beside the buffer and then over the
			 * handler's return address. A length of zero would
			 * capture nothing, which is not a request worth
			 * honoring either.
			 */
		case RFCAT_CAP_LEN:
			if (body_len != 1)
				return 0;
			if (body[0] == 0 || body[0] > EGO_MAX_PACKET_LEN)
				return 0;
			packet_len = body[0];
			break;

		default:
			// error
			return 0;
	}
	return 1;
}
