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
#include "watchdog.h"

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

// The time a maximum-length packet takes on the 1 Mbit/s PHY.
#define LONGEST_PACKET USEC(2120)

// How late in a connection event the peripheral's answer can be, measured
// from the start of the central's packet that it answers: the longest packet
// the central could have sent, and the inter-frame space after it.
//
// This is how much earlier than a single received packet the event's anchor
// could be, because one packet alone does not say which of the two it was.
#define PERIPHERAL_REPLY_LATEST (LONGEST_PACKET + USEC(150))

// How far ahead of the counter a connection event's match has to be armed.
// See timer1_arm_conn_event(): arming it closer than the few instructions
// that arming takes would leave the match behind the counter.
#define CONN_EVENT_ARM_MARGIN USEC(10)

// observed connection anchor must be within ANCHOR_EPSILON of
// calculated anchor
#define ANCHOR_EPSILON USEC(3)

// How long timer1 waits before looking at FS_LOCK again, and how many of
// those re-checks keep the short interval.
//
// Every re-check runs in TIMER1_IRQHandler(), with the mode's loop underneath
// it, and that loop is the only thing servicing USB while this mode runs. A
// re-check costs a status read on the radio's bit-banged bus -- about 1.6
// microseconds with interrupts masked -- plus the interrupt's own entry and
// exit, so at a 3 microsecond interval the loop was left with well under half
// the processor for as long as the synthesizer did not report locked, which
// for a lock that never arrives is forever. The interval is armed from a
// counter reading taken near the end of the handler, so the match is not armed
// in the past and the loop does keep running; what it does not keep is enough
// of the processor to be relied on.
//
// This is not what ends a live capture -- a bench run reproduces that with
// these intervals in place, and the watchdog breadcrumb lands on the loop
// inside its USB service pass rather than in this handler. It is worth not
// leaving in place regardless.
//
// So the short interval is kept, because a healthy lock is found inside it and
// the receiver has to be armed promptly, but only for as long as a healthy
// lock could possibly take. A re-check and its interval together cost about 6
// microseconds, so 512 of them span some three milliseconds -- several times
// the few hundred microseconds the synthesizer needs. Past that the lock is
// overdue and the interval becomes one no interrupt entry can outrun.
//
// The wait never gives up: retrying slowly keeps the capture alive and picks
// the radio up again if it recovers, where giving up would leave the receiver
// disarmed. Every sixteenth slow re-check strobes the synthesizer on again, in
// case a handler switched it off between re-checks -- the same thing
// cc2400_wait_fs() does, and at the same pace. Not on every re-check: a strobe
// arriving while the synthesizer is on its way to a lock starts it over, so
// strobing often enough would be a lock that never arrives.
#define FS_LOCK_RECHECK         USEC(3)
#define FS_LOCK_RECHECK_SLOW    USEC(100)
#define FS_LOCK_FAST_RECHECKS   512
#define FS_LOCK_SLOW_RESTROBE   16

// Reads before emptying the symbol buffer gives up.
//
// The drains below empty the SSP's receive buffer after the radio has been
// strobed off, so symbols that arrived before the strobe took effect are not
// read back as the start of the next packet. All three run in an interrupt
// handler: two directly in le_DMA_IRQHandler(), and one in change_channel(),
// which both TIMER1_IRQHandler() and le_DMA_IRQHandler() reach -- the latter
// through finish_conn_event(). The loop that services USB runs underneath
// them, so a drain that never saw the buffer empty left the board answering
// nothing. Like the overdue lock above, this is not what ends a live capture:
// a board the watchdog resets in here now says so, and the bench runs that
// reproduce the capture dying name the loop's USB service pass instead.
//
// The buffer holds eight frames, so a working SSP empties within a handful of
// reads even while the receiver is still clocking symbols in at one byte every
// eight microseconds. 64 is well past that; each read costs two register
// accesses across the peripheral bus, so running the whole bound against an
// SSP that has stopped answering is a few microseconds. Reaching it costs the
// stale symbols at the head of one packet, which the sync word match drops
// anyway.
#define LE_SSP_DRAIN_READS 64


//////////////////////
// global state

extern le_state_t le; // FIXME - refactor this struct
volatile uint16_t rf_channel;

// The access address the radio is synchronized on, which is what the record of
// a received packet carries.
//
// Held apart from conn.access_address because that one changes as soon as a
// connection request has been read, which happens in the mode's loop, while
// the radio goes on listening for the old sync word until the next hop writes
// the new one. A packet received in between was recorded under an access
// address it had not been received with: advertising packets from other
// devices turned up in the capture as the first packets of the connection
// that had just been opened, hundreds of microseconds before its transmit
// window had even opened.
static volatile uint32_t le_sync_aa = ADVERTISING_AA;
uint8_t le_dma_dest[2];

extern volatile uint8_t mode;
extern volatile uint8_t requested_mode;

// Shared LE state owned by bluetooth_rxtx.c. We only read le.crc_verify here,
// which the host toggles via UBERTOOTH_SET_CRC_VERIFY (ubertooth-btle -k1).
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

	// both in units of 100 ns. win_size has to hold the scaled value: the
	// wire carries it in units of 1.25 ms, so the 1.25 ms minimum is already
	// 12500 of these, which does not fit in a byte.
	uint32_t win_size;
	uint32_t win_offset;

	le_channel_remapping_t remapping;

	// The anchor point of the connection event that last closed, so the
	// next one is due one interval after it. It is always usable while a
	// connection is being followed: a connection request seeds it from the
	// transmit window it named, and every event that closes either measures
	// it or moves it on by one connection interval.
	//
	// anchor_error is how much later than the estimate the real anchor can
	// be, and zero once a received packet has measured it. The estimate is
	// always the earliest the anchor could be -- neither the transmit window
	// it starts from nor the central's packet that a reply answers can begin
	// after the anchor -- so an event opened on it is never opened late, and
	// is given anchor_error longer to run so that the packet it is waiting
	// for is still inside its deadline.
	uint32_t last_anchor;
	uint32_t anchor_error;
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
static void timer1_arm_conn_event(void);
static void timer1_wait_fs_lock(void);
static void timer1_recheck_fs_lock(void);
static void timer1_cancel_fs_lock(void);
static void le_ssp_drain(void);
static void le_dma_interrupt(uint32_t timestamp);
static void timer1_interrupt(void);
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

// Has a received packet measured the connection's anchor point? Nothing has
// to be allowed for in an event's deadline once one has.
static int anchor_measured(void) {
	return conn.anchor_error == 0;
}

// Has the connection reached the event a change was scheduled for?
//
// The counter is 16 bits wide and wraps, so the comparison is a signed
// difference, as the specification's own "instant" rule is: an instant is
// within 32767 events of where the connection is. At or past it, not equal to
// it, because events can go by unobserved -- timer1_arm_conn_event() skips
// whole intervals when the estimate has gone past, and the counter steps over
// an instant with them. An equality test then left the change pending for the
// rest of the connection, and the follow ran on parameters or a channel map
// the connection had left behind.
static int conn_event_reached(uint16_t instant) {
	return (int16_t)(conn.conn_event_counter - instant) >= 0;
}

// How long a connection event is given to produce its packets, counted from
// the moment the event opens.
//
// It has to stay open until the latest a packet of the event could start, and
// no longer: the first byte of a packet cancels this deadline and the
// inter-frame timeout takes over, so the packet's own length is not this
// deadline's business. The latest start is the peripheral's reply, one
// PERIPHERAL_REPLY_LATEST after the anchor, plus however much later than the
// estimate the anchor can be -- a whole transmit window under parameters no
// packet has been seen under, one reply's distance once the estimate comes
// from a single packet, and nothing once a packet has measured the anchor --
// and the receiver's warmup, because the event opens one before its anchor.
//
// Never long enough to run into the next event's own arming, which is one
// warmup before that event is due. At the shortest interval the specification
// allows, 7.5 ms, the longest legal window plus a packet runs about 8.7 ms, so
// an unclamped deadline would fall after the next event was due -- and
// finish_conn_event() would then count two elapsed intervals as one connection
// event. Every connection update and channel map instant is matched against
// that count, so from there on each instant would be matched an event early.
//
// The clamp costs the end of the window in one case: the first event under the
// shortest interval with the longest window the specification allows there.
// That event closes about 1.5 ms before its window does, so a packet starting
// in that last stretch is missed. Listening through the whole window and being
// armed for the next event cannot both be done at those parameters.
static uint32_t conn_event_deadline(void) {
	uint32_t deadline = PERIPHERAL_REPLY_LATEST + RX_WARMUP_TIME +
			conn.anchor_error;

	// Nothing to clamp against when no connection is being followed. That
	// takes a match armed for something which is not a connection event,
	// which the disarming in finish_conn_event() is there to prevent;
	// clamping to nothing would arm the next event at the moment it is read.
	if (conn.conn_interval > RX_WARMUP_TIME) {
		uint32_t limit = conn.conn_interval - RX_WARMUP_TIME;

		if (deadline > limit)
			deadline = limit;
	}

	return deadline;
}

// Arm the match that opens the next connection event, at the estimate the
// connection is being followed on.
//
// The estimate can already have gone past: an event may run to a deadline as
// long as its whole interval, and an interrupt handler can hold the processor
// across one. A match behind the counter is not reached until the counter
// wraps, 429 seconds later, which leaves the receiver parked on one data
// channel and the capture recording nothing, with nothing to recover it: the
// supervision timeout is only checked when an event closes. So the estimate
// skips whole intervals until it is far enough ahead, and the events skipped
// are counted -- that count is what every connection update and channel map
// instant is matched against.
//
// Far enough ahead means CONN_EVENT_ARM_MARGIN, not simply ahead: the counter
// runs while this function reads it, divides, and writes the match register,
// so a match set a tick or two in front of it would be passed before it was
// armed, and that is the same 429-second stall.
static void timer1_arm_conn_event(void) {
	uint32_t target;
	int32_t behind;

	// Only ever zero when no connection is being followed, which leaves
	// nothing to arm. Dividing by it below would trap.
	if (conn.conn_interval == 0)
		return;

	target = conn.last_anchor + conn.conn_interval - RX_WARMUP_TIME;

	// Signed, so this reads as "how far the counter is past the target"
	// however either of them has wrapped. The margin is added to it rather
	// than subtracted from the target so that a target already within the
	// margin counts as past.
	behind = (int32_t)(NOW - target) + (int32_t)CONN_EVENT_ARM_MARGIN;
	if (behind >= 0) {
		// Strictly more than `behind`, so the target lands more than the
		// margin ahead of the counter.
		uint32_t skipped = (uint32_t)behind / conn.conn_interval + 1;

		conn.last_anchor += skipped * conn.conn_interval;
		conn.conn_event_counter += skipped;
		target += skipped * conn.conn_interval;

		// The connection hops once per event, and change_channel() takes
		// one hop when it opens the event this match is for, so the hops
		// of the events gone by have to be taken here. Without them the
		// receiver would be tuned where the connection was `skipped`
		// events ago and hear nothing from it until the supervision
		// timeout ended the follow.
		conn.channel_idx = (conn.channel_idx +
				skipped * conn.hop_increment) % 37;
	}

	timer1_set_match(target);
}

// Arm a match that is not a connection event's: a moment the connection's
// parameters name, with no whole intervals to skip if it has gone past. The
// best that can be done then is to take it as soon as the counter allows,
// because a match already passed is not reached until the counter wraps 429
// seconds later.
static void timer1_set_match_ahead(uint32_t target) {
	if ((int32_t)(NOW - target) + (int32_t)CONN_EVENT_ARM_MARGIN >= 0)
		target = NOW + CONN_EVENT_ARM_MARGIN;

	timer1_set_match(target);
}

// finish a connection event
//
// 1) update the anchor point (see details below)
// 2) increment connection event counter
// 3) check if supervision timeout is exceeded
// 4) setup radio for next packet (data or adv if timeout exceeded)
//
// anchor update logic can be summarized thusly:
// 1) if we received two packets, the first of them is the central's, so the
//    connection anchor is measured: set it to the observed value
// 2) if we received one packet and the anchor was already measured, see if
//    it's within ANCHOR_EPSILON microseconds of where the anchor says it was
//    due. if so, it's the central's packet and the anchor moves to it
// 3) if we received one packet and the anchor was never measured, put the
//    estimate at the earliest the anchor could be, which is one
//    PERIPHERAL_REPLY_LATEST before that packet
// 4) if we received nothing, or one packet that an established anchor says
//    was the peripheral's, move the estimate on by one connection interval
static void finish_conn_event(void) {
	uint32_t last_anchor = 0;
	int last_anchor_set = 0;

	// two packets -- update anchor
	if (conn_event.num_packets == 2) {
		last_anchor = conn_event.anchor;
		last_anchor_set = 1;
	}

	// if there's one packet, we need to find out if it was the master
	else if (conn_event.num_packets == 1 && anchor_measured()) {
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
		conn.anchor_error = 0;
	}

	// One packet, with no measured anchor to put it against: it is either
	// the central's packet or the peripheral's answer to it, so the anchor
	// is somewhere in the PERIPHERAL_REPLY_LATEST before it. Take the
	// earliest it could be. Early is the safe direction -- an event armed
	// early is still open when its packet arrives, one armed late has
	// missed it -- and tracking the estimate from single packets is what
	// follows the peer's clock while no event has caught both packets.
	else if (!anchor_measured() && conn_event.num_packets > 0) {
		conn.last_anchor = conn_event.anchor - PERIPHERAL_REPLY_LATEST;
		conn.anchor_error = PERIPHERAL_REPLY_LATEST;
	}

	// Nothing to put the estimate on: either the event received nothing, or
	// its one packet was too far from an established anchor to be the
	// central's. Move the estimate on by one interval.
	//
	// This is the path an empty first connection event takes, and it used
	// to set the estimate to conn_event.anchor -- a field no packet had
	// written, so it held nothing, or the time of a packet from an earlier
	// connection. The next event was then armed at a match the counter had
	// long gone past, which is not reached until the counter wraps 429
	// seconds later: the receiver stayed parked on one data channel, and
	// the capture picked the connection up again only when its hopping came
	// back round to that channel. Measured against a bench peer, that lost
	// the first 4 to 26 connection events of most connections followed.
	else {
		conn.last_anchor += conn.conn_interval;
	}

	// update last packet for supervision timeout
	if (conn_event.num_packets > 0) {
		conn.last_packet_ts = NOW;
	}

	reset_conn_event();

	// increment connection event counter
	++conn.conn_event_counter;

	// supervision timeout reached - switch back to advertising
	//
	// Both paths that stop following disarm the match as well. Connection
	// events are the only thing it is armed for, and the one left in it has
	// just been taken, so it is behind the counter: left armed, it would be
	// taken again when the counter wrapped 429 seconds later, in the middle
	// of whatever the mode was doing by then -- retuning the radio under an
	// advertising capture, or under another connection.
	if (NOW - conn.last_packet_ts > conn.supervision_timeout) {
		reset_conn();
		timer1_clear_match();
		change_channel();
	}

	// FIXME - hack to cancel following a connection
	else if (cancel_follow) {
		cancel_follow = 0;
		reset_conn();
		timer1_clear_match();
		change_channel();
	}

	// supervision timeout not reached - hop to next channel
	else {
		timer1_arm_conn_event();
	}
}

// DMA handler
// called once per byte. handles all incoming data, but only minimally
// processes received data. at the end of a packet, it enqueues the
// received packet, fetches a new buffer, and restarts RX.
/*
 * The two interrupt handlers below say so while they run, so a board the
 * watchdog resets while one of them has the processor says which one.
 *
 * Without this the breadcrumb could not tell the two shapes of this wedge
 * apart. The loop in le_phy_main() runs beneath both handlers and is the only
 * thing servicing USB in this mode, so whenever a reset catches that loop it
 * records "servicing USB" -- whether the loop was held up inside the USB
 * service pass or simply never given the processor by a handler above it. That
 * cost a long investigation of the wrong half of the firmware.
 */
void le_DMA_IRQHandler(void) {
	/* The counter is read here, before anything else this function does,
	 * because the body dates a packet by it and the PHY accepts a measured
	 * connection anchor only within ANCHOR_EPSILON of its estimate. Marking
	 * the site first would put a call and two writes -- some tenths of a
	 * microsecond, in the firmware's hottest interrupt -- in front of the
	 * reading. */
	uint32_t timestamp = NOW;
	uint8_t site = watchdog_mark_inner(WATCHDOG_SITE_LE_DMA);

	le_dma_interrupt(timestamp);

	watchdog_unmark(site);
}

// Give up on the packet a failed capture transfer interrupted, and put the
// receiver back.
//
// A transfer error ends the capture of a packet part way along it, and on its
// own leaves nothing armed to carry on from. On a data channel the connection
// event's deadline was cleared when the packet's first byte arrived, and the
// timeout that waits for the next packet of the event is only armed where a
// packet ends, which this one never will. The synthesizer-lock and buffer
// waits were both disarmed when receive was armed. So no match is armed and no
// interrupt is due: the event never closes, the supervision timeout -- which is
// only checked when an event closes -- is never reached, and the capture is
// over, while the mode's loop goes on servicing USB so the board answers the
// host as if nothing had happened.
static void le_transfer_error_recover(void) {
	// Stop receiving and take the DMA channel and the SSP down, as the
	// end-of-packet path does: the receiver keeps clocking bytes into the FIFO
	// until SFSON takes effect, and they would otherwise be read as the head
	// of the next packet. SFSON rather than SRFOFF leaves the synthesizer
	// running, which is what the waits below expect.
	cc2400_strobe(SFSON);
	DMACC0Config = 0;
	DMACIntTCClear = (1 << 0);
	DIO_SSP_DMACR &= ~SSPDMACR_RXDMAE;
	le_ssp_drain();
	ICER0 = ICER0_ICE_DMA;

	// Drop what was received of the packet, so the next one does not start
	// part way along the buffer. The buffer is kept rather than released:
	// nothing was handed to the host, so the pool's accounting is unchanged.
	if (current_rxbuf != NULL)
		buffer_clear(current_rxbuf);

	if (btle_channel_index(rf_channel) < 37) {
		// Close the connection event. finish_conn_event() counts the event,
		// checks the supervision timeout and arms the next one, whose
		// change_channel() puts the DMA channel and the SSP back. The match
		// is a margin ahead of the counter, which is the shortest wait that
		// cannot be passed before it is armed.
		//
		// The event is open whenever receive is armed inside one, so the
		// match is taken as a timeout; were it somehow not, the handler opens
		// an event instead, which also re-arms receive.
		timer1_set_match(NOW + CONN_EVENT_ARM_MARGIN);
	} else {
		// An advertising channel has no event to close, and the engine stays
		// on it until the host or a connection request moves it. Waiting for
		// the synthesizer arms both the DMA channel and receive again when it
		// locks.
		//
		// A connection-event match can be armed here, for the first event of
		// a connection whose request the mode's loop has just read, so this
		// path leaves MR0 alone.
		timer1_wait_fs_lock();
	}
}

static void le_dma_interrupt(uint32_t timestamp) {
	unsigned pos;
	int8_t rssi;

	// Nothing to do once the PHY has been torn down: the radio is off, the
	// buffers are no longer owned by this module, and touching the CC2400 here
	// would collide with the main loop's own bit-banged SPI access.
	if (!le_phy_active) {
		dma_clear_interrupts(0);
		return;
	}

	// channel 0
	if (DMACIntStat & (1 << 0)) {
		int transfer_error = 0;

		// Acknowledge a transfer error before the byte below is dealt with,
		// not after: that work has paths which return, and a flag left set is
		// taken as a fresh error the moment the DMA interrupt is enabled
		// again. What to do about the error is decided at the end, once it is
		// known whether a packet arrived with it.
		if (DMACIntErrStat & (1 << 0)) {
			DMACIntErrClr = (1 << 0);
			transfer_error = 1;

			// Counted so that a capture losing packets to the hardware tells
			// itself apart from a quiet channel: ubertooth-util -w reports it.
			watchdog_count_transfer_error();
		}

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
				le_ssp_drain();
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
				// Back-date the stamp to the start of the preamble. This
				// interrupt reports the first header byte, so by now 8 bits
				// of preamble, the 32-bit access address and 8 bits of
				// header have gone past: 48 us at 1 symbol per microsecond.
				current_rxbuf->timestamp = timestamp - USEC(8 + 32 + 8);
				current_rxbuf->channel = rf_channel;
				current_rxbuf->access_address = le_sync_aa;

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
				le_ssp_drain();

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

				// The controller can report an error in the same interrupt
				// as the byte that completed a packet. That packet is on its
				// way to the host and receive has just been armed for the
				// next one, so the error has nothing left to recover: it has
				// been acknowledged and counted, and the recovery below
				// would undo the arming.
				return;
			}
		}

		// A transfer error with no packet to show for it: the half-received
		// packet is dropped and the receiver put back, because nothing else
		// is left armed to carry the capture on.
		if (transfer_error)
			le_transfer_error_recover();
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

	// From here the radio listens for this access address, so this is what
	// the packets it receives were received with.
	le_sync_aa = conn.access_address;

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
	le_ssp_drain();

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
			case BTLE_ADV_CHANNEL_37_MHZ: channel_idx = 37; break;
			case BTLE_ADV_CHANNEL_38_MHZ: channel_idx = 38; break;
			case BTLE_ADV_CHANNEL_39_MHZ: channel_idx = 39; break;
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

// Re-checks of FS_LOCK in the wait timer1 is running now: the short ones,
// which stop being counted once the lock is overdue so the count cannot wrap
// round into the short interval again, and the slow ones after that, which
// only pace the strobe.
//
// Written wherever a wait starts -- the mode's entry, the byte interrupt and
// the timer interrupt -- and from the timer interrupt as a wait runs. The two
// are reset together, which is not atomic, and the byte interrupt sits at the
// lowest interrupt priority so the timer interrupt can preempt it. Harmless
// as the code stands, because MR2 is disarmed on every path that starts a
// wait, so no re-check can fire part way through the reset.
static volatile unsigned fs_lock_rechecks;
static volatile unsigned fs_lock_slow_rechecks;

// Start a wait for the synthesizer to lock, from the beginning: the first
// re-checks get the short interval.
static void timer1_wait_fs_lock(void) {
	fs_lock_rechecks = 0;
	fs_lock_slow_rechecks = 0;
	timer1_recheck_fs_lock();
}

// Arm the next re-check of a wait already running. See FS_LOCK_RECHECK.
static void timer1_recheck_fs_lock(void) {
	if (fs_lock_rechecks < FS_LOCK_FAST_RECHECKS) {
		++fs_lock_rechecks;
		T1MR2 = NOW + FS_LOCK_RECHECK;
	} else {
		if (++fs_lock_slow_rechecks % FS_LOCK_SLOW_RESTROBE == 0)
			cc2400_strobe(SFSON);
		T1MR2 = NOW + FS_LOCK_RECHECK_SLOW;
	}
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

// Empty the SSP's receive buffer, with a bound. See LE_SSP_DRAIN_READS.
static void le_ssp_drain(void) {
	// Say where the board is while this runs, so one that the watchdog
	// resets in here says so instead of naming whatever the loop beneath
	// was doing. The site recorded before is put back on the way out.
	uint8_t site = watchdog_mark_inner(WATCHDOG_SITE_LE_SSP);
	unsigned i;

	for (i = 0; i < LE_SSP_DRAIN_READS; ++i) {
		uint8_t discarded;

		if (!(SSP1SR & SSPSR_RNE))
			break;
		discarded = (uint8_t)DIO_SSP_DR;
		(void)discarded;
	}

	watchdog_unmark(site);
}

void TIMER1_IRQHandler(void) {
	uint8_t site = watchdog_mark_inner(WATCHDOG_SITE_LE_TIMER);

	timer1_interrupt();

	watchdog_unmark(site);
}

static void timer1_interrupt(void) {
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
				conn_event_reached(conn.conn_update_instant)) {

			// on the first past through, handle the transmit window
			// offset. if there's no offset, skip down to else block
			if (!conn_event.opened && conn.win_offset > 0) {
				timer1_set_match_ahead(conn.last_anchor +
						conn.conn_interval + conn.win_offset +
						USEC(1250) - RX_WARMUP_TIME);
				conn_event.opened = 1;
			}

			// after the transmit window offset, or if there is no
			// transmit window, set a packet timeout and change the
			// channel
			else { // conn_event.opened || conn.win_offset == 0
				uint32_t window_opens;

				conn_event.opened = 1;

				// The first transmit window under the new parameters
				// opens 1.25 ms plus the window offset after the event
				// that was next due on the old interval, as a connection
				// request's first window does after the request. Taken
				// before the new interval is, so the step is the old one.
				window_opens = conn.last_anchor + conn.conn_interval +
						conn.win_offset + USEC(1250);

				// this is like a new connection, so set all values
				// accordingly
				conn.anchor_error = conn.win_size;
				conn.conn_interval = conn.conn_update_pending_interval;
				conn.supervision_timeout = conn.conn_update_pending_supervision_timeout;
				conn.conn_update_pending = 0;

				// The grid the events after this one are armed on, held
				// one interval before the window as le_connect_handler()
				// does and for the same reason: this event is open
				// already, so last_anchor has to describe the one before
				// it. Without moving it here at all, the estimate would go
				// on being advanced from the last event under the old
				// parameters, which this window is offset from.
				conn.last_anchor = window_opens - conn.conn_interval;

				// No packet has measured the anchor under the new
				// parameters, so conn_event_deadline() gives this event
				// the whole window, as it does the first event of a new
				// connection. Counted from now, which is when the event
				// opens, as the generic branch below does: counted from
				// the window instead, the event would run a warmup longer
				// than the deadline says and close exactly where the next
				// event has to be armed.
				timer1_set_match(NOW + conn_event_deadline());
				change_channel();
			}
			return;
		}

		// channel map update
		if (conn.channel_map_update_pending &&
				conn_event_reached(conn.channel_map_update_instant)) {
			conn.remapping = conn.pending_remapping;
			conn.channel_map_update_pending = 0;
		}

		// new connection event: set timeout and change channel
		if (!conn_event.opened) {
			conn_event.opened = 1;

			// Every event until one of them measures the anchor spans the
			// whole transmit window the connection named: the central's
			// packet may arrive anywhere inside it, and a deadline that
			// expires earlier closes the event before the packet arrives.
			// See conn_event_deadline().
			timer1_set_match(NOW + conn_event_deadline());
			change_channel();
		}

		// timeout: close connection event and set timer for next hop
		else {
			finish_conn_event();
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

		// if FS is not locked, look again shortly
		else {
			timer1_recheck_fs_lock();
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

/* Set once a packet has been dropped because the ring had no room for it, and
 * cleared again by the next packet that does get through, which carries
 * FIFO_OVERFLOW to the host. So the flag reaches the host on the packet after
 * the gap, which is the same way the Classic path reports one (enqueue() in
 * bluetooth_rxtx.c). Without it a dropped packet left no trace at all, and a
 * capture with a gap in it looked like a quiet channel.
 *
 * One flag rather than a count: the record has no field to put a number in,
 * and what the host needs to know is that packets are missing here. */
static volatile uint8_t le_packet_dropped;

// enqueue a packet for USB
static int usb_enqueue_le(le_rx_t *packet)
{
	/* The channel, timestamp and signal readings of this packet, for however
	 * many records it takes. */
	le_pkt_meta meta;
	int queued;

	meta.status = le_packet_dropped ? FIFO_OVERFLOW : 0;
	meta.channel = ubertooth_channel_byte(packet->channel);
	meta.clkn_high = 0;
	meta.clk100ns = packet->timestamp;
	/* The radio reports RSSI as a negative number, so the sum is negative and
	 * the division has to be a signed one. Without the cast the unsigned size
	 * would convert the sum to a huge positive value first. */
	meta.rssi_avg = packet->rssi_sum / (int) packet->size;
	meta.rssi_min = packet->rssi_min;
	meta.rssi_max = packet->rssi_max;
	meta.rssi_count = 0;

	/* The access address is what the board synchronized on, so it is held
	 * apart from the bytes that were received. */
	queued = usb_enqueue_le_packet(&meta,
	                               (const uint8_t *) &packet->access_address,
	                               packet->data, packet->size);
	/* A packet longer than 50 bytes takes several of the ring's records and
	 * is refused all or nothing, so a nearly full ring drops long packets
	 * while short ones still get through. That is exactly the case the host
	 * has to hear about. */
	le_packet_dropped = queued ? 0 : 1;
	return queued;
}

static uint32_t extract_field(le_rx_t *buf, size_t offset, unsigned size) {
	unsigned i;
	uint32_t ret = 0;

	/* The cast is what keeps the top byte of a four-byte field in range:
	 * without it each byte is promoted to int, and shifting 0x80 or more
	 * left by 24 runs past the largest int there is. */
	for (i = 0; i < size; ++i)
		ret |= (uint32_t)buf->data[offset + i] << (i*8);

	return ret;
}

// The timing parameters of a connection, in the units they arrive in: the
// transmit window and the connection interval in steps of 1.25 ms, the
// supervision timeout in steps of 10 ms. A connection request and a
// connection update carry the same four, at different offsets.
typedef struct _le_conn_timing_t {
	uint32_t win_size;
	uint32_t win_offset;
	uint32_t conn_interval;
	uint32_t supervision_timeout;
} le_conn_timing_t;

// Check a connection's timing parameters against the ranges the
// specification allows, and scale them to the units of 100 ns the rest of
// this file counts in.
//
// Returns 1 with *t scaled, or 0 when one of them is out of range -- in which
// case *t is part-scaled and the caller must not use it. A caller that gets 0
// drops the connection instead of following it. The values are what every
// later deadline is built from, so one outside its range leaves the capture
// chasing a connection that does not exist: a transmit window of 255 steps
// puts 319 ms into the timeout that waits out the first connection event, and
// a supervision timeout of hours is then what ends the chase.
static int le_scale_conn_timing(le_conn_timing_t *t) {
	uint32_t max_win_size;

	// the connection interval is in range [7.5 ms, 4 s]
	if (t->conn_interval < 6 || t->conn_interval > 3200)
		return 0;
	t->conn_interval *= USEC(1250);

	// window offset is in range [0, conn_interval]
	t->win_offset *= USEC(1250);
	if (t->win_offset > t->conn_interval)
		return 0;

	// win size is in range [1.25 ms, MIN(10 ms, conn_interval - 1.25 ms)]
	t->win_size *= USEC(1250);
	max_win_size = t->conn_interval - USEC(1250);
	if (max_win_size > MSEC(10))
		max_win_size = MSEC(10);
	if (t->win_size < USEC(1250) || t->win_size > max_win_size)
		return 0;

	// The connSupervisionTimeout shall be a multiple of 10 ms in the
	// range of 100 ms to 32.0 s and it shall be larger than (1 +
	// connSlaveLatency) * connInterval * 2
	//
	// Checked before it is scaled, because 10 ms in units of 100 ns is
	// 100000 and the field is two bytes wide: the largest values overflow a
	// 32-bit count, and a wrapped one lands back inside the range.
	if (t->supervision_timeout < 10 || t->supervision_timeout > 3200)
		return 0;
	t->supervision_timeout *= MSEC(10);
	// TODO handle slave latency

	return 1;
}

static void le_connect_handler(le_rx_t *buf) {
	uint32_t aa, crc_init;
	le_conn_timing_t timing;

	if (!le.do_follow)
		return;

	if (buf->size != 2 + 6 + 6 + 22 + 3)
		return;

	// FIXME ugly hack
	if (cancel_follow)
		cancel_follow = 0;

	// Every connection starts from nothing, so that none of it is carried
	// over from the last one this mode followed. A run stopped part way
	// through a connection leaves that state behind -- the mode's teardown
	// does not clear it -- and the next connection would otherwise be
	// followed with the previous one's event counter, so every instant in
	// it is matched thousands of events out, and with its hop position, its
	// channel map and any change still waiting for an instant.
	//
	// The counter in particular has to start at zero: the specification
	// counts a connection's events from there, and that is what the
	// instants a connection names are counted in.
	reset_conn();

	conn.access_address     = extract_field(buf, 14, 4);
	conn.crc_init           = extract_field(buf, 18, 3);
	// CRCInit is a 24-bit field. rbit() reverses all 32 bits of the word, so
	// its result sits a byte too high: 0x123456 has to become 0x6a2c48, and
	// rbit() alone gives 0x6a2c4800, which btle_crcgen_lut() masks down to
	// 0x2c4800. Without the shift every data packet of a followed connection
	// is checked against a CRC from the wrong starting value, so
	// `ubertooth-btle -f -k1` drops all of them.
	conn.crc_init_reversed  = rbit(conn.crc_init) >> 8;
	conn.hop_increment      = extract_field(buf, 35, 1) & 0x1f;

	timing.win_size            = extract_field(buf, 21, 1);
	timing.win_offset          = extract_field(buf, 22, 2);
	timing.conn_interval       = extract_field(buf, 24, 2);
	timing.supervision_timeout = extract_field(buf, 28, 2);
	if (!le_scale_conn_timing(&timing))
		goto err_out;

	conn.win_size            = timing.win_size;
	conn.win_offset          = timing.win_offset;
	conn.conn_interval       = timing.conn_interval;
	conn.supervision_timeout = timing.supervision_timeout;

	le_parse_channel_map(&buf->data[30], &conn.remapping);
	if (conn.remapping.total_channels == 0)
		goto err_out;

	// cancel RX on advertising channel
	timer1_cancel_fs_lock();

	// Where the connection's first transmit window opens: 1.25 ms plus the
	// window offset after the end of this connection request. The central's
	// first packet arrives somewhere inside that window, so this is the
	// earliest the connection's anchor can be, and the engine counts
	// intervals from here until a packet measures it.
	//
	// Held one interval earlier than that, because last_anchor is the
	// anchor of the event that last closed and the next event is due one
	// interval after it. The first event has no event before it, so this
	// stands in for one.
	conn.last_anchor = buf->timestamp + PACKET_DURATION(buf) +
			conn.win_offset + USEC(1250) - conn.conn_interval;
	conn.anchor_error = conn.win_size;

	// The connection has delivered nothing yet, so its supervision timeout
	// runs from the request that opened it. Without this, an empty first
	// connection event would compare the clock against whatever an earlier
	// connection left behind.
	conn.last_packet_ts = buf->timestamp;

	reset_conn_event();

	// Through the same arming as every later event, so that a window which
	// has already opened -- this handler runs in the mode's loop, under
	// everything else the board is doing -- moves on to the next event
	// instead of arming a match the counter has gone past.
	timer1_arm_conn_event();
	return;

	// error condition: reset conn and return
err_out:
	reset_conn();
}

static void connection_update_handler(le_rx_t *buf) {
	le_conn_timing_t timing;

	timing.win_size            = extract_field(buf, 3, 1);
	timing.win_offset          = extract_field(buf, 4, 2);
	timing.conn_interval       = extract_field(buf, 6, 2);
	timing.supervision_timeout = extract_field(buf, 10, 2);

	// An update the specification does not allow is refused, as
	// le_connect_handler() refuses the same fields in a CONNECT_IND. Taken
	// on trust, a transmit window of 255 steps would put 319 ms into the
	// wait for the first event under the new parameters, and the
	// supervision timeout it came with would be all that ended the chase.
	//
	// Nothing conformant sends such a value, so a packet asking for one has
	// bit errors (CRC verification is off by default), and this handler is
	// reached by any data packet whose first bytes happen to look like a
	// connection update. Keep following the connection on the parameters it
	// already has, which are in range and end it at its real supervision
	// timeout: ending the follow here would let one bit flip stop a healthy
	// capture. This is what channel_map_update_handler() does with an empty
	// channel map, for the same reason.
	if (!le_scale_conn_timing(&timing))
		return;

	conn.win_size            = timing.win_size;
	conn.win_offset          = timing.win_offset;
	conn.conn_update_pending_interval = timing.conn_interval;
	conn.conn_update_pending_supervision_timeout = timing.supervision_timeout;
	conn.conn_update_instant = extract_field(buf, 12, 2);

	conn.conn_update_pending = 1;
}

// Is every channel in a 37-bit channel map clear? The five bytes carry
// channels 0 to 36, so the top three bits of the last one are reserved and
// le_parse_channel_map() ignores them; this reads the same bits it does.
static int channel_map_is_empty(const uint8_t *map) {
	return (map[0] | map[1] | map[2] | map[3] | (map[4] & 0x1f)) == 0;
}

static void channel_map_update_handler(le_rx_t *buf) {
	// A map with no channels in it is refused, as le_connect_handler() refuses
	// the same field in a CONNECT_IND. The remapping divides by the number of
	// channels in use, and the processor's divide-by-zero trap is off at its
	// reset value: the division would return 0 rather than fault, so from the
	// update's instant on every connection event would tune data channel 0 and
	// the follow would capture nothing until the supervision timeout.
	//
	// Nothing conformant sends an empty map, so a packet asking for one has
	// bit errors (CRC verification is off by default). Keep following the
	// connection on the map it already has.
	//
	// Checked on the packet's own bytes rather than on a parsed copy, because
	// a second le_channel_remapping_t is nearly 200 bytes and this engine's
	// stack is under a kilobyte. Returning here also leaves an update that is
	// already waiting for its instant untouched.
	if (channel_map_is_empty(&buf->data[3]))
		return;

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

// What a capture's size counts: the two-byte PDU header, the payload, and the
// three CRC bytes the radio hands over behind the payload. A length check on
// size therefore has to include the CRC, or it accepts a payload three bytes
// shorter than it meant to and the comparison below reads CRC bytes as the
// tail of an address.
#define LE_HEADER_LEN 2
#define LE_ADDR_LEN   6
#define LE_CRC_LEN    3

// The shortest capture that carries the addresses the filter compares.
#define LE_ONE_ADDR_SIZE (LE_HEADER_LEN + LE_ADDR_LEN + LE_CRC_LEN)
#define LE_TWO_ADDR_SIZE (LE_HEADER_LEN + 2 * LE_ADDR_LEN + LE_CRC_LEN)

// compare a BD addr against target with mask
static int bd_addr_cmp(uint8_t *bd_addr) {
	unsigned i;
	for (i = 0; i < LE_ADDR_LEN; ++i)
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
			if (buf->size < LE_ONE_ADDR_SIZE)
				return 0;
			return bd_addr_cmp(&buf->data[LE_HEADER_LEN]);
			break;

		// ADV_DIRECT_IND, SCAN_REQ, CONNECT_REQ
		case 0x01:
		case 0x03:
		case 0x05:
			if (buf->size < LE_TWO_ADDR_SIZE)
				return 0;
			return bd_addr_cmp(&buf->data[LE_HEADER_LEN]) ||
				   bd_addr_cmp(&buf->data[LE_HEADER_LEN + LE_ADDR_LEN]);
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
	// A drop left over from an earlier run must not mark this run's first
	// packet as following a gap.
	le_packet_dropped = 0;

	// The counter runs from here because NOW, and the FS_LOCK match
	// le_cc2400_init_rf() arms through it, are measured against it. Its
	// interrupt stays masked until the setup below is done.
	timer1_start();

	current_rxbuf = buffer_get();
	rf_channel = le_adv_channel; // FIXME
	// Not just the access address: the whole connection state, so a run
	// stopped part way through a connection leaves nothing behind for this
	// one to follow with.
	reset_conn();
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
			// host's UBERTOOTH_SET_CRC_VERIFY (ubertooth-btle -k1) had no
			// effect on advertising/follow captures and bit-flipped
			// "phantom" advertisers leaked to the host. Advertising-channel
			// PDUs use the fixed CRCInit (reversed: 0xAAAAAA); data-channel
			// PDUs use the value recovered from CONNECT_IND. Disabled by
			// default, so the legacy behaviour is unchanged unless -k1 is set.
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
