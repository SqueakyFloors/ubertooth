/*
 * Copyright 2017 Mike Ryan
 *
 * This file is part of Project Ubertooth and is released under the
 * terms of the GPL. Refer to COPYING for more information.
 */

#include <stdlib.h>
#include <string.h>

#include "ubertooth.h"
#include "ubertooth_clock.h"
#include "ubertooth_dma.h"
#include "ubertooth_usb.h"
#include "bluetooth_le.h"
#include "cc2400_wait.h"
#include "queue.h"

// current time, from timer1
#define NOW T1TC
#define USEC(X) ((X)*10)
#define MSEC(X) ((X)*10000)
#define  SEC(X) ((X)*10000000)
#define PACKET_DURATION(X) (USEC(40 + (X)->size * 8))

#define ADVERTISING_AA (0x8e89bed6)

///////////////////////
// time constants

// time for the radio to warmup + some timing slack
#define RX_WARMUP_TIME USEC(300)

// max inter-frame space between packets in a connection event
#define IFS_TIMEOUT USEC(300)

// observed connection anchor must be within ANCHOR_EPSILON of
// calculated anchor
#define ANCHOR_EPSILON USEC(3)


//////////////////////
// global state

extern le_state_t le; // FIXME - refactor this struct
volatile uint16_t rf_channel;
uint8_t le_dma_dest[2];

extern volatile uint8_t mode;
extern volatile uint8_t requested_mode;

// Shared LE state owned by bluetooth_rxtx.c. We only read le.crc_verify here,
// which the host toggles via UBERTOOTH_SET_CRC_VERIFY (ubertooth-btle -v1).
extern le_state_t le;
extern volatile uint16_t le_adv_channel;
extern volatile int cancel_follow;

////////////////////
// buffers

// packet buffers live in a pool. a minimum of one buffer is always
// being used, either waiting to receive or actively receiving a packet
// (current_rxbuf). once a packet is received, it is placed into the
// packet queue. the main loop pulls packets from this queue and
// processes them, and then returns the buffers back to the pool by
// calling buffer_release()

#define LE_BUFFER_POOL_SIZE 4
typedef struct _le_rx_t {
	uint8_t data[2 + 255 + 3];  // header + PDU + CRC
	unsigned size;              // total data length (known after header rx)
	unsigned pos;               // current input byte offset
	uint32_t timestamp;         // timestamp taken after first byte rx
	unsigned channel;           // physical channel
	uint32_t access_address;    // access address
	int available;              // 1 if available, 0 in use
	int8_t rssi_min, rssi_max;  // min and max RSSI observed values
	int rssi_sum;               // running sum of all RSSI values
} le_rx_t;

// pool of all buffers
static le_rx_t le_buffer_pool[LE_BUFFER_POOL_SIZE];

// buffer waiting for or actively receiving packet
static le_rx_t *current_rxbuf = NULL;

// received packets, waiting to be processed
queue_t packet_queue;

// 1 while le_phy_main() owns the radio, DMA, and timer1.
//
// The interrupt handlers in this file drive the LE state machine, strobe the
// CC2400, and dereference current_rxbuf.  Once le_phy_main() has torn the PHY
// down, none of that is valid any more, so an interrupt that was already
// latched (or a match left armed in timer1) must do nothing.  See the teardown
// comment in le_phy_main() for why a stray interrupt here wedged the board.
static volatile int le_phy_active = 0;


/////////////////////
// connections

// this system is architected so that following multiple connections may
// be possible in the future. all connection state lives in an le_conn_t
// struct. at present only one such structure exists. refer to
// connection event below for how anchors are handled.

typedef struct _le_conn_t {
	uint32_t access_address;
	uint32_t crc_init;
	uint32_t crc_init_reversed;

	uint8_t  channel_idx;
	uint8_t  hop_increment;
	uint32_t conn_interval; // in units of 100 ns
	uint32_t supervision_timeout; // in units of 100 ns

	uint8_t  win_size;
	uint32_t win_offset; // in units of 100 ns

	le_channel_remapping_t remapping;

	uint32_t last_anchor;
	int      anchor_set;
	uint32_t last_packet_ts; // used to check supervision timeout

	uint16_t conn_event_counter;

	int      conn_update_pending;
	uint32_t conn_update_pending_interval;
	uint32_t conn_update_pending_supervision_timeout;
	uint16_t conn_update_instant;

	int      channel_map_update_pending;
	uint16_t channel_map_update_instant;
	le_channel_remapping_t pending_remapping;
} le_conn_t;
le_conn_t conn = { 0, };

// every connection event is tracked using this global le_conn_event_t
// structure named conn_event. when a packet is observed, anchor is set.
// the event may close due to receiving two packets, or if a timeout
// occurs. in both cases, finish_conn_event() is called, which updates
// the active connection's anchor. opened is set to 1 once the radio is
// tuned to the data channel for the connection event.
typedef struct _le_conn_event_t {
	uint32_t anchor;
	unsigned num_packets;
	int opened;
} le_conn_event_t;
le_conn_event_t conn_event;

static void reset_conn(void) {
	memset(&conn, 0, sizeof(conn));
	conn.access_address = ADVERTISING_AA;
}


//////////////////////
// code

// pre-declarations for utility stuff
static void timer1_start(void);
static void timer1_enable_interrupt(void);
static void timer1_stop(void);
static void timer1_set_match(uint32_t match);
static void timer1_clear_match(void);
static void timer1_wait_fs_lock(void);
static void timer1_cancel_fs_lock(void);
static void timer1_wait_buffer(void);
static void timer1_cancel_wait_buffer(void);
static void blink(int tx, int rx, int usr);
static void le_dma_init(void);
static void le_cc2400_strobe_rx(void);
static void change_channel(void);
static uint8_t dewhiten_length(unsigned channel, uint8_t data);

// resets the state of all available buffers
static void buffers_init(void) {
	int i;

	for (i = 0; i < LE_BUFFER_POOL_SIZE; ++i)
		le_buffer_pool[i].available = 1;
}

// clear a buffer for new data
static void buffer_clear(le_rx_t *buf) {
	buf->pos = 0;
	buf->size = 0;
	memset(buf->data, 0, sizeof(buf->data));
	buf->rssi_min = INT8_MAX;
	buf->rssi_max = INT8_MIN;
	buf->rssi_sum = 0;
}

// get a packet buffer
// returns a pointer to a buffer if available
// returns NULL otherwise
static le_rx_t *buffer_get(void) {
	int i;

	for (i = 0; i < LE_BUFFER_POOL_SIZE; ++i) {
		if (le_buffer_pool[i].available) {
			le_buffer_pool[i].available = 0;
			buffer_clear(&le_buffer_pool[i]);
			return &le_buffer_pool[i];
		}
	}

	return NULL;
}

// release a buffer back to the pool
static void buffer_release(le_rx_t *buffer) {
	buffer->available = 1;
}

// clear a connection event
static void reset_conn_event(void) {
	conn_event.num_packets = 0;
	conn_event.opened = 0;
}

// finish a connection event
//
// 1) update the anchor point (see details below)
// 2) increment connection event counter
// 3) check if supervision timeout is exceeded
// 4) setup radio for next packet (data or adv if timeout exceeded)
//
// anchor update logic can be summarized thusly:
// 1) if we received two packets, set the connection anchor to the
//    observed value
// 2) if we received one packet, see if it's within ANCHOR_EPISLON
//    microseconds if the expected anchor time. if so, it's the master
//    and we can update the anchor
// 3) if the single packet is a slave or we received zero packets,
//    update the anchor to the estimated value
//
// FIXME this code does not properly handle the case where the initial
// connection transmit window has no received packets
static void finish_conn_event(void) {
	uint32_t last_anchor = 0;
	int last_anchor_set = 0;

	// two packets -- update anchor
	if (conn_event.num_packets == 2) {
		last_anchor = conn_event.anchor;
		last_anchor_set = 1;
	}

	// if there's one packet, we need to find out if it was the master
	else if (conn_event.num_packets == 1 && conn.anchor_set) {
		// calculate the difference between the estimated and observed anchor
		uint32_t estimated_anchor = conn.last_anchor + conn.conn_interval;
		uint32_t delta = estimated_anchor - conn_event.anchor;
		// see whether the observed anchor is within 3 us of the estimate
		delta += ANCHOR_EPSILON;
		if (delta < 2 * ANCHOR_EPSILON) {
			last_anchor = conn_event.anchor;
			last_anchor_set = 1;
		}
	}

	// if we observed a new anchor, set it
	if (last_anchor_set) {
		conn.last_anchor = last_anchor;
		conn.anchor_set = 1;
	}

	// without a new anchor, estimate the next anchor
	else if (conn.anchor_set) {
		conn.last_anchor += conn.conn_interval;
	}

	else {
		// FIXME this is totally broken if we receive the slave's packet first
		conn.last_anchor = conn_event.anchor;
		conn.last_packet_ts = NOW; // FIXME gross hack
	}

	// update last packet for supervision timeout
	if (conn_event.num_packets > 0) {
		conn.last_packet_ts = NOW;
	}

	reset_conn_event();

	// increment connection event counter
	++conn.conn_event_counter;

	// supervision timeout reached - switch back to advertising
	if (NOW - conn.last_packet_ts > conn.supervision_timeout) {
		reset_conn();
		change_channel();
	}

	// FIXME - hack to cancel following a connection
	else if (cancel_follow) {
		cancel_follow = 0;
		reset_conn();
		change_channel();
	}

	// supervision timeout not reached - hop to next channel
	else {
		timer1_set_match(conn.last_anchor + conn.conn_interval - RX_WARMUP_TIME);
	}
}

// DMA handler
// called once per byte. handles all incoming data, but only minimally
// processes received data. at the end of a packet, it enqueues the
// received packet, fetches a new buffer, and restarts RX.
void le_DMA_IRQHandler(void) {
	unsigned pos;
	int8_t rssi;
	uint32_t timestamp = NOW; // sampled early for most accurate measurement

	// Nothing to do once the PHY has been torn down: the radio is off, the
	// buffers are no longer owned by this module, and touching the CC2400 here
	// would collide with the main loop's own bit-banged SPI access.
	if (!le_phy_active) {
		dma_clear_interrupts(0);
		return;
	}

	// channel 0
	if (DMACIntStat & (1 << 0)) {
		// terminal count - byte received
		if (DMACIntTCStat & (1 << 0)) {
			DMACIntTCClear = (1 << 0);

			// RX must never be armed without a buffer, but if it ever is,
			// stop rather than dereference NULL: a fault here lands in the
			// fault.c LED loop, which also stops USB being serviced, so the
			// board would answer nothing until it was re-plugged.
			if (current_rxbuf == NULL) {
				// SFSON, not SRFOFF: stop receiving but leave the
				// synthesizer running, because the buffer wait below
				// hands over to the FS_LOCK path, which only arms RX
				// once the synthesizer reports locked.
				cc2400_strobe(SFSON);

				// same DMA teardown as the end-of-packet path below,
				// including draining the SSP: the receiver keeps clocking
				// bytes into the FIFO until SFSON takes effect, and they
				// would otherwise be re-armed as the head of the next packet
				DMACC0Config = 0;
				DMACIntTCClear = (1 << 0);
				DIO_SSP_DMACR &= ~SSPDMACR_RXDMAE;
				while (SSP1SR & SSPSR_RNE) {
					uint8_t tmp = (uint8_t)DIO_SSP_DR;
					(void)tmp;
				}
				ICER0 = ICER0_ICE_DMA;

				timer1_wait_buffer();
				return;
			}

			// poll RSSI
			rssi = (int8_t)(cc2400_get(RSSI) >> 8);
			current_rxbuf->rssi_sum += rssi;
			if (rssi < current_rxbuf->rssi_min) current_rxbuf->rssi_min = rssi;
			if (rssi > current_rxbuf->rssi_max) current_rxbuf->rssi_max = rssi;

			// grab byte from DMA buffer
			pos = current_rxbuf->pos;
			current_rxbuf->data[pos] = le_dma_dest[pos & 1]; // dirty hack
			pos += 1;
			current_rxbuf->pos = pos;

			if (pos == 1) {
				current_rxbuf->timestamp = timestamp - USEC(8 + 32); // packet starts at preamble
				current_rxbuf->channel = rf_channel;
				current_rxbuf->access_address = conn.access_address;

				// data packet received: cancel timeout
				// new timeout or hop timer will be set at end of packet RX
				if (btle_channel_index(rf_channel) < 37) {
					timer1_clear_match();
				}
			}

			// get length from header
			if (pos == 2) {
				uint8_t length = dewhiten_length(current_rxbuf->channel, current_rxbuf->data[1]);
				current_rxbuf->size = length + 2 + 3; // two bytes for header and three for CRC
			}

			// finished packet - state transition
			if (pos > 2 && pos >= current_rxbuf->size) {
				// stop the CC2400 before flushing SSP
				cc2400_strobe(SFSON);

				// stop DMA on this channel and flush SSP
				DMACC0Config = 0;
				DMACIntTCClear = (1 << 0); // if we don't clear a second time, data is corrupt

				DIO_SSP_DMACR &= ~SSPDMACR_RXDMAE;
				while (SSP1SR & SSPSR_RNE) {
					uint8_t tmp = (uint8_t)DIO_SSP_DR;
				}

				// XXX disable DMA interrupt as a workaround
				ICER0 = ICER0_ICE_DMA;

				// TODO error transition on queue_insert
				queue_insert(&packet_queue, current_rxbuf);

				// track connection events
				if (btle_channel_index(rf_channel) < 37) {
					++conn_event.num_packets;

					// first packet: set connection anchor
					if (conn_event.num_packets == 1) {
						conn_event.anchor = current_rxbuf->timestamp;
						timer1_set_match(NOW + IFS_TIMEOUT); // set a timeout for next packet
					}

					// second packet: close connection event, and set hop timer
					else if (conn_event.num_packets == 2) {
						cc2400_strobe(SRFOFF);
						// may be NULL if the pool is exhausted; the
						// change_channel() and timer1 paths below check
						// for that and wait for a buffer instead of
						// arming RX without one
						current_rxbuf = buffer_get();
						finish_conn_event();
						return;
					}
				}

				// get a new packet
				// TODO handle error transition
				current_rxbuf = buffer_get();
				if (current_rxbuf == NULL) {
					// if all buffers are in use, wait for one to free up in background
					timer1_wait_buffer();
				} else {
					// wait for FS_LOCK in background
					timer1_wait_fs_lock();
				}
			}
		}

		// error - transition to error state
		if (DMACIntErrStat & (1 << 0)) {
			// TODO error state transition
			DMACIntErrClr = (1 << 0);
		}
	}
}

static void le_dma_init(void) {
	int i;

	// DMA linked list items
	typedef struct {
		uint32_t src;
		uint32_t dest;
		uint32_t next_lli;
		uint32_t control;
	} dma_lli;
	static dma_lli le_dma_lli[2];

	for (i = 0; i < 2; ++i) {
		le_dma_lli[i].src = (uint32_t)&(DIO_SSP_DR);
		le_dma_lli[i].dest = (uint32_t)&le_dma_dest[i];
		le_dma_lli[i].next_lli = (uint32_t)&le_dma_lli[1-i]; // two elements pointing back at each other
		le_dma_lli[i].control = 1 |
				(0 << 12) |        // source burst size = 1
				(0 << 15) |        // destination burst size = 1
				(0 << 18) |        // source width 8 bits
				(0 << 21) |        // destination width 8 bits
				DMACCxControl_I;   // terminal count interrupt enable
	}

	// configure DMA channel 0
	DMACC0SrcAddr = le_dma_lli[0].src;
	DMACC0DestAddr = le_dma_lli[0].dest;
	DMACC0LLI = le_dma_lli[0].next_lli;
	DMACC0Control = le_dma_lli[0].control;
	DMACC0Config =
			DIO_SSP_SRC |
			(0x2 << 11) |     // peripheral to memory
			DMACCxConfig_IE | // allow error interrupts
			DMACCxConfig_ITC; // allow terminal count interrupts
}

// initalize USB, SSP, and DMA
static void le_sys_init(void) {
	usb_queue_init(); // USB FIFO FIXME replace with safer queue
	dio_ssp_init();   // init SSP and raise !CS (self-routed GPIO)
	le_dma_init();    // prepare DMA + interrupts
	dio_ssp_start();  // enable SSP + DMA
}

// initialize RF and strobe FSON
static void le_cc2400_init_rf(void) {
	u16 grmdm, mdmctrl;
	uint32_t sync = rbit(conn.access_address);

	mdmctrl = 0x0040; // 250 kHz frequency deviation
	grmdm = 0x44E1; // un-buffered mode, packet w/ sync word detection
	// 0 10 00 1 001 11 0 00 0 1
	//   |  |  | |   |  +--------> CRC off
	//   |  |  | |   +-----------> sync word: 32 MSB bits of SYNC_WORD
	//   |  |  | +---------------> 1 preamble byte of 01010101
	//   |  |  +-----------------> packet mode
	//   |  +--------------------> un-buffered mode
	//   +-----------------------> sync error bits: 2

	cc2400_set(MANAND,  0x7ffe);
	cc2400_set(LMTST,   0x2b22);

	cc2400_set(MDMTST0, 0x124b);
	// 1      2      4b
	// 00 0 1 0 0 10 01001011
	//    | | | | |  +---------> AFC_DELTA = ??
	//    | | | | +------------> AFC settling = 4 pairs (8 bit preamble)
	//    | | | +--------------> no AFC adjust on packet
	//    | | +----------------> do not invert data
	//    | +------------------> TX IF freq 1 0Hz
	//    +--------------------> PRNG off
	//
	// ref: CC2400 datasheet page 67
	// AFC settling explained page 41/42

	cc2400_set(GRMDM,   grmdm);

	cc2400_set(SYNCL,   sync & 0xffff);
	cc2400_set(SYNCH,   (sync >> 16) & 0xffff);

	cc2400_set(FSDIV,   rf_channel - 1); // 1 MHz IF
	cc2400_set(MDMCTRL, mdmctrl);

	// XOSC16M should always be stable, but leave this test anyway.
	//
	// Bounded, and this one has to be: change_channel() calls this from
	// TIMER1_IRQHandler() on every connection-event hop, and this mode polls
	// USB from its own loop. A poll that never satisfies hangs the interrupt
	// handler, which stops the loop with it -- board still enumerated, every
	// control request timing out, nothing but a power cycle to recover. The
	// status read is a bit-banged SPI transfer on a bus interrupt handlers
	// also drive, so a corrupt status byte is a real possibility rather than
	// a hypothetical one.
	cc2400_wait_xosc_stable(NULL);

	// wait for FS_LOCK in background
	cc2400_strobe(SFSON);
	timer1_wait_fs_lock();
}

// strobe RX and enable PA
static void le_cc2400_strobe_rx(void) {
	cc2400_strobe(SRX);
#ifdef UBERTOOTH_ONE
	PAEN_SET;
	HGM_SET;
#endif
}

// change channel and init rx
static void change_channel(void) {
	uint8_t channel_idx = 0;

	cc2400_strobe(SRFOFF);

	// stop DMA and flush SSP
	DIO_SSP_DMACR &= ~SSPDMACR_RXDMAE;
	while (SSP1SR & SSPSR_RNE) {
		uint8_t tmp = (uint8_t)DIO_SSP_DR;
	}

	// current_rxbuf can be NULL when the buffer pool was exhausted at the end
	// of a connection event. buffer_clear(NULL) faulted the CPU, so take a
	// buffer if one has since been freed; without one, do not re-arm DMA or
	// the SSP either. le_cc2400_init_rf() below hands over to the timer1
	// FS_LOCK path, which waits for a buffer before arming them.
	if (current_rxbuf == NULL)
		current_rxbuf = buffer_get();
	if (current_rxbuf != NULL) {
		buffer_clear(current_rxbuf);
		le_dma_init();
		dio_ssp_start();
	}

	if (conn.access_address == ADVERTISING_AA) {
		// FIXME
		switch (le_adv_channel) {
			case 2402: channel_idx = 37; break;
			case 2426: channel_idx = 38; break;
			case 2480: channel_idx = 39; break;
			default:   channel_idx = 37; break;
		}
	} else {
		conn.channel_idx = (conn.channel_idx + conn.hop_increment) % 37;
		channel_idx = le_map_channel(conn.channel_idx, &conn.remapping);
	}

	rf_channel = btle_channel_index_to_phys(channel_idx);
	le_cc2400_init_rf();
}

///////
// timer stuff

// Start the counter. It has to be running before the radio is set up, because
// the matches are armed against NOW, which reads it. That is why unmasking the
// interrupt is a separate step: see le_phy_main().
static void timer1_start(void) {
	T1TCR = TCR_Counter_Reset;
	T1PR = 4; // 100 ns

	// Start from nothing pending. The teardown masks the line, but a match
	// taken just before the mask stays pending in the NVIC, and a match flag
	// can be left in T1IR; either would be delivered as the first interrupt
	// of this run, after le_phy_active is set, and the handler would act on
	// it as a live event. Clearing it here, before the counter runs, is also
	// before le_cc2400_init_rf() arms the FS_LOCK match this run does want.
	//
	// Best-effort on the last line: if the T1IR write has not reached the
	// timer when the pending bit is cleared, the still-asserted line latches
	// it again. Harmless, because the handler then finds no T1IR bit set and
	// falls through every branch.
	T1MCR = 0;
	T1IR = 0xff; // write-1-to-clear: every match and capture flag
	ICPR0 = ICPR0_ICP_TIMER1;

	T1TCR = TCR_Counter_Enable;
}

// Let timer1's matches reach TIMER1_IRQHandler(). The handler drives the LE
// state machine, so this comes after le_phy_active says that state is set up.
static void timer1_enable_interrupt(void) {
	ISER0 = ISER0_ISE_TIMER1;
}

static void timer1_stop(void) {
	T1TCR = TCR_Counter_Reset;

	// clear interrupt handler
	ICER0 = ICER0_ICE_TIMER1;
}

static void timer1_set_match(uint32_t match) {
	T1MR0 = match;
	T1MCR |= TMCR_MR0I;
}

static void timer1_clear_match(void) {
	T1MCR &= ~TMCR_MR0I;
}

static void timer1_wait_fs_lock(void) {
	T1MR2 = NOW + USEC(3);
	T1MCR |= TMCR_MR2I;
}

static void timer1_cancel_fs_lock(void) {
	T1MCR &= ~TMCR_MR2I;
}

static void timer1_wait_buffer(void) {
	T1MR3 = NOW + USEC(100);
	T1MCR |= TMCR_MR3I;
}

static void timer1_cancel_wait_buffer(void) {
	T1MCR &= ~TMCR_MR3I;
}

void TIMER1_IRQHandler(void) {
	// timer1 belongs to this PHY; after teardown just acknowledge whatever is
	// pending so a stale match cannot restart the radio or the DMA channel.
	if (!le_phy_active) {
		T1MCR = 0;
		T1IR = 0xff; // write-1-to-clear: every match and capture flag
		return;
	}

	// MR0: connection events
	if (T1IR & TIR_MR0_Interrupt) {
		// ack the interrupt
		T1IR = TIR_MR0_Interrupt;

		// connection update procedure
		if (conn.conn_update_pending &&
				conn.conn_event_counter == conn.conn_update_instant) {

			// on the first past through, handle the transmit window
			// offset. if there's no offset, skip down to else block
			if (!conn_event.opened && conn.win_offset > 0) {
				timer1_set_match(conn.last_anchor + conn.conn_interval +
						conn.win_offset - RX_WARMUP_TIME);
				conn_event.opened = 1;
			}

			// after the transmit window offset, or if there is no
			// transmit window, set a packet timeout and change the
			// channel
			else { // conn_event.opened || conn.win_offset == 0
				conn_event.opened = 1;

				// this is like a new connection, so set all values
				// accordingly
				conn.anchor_set = 0;
				conn.conn_interval = conn.conn_update_pending_interval;
				conn.supervision_timeout = conn.conn_update_pending_supervision_timeout;
				conn.conn_update_pending = 0;

				// timeout after conn window + max packet length
				timer1_set_match(conn.last_anchor + conn.conn_interval +
						conn.win_offset + conn.win_size + USEC(2120));
				change_channel();
			}
			return;
		}

		// channel map update
		if (conn.channel_map_update_pending &&
				conn.conn_event_counter == conn.channel_map_update_instant) {
			conn.remapping = conn.pending_remapping;
			conn.channel_map_update_pending = 0;
		}

		// new connection event: set timeout and change channel
		if (!conn_event.opened) {
			conn_event.opened = 1;
			// timeout is max packet length + warmup time (slack)
			timer1_set_match(NOW + USEC(2120) + RX_WARMUP_TIME);
			change_channel();
		}

		// regular connection event, plus timeout from connection updates
		// FIXME connection update timeouts and initial connection
		// timeouts need to be handled differently: they should have a
		// full window until the packets from the new connection are
		// captured and a new anchor is set.
		else {
			// new connection event: set timeout and change channel
			if (!conn_event.opened) {
				conn_event.opened = 1;

				// timeout is max packet length + warmup time (slack)
				timer1_set_match(NOW + USEC(2120) + RX_WARMUP_TIME);
				change_channel();
			}

			// timeout: close connection event and set timer for next hop
			else {
				finish_conn_event();
			}
		}
	}

	// LEDs
	if (T1IR & TIR_MR1_Interrupt) {
		T1IR = TIR_MR1_Interrupt;
		T1MCR &= ~TMCR_MR1I;

		TXLED_CLR;
		RXLED_CLR;
		USRLED_CLR;
	}

	// check FS_LOCK
	if (T1IR & TIR_MR2_Interrupt) {
		T1IR = TIR_MR2_Interrupt;

		// if FS is locked, strobe RX and clear interrupt
		if (cc2400_status() & FS_LOCK) {
			T1MCR &= ~TMCR_MR2I;

			// Never arm RX without a packet buffer: le_DMA_IRQHandler()
			// would have nowhere to put the bytes. Wait for one instead.
			if (current_rxbuf == NULL) {
				timer1_wait_buffer();
			} else {
				// restart DMA and SSP
				le_dma_init();
				dio_ssp_start();
				ISER0 = ISER0_ISE_DMA;

				le_cc2400_strobe_rx();
			}
		}

		// if FS is not locked, check again in 3 us
		else {
			timer1_wait_fs_lock();
		}
	}

	// check if an rxbuf is available and transition to FS_LOCK/RX
	if (T1IR & TIR_MR3_Interrupt) {
		T1IR = TIR_MR3_Interrupt;

		// only take a buffer if we do not already hold one: change_channel()
		// can also acquire one while this wait is armed, and overwriting the
		// pointer would leak that buffer for the rest of the mode
		if (current_rxbuf == NULL)
			current_rxbuf = buffer_get();

		if (current_rxbuf == NULL) {
			timer1_wait_buffer();
		} else {
			// the wait is over: disarm the match, or it fires again after the
			// counter wraps and overwrites current_rxbuf, leaking a buffer
			timer1_cancel_wait_buffer();

			if (cc2400_status() & FS_LOCK) {
				// restart DMA and SSP
				le_dma_init();
				dio_ssp_start();
				ISER0 = ISER0_ISE_DMA;

				le_cc2400_strobe_rx();
			} else {
				timer1_wait_fs_lock();
			}
		}
	}
}

static void blink(int tx, int rx, int usr) {
	if (tx)
		TXLED_SET;
	if (rx)
		RXLED_SET;
	if (usr)
		USRLED_SET;

	// blink for 10 ms
	T1MR1 = NOW + MSEC(10);
	T1MCR |= TMCR_MR1I;
}

// helper function to dewhiten length from whitened data (only used
// during DMA)
static uint8_t dewhiten_length(unsigned channel, uint8_t data) {
	unsigned int i, bit;
	int idx = whitening_index[btle_channel_index(channel)];
	uint8_t out = 0;

	// length is second byte of packet
	idx = (idx + 8) % sizeof(whitening);

	for (i = 0; i < 8; ++i) {
		bit = (data >> (7-i)) & 1;
		bit ^= whitening[idx];
		idx = (idx + 1) % sizeof(whitening);
		out |= bit << i;
	}

	return out;
}

// enqueue a packet for USB
// FIXME this is cribbed from existing code, but does not have enough
// room for larger LE packets
static int usb_enqueue_le(le_rx_t *packet) {
	usb_pkt_rx* f = usb_enqueue();

	// fail if queue is full
	if (f == NULL) {
		return 0;
	}

	f->pkt_type = LE_PACKET;

	f->clkn_high = 0;
	f->clk100ns = packet->timestamp;

	f->channel = (uint8_t)((packet->channel - 2402) & 0xff);
	f->rssi_avg = packet->rssi_sum / packet->size;
	f->rssi_min = packet->rssi_min;
	f->rssi_max = packet->rssi_max;
	f->rssi_count = 0;

	memcpy(f->data, &packet->access_address, 4);
	memcpy(f->data+4, packet->data, DMA_SIZE-4);

	f->status = 0;

	return 1;
}

static unsigned extract_field(le_rx_t *buf, size_t offset, unsigned size) {
	unsigned i, ret = 0;

	// this could just be replaced by memcpy... right?
	for (i = 0; i < size; ++i)
		ret |= buf->data[offset + i] << (i*8);

	return ret;
}

static void le_connect_handler(le_rx_t *buf) {
	uint32_t aa, crc_init;
	uint32_t win_size, max_win_size;

	if (!le.do_follow)
		return;

	if (buf->size != 2 + 6 + 6 + 22 + 3)
		return;

	// FIXME ugly hack
	if (cancel_follow)
		cancel_follow = 0;

	conn.access_address     = extract_field(buf, 14, 4);
	conn.crc_init           = extract_field(buf, 18, 3);
	conn.crc_init_reversed  = rbit(conn.crc_init);
	conn.win_size           = extract_field(buf, 21, 1);
	conn.win_offset         = extract_field(buf, 22, 2);
	conn.conn_interval      = extract_field(buf, 24, 2);
	conn.supervision_timeout = extract_field(buf, 28, 2);
	conn.hop_increment      = extract_field(buf, 35, 1) & 0x1f;

	if (conn.conn_interval < 6 || conn.conn_interval > 3200) {
		goto err_out;
	} else {
		conn.conn_interval *= USEC(1250);
	}

	// window offset is in range [0, conn_interval]
	conn.win_offset *= USEC(1250);
	if (conn.win_offset > conn.conn_interval)
		goto err_out;

	// win size is in range [1.25 ms, MIN(10 ms, conn_interval - 1.25 ms)]
	win_size = conn.win_size * USEC(1250);
	max_win_size = conn.conn_interval - USEC(1250);
	if (max_win_size > MSEC(10))
		max_win_size = MSEC(10);
	if (win_size < USEC(1250) || win_size > max_win_size)
		goto err_out;

	// The connSupervisionTimeout shall be a multiple of 10 ms in the
	// range of 100 ms to 32.0 s and it shall be larger than (1 +
	// connSlaveLatency) * connInterval * 2
	conn.supervision_timeout *= MSEC(10);
	if (conn.supervision_timeout < MSEC(100) || conn.supervision_timeout > SEC(32))
		goto err_out;
	// TODO handle slave latency

	le_parse_channel_map(&buf->data[30], &conn.remapping);
	if (conn.remapping.total_channels == 0)
		goto err_out;

	// cancel RX on advertising channel
	timer1_cancel_fs_lock();

	reset_conn_event();
	timer1_set_match(buf->timestamp + PACKET_DURATION(buf) +
			conn.win_offset + USEC(1250) - RX_WARMUP_TIME);
	return;

	// error condition: reset conn and return
err_out:
	reset_conn();
}

static void connection_update_handler(le_rx_t *buf) {
	conn.win_size            = extract_field(buf, 3, 1);
	conn.win_offset          = extract_field(buf, 4, 2);
	conn.conn_update_pending_interval = extract_field(buf, 6, 2);
	conn.conn_update_pending_supervision_timeout = extract_field(buf, 10, 2);
	conn.conn_update_instant = extract_field(buf, 12, 2);

	// TODO check for invalid values. XXX what do we even do in that
	// case? we will probably drop the connection, but at least it's on
	// our own terms and not some impossibly long supervision timeout.
	conn.win_size   *= USEC(1250);
	conn.win_offset *= USEC(1250);
	conn.conn_update_pending_interval *= USEC(1250);
	conn.conn_update_pending_supervision_timeout *= MSEC(10);

	conn.conn_update_pending = 1;
}

static void channel_map_update_handler(le_rx_t *buf) {
	conn.channel_map_update_pending = 1;
	conn.channel_map_update_instant = extract_field(buf, 8, 2);
	le_parse_channel_map(&buf->data[3], &conn.pending_remapping);
}

static void packet_handler(le_rx_t *buf) {
	// advertising packet
	if (btle_channel_index(buf->channel) >= 37) {
		switch (buf->data[0] & 0xf) {
			// CONNECT_REQ
			case 0x05:
				// TODO validate length
				le_connect_handler(buf);
				break;
		}
	}

	// data packet
	else {
		// LL control PDU
		if ((buf->data[0] & 0b11) == 0b11 && buf->data[1] > 0) {
			switch (buf->data[2]) {
				// LE_CONNECTION_UPDATE_REQ -- update connection parameters
				case 0x0:
					if (buf->data[1] == 12)
						connection_update_handler(buf);
					break;

				// LE_CHANNEL_MAP_REQ -- update channel map
				case 0x1:
					if (buf->data[1] == 8)
						channel_map_update_handler(buf);
					break;
			}
		}
	}
}

// compare a BD addr against target with mask
static int bd_addr_cmp(uint8_t *bd_addr) {
	unsigned i;
	for (i = 0; i < 6; ++i)
		if ((bd_addr[i] & le.target_mask[i]) != le.target[i])
			return 0;
	return 1;
}

static int filter_match(le_rx_t *buf) {
	if (!le.target_set)
		return 1;

	// allow all data channel packets
	if (btle_channel_index(buf->channel) < 37)
		return 1;

	switch (buf->data[0] & 0xf) {
		// ADV_IND, ADV_NONCONN_IND, ADV_SCAN_IND, SCAN_RSP
		case 0x00:
		case 0x02:
		case 0x06:
		case 0x04:
			// header + one address
			if (buf->size < 2 + 6)
				return 0;
			return bd_addr_cmp(&buf->data[2]);
			break;

		// ADV_DIRECT_IND, SCAN_REQ, CONNECT_REQ
		case 0x01:
		case 0x03:
		case 0x05:
			// header + two addresses
			if (buf->size < 2 + 6 + 6)
				return 0;
			return bd_addr_cmp(&buf->data[2]) ||
				   bd_addr_cmp(&buf->data[8]);
			break;

		default:
			break;
	}

	return 0;
}

void le_phy_main(void) {
	// disable USB interrupts -- we poll them below
	// n.b., they should not be enabled but let's be careful
	ICER0 = ICER0_ICE_USB;
	// disable clkn and timer0
	clkn_disable();

	buffers_init();
	queue_init(&packet_queue);

	// The counter runs from here because NOW, and the FS_LOCK match
	// le_cc2400_init_rf() arms through it, are measured against it. Its
	// interrupt stays masked until the setup below is done.
	timer1_start();

	current_rxbuf = buffer_get();
	rf_channel = le_adv_channel; // FIXME
	conn.access_address = ADVERTISING_AA;
	le_sys_init();
	le_cc2400_init_rf();

	// Everything the interrupt handlers read is set up now: the buffer pool
	// and the packet queue, the first receive buffer, the DMA channel and the
	// SSP, and the radio with its FS_LOCK match armed. Nothing above needs
	// le_phy_active set, so the flag can wait until here, and the interrupt
	// that checks it is unmasked only after it is set. That closes the window
	// in which a timer1 match left over from an earlier run could be taken
	// with current_rxbuf still the NULL the teardown left behind; the NULL
	// guards in le_DMA_IRQHandler() and TIMER1_IRQHandler() stay as the
	// backstop for the buffer pool running dry while the mode runs.
	//
	// One interrupt can be taken in the window: dio_ssp_start() inside
	// le_sys_init() arms DMA channel 0, and the DMA line is live on entry
	// (the teardown leaves it enabled for the modes that follow). With the
	// flag clear, le_DMA_IRQHandler() acknowledges and returns instead of
	// writing bytes into a buffer whose RX has not been armed yet, which is
	// the behaviour wanted here: nothing should arrive until the FS_LOCK path
	// strobes RX.
	le_phy_active = 1;
	timer1_enable_interrupt();

	cancel_follow = 0;

	while (requested_mode == MODE_BT_FOLLOW_LE) {
		le_rx_t *packet = NULL;
		if (queue_remove(&packet_queue, (void **)&packet)) {
			le_dewhiten(packet->data, packet->size, packet->channel);

			// On-device CRC validation. This modern LE engine previously
			// forwarded every packet including bit-errored ones, so the
			// host's UBERTOOTH_SET_CRC_VERIFY (ubertooth-btle -v1) had no
			// effect on advertising/follow captures and bit-flipped
			// "phantom" advertisers leaked to the host. Advertising-channel
			// PDUs use the fixed CRCInit (reversed: 0xAAAAAA); data-channel
			// PDUs use the value recovered from CONNECT_IND. Disabled by
			// default, so the legacy behaviour is unchanged unless -v1 is set.
			int crc_ok = 1;
			if (le.crc_verify && packet->size > 3) {
				unsigned pdu_len = packet->size - 3;
				uint32_t crc_init_rev =
					(btle_channel_index(packet->channel) >= 37)
						? 0xAAAAAA : conn.crc_init_reversed;
				uint32_t calc_crc =
					btle_crcgen_lut(crc_init_rev, packet->data, pdu_len);
				uint32_t wire_crc =
					  (packet->data[pdu_len + 2] << 16)
					| (packet->data[pdu_len + 1] << 8)
					|  packet->data[pdu_len + 0];
				crc_ok = (calc_crc == wire_crc);
			}

			if (crc_ok && filter_match(packet)) {
				blink(0, 1, 0); // RX LED
				usb_enqueue_le(packet);
				packet_handler(packet);
			}

			buffer_release(packet);
		}

		// Polled USB handling, control requests only. The host for this
		// mode (ubertooth-btle -n/-f) collects packets with UBERTOOTH_POLL
		// and never reads the bulk IN endpoint, so handing packets to that
		// endpoint loses them: it has two hardware buffers, and this mode's
		// first two packets went into them every session.
		handle_usb_control();

		// XXX maybe LED light show?
	}

	// Tear the PHY down before handing control back to the main loop.
	//
	// This used to stop timer1 and clear the LEDs only, leaving the DMA
	// interrupt enabled, DMA channel 0 and the SSP running, and the CC2400 in
	// RX. mode was still MODE_BT_FOLLOW_LE, so DMA_IRQHandler() kept
	// dispatching arriving bytes into le_DMA_IRQHandler(), which strobes the
	// synthesizer back on (SFSON) and reads RSSI over the CC2400's bit-banged
	// SPI bus. So the handler kept switching the radio on under the
	// cc2400_idle() the main loop runs next, whose FS_LOCK wait was unbounded:
	// the board could sit there forever with USB unserviced, and every later
	// control request timed out (-7) or stalled (-9) until the board was
	// physically re-plugged.
	//
	// Order matters. Clearing le_phy_active first is what makes this safe: a
	// write to NVIC ICER0 is not self-synchronising on Cortex-M3, so an
	// already-pending interrupt can still be taken after the mask, and the
	// flag is what neuters it. Then stop what feeds the interrupt, then quiet
	// the radio.
	le_phy_active = 0;

	ICER0 = ICER0_ICE_DMA;
	dio_ssp_stop();

	// dio_ssp_stop() has disabled DMA channel 0 and cleared its flags, so
	// nothing can raise the interrupt now. Put the NVIC line back the way
	// dma_poweron() left it at boot: it is global, and the modes that run
	// after this one (bt_stream_rx(), bt_generic_le(), bt_le_sync()) all wait
	// on rx_tc from legacy_DMA_IRQHandler().
	ISER0 = ISER0_ISE_DMA;

	// drop every armed match, including MR1 (the LED blink), and any flag one
	// of them has already latched, before stopping the timer, so the hardware
	// is left quiet. The next entry clears all of this again rather than
	// trusting it.
	T1MCR = 0;
	T1IR = 0xff; // write-1-to-clear: every match and capture flag
	timer1_stop();

	// kill the CC2400: synthesizer off, and the Ubertooth One amplifier with it
	cc2400_strobe(SRFOFF);
#ifdef UBERTOOTH_ONE
	PAEN_CLR;
	HGM_CLR;
#endif

	// release the buffers this mode owned, and drop the queue that pointed
	// into them, so the pool and the queue stay consistent
	current_rxbuf = NULL;
	queue_init(&packet_queue);
	buffers_init();

	// reset state
	RXLED_CLR;
	TXLED_CLR;
	USRLED_CLR;
	clkn_init();
}
