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
#include "ubertooth_rssi.h"
#include "ubertooth_usb.h"
#include "bluetooth_le.h"
#include "cc2400_wait.h"
#include "queue.h"
#include "watchdog.h"
#include "irq.h"

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

// How long an LED stays on for one packet handed to the host. See BLINK_NOW.
#define LE_BLINK_LENGTH MSEC(10)

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

// The interference mode the host asked for, JAM_NONE unless `ubertooth-btle`
// was given -j (JAM_ONCE) or -J (JAM_CONTINUOUS). Owned by bluetooth_rxtx.c,
// read here to decide whether to transmit on a followed connection's events.
extern volatile uint8_t jam_mode;

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
	rssi_power_t power;         // the packet's own power, averaged in power
	int transfer_error;         // 1 if a capture transfer faulted during it

	// Which connection event this packet started in, noted as its first
	// byte arrives, so the bookkeeping done when it ends goes to that event
	// and not to whichever one is open by then. in_open_event is 0 on an
	// advertising channel and whenever no event was open.
	int in_open_event;
	uint16_t event_counter;
} le_rx_t;

// pool of all buffers, in the other RAM bank to leave main SRAM to the stack
static AHB_SRAM le_rx_t le_buffer_pool[LE_BUFFER_POOL_SIZE];

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

	// Set when this connection hops with channel selection algorithm 2,
	// which is what both ends ask for in the ChSel bit of the advertisement
	// and of the connection request. The hop increment then means nothing to
	// it; see le_csa2_channel().
	uint8_t  chan_sel_2;
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

	// Set when a packet announcing the end of the connection has been read,
	// so the next connection event to close stops following it. It belongs
	// to the connection rather than being a flag of its own: reset_conn()
	// then clears it, and a packet read just after a follow ended cannot
	// reach into the follow that comes next.
	//
	// Written from the mode's loop without holding interrupts off, where
	// the two updates above need a guard, because this is one word on its
	// own: there is no second field the interrupt could read out of step
	// with it. A write that lands after the interrupt has reset the
	// connection leaves the note on a connection that is no longer
	// followed, which is harmless for the same reason.
	int      terminated;
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

// Should this connection event be jammed rather than received?
//
// Jamming interferes with a connection the capture is already following, so it
// transmits only once a CONNECT_IND has been read and conn.access_address is
// the connection's own, not the advertising value every capture starts on.
// While listening on the advertising channels there is nothing to jam, so an
// advertising event is always received. The host asks for this with
// `ubertooth-btle -j`/`-J`, which sets jam_mode; nothing sets it on firmware
// built without transmit support, so this is always false there.
static int le_jam_event(void) {
	return jam_mode != JAM_NONE && conn.access_address != ADVERTISING_AA;
}


//////////////////////
// code

// pre-declarations for utility stuff
static void timer1_start(void);
static void timer1_enable_interrupt(void);
static void timer1_stop(void);
static void timer1_arm_match(volatile uint32_t *match, uint32_t when, uint32_t bit);
static void timer1_arm_match_in(volatile uint32_t *match, uint32_t delta, uint32_t bit);
static void timer1_disarm_match_interrupt(uint32_t bit);
static void timer1_set_match(uint32_t match);
static void timer1_set_match_in(uint32_t delta);
static void timer1_set_match_in_if_unarmed(uint32_t delta);
static void timer1_clear_match(void);
static void note_the_event_and_drop_its_deadline(le_rx_t *buf);
static void timer1_arm_conn_event(void);
static void timer1_wait_fs_lock(void);
static void timer1_recheck_fs_lock(void);
static void timer1_cancel_fs_lock(void);
static void le_ssp_drain(void);
static void le_dma_interrupt(uint32_t timestamp);
static void timer1_interrupt(void);
static void timer1_wait_buffer(void);
static void timer1_cancel_wait_buffer(void);
static void blink_timer_start(void);
static void blink_timer_stop(void);
static void blink(int tx, int rx, int usr);
static void blink_expire(void);
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
	rssi_power_reset(&buf->power);
	buf->transfer_error = 0;
	buf->in_open_event = 0;
	buf->event_counter = 0;
}

// get a packet buffer
// returns a pointer to a buffer if available
// returns NULL otherwise
//
// Three contexts take buffers from this pool: the main loop as the mode
// starts, the byte interrupt at the end of every packet, and the timer
// interrupt on both the buffer wait and the channel change. Finding a free
// buffer and marking it taken has to be one step for all three. Two steps --
// read `available`, then clear it -- let an interrupt that lands between them
// find the same buffer free and take it as well, and the two contexts then
// receive into one buffer, or hand it to the host twice.
//
// Only the test and the clear of the flag are guarded. Clearing the buffer's
// contents is a 260-byte memset, and by then the buffer belongs to this caller
// and no other context can reach it, so the pool adds no mask of its own
// around it. A caller that needs the clear covered as well takes its own guard
// around the call, and the guards nest -- which is what the end-of-packet
// handover does, so on that one path the memset does run masked. The span is
// sized there.
static le_rx_t *buffer_get(void) {
	int i;

	for (i = 0; i < LE_BUFFER_POOL_SIZE; ++i) {
		uint32_t primask = irq_save_disable();
		int taken = le_buffer_pool[i].available;

		if (taken)
			le_buffer_pool[i].available = 0;
		irq_restore(primask);

		if (taken) {
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
	uint32_t primask;
	int32_t behind;

	// Only ever zero when no connection is being followed, which leaves
	// nothing to arm. Dividing by it below would trap.
	if (conn.conn_interval == 0)
		return;

	target = conn.last_anchor + conn.conn_interval - RX_WARMUP_TIME;

	// From here to the arming the counter must not move under a handler: the
	// reading below is what decides whether the target has gone by, and the
	// margin it is compared against is 10 microseconds, which an interrupt
	// can outlast. The events skipped are counted here too, so a handler
	// cannot see the grid half advanced.
	primask = irq_save_disable();

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
		//
		// A connection hopping with algorithm 2 never reads this, since
		// that algorithm is asked for the channel of the event it is
		// opening; the arithmetic is left unconditional because a branch
		// here would cost as much as it saves.
		conn.channel_idx = (conn.channel_idx +
				skipped * conn.hop_increment) % 37;
	}

	timer1_set_match(target);
	irq_restore(primask);
}

// Arm a match that is not a connection event's: a moment the connection's
// parameters name, with no whole intervals to skip if it has gone past. The
// best that can be done then is to take it as soon as the counter allows,
// because a match already passed is not reached until the counter wraps 429
// seconds later.
static void timer1_set_match_ahead(uint32_t target) {
	// One guard over the reading, the decision and the arming, for the
	// reason timer1_arm_conn_event() gives: the margin is 10 microseconds
	// and an interrupt taken in between can outlast it, which would arm the
	// match behind the counter -- the stall this function exists to avoid.
	uint32_t primask = irq_save_disable();

	if ((int32_t)(NOW - target) + (int32_t)CONN_EVENT_ARM_MARGIN >= 0)
		target = NOW + CONN_EVENT_ARM_MARGIN;

	timer1_set_match(target);
	irq_restore(primask);
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

	// if there's one packet, we need to find out if it was the Central
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

	// The connection is over, either because it has gone quiet for longer
	// than its supervision timeout or because the peers announced the end
	// of it on the air - switch back to advertising
	//
	// Every path that stops following disarms the match as well. Connection
	// events are the only thing it is armed for, and the one left in it has
	// just been taken, so it is behind the counter: left armed, it would be
	// taken again when the counter wrapped 429 seconds later, in the middle
	// of whatever the mode was doing by then -- retuning the radio under an
	// advertising capture, or under another connection.
	if (NOW - conn.last_packet_ts > conn.supervision_timeout ||
	    conn.terminated) {
		reset_conn();
		timer1_clear_match();

		// A single-shot jam (ubertooth-btle -j) interferes with one
		// connection and then returns to idle. Jamming transmits in place
		// of receiving, so no packet has refreshed last_packet_ts and this
		// is where that connection ends: drop the jam mode and ask for
		// idle. A continuous jam (-J) and a plain follow go back to the
		// advertising channels below for the next connection.
		if (jam_mode == JAM_ONCE) {
			jam_mode = JAM_NONE;
			requested_mode = MODE_IDLE;
			return;
		}

		change_channel();
	}

	// FIXME - hack to cancel following a connection
	else if (cancel_follow) {
		cancel_follow = 0;
		reset_conn();
		timer1_clear_match();
		change_channel();
	}

	// the connection is still running - hop to next channel
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
	// of the next packet. SFSON rather than SRFOFF stops receiving without
	// stopping the synthesizer, which is what a wait for its lock needs; the
	// path through a connection event instead reaches change_channel(), which
	// strobes SRFOFF itself.
	//
	// The terminal count is cleared here as the end-of-packet path clears it
	// after taking the channel down, for the same reason: a count raised by a
	// byte the receiver clocked out while SFSON took effect would otherwise be
	// waiting, and the next transfer would start on it.
	cc2400_strobe(SFSON);
	DMACC0Config = 0;
	DMACIntTCClear = (1 << 0);
	dio_ssp_rx_dma_disable();
	le_ssp_drain();
	ICER0 = ICER0_ICE_DMA;

	// Drop what was received of the packet, so the next one does not start
	// part way along the buffer. The buffer is kept rather than released:
	// nothing was handed to the host, so the pool's accounting is unchanged.
	if (current_rxbuf != NULL)
		buffer_clear(current_rxbuf);

	// Inside a connection event, close the event: a match a margin ahead of
	// the counter is the shortest wait that cannot be passed before it is
	// armed. The timer interrupt takes it and carries the state machine on --
	// finish_conn_event() counts the event, checks the supervision timeout and
	// arms the next one, or, if an update's instant has arrived by then, the
	// handler applies the update -- and either way the event that follows
	// reaches change_channel(), which puts the DMA channel and the SSP back.
	//
	// A match already armed is left alone, for two reasons. The error can
	// arrive before the packet's first byte, which is what clears the event's
	// deadline, and that deadline is when the event is meant to close. And the
	// timer interrupt preempts this one, so a match can be taken while this
	// function runs: finish_conn_event() has armed the next event by then, and
	// arming over it would open that event an interval early and lose its
	// packets.
	if (btle_channel_index(rf_channel) < 37 && conn.conn_interval != 0) {
		timer1_set_match_in_if_unarmed(CONN_EVENT_ARM_MARGIN);
		return;
	}

	// Everything else waits for the synthesizer, which arms the DMA channel,
	// the DMA interrupt and receive again when it locks. That is an
	// advertising channel, where there is no event to close and the engine
	// stays until the host or a connection request moves it; and a data
	// channel with no connection left to follow, which is how the state
	// machine is found if the connection ended at its supervision timeout
	// while this function ran. Arming a connection event for that one would
	// open an event with no connection to open it for, and arming nothing at
	// all would leave exactly the silence this function is here to prevent.
	//
	// A connection-event match can be armed on an advertising channel, for the
	// first event of a connection whose request the mode's loop has just read,
	// so this path leaves MR0 alone.
	timer1_wait_fs_lock();
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
		//
		// The mark the host reads goes on the receive buffer as the byte is
		// taken, below, for the same reason: a packet this interrupt
		// completes is handed over in there. An error reported with no byte
		// alongside it marks nothing, because the packet it interrupted will
		// never be finished -- the recovery drops what was received and takes
		// a fresh buffer.
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
				dio_ssp_rx_dma_disable();
				le_ssp_drain();
				ICER0 = ICER0_ICE_DMA;

				// A transfer error reported in this same interrupt needs no
				// recovery either: the buffer wait just armed is what carries
				// on from here, and there was no packet to drop.
				timer1_wait_buffer();
				return;
			}

			// A transfer error reported in this interrupt belongs to the
			// packet being received, and is marked on its buffer. A packet
			// the same interrupt completes is kept -- it is whole, and the
			// host asked for it -- but it went through a controller that
			// reported a fault, so some of its bytes may be wrong while its
			// length is right. The mark lets the host tell it apart from a
			// clean one; CRC checking is off by default, so nothing else
			// would. The capture loops in bluetooth_rxtx.c mark their own
			// records on the same condition.
			//
			// A fault that does not complete a packet takes the mark away
			// with the packet, because the recovery clears the buffer it
			// keeps. So nothing carries a fault to a packet it did not
			// happen to.
			current_rxbuf->transfer_error |= transfer_error;

			// poll RSSI
			rssi = (int8_t)(cc2400_get(RSSI) >> 8);
			rssi_power_add(&current_rxbuf->power, rssi);
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

				// data packet received: cancel the event's deadline,
				// and remember the event. A new timeout or the hop
				// timer is armed at the end of the packet.
				if (btle_channel_index(rf_channel) < 37) {
					note_the_event_and_drop_its_deadline(
							current_rxbuf);
				}
			}

			// get length from header
			if (pos == 2) {
				uint8_t length = dewhiten_length(current_rxbuf->channel, current_rxbuf->data[1]);
				current_rxbuf->size = length + 2 + 3; // two bytes for header and three for CRC
			}

			// finished packet - state transition
			if (pos > 2 && pos >= current_rxbuf->size) {
				le_rx_t *packet;
				uint32_t packet_timestamp = current_rxbuf->timestamp;
				int in_open_event = current_rxbuf->in_open_event;
				uint16_t event_counter = current_rxbuf->event_counter;
				uint32_t primask;

				// stop the CC2400 before flushing SSP
				cc2400_strobe(SFSON);

				// stop DMA on this channel and flush SSP
				DMACC0Config = 0;
				DMACIntTCClear = (1 << 0); // if we don't clear a second time, data is corrupt

				dio_ssp_rx_dma_disable();
				le_ssp_drain();

				// XXX disable DMA interrupt as a workaround
				ICER0 = ICER0_ICE_DMA;

				// Hand the finished packet to the host and take the
				// buffer for the next one, in one step with interrupts
				// held off.
				//
				// current_rxbuf is where every context puts the next
				// packet, and the timer interrupt acts on it: the channel
				// change clears the buffer it points at, and the
				// synthesizer-lock and buffer waits arm receive into it.
				// So between handing this buffer over and pointing
				// current_rxbuf at a fresh one, the pointer names a buffer
				// that is on its way to the host. A timer match taken in
				// that window cleared it or received into it, and the host
				// got a record of zeroes, or one holding bytes from a
				// later moment in the air.
				//
				// The new buffer is taken first so that the pointer is
				// never left naming a queued buffer even for an
				// instruction, and the guard covers the handover so that
				// no match can be taken part way through it.
				//
				// Taking a buffer clears its 260 bytes, so the span is
				// worth a word. Measured from the built image it is 250
				// to 280 cycles, about 2.6 microseconds at 100 MHz: the
				// four-entry pool scan, that memset and the queue insert.
				// This handler reads RSSI over the bit-banged radio bus
				// for every byte of the packet, a 24-bit transfer that
				// masks interrupts for about 4.6 microseconds every 8
				// microseconds of air, so one clear per packet is less
				// than what this path already holds off for a single
				// byte. It also lands after the counter reading this
				// packet is dated by, so no measurement moves; what waits
				// for it is the next match, and the first one armed below
				// re-checks every 3 microseconds until the synthesizer
				// locks.
				//
				// Taking the buffer outside the guard instead would not
				// help: the pointer would still name the finished packet
				// while the new one was cleared, which is the same window
				// a few instructions narrower.
				//
				// The new buffer may be NULL if the pool is exhausted;
				// the waits below and the change_channel() and timer1
				// paths check for that and wait for a buffer rather than
				// arm receive without one.
				//
				// The insert cannot fail and so has no error
				// path: the queue holds FIFOSIZE-1 pointers,
				// nine, and the whole pool is four buffers.
				primask = irq_save_disable();
				packet = current_rxbuf;
				current_rxbuf = buffer_get();
				queue_insert(&packet_queue, packet);
				irq_restore(primask);

				// Count this packet into the connection event it
				// arrived in -- noted on its buffer as its first byte
				// arrived -- and only while the connection is still in
				// that event. See
				// note_the_event_and_drop_its_deadline() for what
				// counting it into the wrong one cost.
				//
				// A packet the connection has moved past is still sent
				// to the host, unless the hop that opened the next event
				// cleared the buffer it was arriving into, which is a
				// window this guard does not close. What is dropped is
				// bookkeeping with nowhere to go, including the refresh
				// of conn.last_packet_ts: the supervision timeout is 100
				// ms at the shortest and events are milliseconds apart,
				// so one event's worth makes no difference to it.
				//
				// Closing the event on the second packet stays outside
				// the guard, because it reaches change_channel() and the
				// radio. Nothing can close the event in between: its
				// deadline was dropped as this packet's first byte
				// arrived, and the count below is only reached while the
				// connection is still in it.
				if (in_open_event && btle_channel_index(rf_channel) < 37) {
					unsigned num_packets = 0;

					primask = irq_save_disable();
					if (conn.conn_event_counter == event_counter) {
						num_packets = ++conn_event.num_packets;

						// first packet: set connection anchor
						if (num_packets == 1) {
							// The stamp was read before the packet
							// was handed over: once it is queued the
							// host owns it, and the buffer can be
							// released and handed out again.
							conn_event.anchor = packet_timestamp;
							timer1_set_match_in(IFS_TIMEOUT); // set a timeout for next packet
						}
					}
					irq_restore(primask);

					// second packet: close connection event, and set hop timer
					if (num_packets == 2) {
						cc2400_strobe(SRFOFF);
						finish_conn_event();
						return;
					}
				}

				if (current_rxbuf == NULL) {
					// if all buffers are in use, wait for one to free up in background
					timer1_wait_buffer();
				} else {
					// wait for FS_LOCK in background
					timer1_wait_fs_lock();
				}

				// The controller can report an error in the same interrupt as
				// the byte that completed a packet. That packet is whole and
				// on its way to the host, and receive is armed for the next
				// one, so there is nothing for the recovery below to do: the
				// error has been acknowledged and counted already. Returning
				// says which of the two happened rather than leaving it to
				// the recovery to work out that it has nothing to put back.
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

	// Jamming a connection event transmits instead of receiving: a
	// pseudo-random carrier on the event's channel, with no sync word and
	// no CRC, so neither peer's packet decodes. Same radio setup as le_jam()
	// in bluetooth_rxtx.c -- the PRNG drives the modem (MDMTST0) and GRMDM
	// turns sync-word detection and the CRC off -- but it hands over to the
	// engine's own non-blocking FS_LOCK wait rather than spinning, so the
	// mode's loop keeps servicing USB. The strobe that starts the carrier is
	// in the FS_LOCK handler, as the receive strobe is. FSDIV is the channel
	// itself here, with no 1 MHz offset, because that offset is for the
	// receive IF.
	if (le_jam_event()) {
		cc2400_set(MANAND,  0x7fff);
		cc2400_set(LMTST,   0x2b22);
		cc2400_set(MDMTST0, 0x234b); // PRNG on, 1 MHz offset
		cc2400_set(GRMDM,   0x0c01); // buffered, packet mode, CRC off
		cc2400_set(FREND,   frend_keep_pa_level(cc2400_get(FREND)));
		cc2400_set(FSDIV,   rf_channel);
		cc2400_set(MDMCTRL, 0x0040); // 250 kHz frequency deviation
		cc2400_wait_xosc_stable(NULL);
		cc2400_strobe(SFSON);
		timer1_wait_fs_lock();
		return;
	}

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
	// Clear the transmit LED: a continuous jam (-J) lights it while jamming a
	// connection and comes back here to listen for the next one, so this is
	// where it goes off again.
	TXLED_CLR;
	cc2400_strobe(SRX);
#ifdef UBERTOOTH_ONE
	PAEN_SET;
	HGM_SET;
#endif
}

// strobe TX and enable PA, to jam the connection event the radio is tuned to.
// No DMA or receive buffer: the carrier runs until the next hop's SRFOFF.
// Only PAEN, not HGM: HGM is the receive path's gain control and means nothing
// while transmitting, which is why le_jam() leaves it alone too.
static void le_cc2400_strobe_tx(void) {
	cc2400_strobe(STX);
#ifdef UBERTOOTH_ONE
	PAEN_SET;
#endif
	TXLED_SET;
}

// change channel and init rx
static void change_channel(void) {
	uint8_t channel_idx = 0;

	cc2400_strobe(SRFOFF);

	// stop DMA and flush SSP
	dio_ssp_rx_dma_disable();
	le_ssp_drain();

	// current_rxbuf can be NULL when the buffer pool was exhausted at the end
	// of a connection event. buffer_clear(NULL) faulted the CPU, so take a
	// buffer if one has since been freed; without one, do not re-arm DMA or
	// the SSP either. le_cc2400_init_rf() below hands over to the timer1
	// FS_LOCK path, which waits for a buffer before arming them.
	//
	// A jammed event transmits instead of receiving, so it arms neither: the
	// FS_LOCK path strobes TX directly. Any buffer already held is kept for
	// the next received event.
	if (!le_jam_event()) {
		if (current_rxbuf == NULL)
			current_rxbuf = buffer_get();
		if (current_rxbuf != NULL) {
			buffer_clear(current_rxbuf);
			le_dma_init();
			dio_ssp_start();
		}
	}

	if (conn.access_address == ADVERTISING_AA) {
		// FIXME
		switch (le_adv_channel) {
			case BTLE_ADV_CHANNEL_37_MHZ: channel_idx = 37; break;
			case BTLE_ADV_CHANNEL_38_MHZ: channel_idx = 38; break;
			case BTLE_ADV_CHANNEL_39_MHZ: channel_idx = 39; break;
			default:   channel_idx = 37; break;
		}
	} else if (conn.chan_sel_2) {
		// This algorithm's channel comes from the event counter rather
		// than from the channel of the event before, so conn.channel_idx
		// is not kept for such a connection at all.
		channel_idx = le_csa2_channel(conn.conn_event_counter,
				conn.access_address, &conn.remapping);
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

// Arm one of timer1's matches -- the moment and the interrupt that reports it
// together -- or disarm one.
//
// T1MCR holds the enable bit of all three matches this engine uses: the
// connection event (MR0), the synthesizer-lock wait (MR2), and the buffer
// wait (MR3). Setting or clearing one bit of it is a read-modify-write, and the
// three matches do not belong to one context: the main loop arms the event
// and lock matches on the connection-following path, while both interrupt
// handlers arm and disarm the lock and buffer waits.
//
// So an interrupt landing between the read and the write costs the interrupt
// its bit. The main loop writes back a value it read before the handler ran,
// which arms whatever the loop asked for and silently un-arms what the
// handler asked for. When the bit lost is the lock wait's, nothing is left to
// arm receive: the engine's main loop runs on, feeding the watchdog and
// answering the host, while the capture stays silent until the mode is
// restarted. Every read-modify-write of this register goes through these two,
// with interrupts held off for the few instructions it takes, so no write
// can be built on a value another context has already moved on from.
//
// Writes of the whole register are left as they are: timer1_start() and the
// teardown path write 0, which cannot lose a bit that is on its way in,
// because nothing the engine is doing at that point wants one.
//
// The moment is written inside the same guard as the bit that arms it, so the
// two cannot be split by an interrupt: an enable bit that lands after the
// counter is already by the match waits out a whole period of the counter,
// which is 429 seconds.
//
// One rule covers the whole file: a moment derived from the counter is derived
// inside the guard. timer1_arm_match_in() below does that, and every wait and
// timeout goes through it. This absolute form is only for a moment that does
// not come from the counter at all -- the connection's own grid, where the
// next event's time comes from the last anchor -- and the two callers that
// read the counter to ask whether such a moment has gone by take a guard of
// their own around the reading, the question and the arming.
//
// What these two give is that nothing is set or cleared on a value another
// context has already moved on from. A caller that has to decide
// something from the register and then write it -- the transfer-error
// recovery, which arms the event match only if nothing has armed it -- needs
// the decision inside the guard as well, and takes its own: see
// timer1_set_match_in_if_unarmed().
static void timer1_arm_match(volatile uint32_t *match, uint32_t when, uint32_t bit) {
	uint32_t primask = irq_save_disable();

	*match = when;
	T1MCR |= bit;
	irq_restore(primask);
}

// Arm a match a fixed interval from now, with the counter read inside the
// guard.
//
// For a wait, "when" is only ever "this long from now", and the shortest of
// them is 3 microseconds -- less than an interrupt entry. Reading the counter
// before the guard and writing it inside would let an interrupt taken in
// between spend longer than the interval, and the match would go in behind the
// counter. So the reading and both writes happen together.
//
// Every wait and timeout in the engine arms its match through this, so no
// interval is measured from a counter reading that an interrupt has had time
// to overtake. The shortest is the 3 microsecond synthesizer re-check; the
// connection grid is the one thing that does not come through here, and
// timer1_arm_match() above says why.
static void timer1_arm_match_in(volatile uint32_t *match, uint32_t delta, uint32_t bit) {
	uint32_t primask = irq_save_disable();

	*match = NOW + delta;
	T1MCR |= bit;
	irq_restore(primask);
}

static void timer1_disarm_match_interrupt(uint32_t bit) {
	uint32_t primask = irq_save_disable();

	T1MCR &= ~bit;
	irq_restore(primask);
}

static void timer1_stop(void) {
	T1TCR = TCR_Counter_Reset;

	// clear interrupt handler
	ICER0 = ICER0_ICE_TIMER1;
}

static void timer1_set_match(uint32_t match) {
	timer1_arm_match(&T1MR0, match, TMCR_MR0I);
}

static void timer1_clear_match(void) {
	timer1_disarm_match_interrupt(TMCR_MR0I);
}

// Arm the connection event's match a fixed interval from now.
static void timer1_set_match_in(uint32_t delta) {
	timer1_arm_match_in(&T1MR0, delta, TMCR_MR0I);
}

// The same, unless something has armed that match already.
//
// The reading, the test and the arm are one step. The timer interrupt preempts
// the byte interrupt, which is where this is called from, and it arms this
// same match for the next event: read, lose the processor to that, and the arm
// that follows overwrites the time the interrupt set with one a whole interval
// early -- which is what the caller is trying not to do -- or with one the
// counter has already gone past, which is worse.
static void timer1_set_match_in_if_unarmed(uint32_t delta) {
	uint32_t primask = irq_save_disable();

	if (!(T1MCR & TMCR_MR0I))
		timer1_set_match_in(delta);
	irq_restore(primask);
}

// Note which connection event a packet is starting in, and drop that event's
// deadline, which the packet's arrival replaces.
//
// The deadline is what closes an event that produced nothing. A packet's first
// byte ends it, because from here the inter-frame timeout armed at the end of
// the packet is what closes the event instead.
//
// Only while an event is open, and the event is remembered on the buffer so
// that what is done when the packet ends goes to the event it arrived in. A
// packet is allowed to begin as late as the deadline itself -- the
// peripheral's answer may start a whole PERIPHERAL_REPLY_LATEST after the
// anchor -- so the deadline can expire between the packet's first symbol and
// this interrupt, and a match already latched is taken the instant the guard
// below ends, which closes the event while the packet is still arriving.
// Either way the connection has moved to another event by the time the packet
// ends, and the event counter says so: every close advances it.
//
// Without that, the packet was counted as the first packet of an event it did
// not arrive in, anchored that event from before it opened, and armed a 300
// microsecond inter-frame timeout over the event's own match. The event after
// opened behind a packet instead of on the connection's grid, and the counter
// every connection update and channel map instant is matched against ended one
// ahead of the peer's.
//
// The three steps are one guarded step, for the reason
// timer1_set_match_in_if_unarmed() gives: the timer interrupt can close the
// event between any two of them, and dropping the match after it has closed
// would drop the next event's own arming.
static void note_the_event_and_drop_its_deadline(le_rx_t *buf) {
	uint32_t primask = irq_save_disable();

	if (conn_event.opened) {
		buf->in_open_event = 1;
		buf->event_counter = conn.conn_event_counter;
		timer1_clear_match();
	}
	irq_restore(primask);
}

// Re-checks of FS_LOCK in the wait timer1 is running now: the short ones,
// which stop being counted once the lock is overdue so the count cannot wrap
// round into the short interval again, and the slow ones after that, which
// only pace the strobe.
//
// Written wherever a wait starts -- the mode's entry, the byte interrupt and
// the timer interrupt -- and from the timer interrupt as a wait runs. The two
// are reset together, which is not atomic, and the byte interrupt sits at the
// lowest interrupt priority so the timer interrupt can preempt it. Harmless as
// the code stands: every path that starts a wait from the byte interrupt runs
// with MR2 disarmed, because arming receive disarms it, so no re-check can fire
// part way through the reset. The one way a wait starts with MR2 already armed
// is the timer interrupt preempting the byte interrupt and arming a wait of its
// own in between, which leaves a spare re-check armed rather than a reset read
// half-written.
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
	uint32_t delta;

	if (fs_lock_rechecks < FS_LOCK_FAST_RECHECKS) {
		++fs_lock_rechecks;
		delta = FS_LOCK_RECHECK;
	} else {
		if (++fs_lock_slow_rechecks % FS_LOCK_SLOW_RESTROBE == 0)
			cc2400_strobe(SFSON);
		delta = FS_LOCK_RECHECK_SLOW;
	}
	timer1_arm_match_in(&T1MR2, delta, TMCR_MR2I);
}

static void timer1_cancel_fs_lock(void) {
	timer1_disarm_match_interrupt(TMCR_MR2I);
}

static void timer1_wait_buffer(void) {
	timer1_arm_match_in(&T1MR3, USEC(100), TMCR_MR3I);
}

static void timer1_cancel_wait_buffer(void) {
	timer1_disarm_match_interrupt(TMCR_MR3I);
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
						conn.conn_interval + conn.win_offset -
						RX_WARMUP_TIME);
				conn_event.opened = 1;
			}

			// after the transmit window offset, or if there is no
			// transmit window, set a packet timeout and change the
			// channel
			else { // conn_event.opened || conn.win_offset == 0
				uint32_t window_opens;

				conn_event.opened = 1;

				// The first transmit window under the new parameters
				// opens one window offset after the anchor point of the
				// event the instant names. Taken before the new interval
				// is, so the step from the event before it is the old one.
				//
				// No fixed delay here, where a connection request's first
				// window has one: the request's 1.25 ms covers the gap
				// between the request going out and the central's first
				// packet, and an update's window is measured from an
				// anchor point, with no packet before it to cover.
				window_opens = conn.last_anchor + conn.conn_interval +
						conn.win_offset;

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
				timer1_set_match_in(conn_event_deadline());
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
			timer1_set_match_in(conn_event_deadline());
			change_channel();
		}

		// timeout: close connection event and set timer for next hop
		else {
			finish_conn_event();
		}
	}

	// check FS_LOCK
	if (T1IR & TIR_MR2_Interrupt) {
		T1IR = TIR_MR2_Interrupt;

		// if FS is locked, start the event and clear the interrupt
		if (cc2400_status() & FS_LOCK) {
			timer1_disarm_match_interrupt(TMCR_MR2I);

			if (le_jam_event()) {
				// Jamming transmits a carrier; it needs no DMA, no SSP and
				// no receive buffer, so none of the arming below applies.
				le_cc2400_strobe_tx();
			}
			// Never arm RX without a packet buffer: le_DMA_IRQHandler()
			// would have nowhere to put the bytes. Wait for one instead.
			else if (current_rxbuf == NULL) {
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

		// A buffer wait left armed from receiving can still fire once a
		// connection event has turned to jamming: the pool ran dry during
		// advertising, then a connection opened. Jamming needs no buffer, so
		// drop the wait and transmit once FS has locked rather than arming
		// receive on the event that was meant to be jammed.
		if (le_jam_event()) {
			timer1_cancel_wait_buffer();
			if (cc2400_status() & FS_LOCK)
				le_cc2400_strobe_tx();
			else
				timer1_wait_fs_lock();
			return;
		}

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

// The blink's own counter, timer2, and the moment the LEDs go off again.
//
// The blink marks a packet handed to the host, so it is armed from the mode's
// loop, and it used to arm a fourth match of timer1. Timer1 is the radio's
// clock: the connection event, the synthesizer-lock wait, and the buffer wait
// are all matches of it, and its counter is what every deadline in this file
// is measured against. So the loop wrote that register once for every packet
// it handed to the host, which is what giving the LED a counter of its own
// stops. The connection-following path still arms the event and the lock
// match from the loop, under the guards timer1_arm_match() describes.
//
// Timer2 is free while this mode runs: the only other user is the xmas LED
// demo, which is a mode of its own and cannot be running at the same time.
//
// No interrupt is enabled, so this costs the radio's timing nothing: the
// deadline is polled from the loop, which is a plain counter read, and the
// loop comes round every few microseconds -- far oftener than the 10 ms the
// LED is on. Leaving the match control register at 0, and clearing whatever
// the xmas demo latched, means nothing of timer2 reaches the interrupt
// controller, so its TIMER2_IRQHandler() over in xmas.c is never entered from
// here.
//
// Note the name: NOW above is timer1's counter, and this is timer2's. The
// blink measures its deadline against this one.
#define BLINK_NOW T2TC

// Both are the loop's alone: blink() and blink_expire() are called from it
// and from nowhere else, so neither needs a guard.
static uint32_t blink_until;
static int blink_armed;

static void blink_timer_start(void) {
	PCONP |= PCONP_PCTIM2;
	T2TCR = TCR_Counter_Reset;
	// clock_start() puts timer2 on the same peripheral clock divider as
	// timer1, so timer1's prescaler makes a tick of the same length here and
	// the file's USEC() and MSEC() mean the same on either counter.
	T2PR = 4;
	T2MCR = 0;
	T2IR = 0xff; // write-1-to-clear: every match and capture flag
	T2TCR = TCR_Counter_Enable;

	blink_armed = 0;
}

static void blink_timer_stop(void) {
	T2TCR = TCR_Counter_Reset;

	PCONP &= ~PCONP_PCTIM2;
}

static void blink(int tx, int rx, int usr) {
	if (tx)
		TXLED_SET;
	if (rx)
		RXLED_SET;
	if (usr)
		USRLED_SET;

	blink_until = BLINK_NOW + LE_BLINK_LENGTH;
	blink_armed = 1;
}

// Turn the LEDs off once a blink has had its 10 ms. Called every time round
// the mode's loop.
//
// The comparison is on the signed difference, so a deadline that lands beyond
// the counter's wrap is still in the future: the counter is 32 bits of 100 ns
// ticks, which wraps every 429 seconds.
static void blink_expire(void) {
	if (!blink_armed)
		return;
	if ((int32_t)(BLINK_NOW - blink_until) < 0)
		return;

	blink_armed = 0;
	TXLED_CLR;
	RXLED_CLR;
	USRLED_CLR;
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
	/* A capture transfer that faulted as this packet completed. The packet is
	 * whole and is sent, but the host is told rather than left to read it as
	 * a clean one. */
	if (packet->transfer_error)
		meta.status |= DMA_ERROR;
	meta.channel = ubertooth_channel_byte(packet->channel);
	meta.clkn_high = 0;
	meta.clk100ns = packet->timestamp;
	/* The packet's own power: one reading was taken per byte received, all of
	 * them while the packet was on the air. */
	meta.rssi_avg = rssi_power_mean(&packet->power);
	meta.rssi_min = packet->rssi_min;
	meta.rssi_max = packet->rssi_max;
	meta.rssi_count = 0;
	meta.status |= SIGNAL_VALID;
	/* This engine takes a reading only while a packet is arriving, so it has
	 * nothing to estimate the background from and the records say so. */
	meta.rssi_noise = 0;

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
	if (t->conn_interval < LE_MIN_CONN_INTERVAL ||
			t->conn_interval > LE_MAX_CONN_INTERVAL)
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
	// connPeripheralLatency) * connInterval * 2
	//
	// Checked before it is scaled, because 10 ms in units of 100 ns is
	// 100000 and the field is two bytes wide: the largest values overflow a
	// 32-bit count, and a wrapped one lands back inside the range.
	if (t->supervision_timeout < 10 || t->supervision_timeout > 3200)
		return 0;
	t->supervision_timeout *= MSEC(10);
	// TODO handle Peripheral latency

	return 1;
}

// The ChSel bit of the last connectable advertisement, with the address that
// sent it. A connection hops with channel selection algorithm 2 only if both
// ends said they support it, and the advertiser says so in the advertisement
// rather than in the connection request: an initiator that supports the
// algorithm is allowed to set its own bit either way when the advertiser does
// not (Core 5.4, Vol 6, Part B, 2.3.3.1). One slot is enough, because the
// request follows the advertisement it answers by one inter-frame space on
// the same channel.
static uint8_t adv_chan_sel_2;
static uint8_t adv_chan_sel_2_addr[6]; // AdvA, as it arrives on the air
static uint8_t adv_chan_sel_2_seen;

// Start again with nothing heard from any advertiser.
static void forget_last_advertisement(void) {
	adv_chan_sel_2 = 0;
	adv_chan_sel_2_seen = 0;
	memset(adv_chan_sel_2_addr, 0, sizeof(adv_chan_sel_2_addr));
}

// Did the advertiser this request answers say it supports algorithm 2?
// 1 yes, 0 no, -1 nothing was heard from that address -- the capture may have
// missed its advertisement, and the request's own bit is then the best there
// is.
static int advertiser_asked_for_chan_sel_2(const uint8_t *adva) {
	if (!adv_chan_sel_2_seen ||
			memcmp(adv_chan_sel_2_addr, adva,
					sizeof(adv_chan_sel_2_addr)) != 0)
		return -1;
	return adv_chan_sel_2;
}

static void le_connect_handler(le_rx_t *buf) {
	uint32_t aa;
	le_conn_timing_t timing;

	if (!le.do_follow)
		return;

	if (buf->size != 2 + 6 + 6 + 22 + 3)
		return;

	// A connection's access address cannot be the advertising one. The
	// specification forbids it (Core 6.0, Vol 6, Part B, 2.1.2), and this
	// engine reads that value as "no connection is being followed", so a
	// follow opened on it would stay tuned to the advertising channels and
	// never reach the connection's data channels. Refused, like a
	// parameter outside its range.
	aa = extract_field(buf, 14, 4);
	if (aa == ADVERTISING_AA)
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

	conn.access_address     = aa;
	conn.crc_init           = extract_field(buf, 18, 3);
	// CRCInit is a 24-bit field. rbit() reverses all 32 bits of the word, so
	// its result sits a byte too high: 0x123456 has to become 0x6a2c48, and
	// rbit() alone gives 0x6a2c4800, which btle_crcgen_lut() masks down to
	// 0x2c4800. Without the shift every data packet of a followed connection
	// is checked against a CRC from the wrong starting value, so
	// `ubertooth-btle -f -k1` drops all of them.
	conn.crc_init_reversed  = rbit(conn.crc_init) >> 8;
	conn.hop_increment      = extract_field(buf, 35, 1) & 0x1f;

	// Which algorithm this connection hops with. Both ends have to have
	// said they support algorithm 2: the initiator in this request's ChSel
	// bit, the advertiser in the advertisement this request answers (Core
	// 5.4, Vol 6, Part B, 2.3.3.1 and 4.4.4). Where the advertisement was
	// not captured there is nothing to check it against, and the request's
	// own bit is the better guess: an initiator sets it when it supports
	// the algorithm, and a peer old enough not to is rare.
	{
		int chsel_req = (buf->data[0] >> 5) & 1;
		int chsel_adv = advertiser_asked_for_chan_sel_2(&buf->data[8]);

		conn.chan_sel_2 = chsel_req && chsel_adv != 0;

		// The specification ties the pair of bits to one exchange: the
		// advertisement this request answered. So the record is used
		// once and dropped, rather than left to decide a later
		// connection from the same address whose own advertisement was
		// missed.
		forget_last_advertisement();
	}

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
	uint32_t primask;
	uint16_t instant;

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

	instant = extract_field(buf, 12, 2);

	// The whole update goes in at once. This handler runs in the mode's
	// loop, the timer interrupt reads these five fields on every connection
	// event, and it preempts the loop: an update that arrives while an
	// earlier one is still waiting for its instant would otherwise be
	// applied as a mixture of the two -- the old instant with the new
	// interval, or the new window with the old timeout -- and every deadline
	// the follow uses is built from them.
	//
	// Interrupts held off rather than the flag written last, because none of
	// these fields is volatile: the compiler may put the stores in whatever
	// order it likes, and a flag cleared and set again with only ordinary
	// stores in between is a store it may drop altogether. The guard carries
	// a memory clobber, so it orders them as well as keeping the interrupt
	// out.
	primask = irq_save_disable();
	conn.win_size            = timing.win_size;
	conn.win_offset          = timing.win_offset;
	conn.conn_update_pending_interval = timing.conn_interval;
	conn.conn_update_pending_supervision_timeout = timing.supervision_timeout;
	conn.conn_update_instant = instant;
	conn.conn_update_pending = 1;
	irq_restore(primask);
}

// Is every channel in a 37-bit channel map clear? The five bytes carry
// channels 0 to 36, so the top three bits of the last one are reserved and
// le_parse_channel_map() ignores them; this reads the same bits it does.
static int channel_map_is_empty(const uint8_t *map) {
	return (map[0] | map[1] | map[2] | map[3] | (map[4] & 0x1f)) == 0;
}

static void channel_map_update_handler(le_rx_t *buf) {
	uint32_t primask;
	uint16_t instant;

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

	// Nothing is pending while the map is being written. This handler runs
	// in the mode's loop and the timer interrupt, which preempts it, reads
	// the flag on every connection event; le_parse_channel_map() starts by
	// clearing the map it is filling, so an update applied while it ran gave
	// the follow a map with some of this update's channels in it and the rest
	// missing, and the receiver tuned where the peer was not for the rest of
	// the connection.
	//
	// Two short guards rather than one around the parse: the parse clears and
	// fills nearly 200 bytes, and that is longer than this engine wants to
	// hold interrupts off for. Guards rather than the order of the stores,
	// because none of these fields is volatile -- see
	// connection_update_handler().
	primask = irq_save_disable();
	conn.channel_map_update_pending = 0;
	irq_restore(primask);

	le_parse_channel_map(&buf->data[3], &conn.pending_remapping);

	// The map is whole, so the instant and the flag go in together.
	instant = extract_field(buf, 8, 2);

	primask = irq_save_disable();
	conn.channel_map_update_instant = instant;
	conn.channel_map_update_pending = 1;
	irq_restore(primask);
}

// What a capture's size counts: the two-byte PDU header, the payload, and the
// three CRC bytes the radio hands over behind the payload. A length check on
// size therefore has to include the CRC, or it accepts a payload three bytes
// shorter than it meant to and a reader of those bytes takes CRC bytes for the
// tail of an address.
#define LE_HEADER_LEN 2
#define LE_ADDR_LEN   6
#define LE_CRC_LEN    3

// The shortest capture that carries the addresses the filter compares.
#define LE_ONE_ADDR_SIZE (LE_HEADER_LEN + LE_ADDR_LEN + LE_CRC_LEN)
#define LE_TWO_ADDR_SIZE (LE_HEADER_LEN + 2 * LE_ADDR_LEN + LE_CRC_LEN)

// Does this advertising-channel packet's CRC check out? Advertising PDUs are
// checked against the fixed starting value the specification gives them
// (reversed, 0xAAAAAA), the same one the mode's loop uses for -k1.
static int advertising_crc_ok(le_rx_t *buf) {
	unsigned pdu_len;
	uint32_t wire_crc;

	if (buf->size <= LE_CRC_LEN)
		return 0;

	pdu_len = buf->size - LE_CRC_LEN;
	wire_crc = ((uint32_t)buf->data[pdu_len + 2] << 16) |
			((uint32_t)buf->data[pdu_len + 1] << 8) |
			buf->data[pdu_len];

	return btle_crcgen_lut(0xAAAAAA, buf->data, pdu_len) == wire_crc;
}

static void packet_handler(le_rx_t *buf) {
	// advertising packet
	if (btle_channel_index(buf->channel) >= 37) {
		switch (buf->data[0] & 0xf) {
			// ADV_IND and ADV_DIRECT_IND, the two advertisements a
			// connection request can answer. Only the ChSel bit is
			// wanted, and only until that request arrives.
			//
			// The CRC is checked here whatever the host asked for,
			// because of what this bit decides. A cleared bit is the
			// only thing an advertisement can say that changes the
			// hopping -- a set bit gives the same answer as having
			// heard no advertisement at all -- so one bit error in
			// this header would send the follow back to algorithm 1
			// for the whole life of that connection, and no bit
			// error can help. An ADV_DIRECT_IND carries two
			// addresses, as the filter below also requires of it.
			// Nothing reads the bit unless the host asked to follow,
			// and a capture of advertising alone is busy, so the
			// check is skipped there.
			case 0x00:
			case 0x01:
				if (le.do_follow &&
						buf->size >= (((buf->data[0] & 0xf) == 0x01)
							? LE_TWO_ADDR_SIZE
							: LE_ONE_ADDR_SIZE) &&
						advertising_crc_ok(buf)) {
					memcpy(adv_chan_sel_2_addr, &buf->data[2],
							sizeof(adv_chan_sel_2_addr));
					adv_chan_sel_2 = (buf->data[0] >> 5) & 1;
					adv_chan_sel_2_seen = 1;
				}
				break;

			// CONNECT_REQ, which le_connect_handler() checks the
			// length of. Its own bits are taken on trust, as the
			// access address, the CRC starting value and the timing
			// in the same packet already are: a request this
			// corrupt gives a follow that fails for larger reasons
			// than which algorithm it hops with.
			case 0x05:
				// Only while no connection is being followed. Acting on a
				// request mid-follow would rebuild the whole connection
				// from this loop, with the timer interrupt free to take a
				// connection event part way through and hop on
				// half-written state, and it would move the capture onto
				// a different connection. The packet still reaches the
				// host, which happens before this runs.
				//
				// The advertising access address means "not following":
				// see le_connect_handler(), which keeps it that way.
				// Only this loop starts a follow, so no interrupt can
				// make this test too permissive. One can end a follow
				// just after it, which costs this single request; the
				// next one opens a follow.
				if (conn.access_address == ADVERTISING_AA)
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

				// LL_TERMINATE_IND -- the peers are closing the
				// connection. Noted here and acted on by the next
				// connection event to close, which is where the
				// supervision timeout ends a follow too: the connection
				// state belongs to the timer interrupt that reads it on
				// every event, so this loop must not reset it.
				//
				// The control LLID and the length are all that stands
				// between a bit-errored data packet and the end of a
				// healthy follow, as they are for the two updates
				// above. A capture that cannot afford that asks for
				// CRC verification (`ubertooth-btle -k1`), which drops
				// a damaged packet before it reaches this dispatch.
				case 0x2:
					if (buf->data[1] == 2)
						conn.terminated = 1;
					break;
			}
		}
	}
}

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

	// The LED blink's counter. See BLINK_NOW.
	blink_timer_start();

	current_rxbuf = buffer_get();
	rf_channel = le_adv_channel; // FIXME
	// Not just the access address: the whole connection state, so a run
	// stopped part way through a connection leaves nothing behind for this
	// one to follow with. What was last heard on an advertising channel
	// goes with it.
	reset_conn();
	forget_last_advertisement();
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

		blink_expire();

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

	// drop every armed match, and any flag one of them has already latched,
	// before stopping the timer, so the hardware is left quiet. The next
	// entry clears all of this again rather than trusting it.
	T1MCR = 0;
	T1IR = 0xff; // write-1-to-clear: every match and capture flag
	timer1_stop();

	// The LED blink's counter goes back off, and its power with it. The LEDs
	// themselves are cleared below, whatever the blink was in the middle of.
	blink_timer_stop();

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
