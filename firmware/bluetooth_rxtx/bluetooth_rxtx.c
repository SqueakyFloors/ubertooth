/*
 * Copyright 2010-2013 Michael Ossmann
 * Copyright 2011-2013 Dominic Spill
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

#include <stddef.h>
#include <string.h>

#include "version.h"
#include "ubertooth.h"
#include "ubertooth_usb.h"
#include "ubertooth_interface.h"
#include "ubertooth_rssi.h"
#include "ubertooth_cs.h"
#include "ubertooth_dma.h"
#include "ubertooth_clock.h"
#include "bluetooth.h"
#include "bluetooth_le.h"
#include "cc2400_rangetest.h"
#include "cc2400_wait.h"
#include "ego.h"
#include "rx_fifo_lead.h"
#include "debug_uart.h"
#include "xmas.h"
#include "identify.h"
#include "watchdog.h"
#include "bl_report.h"

#define MIN(x,y)	((x)<(y)?(x):(y))
#define MAX(x,y)	((x)>(y)?(x):(y))

#ifdef STACK_GUARD_TEST
/*
 * A stack overflow on demand, so the guard at the bottom of the stack can be
 * shown to catch one. No shipped image defines STACK_GUARD_TEST; the bench
 * procedure is in TEST_PLAN.md. UBERTOOTH_STOP is the trigger because every
 * tool sends it as it exits.
 *
 * Every word of each frame is written, so the descent cannot step over the
 * 32-byte guard, and the recursive call goes through a volatile pointer: at
 * -Os the compiler otherwise turns plain self-recursion into a loop over one
 * frame and the stack never grows.
 */
static volatile uint8_t overflow_the_stack_now = 0;

static unsigned long overflow_the_stack(unsigned long depth);
static unsigned long (*volatile overflow_next)(unsigned long) =
	overflow_the_stack;

static __attribute__((noinline)) unsigned long
overflow_the_stack(unsigned long depth)
{
	volatile unsigned long frame[8];
	unsigned i;

	for (i = 0; i < 8; i++)
		frame[i] = depth;
	return frame[depth & 7] + overflow_next(depth + 1);
}
#endif

/*
 * The build string reported by UBERTOOTH_GET_COMPILE_INFO, which is what
 * ubertooth-util -V prints: "ubertooth ", the revision, then who built it,
 * where, and when. Keep this identical to the one in
 * btbr/src/btctl/vendor_request_handler.c. The image name and revision a host
 * matches against are in the firmware info block (../common/fw_info.c), not
 * here.
 */
const char compile_info[] =
	"ubertooth " GIT_REVISION " (" COMPILE_BY "@" COMPILE_HOST ") " TIMESTAMP;

/* hopping stuff */
volatile uint8_t  hop_mode = HOP_NONE;
volatile uint8_t  do_hop = 0;                  // set by timer interrupt
volatile uint16_t channel = 2441;
volatile uint16_t hop_direct_channel = 0;      // for hopping directly to a channel
volatile uint16_t hop_timeout = 158;
volatile uint16_t requested_channel = 0;
volatile uint16_t le_adv_channel = BT_FREQ_MIN;
volatile int      cancel_follow = 0;

/* bulk USB stuff */
volatile uint8_t  idle_buf_clkn_high = 0;
volatile uint32_t idle_buf_clk100ns = 0;
volatile uint16_t idle_buf_channel = 0;
volatile uint8_t  dma_discard = 0;
volatile uint8_t  status = 0;

/* operation mode */
volatile uint8_t mode = MODE_IDLE;
volatile uint8_t requested_mode = MODE_IDLE;
volatile uint8_t jam_mode = JAM_NONE;
volatile uint8_t ego_mode = EGO_FOLLOW;
/* Set by UBERTOOTH_EGO, read and cleared by ego_main(): a mode start for the
 * running E-GO loop, which the mode dispatch never sees. */
volatile uint8_t ego_restart = 0;

volatile uint8_t modulation = MOD_BT_BASIC_RATE;

/* specan stuff */
volatile uint16_t low_freq = 2400;
volatile uint16_t high_freq = 2483;
/* raw-scale LED specan threshold; 16-bit so the host's whole -q range fits
 * (see ubertooth_rssi.h) */
volatile int16_t rssi_threshold = -30;  // -54dBm - 30 = -84dBm

/* Generic TX stuff */
generic_tx_packet tx_pkt;

/*
 * How many times to send tx_pkt, from the request's wValue.
 *
 * One request per packet does not work at the rate a host can send them: the
 * board answers no USB request while a packet is going out, and this is about
 * a millisecond of radio waits, so a request handed over in that window comes
 * back to the host as a transfer error and that packet is lost. Measured on
 * two boards, a burst of 30 requests sent as fast as the host could send them
 * had 15 of them fail that way. Carrying the count in the request instead
 * costs one transfer for the whole burst, and the host never waits on the
 * board at all.
 */
volatile uint16_t tx_pkt_repeat;

/* The registers, length and level a UBERTOOTH_TX_GENERIC_PACKET request
 * carries in front of its payload. */
#define TX_GENERIC_HEADER_LEN offsetof(generic_tx_packet, data)

/* le Peripheral stuff */
uint8_t peripheral_mac_address[6] = { 0, };
/* The advertising data MODE_BT_PERIPHERAL_LE transmits, as the host set it. Sized
 * for the most a legacy ADV_IND can carry, which is all this ever sends. */
uint8_t le_adv_data[ADV_IND_MAX_DATA] = { 0x02, 0x01, 0x05 };
unsigned le_adv_len = 3;

le_state_t le = {
	.access_address = 0x8e89bed6,           // advertising channel access address
	.access_address_set = 0,                // the host has not given one
	.synch = 0x6b7d,                        // bit-reversed adv channel AA
	.syncl = 0x9171,
	.crc_init  = 0x555555,                  // advertising channel CRCInit
	.crc_init_reversed = 0xAAAAAA,
	.crc_verify = 0,

	.link_state = LINK_INACTIVE,
	.conn_epoch = 0,
	.do_follow = 1,
	.target_set = 0,
	.last_packet = 0,
};

typedef struct _le_promisc_active_aa_t {
	u32 aa;
	int count;
} le_promisc_active_aa_t;

/* How many of the gaps between one connection's returns to this channel the
 * estimator keeps. It looks for the pattern they repeat, and needs two runs of
 * one pattern to see a repeat, so this is twice the longest pattern it can
 * find: sixteen gaps, so up to eight returns a trip round the channels.
 *
 * A full channel map returns once a trip. A smaller map returns about 37/U
 * times, U being the channels it uses, because the remapping spreads the
 * unused positions over the used ones: twice or three times for the maps of
 * 18 and 19 channels the connections on the bench negotiate, eight times for
 * a map of five. A map smaller than that returns too often for the pattern to
 * fit here, and then nothing is reported. */
#define LE_PROMISC_GAPS 16

typedef struct _le_promisc_state_t {
	// LFU cache of recently seen AA's
	le_promisc_active_aa_t active_aa[32];

	/* Recovering the connection interval.
	 *
	 * The mode listens on one data channel, so it sees the connection
	 * whenever the hop sequence brings it back here -- a whole number of
	 * connection events later. Every gap between two sightings is
	 * therefore a whole number of connection intervals.
	 *
	 * The spacings between returns repeat every trip round the 37 hop
	 * positions and add up to 37 connection events, so the gaps are kept
	 * in the order they arrived and the estimator looks for the pattern
	 * they repeat. The oldest falls off the end, which is how a figure
	 * already reported stops being reported once the connection it
	 * described has changed or gone. */
	u32 gap[LE_PROMISC_GAPS];
	u8 gap_count;
	int consec_intervals;

	/* When a packet of the connection was last seen on the channel being
	 * listened to, and whether one has been. The estimator times how long
	 * the connection takes to come back, so it must have no stamp to
	 * measure from when a pass starts: the mode's teardown stops the
	 * capture clock and zeroes it, so a stamp left by an earlier pass is a
	 * reading of a clock that has since restarted, and the gap computed
	 * from it belongs to nothing. */
	u32 prev_clk;
	u8 seen_a_packet;

	/* The sighting on 2404 MHz that the hop increment is counted from,
	 * which is the same kind of stamp and wants the same clearing. */
	u32 first_ts;

	/* The same gaps again, for the algorithm-2 search, which needs more of
	 * them than the estimator above: a ring of its own rather than a
	 * longer shared one, because the estimator's length is what bounds the
	 * pattern it looks for.
	 *
	 * csa2_gap_count is how many the ring holds and sightings is how many
	 * have arrived since the last search, which is what gives each
	 * estimator a run of consecutive returns to work from. */
	u32 csa2_gap[LE_CSA2_SIGHTINGS - 1];
	u8 csa2_gap_count;
	u16 sightings;
} le_promisc_state_t;
le_promisc_state_t le_promisc;
#define AA_LIST_SIZE (int)(sizeof(le_promisc.active_aa) / sizeof(le_promisc_active_aa_t))

/* LE jamming */
#define JAM_COUNT_DEFAULT 40
int le_jam_count = 0;

/* set LE access address */
static void le_set_access_address(u32 aa);

/* Tell the host one parameter of the connection promiscuous mode has worked
 * out. The access address is reported by bt_le_sync(), which is further up the
 * file than this definition. */
void le_promisc_state(u8 type, void *data, unsigned len);

typedef int (*data_cb_t)(char *);
data_cb_t data_cb = NULL;

typedef void (*packet_cb_t)(u8 *);
packet_cb_t packet_cb = NULL;

/*
 * The receive buffer of a low-energy mode. The two halves of such a mode need
 * very different buffers, and share one because they never run at the same
 * time.
 *
 * It is in the other RAM bank (AHB_SRAM), which the transfer controller
 * reaches as it reaches main SRAM: bt_le_sync() receives straight into
 * `packet`.
 *
 * bt_generic_le() fills `symbols` and hands it to its data callback, which
 * searches it for an access address. It has returned before bt_le_sync() is
 * entered, and bt_le_sync() has no data callback: it receives whole packets
 * into `packet` instead. bt_promisc_le() runs the two in that order, so a
 * packet never lands on symbols still being read, or the other way round.
 */
static AHB_SRAM union {
	/* One byte per received symbol, two DMA buffers' worth: the previous
	 * buffer followed by the one just received, so an access address that
	 * straddles the two is still found. */
	char symbols[DMA_SIZE*8*2];
	/* One whole captured packet: the four bytes of the access address the
	 * radio synchronized on, then the PDU header, the payload and the CRC.
	 * Long enough for any payload the header's 8-bit length field can
	 * claim, so a Data Length Extension packet arrives whole. */
	uint8_t packet[LE_MAX_USB_PACKET_SIZE];
} le_rx;

/* How many bytes the radio can put after the access address: everything in
 * le_rx.packet except the four the access address takes. */
#define LE_RX_PDU_MAX (LE_MAX_USB_PACKET_SIZE - 4)

/* dma_init_le() splits what it is given into a first item of LE_DMA_HEAD
 * bytes and a second of the rest, and the controller's transfer size is a
 * 12-bit field. Both bounds are checked here because the length is a constant
 * at the only call site; a transfer size that overflowed would run into the
 * burst-size bits and mis-program the controller rather than fail. */
_Static_assert(LE_RX_PDU_MAX > LE_DMA_HEAD,
               "the low-energy receive buffer is shorter than the first DMA item");
_Static_assert(LE_RX_PDU_MAX - LE_DMA_HEAD <= 0xfff,
               "the low-energy receive buffer does not fit one DMA transfer");

static int enqueue(uint8_t type, uint8_t* buf)
{
	usb_pkt_rx* f = usb_enqueue();

	/* fail if queue is full */
	if (f == NULL) {
		status |= FIFO_OVERFLOW;
		return 0;
	}

	f->pkt_type = type;
	/* No power figures unless the branch below puts them in, and the status
	 * bits that would make them readable stay clear: a record comes off the
	 * ring with whatever the last one left in it. */
	f->rssi_noise = 0;
	f->reserved = 0;
	if(type == SPECAN) {
		f->clkn_high = (clkn >> 20) & 0xff;
		f->clk100ns = CLK100NS;
	} else {
		f->clkn_high = idle_buf_clkn_high;
		f->clk100ns = idle_buf_clk100ns;
		f->channel = ubertooth_channel_byte(idle_buf_channel);
		f->rssi_min = rssi_min;
		f->rssi_max = rssi_max;
		f->rssi_avg = rssi_get_signal();
		f->rssi_count = rssi_count;
		/* Only a record that carries a packet gets the power figures. The
		 * others this builds -- a state change, a message -- carry no
		 * packet for a signal figure to belong to. */
		if (type == BR_PACKET && rssi_count != 0) {
			status |= SIGNAL_VALID;
			if (rssi_noise_estimated()) {
				f->rssi_noise = (uint8_t) rssi_get_noise();
				status |= NOISE_ESTIMATE;
			}
		}
	}

	memcpy(f->data, buf, DMA_SIZE);

	f->status = status;
	status = 0;

	return 1;
}

int enqueue_with_ts(uint8_t type, uint8_t* buf, uint32_t ts)
{
	usb_pkt_rx* f = usb_enqueue();

	/* fail if queue is full */
	if (f == NULL) {
		status |= FIFO_OVERFLOW;
		return 0;
	}

	f->pkt_type = type;

	f->clkn_high = 0;
	f->clk100ns = ts;

	f->channel = ubertooth_channel_byte(channel);
	f->rssi_avg = 0;
	f->rssi_count = 0;
	/* This path takes no readings, so the record carries no noise figure and
	 * the status bit that would make one readable stays clear. */
	f->rssi_noise = 0;
	f->reserved = 0;

	memcpy(f->data, buf, DMA_SIZE);

	f->status = status;
	status = 0;

	return 1;
}

/*
 * Registers one UBERTOOTH_WRITE_REGISTERS request can write: a register
 * number and a 16-bit value each, within the buffer the request's data stage
 * lands in. The count arrives in a 16-bit field, so without this bound the
 * handler would walk a count the buffer cannot hold -- reading past it, and
 * never finishing at all in a counter narrower than the field.
 */
#define WRITE_REGISTERS_MAX (VENDOR_REQ_DATA_LEN / 3)

/*
 * The registers UBERTOOTH_READ_ALL_REGISTERS answers with: every CC2400
 * address from MAIN (0x00) through SYNCH (0x2d), the last register below the
 * command strobes. A few addresses in that run are not registers the radio
 * documents, and they are read anyway, as they always have been. Each address
 * takes three bytes of the reply, its number and the 16-bit value read from
 * it, so the length of the reply follows from the count. The
 * loop that fills the reply and the length the reply reports both come from
 * this one count, so the reply cannot claim fewer registers than it carries.
 */
#define READ_ALL_REGISTERS_COUNT (SYNCH + 1)
#define READ_ALL_REGISTERS_LEN   (READ_ALL_REGISTERS_COUNT * 3)

/*
 * Forget the AFH channel map and the flag that says to hop on it.
 *
 * The map belongs to the run that found it: a channel set the host narrowed
 * down for one piconet says nothing about the next capture. used_channels is
 * the count of entries the map put in the hopping table, so it goes too.
 */
static void clear_afh_map(void)
{
	size_t i;

	for (i = 0; i < sizeof(afh_map); i++)
		afh_map[i] = 0;
	afh_enabled = 0;
	used_channels = 0;
}

/*
 * The three modes bt_stream_rx()'s loop keeps streaming for. A request for one
 * of them that arrives while another of them runs is absorbed by that loop and
 * never reaches the mode dispatch in main().
 */
static int streams_symbols(uint8_t m)
{
	return m == MODE_RX_SYMBOLS || m == MODE_BT_FOLLOW || m == MODE_AFH;
}

/*
 * The hopping a mode start forgets: the hop mode, a hop the clock asked for
 * and has not been served, a channel a fixed-channel hop would return to, and
 * the AFH map with its enable flag and used-channel count.
 *
 * cc2400_idle() does this for a mode start that reaches the dispatch; the two
 * callers below are the mode starts that do not, so the rule holds wherever a
 * mode starts. The channel is not part of it: one set while no mode was
 * running belongs to the mode about to start.
 */
static void forget_hop_state(void)
{
	hop_mode = HOP_NONE;
	do_hop = 0;
	hop_direct_channel = 0;
	clear_afh_map();
}

/*
 * Reset what a mode start resets, for a mode request the running stream
 * absorbs.
 *
 * Starting a mode through the dispatch always leaves the previous one first,
 * which runs cc2400_idle() and so puts the hop mode back to HOP_NONE and
 * forgets the AFH map. A request for one of the three modes above, arriving
 * while another of them runs, never gets there: the loop carries on under the
 * new mode's name. Without this it also carried on with the old mode's hop
 * mode and map, so a killed ubertooth-afh left the board streaming and the
 * next ubertooth-rx captured while hopping adaptively on the old map.
 *
 * Only state a running receive can be reset under: the radio stays tuned and
 * receiving, so the channel it is on is left alone. That channel is also the
 * one the host set for the capture it is starting, a request or two before
 * this one.
 *
 * The modulation is put back, which it could not be while cc2400_rx() was the
 * only thing that wrote MDMCTRL: a value set here would not have reached the
 * radio. The capture loop writes that register when the modulation changes
 * (program_stated_modulation()), so it does now, and each of the three modes
 * absorbed here reads Basic Rate packets. Without it, a killed
 * `ubertooth-dump -l` left the board demodulating low energy and the AFH
 * detection or follow absorbed here ran on that, finding little and saying
 * nothing. A tool that wants another modulation states it after its request.
 *
 * The per-channel averages are left alone, unlike in the dispatch, which drops
 * them. They are not something the mode configures: they are what the radio
 * has been hearing on each channel, and the three modes here all receive the
 * same modulation over the same 79 channels. The receive never stopped, so the
 * measurement still holds -- and dropping it would cost the new capture the
 * few hundred packets the filter needs to build up again, which it would spend
 * reporting the -55 dBm an empty filter gives.
 *
 * Call this before the request's own setup, which is what the absorbed mode
 * starts from. Re-sending the mode that is already streaming resets nothing:
 * it is not a mode start, and the dispatch does not reset for one either. That
 * is what keeps a follow that re-syncs its clock mid-session -- which sends
 * UBERTOOTH_START_HOPPING again -- from throwing away the AFH map it is
 * following on.
 *
 * Which mode is streaming is `requested_mode`, not `mode`: an absorbed request
 * never reaches the dispatch, so `mode` still names the mode that entered the
 * loop, while the loop is streaming whatever the last accepted request asked
 * for. Reading `mode` here would skip the reset on a second switch back, as in
 * follow, then capture, then follow again.
 */
static void reset_absorbed_mode_state(uint8_t requested)
{
	if (requested == requested_mode || !streams_symbols(mode) ||
	    !streams_symbols(requested))
		return;

	forget_hop_state();
	modulation = MOD_BT_BASIC_RATE;
}

/* Declared here because the request handler calls it; defined with the rest of
 * the copy below, where what it drops is explained. */
static void drop_kept_settings(uint8_t requested);

/*
 * Tune to a channel, or sweep the band, as UBERTOOTH_SET_CHANNEL asks.
 *
 * A frequency above the band's top asks for the band sweep rather than for a
 * channel, and one inside it for a fixed channel, which the hop modes read as
 * they run. The mode dispatch calls this too, to re-apply a channel the host
 * stated after its mode request; see below.
 *
 * Every frequency that reaches here is one the radio can tune: the request
 * handler refuses the rest, and the channel the dispatch re-applies is one it
 * already accepted.
 */
static void set_channel_request(uint16_t requested)
{
	requested_channel = requested;
	/* bluetooth band sweep mode, start at the bottom of the band */
	if (requested_channel > MAX_FREQ) {
		hop_mode = HOP_SWEEP;
		requested_channel = BT_FREQ_MIN;
	}
	/* Fixed channel mode, which can be outside the Bluetooth band: the
	 * radio tunes from MIN_FREQ to MAX_FREQ and a frequency in that range
	 * is taken as it was asked for. There is nothing to clamp -- the
	 * request handler refuses anything below MIN_FREQ, and this branch is
	 * the one at or below MAX_FREQ. */
	else {
		hop_mode = HOP_NONE;
	}

	le_adv_channel = requested_channel;
	channel = requested_channel;
	requested_channel = 0;

	/* The carrier-sense threshold is worked out from the channel, so it is
	 * set here, for whichever mode is running. Except in LE follow mode: the
	 * low-energy engine (firmware/bluetooth_rxtx/le_phy.c) owns the radio
	 * while it runs and reaches it over the same serial bus a write from
	 * here would use, and it does not use carrier sense at all. Whatever
	 * mode starts next sets the threshold from `channel` as it brings the
	 * radio up (cc2400_rx()), so nothing is lost by leaving it.
	 *
	 * `channel` itself is written in every mode. In LE follow mode it used
	 * to be left alone, because the mode loop of the day picked up
	 * `requested_channel` and retuned from there; that loop went when
	 * le_phy.c took the mode over, which left the host told its channel had
	 * been set while the board kept the old one. */
	if (mode != MODE_BT_FOLLOW_LE)
		cs_threshold_calc_and_set(channel);
}

/*
 * What the host stated after its mode request, kept for the mode that is
 * starting.
 *
 * A mode start resets the board's mode state, so that one run cannot capture
 * with what another left behind: the hop mode, the channel, the modulation,
 * the AFH map and the E-GO access code all go back to their defaults. A tool
 * therefore states its channel -- or its band sweep, its modulation, or its
 * access code -- after the request that starts its mode, not before.
 *
 * That only worked on a board that had been left capturing symbols, where the
 * reset runs inside the request handler, while the request is answered, so
 * anything stated afterwards survives it. A board left in a mode that captures
 * no symbols -- a killed ubertooth-specan, -btle or -ego -- resets later: that
 * mode's loop notices the request, returns, and the dispatch in main() runs
 * the reset on the way out of it, by which time the channel stated straight
 * after the request has usually arrived and is thrown away. The capture then
 * ran on 2441 MHz, the channel the reset leaves behind, while the user had
 * asked for the whole band, and nothing said so.
 *
 * So the requests that state one of these keep a copy, and the dispatch
 * applies it again after the reset. The copy names the mode request it
 * followed, and is only applied to that mode: one left behind by a run the
 * host has moved on from is dropped rather than carried into whatever starts
 * next, which is the carry-over the reset exists to prevent.
 */
#define STATED_CHANNEL     (1 << 0)
#define STATED_ACCESS_CODE (1 << 1)
#define STATED_AFH_MAP     (1 << 2)
#define STATED_MODULATION  (1 << 3)

static volatile uint8_t  stated_what;       /* STATED_* bits, 0 for nothing */
static volatile uint8_t  stated_for_mode;   /* the mode request it followed */
static volatile uint16_t stated_channel;    /* as the host asked for it */
static volatile uint8_t  stated_modulation;
static volatile uint32_t stated_access_code;
static volatile int      stated_access_code_len;
/* The ten map bytes as the host sent them, one bit per channel. The map is
 * kept rather than the hop sequence built from it, so applying it runs the
 * same code the request ran. */
static volatile uint8_t  stated_afh_map[sizeof(afh_map)];

/*
 * Note which mode request this setting followed. A copy kept for an earlier
 * request is replaced, not added to: that request's mode is not the one
 * starting.
 *
 * Only the value is recorded, not the state it leaves behind, so applying it
 * again runs the same code the request ran -- with `mode` naming the mode that
 * has started by then, which is what a channel request reads.
 */
static void stated_for_starting_mode(uint8_t what)
{
	if (stated_for_mode != requested_mode)
		stated_what = 0;
	stated_for_mode = requested_mode;
	stated_what |= what;
}

/*
 * Apply what was stated after the request for the mode now starting.
 *
 * Called from the dispatch after the reset, and before the mode's own setup,
 * which still wins: a follow or an AFH detection asks for its hop mode there,
 * as it has to, since the reset clears that too, and the two low-energy modes
 * set their own modulation there.
 *
 * A copy for another mode is dropped, unless that mode is still the one the
 * host is waiting for -- its mode start is yet to come. The dispatch reads the
 * request once on its way in and the reset takes a while, so a second tool's
 * request and its setting can both be answered in between: the mode starting
 * here is then the first tool's, and dropping the second tool's copy would
 * leave its capture on the reset defaults.
 */
static void apply_stated_settings(uint8_t starting_mode)
{
	uint8_t what;

	if (stated_what == 0)
		return;

	if (stated_for_mode != starting_mode) {
		/* Still pending: leave it for the mode start it was stated for.
		 * Otherwise it belongs to a run that is over, and keeping it
		 * would carry that run's channel into a later mode -- the
		 * carry-over the reset exists to prevent. */
		if (stated_for_mode != requested_mode)
			stated_what = 0;
		return;
	}

	what = stated_what;
	stated_what = 0;

	/* A modulation is never left on an idle board. A stop is a mode request
	 * like any other, so one answered between the stop and this -- a window
	 * of a whole mode teardown -- would be applied here, after the reset
	 * had put the modulation back, and a mode starting from idle runs no
	 * reset of its own to undo it. The channel is deliberately kept across
	 * a stop, which is what `ubertooth-util -S -c 2440` asks for. */
	if (starting_mode == MODE_IDLE)
		what &= ~STATED_MODULATION;

	if (what & STATED_CHANNEL)
		set_channel_request(stated_channel);
	if (what & STATED_MODULATION)
		modulation = stated_modulation;
	if (what & STATED_ACCESS_CODE) {
		access_code = stated_access_code;
		access_code_len = stated_access_code_len;
	}
	if (what & STATED_AFH_MAP) {
		uint8_t map[sizeof(afh_map)];
		unsigned i;

		/* Copied out first, because bluetooth_set_afh_map() takes a plain
		 * pointer and the copy is volatile. It rebuilds the hop sequence
		 * from the map, which is what the capture hops on. */
		for (i = 0; i < sizeof(map); i++)
			map[i] = stated_afh_map[i];
		bluetooth_set_afh_map(map);
	}
}

/*
 * What the host stated *before* its mode request, kept for the mode that
 * request starts.
 *
 * One setting is sent in that order rather than after the request: the jam
 * mode, which ubertooth-btle sends for -j. A mode start puts it back to no
 * jamming, so on a board left in a mode by a killed tool it was thrown away --
 * after the request carrying it had been answered -- and `ubertooth-btle -f -j`
 * followed the connection and never jammed, without saying so.
 *
 * So that request leaves a copy, and the dispatch applies it again after the
 * reset: the shape the settings stated after a mode request use above, with
 * the difference that this one names no mode, because the request that names
 * one has not arrived yet.
 *
 * The mode request that follows is as far as the copy reaches, and it is
 * dropped there whether it was used or not, so a run that stated a jam mode
 * cannot reach a later run -- the carry-over the reset exists to prevent.
 *
 * Which mode that is, the copy cannot say, because the request that names one
 * has not arrived yet, so the drop happens in both places a mode request can
 * end up. One that starts a mode reaches the dispatch, which applies the copy
 * after the reset and clears it (apply_kept_settings()). One the running loop
 * absorbs never gets there, and needs no copy either -- no reset runs, so the
 * value the host set is already in force -- so each of the requests that can
 * be absorbed drops it instead (drop_kept_settings()).
 *
 * What is left is a tool killed between the setting and its own mode request,
 * which leaves a copy for the next mode start, whatever asked for that mode.
 * The modulation used to be kept this way too and no longer is: the tools
 * state it after their mode request, as they do the channel, so its copy names
 * a mode and an orphan is dropped instead of applied. Closing it for the jam
 * mode is the same move, and waits on the jamming work.
 *
 * Nothing is kept while the board is idle: a mode starting from idle runs no
 * teardown, so the value the host just set already survives.
 *
 * The E-GO capture length is kept this way too: ubertooth-ego sends it before
 * its mode request, so it cannot be stated for a mode the host has not named
 * yet.
 */
#define KEEP_JAM_MODE   (1 << 0)
#define KEEP_CAP_LEN    (1 << 1)

static volatile uint8_t keep_what;        /* KEEP_* bits, 0 for nothing */
static volatile uint8_t keep_jam_mode;
static volatile uint8_t keep_cap_len;

/* Keep what the request has just set, for the mode start it comes before. */
static void keep_for_next_mode_start(uint8_t what)
{
	if (mode == MODE_IDLE)
		return;
	if (what & KEEP_JAM_MODE)
		keep_jam_mode = jam_mode;
	if (what & KEEP_CAP_LEN)
		keep_cap_len = packet_len;
	keep_what |= what;
}

/*
 * Put the kept E-GO capture length back, and forget it.
 *
 * Its own function because the E-GO mode start happens in two places. One is
 * the dispatch below. The other is an E-GO request that a running E-GO loop
 * absorbs: that does the mode start's work inside the request handler,
 * ego_capture_reset() included, so the length the host stated a moment before
 * has to be put back there as well.
 *
 * Only this bit is cleared. The other absorbable requests call
 * drop_kept_settings(), which throws the whole copy away so it cannot reach a
 * run the host has moved on from; the E-GO request does not, so a jam mode
 * kept by a killed `ubertooth-btle -j` survives it and lands on whatever mode
 * starts next. Nothing today sets both in one run.
 *
 * The same throwing away loses a stated length when an absorbable request for
 * the running mode arrives between the length and the E-GO request: the copy
 * is gone while the E-GO start still resets the live value. ubertooth-ego
 * sends no such request, so this needs another program driving the board, and
 * the length it gets is the mode's default rather than a stale one.
 */
static void apply_kept_cap_len(void)
{
	if (!(keep_what & KEEP_CAP_LEN))
		return;
	keep_what &= (uint8_t)~KEEP_CAP_LEN;
	packet_len = keep_cap_len;
}

/*
 * Drop a copy for a mode request the running loop absorbs.
 *
 * No mode start follows such a request, so the reset that would have thrown
 * the setting away does not run and the live value stands on its own. Keeping
 * the copy would leave it waiting for whatever mode starts next, which is a
 * run the host has moved on from.
 */
static void drop_kept_settings(uint8_t requested)
{
	if (requested == mode)
		keep_what = 0;
}

/*
 * Put back what was stated before the mode request, and forget it.
 *
 * Called from the dispatch after the reset and before the mode's own setup,
 * which still wins.
 *
 * A stop keeps nothing. It ends the run the setting was stated for, and a
 * value left on an idle board would reach the next mode whatever that mode
 * asked for, because a mode starting from idle runs no reset of its own.
 */
static void apply_kept_settings(uint8_t starting_mode)
{
	uint8_t what = keep_what;

	keep_what = 0;

	if (starting_mode == MODE_IDLE)
		return;
	if (what & KEEP_JAM_MODE)
		jam_mode = keep_jam_mode;
	if (what & KEEP_CAP_LEN)
		packet_len = keep_cap_len;
}

/*
 * Returning 0 stalls the request. `data` is the buffer the USB stack filled
 * from the request's data stage and `data_in_len` how many bytes the host
 * actually sent with it, so a case that reads `data` refuses a request
 * carrying less than it reads: that keeps a short request out, and it is also
 * what keeps a device-to-host request of the same number out, since that one
 * reaches the handler with no data stage behind it and a buffer still holding
 * the previous request's bytes. A case whose body may be empty cannot tell
 * those two apart by length, and uses `data_from_host` instead.
 */
static int vendor_request_handler(uint8_t request, uint16_t* request_params,
                                  uint8_t* data, int data_from_host,
                                  unsigned data_in_len, int* data_len)
{
	uint32_t clock;
	size_t length; // string length
	usb_pkt_rx* p = NULL;
	uint16_t reg_val;
	/* The channel a generic transmit asks for, which is a frequency and not
	 * a register value, so it does not share reg_val above. Named apart
	 * from the global `channel` the receives tune, which it must not
	 * shadow. */
	uint16_t tx_channel;
	uint8_t i;
	int32_t drift_ppm;

	switch (request) {

	case UBERTOOTH_PING:
		*data_len = 0;
		break;

	case UBERTOOTH_RX_SYMBOLS:
		/* A plain symbol capture sets nothing up of its own, so this is
		 * all that keeps it from inheriting a running stream's hopping. */
		reset_absorbed_mode_state(MODE_RX_SYMBOLS);
		drop_kept_settings(MODE_RX_SYMBOLS);
		requested_mode = MODE_RX_SYMBOLS;
		*data_len = 0;
		break;

#ifdef TX_ENABLE
	case UBERTOOTH_TX_SYMBOLS:
		hop_mode = HOP_BLUETOOTH;
		requested_mode = MODE_TX_SYMBOLS;
		*data_len = 0;
		break;
#endif

	case UBERTOOTH_GET_USRLED:
		data[0] = (USRLED) ? 1 : 0;
		*data_len = 1;
		break;

	case UBERTOOTH_SET_USRLED:
		if (request_params[0])
			USRLED_SET;
		else
			USRLED_CLR;
		break;

	case UBERTOOTH_GET_RXLED:
		data[0] = (RXLED) ? 1 : 0;
		*data_len = 1;
		break;

	case UBERTOOTH_SET_RXLED:
		if (request_params[0])
			RXLED_SET;
		else
			RXLED_CLR;
		break;

	case UBERTOOTH_GET_TXLED:
		data[0] = (TXLED) ? 1 : 0;
		*data_len = 1;
		break;

	case UBERTOOTH_SET_TXLED:
		if (request_params[0])
			TXLED_SET;
		else
			TXLED_CLR;
		break;

	case UBERTOOTH_GET_1V8:
		data[0] = (CC1V8) ? 1 : 0;
		*data_len = 1;
		break;

	case UBERTOOTH_SET_1V8:
		if (request_params[0])
			CC1V8_SET;
		else
			CC1V8_CLR;
		break;

	case UBERTOOTH_GET_PARTNUM:
		get_part_num(data, data_len);
		break;

	case UBERTOOTH_RESET:
		requested_mode = MODE_RESET;
		break;

	case UBERTOOTH_GET_SERIAL:
		get_device_serial(data, data_len);
		break;

#ifdef UBERTOOTH_ONE
	case UBERTOOTH_GET_PAEN:
		data[0] = (PAEN) ? 1 : 0;
		*data_len = 1;
		break;

	case UBERTOOTH_SET_PAEN:
		if (request_params[0])
			PAEN_SET;
		else
			PAEN_CLR;
		break;

	case UBERTOOTH_GET_HGM:
		data[0] = (HGM) ? 1 : 0;
		*data_len = 1;
		break;

	case UBERTOOTH_SET_HGM:
		if (request_params[0])
			HGM_SET;
		else
			HGM_CLR;
		break;
#endif

#ifdef TX_ENABLE
	case UBERTOOTH_TX_TEST:
		requested_mode = MODE_TX_TEST;
		break;

	case UBERTOOTH_GET_PALEVEL:
		data[0] = cc2400_get(FREND) & FREND_PA_LEVEL;
		*data_len = 1;
		break;

	case UBERTOOTH_SET_PALEVEL:
		if( request_params[0] < 8 ) {
			cc2400_set(FREND, FREND_RESERVED_ONE | request_params[0]);
		} else {
			return 0;
		}
		break;

	case UBERTOOTH_RANGE_TEST:
		requested_mode = MODE_RANGE_TEST;
		break;

	case UBERTOOTH_REPEATER:
		requested_mode = MODE_REPEATER;
		break;
#endif

	case UBERTOOTH_RANGE_CHECK:
		data[0] = rr.valid;
		data[1] = rr.request_pa;
		data[2] = rr.request_num;
		data[3] = rr.reply_pa;
		data[4] = rr.reply_num;
		data[5] = rr.discarded;
		*data_len = UBERTOOTH_RANGE_RESULT_LEN;
		break;

	case UBERTOOTH_STOP:
		requested_mode = MODE_IDLE;
#ifdef STACK_GUARD_TEST
		overflow_the_stack_now = 1;
#endif
		break;

	case UBERTOOTH_GET_MOD:
		data[0] = modulation;
		*data_len = 1;
		break;

	case UBERTOOTH_SET_MOD:
		/* Only the two modulations the radio setup configures. Storing
		 * any other value was silent: cc2400_rx() and cc2400_rx_sync()
		 * write no modem registers for a value they do not recognize, so
		 * the next capture ran with whatever the previous mode had left
		 * in the radio.
		 *
		 * That rules out the two remaining values of enum modulations as
		 * well. No mode implements MOD_80211_FHSS. MOD_NONE means "no
		 * modulation", and the one mode that wants it -- spectrum
		 * analysis -- sets it itself, so there is nothing for a request
		 * to ask for. */
		if (request_params[0] != MOD_BT_BASIC_RATE &&
		    request_params[0] != MOD_BT_LOW_ENERGY)
			return 0;
		modulation = request_params[0];
		/* Stated after the mode request, as the channel is, so a mode
		 * start that is still to come keeps it; see
		 * stated_for_starting_mode(). The radio is programmed from it
		 * at the mode start and again by the capture loop, which is
		 * what lets this arrive after the request. */
		if (requested_mode != mode) {
			stated_modulation = request_params[0];
			stated_for_starting_mode(STATED_MODULATION);
		}
		break;

	case UBERTOOTH_GET_CHANNEL:
		data[0] = channel & 0xFF;
		data[1] = (channel >> 8) & 0xFF;
		*data_len = 2;
		break;

	case UBERTOOTH_SET_CHANNEL:
		/* A frequency the synthesizer cannot reach is refused rather
		 * than pulled up to MIN_FREQ, which is what used to happen: a
		 * request for 2000 MHz tuned 2268 MHz instead and reported
		 * success, so a capture came back on a frequency nobody asked
		 * for. Anything from MIN_FREQ up is kept, including the
		 * deliberately out-of-band frequencies the radio can reach.
		 *
		 * Above MAX_FREQ still means the band sweep, which is how the
		 * host asks for it (it sends 9999). Nothing is refused there.
		 *
		 * Before set_channel_request(), so a refused request leaves the
		 * channel and the hop mode as it found them, and before the
		 * channel is kept for a starting mode, so a mode does not begin
		 * on a frequency the board said it could not have.
		 */
		if (request_params[0] < MIN_FREQ)
			return 0;

		set_channel_request(request_params[0]);
		if (requested_mode != mode) {
			stated_channel = request_params[0];
			stated_for_starting_mode(STATED_CHANNEL);
		}
		break;

	case UBERTOOTH_SET_ISP:
		/* Handed to the mode dispatch rather than done here, for the
		 * reason UBERTOOTH_RESET is: the on-chip programmer takes the
		 * board off the bus, and this transfer is not finished when this
		 * handler returns -- its status stage is written afterwards and
		 * waits in the endpoint 0 buffer for the host's next IN token, up
		 * to a whole 1 ms frame away. Going away from in here threw that
		 * packet away, so `ubertooth-util -i` reported a board that had
		 * timed out (-7) for a request the board had accepted.
		 *
		 * The price is that the programmer is no longer reached from
		 * inside the USB service pass: the running mode's loop has to
		 * notice this and return to the dispatch, as it does for a reset
		 * request. A loop that has stopped doing that is the wedge the
		 * watchdog resets the board for. */
		requested_mode = MODE_ISP;
		break;

	case UBERTOOTH_FLASH:
		/* The bootloader is reached by resetting, so MODE_RESET below is
		 * what tells the watchdog the reset was asked for. Saying it here
		 * instead would excuse a board that wedged before getting there. */
		bootloader_ctrl = DFU_MODE;
		requested_mode = MODE_RESET;
		break;

	case UBERTOOTH_GET_WATCHDOG:
		watchdog_report(data);
		*data_len = UBERTOOTH_WATCHDOG_INFO_LEN;
		break;

	case UBERTOOTH_GET_RADIO_TIMEOUTS:
		cc2400_wait_report(data);
		*data_len = UBERTOOTH_RADIO_TIMEOUTS_LEN;
		break;

	case UBERTOOTH_SPECAN:
		if (request_params[0] < 2049 || request_params[0] > 3072 ||
				request_params[1] < 2049 || request_params[1] > 3072 ||
				request_params[1] < request_params[0])
			return 0;
		low_freq = request_params[0];
		high_freq = request_params[1];
		requested_mode = MODE_SPECAN;
		*data_len = 0;
		break;

	case UBERTOOTH_RX_GENERIC:
		requested_mode = MODE_RX_GENERIC;
		*data_len = 0;
		break;

	case UBERTOOTH_LED_SPECAN:
		/* the threshold is -N dBm, N as ubertooth-util accepts it; refuse
		 * anything else, as the other range-checked requests do */
		if (!rssi_threshold_n_valid(request_params[0]))
			return 0;
		rssi_threshold = rssi_threshold_from_n(request_params[0]);
		requested_mode = MODE_LED_SPECAN;
		*data_len = 0;
		break;

	case UBERTOOTH_GET_REV_NUM:
		data[0] = 0x00;
		data[1] = 0x00;

		length = strlen(GIT_REVISION);
		if (length > UBERTOOTH_REVISION_MAX)
			length = UBERTOOTH_REVISION_MAX;
		data[2] = (u8)length;
		memcpy(&data[3], GIT_REVISION, length);
		*data_len = 2 + 1 + length;
		break;

	case UBERTOOTH_GET_COMPILE_INFO:
		length = strlen(compile_info);
		if (length > UBERTOOTH_COMPILE_INFO_MAX)
			length = UBERTOOTH_COMPILE_INFO_MAX;
		data[0] = (u8)length;
		memcpy(&data[1], compile_info, length);
		*data_len = 1 + length;
		break;

	/* The same 64 bytes this image carries at a fixed offset, so a board and
	 * a firmware file describe themselves in one format. Keep this identical
	 * to the one in btbr/src/btctl/vendor_request_handler.c. */
	case UBERTOOTH_GET_FW_INFO:
		memcpy(data, &fw_info_block, UBERTOOTH_FW_INFO_LENGTH);
		*data_len = UBERTOOTH_FW_INFO_LENGTH;
		break;

	/* What this board's bootloader is. Keep this identical to the one in
	 * btbr/src/btctl/vendor_request_handler.c. */
	case UBERTOOTH_GET_BOOTLOADER_INFO:
		/* Refused until startup has worked the report out, rather than
		 * answered with a hash of zero. */
		if (!bl_report_build(data))
			return 0;
		*data_len = UBERTOOTH_BL_REPORT_LENGTH;
		break;

	case UBERTOOTH_GET_BOARD_ID:
		data[0] = BOARD_ID;
		*data_len = 1;
		break;

	case UBERTOOTH_SET_SQUELCH:
		cs_threshold_req = (int8_t)request_params[0];
		cs_threshold_calc_and_set(channel);
		break;

	case UBERTOOTH_GET_SQUELCH:
		data[0] = cs_threshold_req;
		*data_len = 1;
		break;

	case UBERTOOTH_SET_BDADDR:
		if (data_in_len < 16) /* an 8-byte address and an 8-byte syncword */
			return 0;
		target.address = 0;
		target.syncword = 0;
		for(int i=0; i < 8; i++) {
			target.address |= (uint64_t)data[i] << 8*i;
		}
		for(int i=0; i < 8; i++) {
			target.syncword |= (uint64_t)data[i+8] << 8*i;
		}
		precalc();
		break;

	case UBERTOOTH_START_HOPPING:
		if (data_in_len < 6) /* a 4-byte CLKN offset and a 2-byte 100 ns one */
			return 0;
		clkn_offset = 0;
		for(int i=0; i < 4; i++) {
			clkn_offset <<= 8;
			clkn_offset |= data[i];
		}
		reset_absorbed_mode_state(MODE_BT_FOLLOW);
		hop_mode = HOP_BLUETOOTH;
		dma_discard = 1;
		DIO_SSEL_SET;
		clk100ns_offset = (data[4] << 8) | (data[5] << 0);
		requested_mode = MODE_BT_FOLLOW;
		break;

	/*
	 * Start AFH detection: hop to the channels the piconet is not known to
	 * use and stream what arrives there, from an empty map that the host
	 * fills in with UBERTOOTH_SET_AFHMAP as packets come in. This is the
	 * whole request the host needs; it used to send UBERTOOTH_RX_SYMBOLS on
	 * top, which took the board straight back out of this mode.
	 */
	case UBERTOOTH_AFH:
		reset_absorbed_mode_state(MODE_AFH);
		hop_mode = HOP_AFH;
		requested_mode = MODE_AFH;

		clear_afh_map();
		afh_enabled = 1;
		break;

	case UBERTOOTH_HOP:
		do_hop = 1;
		break;

	case UBERTOOTH_SET_CLOCK:
		if (data_in_len < 4)
			return 0;
		clock = data[0] | data[1] << 8 | data[2] << 16 | data[3] << 24;
		clkn = clock;
		cs_threshold_calc_and_set(channel);
		break;

	case UBERTOOTH_SET_AFHMAP:
		if (data_in_len < sizeof(afh_map))
			return 0;
		bluetooth_set_afh_map(data);
		/* Kept for the mode request this followed, as a channel is: a
		 * follow states its map just after asking for the hopping
		 * (ubertooth-follow), and on a board left in a mode that
		 * captures nothing the teardown's clear_afh_map() runs a moment
		 * after that request was answered. Without the copy the map
		 * arriving next is thrown away, and the follow hops all 79
		 * channels instead of the piconet's.
		 *
		 * Only when the mode the host asked for is not the one running,
		 * which is the test the channel request makes. A map sent by a
		 * capture already in that mode -- AFH detection sends one
		 * whenever it finds another channel -- needs no copy, and
		 * keeping one would hand that run's map to the next capture
		 * asking for the same mode.
		 *
		 * `mode` is the mode the dispatch last entered, so a request
		 * absorbed into a streaming run still takes the copy. Nothing
		 * reads it there: the next dispatched mode start drops a copy
		 * that is not its own. */
		if (requested_mode != mode) {
			for (i = 0; i < sizeof(afh_map); i++)
				stated_afh_map[i] = data[i];
			stated_for_starting_mode(STATED_AFH_MAP);
		}
		*data_len = 10;
		break;

	case UBERTOOTH_CLEAR_AFHMAP:
		clear_afh_map();
		/* Including a map stated for a mode whose start is still to come:
		 * a host that clears the map means the capture to hop the whole
		 * band, and applying the earlier map at the mode start would be
		 * the opposite of what it asked for. */
		stated_what &= ~STATED_AFH_MAP;
		*data_len = 10;
		break;

	case UBERTOOTH_GET_CLOCK:
		clock = clkn;
		for(int i=0; i < 4; i++) {
			data[i] = (clock >> (8*i)) & 0xff;
		}
		*data_len = 4;
		break;

	case UBERTOOTH_TRIM_CLOCK:
		if (data_in_len < 2)
			return 0;
		clk100ns_offset = (data[0] << 8) | (data[1] << 0);
		break;

	case UBERTOOTH_FIX_CLOCK_DRIFT:
		if (data_in_len < 2)
			return 0;
		/*
		 * Each request adds to a running figure, and the figure is kept
		 * within what the correction can act on: one correction every
		 * CLKN tick is CLK_DRIFT_PPM_MAX, and nothing beyond it corrects
		 * any faster.
		 *
		 * Added in 32 bits and then stored, because the figure is 16 bits
		 * wide: a run of requests at the largest useful value used to
		 * walk past what it holds, and an overflowed figure comes back
		 * with the other sign -- the board would then speed its clock up
		 * because the host had asked it to slow down.
		 */
		drift_ppm = (int32_t)clk_drift_ppm +
		            (int16_t)((data[0] << 8) | (data[1] << 0));
		if (drift_ppm > CLK_DRIFT_PPM_MAX)
			drift_ppm = CLK_DRIFT_PPM_MAX;
		else if (drift_ppm < -CLK_DRIFT_PPM_MAX)
			drift_ppm = -CLK_DRIFT_PPM_MAX;
		clk_drift_ppm = (int16_t)drift_ppm;

		/* How many CLKN ticks apart the corrections go. The bound above
		 * is where the division stops being useful, and a correction
		 * interval of zero is how the handler below says "do not correct
		 * at all", so the floor stands as well: between them, the fastest
		 * this can correct is once every tick. */
		// Too slow
		if (clk_drift_ppm < 0) {
			clk_drift_correction = CLK_DRIFT_PPM_MAX / (uint16_t)(-clk_drift_ppm);
			if (clk_drift_correction == 0)
				clk_drift_correction = 1;
			clkn_next_drift_fix = clkn_last_drift_fix + clk_drift_correction;
		}
		// Too fast
		else if (clk_drift_ppm > 0) {
			clk_drift_correction = CLK_DRIFT_PPM_MAX / clk_drift_ppm;
			if (clk_drift_correction == 0)
				clk_drift_correction = 1;
			clkn_next_drift_fix = clkn_last_drift_fix + clk_drift_correction;
		}
		// Don't trim
		else {
			clk_drift_correction = 0;
			clkn_next_drift_fix = 0;
		}

		break;

	case UBERTOOTH_BTLE_SNIFFING:
		le.do_follow = request_params[0];
		*data_len = 0;

		do_hop = 0;
		hop_mode = HOP_BTLE;
		drop_kept_settings(MODE_BT_FOLLOW_LE);
		requested_mode = MODE_BT_FOLLOW_LE;

		usb_queue_init();
		cs_threshold_calc_and_set(channel);
		break;

	case UBERTOOTH_GET_ACCESS_ADDRESS:
		for(int i=0; i < 4; i++) {
			data[i] = (le.access_address >> (8*i)) & 0xff;
		}
		*data_len = 4;
		break;

	case UBERTOOTH_SET_ACCESS_ADDRESS:
		if (data_in_len < 4)
			return 0;
		le_set_access_address(data[0] | data[1] << 8 | data[2] << 16 | data[3] << 24);
		/* The only request that gives promiscuous mode an access
		 * address to follow. It has to be a setting of its own:
		 * UBERTOOTH_BTLE_SET_TARGET names a device address and no
		 * access address at all, so a target set for follow mode must
		 * not make promiscuous mode skip its discovery half. */
		le.access_address_set = 1;
		break;

	case UBERTOOTH_GET_CRC_VERIFY:
		data[0] = le.crc_verify ? 1 : 0;
		*data_len = 1;
		break;

	case UBERTOOTH_SET_CRC_VERIFY:
		le.crc_verify = request_params[0] ? 1 : 0;
		break;

	case UBERTOOTH_POLL:
		p = dequeue();
		if (p != NULL) {
			memcpy(data, (void *)p, sizeof(usb_pkt_rx));
			*data_len = sizeof(usb_pkt_rx);
		} else {
			data[0] = 0;
			*data_len = 1;
		}
		break;

	case UBERTOOTH_BTLE_PROMISC:
		*data_len = 0;

		hop_mode = HOP_NONE;
		drop_kept_settings(MODE_BT_PROMISC_LE);
		requested_mode = MODE_BT_PROMISC_LE;

		usb_queue_init();
		cs_threshold_calc_and_set(channel);
		break;

	case UBERTOOTH_READ_REGISTER:
		reg_val = cc2400_get(request_params[0]);
		data[0] = (reg_val >> 8) & 0xff;
		data[1] = reg_val & 0xff;
		*data_len = 2;
		break;

	case UBERTOOTH_WRITE_REGISTER:
		cc2400_set(request_params[0] & 0xff, request_params[1]);
		break;

	case UBERTOOTH_WRITE_REGISTERS:
		/* A count the buffer cannot hold is refused before the count
		 * is used at all. */
		if (request_params[0] > WRITE_REGISTERS_MAX)
			return 0;
		/* a register number and a 16-bit value per register */
		if (data_in_len < request_params[0] * 3u)
			return 0;
		for (unsigned reg = 0; reg < request_params[0]; reg++) {
			reg_val = (data[(reg*3)+1] << 8) | data[(reg*3)+2];
			cc2400_set(data[reg*3], reg_val);
		}
		break;

	case UBERTOOTH_READ_ALL_REGISTERS:
		for (i = 0; i < READ_ALL_REGISTERS_COUNT; i++) {
			reg_val = cc2400_get(i);
			data[i*3] = i;
			data[(i*3)+1] = (reg_val >> 8) & 0xff;
			data[(i*3)+2] = reg_val & 0xff;
		}
		*data_len = READ_ALL_REGISTERS_LEN;
		break;

#ifdef TX_ENABLE
	case UBERTOOTH_TX_GENERIC_PACKET:
		/*
		 * The payload length is a byte of the request, so it is checked
		 * before anything is copied: tx_pkt has room for DMA_SIZE
		 * payload bytes and other globals sit behind it, and the length
		 * reaches the radio's FIFO in tx_generic(). The length the host
		 * declared bounds the copy too, so it cannot run past what was
		 * sent -- which is not the same as the bytes being the host's: a
		 * device-to-host request reaches this handler before any data
		 * stage, with whatever the last request left in the buffer.
		 */
		if (data_in_len < TX_GENERIC_HEADER_LEN)
			return 0;
		i = data[offsetof(generic_tx_packet, length)];
		if (i > DMA_SIZE || data_in_len - TX_GENERIC_HEADER_LEN < i)
			return 0;
		/*
		 * The channel reaches FSDIV unchanged in tx_generic(), so only
		 * a frequency the synthesizer can reach is accepted. Taken, one
		 * outside that range would have the transmit wait out its lock
		 * timeout and send nothing, and leave that frequency in the
		 * register for whatever ran next.
		 *
		 * Both ends are refused, which is where this differs from
		 * UBERTOOTH_SET_CHANNEL: there a frequency above MAX_FREQ is
		 * how the host asks for the band sweep, and nothing here
		 * sweeps.
		 */
		tx_channel = data[offsetof(generic_tx_packet, channel)]
		           | ((uint16_t)data[offsetof(generic_tx_packet, channel) + 1] << 8);
		if (tx_channel < MIN_FREQ || tx_channel > MAX_FREQ)
			return 0;
		/*
		 * A burst is running, and it sends from tx_pkt: this request
		 * would splice its payload into the packets the burst has left
		 * and then lose the count it asked for, because the burst read
		 * one already. Refused instead -- the stack stalls a request
		 * the handler returns 0 for, so the host is told -- and refused
		 * before the copy, or the splice has happened by the time the
		 * refusal is decided. Nothing shipped here sends a second
		 * request during a burst, since ubertooth-tx waits for the
		 * board.
		 *
		 * This covers the request that would break the burst it lands
		 * in, and nothing else: a register write or a power level
		 * serviced between packets reaches the radio, changes the
		 * packets the burst has left, and is then put back by the
		 * restore at the end of it.
		 */
		if (mode == MODE_TX_GENERIC)
			return 0;
		memcpy(&tx_pkt, data, TX_GENERIC_HEADER_LEN + i);
		/* wValue is how many of this packet to send. A host that leaves
		 * it 0 gets one, which is what every host did before the field
		 * meant anything. */
		tx_pkt_repeat = request_params[0] > 0 ? request_params[0] : 1;
		requested_mode = MODE_TX_GENERIC;
		*data_len = 0;
		break;

	case UBERTOOTH_BTLE_PERIPHERAL:
		if (data_in_len < 6)
			return 0;
		memcpy(peripheral_mac_address, data, 6);
		drop_kept_settings(MODE_BT_PERIPHERAL_LE);
		requested_mode = MODE_BT_PERIPHERAL_LE;
		break;
#endif

	case UBERTOOTH_LE_SET_ADV_DATA:
		/* An empty payload is a request in its own right, so the
		 * direction is what tells it apart from a read of this number;
		 * every other case that reads data needs a byte of it and is
		 * refused by its own length check.
		 *
		 * The bound is what the advertising mode can transmit, so a
		 * program that asks for more is told here, where it can see
		 * which request the board refused. Taking it and then dropping
		 * out of the mode left the board quietly idle instead. */
		if (!data_from_host || data_in_len > ADV_IND_MAX_DATA)
			return 0;
		le_adv_len = data_in_len;
		memcpy(le_adv_data, data, le_adv_len);
		break;

	case UBERTOOTH_BTLE_SET_TARGET:
		if (data_in_len < 7) /* six address bytes and a mask length */
			return 0;
		// Addresses appear in packets in reverse-octet order.
		// Store the target address in reverse order so that we can do a simple memcmp later
		if (data[6] > 48) {
			return 0; // invalid mask
		}
		else if (data[6] == 0) {
			le.target_set = 0;
			memset(le.target, 0, 6);
			memset(le.target_mask, 0, 6);
		} else {
			unsigned last;

			for (i = 0; i < 6; ++i)
				le.target[i] = data[5-i];

			// compute mask
			memset(le.target_mask, 0, 6);
			for (i = 5; data[6] > 8; --i, data[6] -= 8) {
				le.target_mask[i] = 0xff;
			}
			last = i;

			if (data[6] > 0) {
				uint8_t final_byte = 0;
				for (i = 0; i < data[6]; ++i) {
					final_byte >>= 1;
					final_byte |= 0b10000000;
				}
				le.target_mask[last] = final_byte;
			}

			// in case the user specifies a bad mask
			for (i = 0; i < 5; ++i)
				le.target[i] &= le.target_mask[i];

			le.target_set = 1;
		}
		break;

	case UBERTOOTH_CANCEL_FOLLOW:
		// cancel following an active connection
		cancel_follow = 1;
		break;

#ifdef TX_ENABLE
	case UBERTOOTH_JAM_MODE:
		jam_mode = request_params[0];
		/* ubertooth-btle sends this before its mode request too. */
		keep_for_next_mode_start(KEEP_JAM_MODE);
		break;
#endif

	case UBERTOOTH_EGO:
		/*
		 * Checked before anything is stored, so a refused request
		 * leaves no mode behind for a later run to start in: asking to
		 * interfere on a firmware built without transmit is refused
		 * here, and ego_main() has no state machine for a mode it does
		 * not know.
		 */
		switch (request_params[0]) {
		case EGO_FOLLOW:
		case EGO_CONTINUOUS_RX:
#ifdef TX_ENABLE
		case EGO_JAM:
#endif
			break;
		default:
			return 0;
		}

		ego_mode = request_params[0];

		/*
		 * A board left capturing by a killed ubertooth-ego absorbs this
		 * request, the way the symbol captures absorb theirs: its loop
		 * runs while MODE_EGO is the mode asked for, so no mode starts
		 * and the reset the dispatch runs never does. Do that mode
		 * start's work here instead: put the access code back to the
		 * one ubertooth-ego(1) documents, forget the hopping, and tell
		 * the loop to start its state machine over.
		 *
		 * The last of those is what makes this a mode start at all.
		 * Without it the state machine the first run entered carried
		 * on, and the mode stored above was never read: an
		 * `ubertooth-ego -r` after a killed `-f` captured in follow
		 * mode. The flag is set after the mode, because the loop reads
		 * the mode only once it has seen the flag.
		 */
		if (mode == MODE_EGO && requested_mode == MODE_EGO) {
			ego_capture_reset();
			/* The length this run stated a moment ago, which the
			 * reset has just put back to the default: the host
			 * sends it before this request, so there is nothing
			 * else to put it back. */
			apply_kept_cap_len();
			forget_hop_state();
			ego_restart = 1;
		}
		requested_mode = MODE_EGO;
		break;

	case UBERTOOTH_RFCAT_SUBCMD:
		if (!rfcat_subcommand(request_params[0], data, data_in_len))
			return 0;
		/* An access code stated after the mode request belongs to the
		 * capture that is starting, and the mode start puts the code
		 * back to its default: keep a copy for it. */
		if (request_params[0] == RFCAT_SET_AA && requested_mode != mode) {
			stated_access_code = access_code;
			stated_access_code_len = access_code_len;
			stated_for_starting_mode(STATED_ACCESS_CODE);
		}
		/* The capture length comes before the mode request instead, so
		 * it is kept rather than stated: the mode start puts it back to
		 * the default, and without a copy `ubertooth-ego -l` would
		 * capture at that default. */
		if (request_params[0] == RFCAT_CAP_LEN)
			keep_for_next_mode_start(KEEP_CAP_LEN);
		break;

	case UBERTOOTH_XMAS:
		requested_mode = MODE_XMAS;
		break;

	case UBERTOOTH_IDENTIFY:
		requested_mode = MODE_IDENTIFY;
		break;

	default:
		return 0;
	}
	return 1;
}

/* Update CLKN. */
void TIMER0_IRQHandler()
{
	if (T0IR & TIR_MR0_Interrupt) {

		clkn += clkn_offset + 1;
		clkn_offset = 0;

		uint32_t le_clk = (clkn - le.conn_epoch) & 0x03;

		/* Trigger hop based on mode */

		/* NONE or SWEEP -> 25 Hz */
		if (hop_mode == HOP_NONE || hop_mode == HOP_SWEEP) {
			if ((clkn & 0x7f) == 0)
				do_hop = 1;
		}
		/* BLUETOOTH -> 1600 Hz */
		else if (hop_mode == HOP_BLUETOOTH) {
			if ((clkn & 0x1) == 0)
				do_hop = 1;
		}
		/* BLUETOOTH Low Energy -> 7.5ms - 4.0s in multiples of 1.25 ms */
		else if (hop_mode == HOP_BTLE) {
			// Only hop if connected
			if (le.link_state == LINK_CONNECTED && le_clk == 0) {
				--le.interval_timer;
				if (le.interval_timer == 0) {
					do_hop = 1;
					++le.conn_count;
					le.interval_timer = le.conn_interval;
				} else {
					TXLED_CLR; // hack!
				}
			}
		}
		else if (hop_mode == HOP_AFH) {
			if( (last_hop + hop_timeout) == clkn ) {
				do_hop = 1;
			}
		}

		// Fix linear clock drift deviation
		if(clkn_next_drift_fix != 0 && clk100ns_offset == 0) {
			if(clkn >= clkn_next_drift_fix) {

				// Too fast
				if(clk_drift_ppm >= 0) {
					clk100ns_offset = 1;
				}

				// Too slow
				else {
					clk100ns_offset = 6249;
				}
				clkn_last_drift_fix = clkn;
				clkn_next_drift_fix = clkn_last_drift_fix + clk_drift_correction;
			}
		}

		// Negative clock correction
		if(clk100ns_offset > 3124)
			clkn += 2;

		T0MR0 = 3124 + clk100ns_offset;
		clk100ns_offset = 0;

		/* Ack interrupt */
		T0IR = TIR_MR0_Interrupt;
	}
}

/* The carrier-sense edge on GIO6 arrives here, on the GPIO interrupt's shared
 * EINT3 vector. A board whose GIO6 cannot raise that interrupt (see
 * GIO6_HAS_INTERRUPT in ubertooth.h) never enables EINT3, so it gets no
 * handler from here and keeps the weak default; the badge has its own in
 * ubertooth.c for its button. */
#if GIO6_HAS_INTERRUPT
void EINT3_IRQHandler()
{
	/* TODO - check specific source of shared interrupt */
	GIO6_INT_CLR;           // clear interrupt
	DIO_SSEL_CLR;           // enable SPI
	cs_trigger  = 1;        // signal trigger
	if (hop_mode == HOP_BLUETOOTH)
		dma_discard = 0;

}
#endif // GIO6_HAS_INTERRUPT

/*
 * Sleep (busy wait) for 'millis' milliseconds
 * Needs clkn. Be sure to call clkn_init() before using it.
 */
static void msleep(uint32_t millis)
{
	uint32_t now = (clkn & 0xffffff);
	uint32_t stop_at = now + millis * 10000 / CLK100NS_PER_CLKN; // millis -> clkn ticks

	// handle clkn overflow
	if (stop_at >= ((uint32_t)1<<28)) {
		stop_at -= ((uint32_t)1<<28);
		while ((clkn & 0xffffff) >= now || (clkn & 0xffffff) < stop_at);
	} else {
		while ((clkn & 0xffffff) < stop_at);
	}
}

void legacy_DMA_IRQHandler();
void le_DMA_IRQHandler();
void DMA_IRQHandler(void) {
	if (mode == MODE_BT_FOLLOW_LE)
		le_DMA_IRQHandler();
	else
		legacy_DMA_IRQHandler();

	// DMA channel 7: debug UART
	if (DMACIntStat & (1 << 7)) {
		// TC -- DMA completed, unset flag so another printf can occur
		if (DMACIntTCStat & (1 << 7)) {
			DMACIntTCClear = (1 << 7);
			debug_dma_active = 0;
		}
		// error -- blow up
		if (DMACIntErrStat & (1 << 7)) {
			DMACIntErrClr = (1 << 7);
			// FIXME do something better here
			USRLED_SET;
			/* This loop never returns and never services USB, so the
			 * watchdog resets the board a few seconds later. The mark
			 * is what says so afterwards: without it,
			 * ubertooth-util -w names whatever the board was doing
			 * before the transfer failed. */
			watchdog_mark(WATCHDOG_SITE_DEBUG_DMA);
			while (1) { }
		}
	}
}

/*
 * Every mode that receives through the symbol DMA has to be listed here: the
 * buffer swap below is what makes a transfer complete for the mode loop
 * (rx_tc), so a mode left out of the list waits for a buffer that is never
 * handed over. MODE_AFH was missing while nothing ran under it.
 */
void legacy_DMA_IRQHandler()
{
	if ( mode == MODE_RX_SYMBOLS
	   || mode == MODE_BT_FOLLOW
	   || mode == MODE_AFH
	   || mode == MODE_SPECAN
	   || mode == MODE_BT_FOLLOW_LE
	   || mode == MODE_BT_PROMISC_LE
	   || mode == MODE_BT_PERIPHERAL_LE
	   || mode == MODE_RX_GENERIC)
	{
		/* interrupt on channel 0 */
		if (DMACIntStat & (1 << 0)) {
			if (DMACIntTCStat & (1 << 0)) {
				DMACIntTCClear = (1 << 0);

				if (hop_mode == HOP_BLUETOOTH)
					DIO_SSEL_SET;

				idle_buf_clk100ns  = CLK100NS;
				idle_buf_clkn_high = (clkn >> 20) & 0xff;
				idle_buf_channel   = channel;

				/* Keep buffer swapping in sync with DMA. */
				volatile uint8_t* tmp = active_rxbuf;
				active_rxbuf = idle_rxbuf;
				idle_rxbuf = tmp;

				++rx_tc;
			}
			if (DMACIntErrStat & (1 << 0)) {
				DMACIntErrClr = (1 << 0);
				++rx_err;
				watchdog_count_transfer_error();
			}
		}
	}
}

/*
 * Put the radio and the board back to a known state.
 *
 * This decides nothing about what runs next: the main loop's dispatch owns
 * `mode` and `requested_mode`, and runs this whenever a mode is left.
 *
 * Keeping the two apart is what stops the teardown from dropping a mode
 * request that arrives while it runs. This used to end with
 * `requested_mode = MODE_IDLE`, and the LE paths call it from inside
 * bt_generic_le() and bt_le_sync() with USB interrupts enabled, so a request
 * serviced during the teardown was overwritten: the board stayed idle while
 * the host, whose vendor request had succeeded, believed the mode had started.
 */
static void cc2400_idle()
{
	cc2400_strobe(SRFOFF);
	cc2400_wait_fs_unlock();

	/*
	 * Drop the manual signal overrides the mode that is ending may have
	 * written, so the next mode starts with the radio front end a freshly
	 * booted board has.
	 *
	 * Nothing used to do this. The LE PHY forces the low-noise amplifier
	 * and mixers on (MANAND 0x7ffe, le_phy.c) and the receive paths hold
	 * the receive gain amplifier out of reset (0x7fff), and a mode that
	 * writes MANAND itself was unaffected -- but the two spectrum
	 * analyzers do not, so what they measured depended on which mode had
	 * run before them on that board.
	 */
	cc2400_set(MANAND, MANAND_RESET_VALUE);

#ifdef UBERTOOTH_ONE
	PAEN_CLR;
	HGM_CLR;
#endif

	RXLED_CLR;
	TXLED_CLR;
	USRLED_CLR;

	clkn_stop();
	dio_ssp_stop();
	cs_reset();
	rssi_reset();

	/* hopping stuff */
	hop_mode = HOP_NONE;
	do_hop = 0;
	channel = 2441;
	hop_direct_channel = 0;
	hop_timeout = 158;
	requested_channel = 0;
	le_adv_channel = BT_FREQ_MIN;

	/*
	 * The AFH map goes with the mode that was using it. Nothing used to
	 * clear it, so a board that had once run AFH detection or been given a
	 * map kept hopping on it: the next ubertooth-rx, -tx or -ducky hopped
	 * adaptively on a channel set found for someone else's piconet, while
	 * the host reported that it was not using AFH. A mode that wants a map
	 * sends one once it has started.
	 */
	clear_afh_map();

	/* bulk USB stuff */
	idle_buf_clkn_high = 0;
	idle_buf_clk100ns = 0;
	idle_buf_channel = 0;
	dma_discard = 0;
	status = 0;

	/* operation mode */
	jam_mode = JAM_NONE;

	/* Not ego_mode. This teardown runs between the request that stores it
	 * and the dispatch that reads it, so resetting it here would throw
	 * away the mode that request asked for. Nothing else writes it, and
	 * MODE_EGO is only ever asked for by that request. */

	modulation = MOD_BT_BASIC_RATE;

	/* specan stuff */
	low_freq = 2400;
	high_freq = 2483;
	rssi_threshold = -30;

	target.address = 0;
	target.syncword = 0;
}

/*
 * What a polled mode loop does between radio status polls instead of spinning.
 *
 * USB in this firmware is polled from whichever mode loop is running
 * (handle_usb()), and endpoint 0 -- the one control requests arrive on -- is
 * single-buffered. A loop that polls once per pass is therefore a whole pass
 * behind the host: a SETUP packet arriving while the previous transfer's
 * status packet is still in the endpoint is read back short, decodes to a
 * request this firmware does not know, and lpcusb halts the endpoint. The host
 * sees a stalled control transfer (-9). led_specan() lost one every few
 * samples that way; Bluetooth follow mode loses roughly one GET_CHANNEL in
 * twenty, because the loop's pass is short but the hop in the middle of it is
 * not.
 *
 * Passing this into the bounded radio waits keeps endpoint 0 within a few
 * microseconds of the host, and keeps it serviced between the waits'
 * bit-banged CC2400 transfers rather than inside one.
 *
 * The alternative the six interrupt-driven modes use -- enabling the USB
 * interrupt for the duration of the mode -- also works, and was measured
 * working. A control request that drives the radio (a register read or write,
 * the squelch, the channel) can then run while a loop is mid-transfer. For a
 * register transfer it waits, because cc2400_spi() masks interrupts for its
 * length; for one of the longer FIFO transfers, which are not masked, it still
 * cuts in. Polling keeps the servicing between transfers, where it has always
 * been.
 *
 * clkn is the bulk stream's keep-alive timestamp, and it stands still in the
 * modes that never start the Bluetooth clock: led_specan(), the range test and
 * the transmit test. The repeater does start it, for the timing of its reply.
 * None of these modes streams bulk packets, so there is no keep-alive to be
 * late either way. Every other caller has the clock running.
 */
void service_usb(void)
{
	handle_usb(clkn);
}

/* Whether the Bluetooth receive below configures the radio for this
 * modulation. The other values of enum modulations reach no modem register
 * here: the generic receive is set up by the host, and spectrum analysis uses
 * no demodulator at all. */
static int programs_modulation(uint8_t m)
{
	return m == MOD_BT_BASIC_RATE || m == MOD_BT_LOW_ENERGY;
}

/*
 * The frequency deviation the demodulator expects for one of those two, as
 * MDMCTRL holds it.
 *
 * In one place rather than in each caller, so that the capture loop -- which
 * writes this register again when the host states a modulation after its mode
 * request -- writes what the mode start would have written.
 */
static u16 mdmctrl_deviation(uint8_t m)
{
	if (m == MOD_BT_BASIC_RATE)
		return 0x0029; // 160 kHz frequency deviation
	return 0x0040;         // 250 kHz frequency deviation
}

/* start un-buffered rx */
static void cc2400_rx()
{
	if (programs_modulation(modulation)) {
		u16 mdmctrl = mdmctrl_deviation(modulation);

		cc2400_set(MANAND,  0x7fff);
		cc2400_set(LMTST,   0x2b22);
		cc2400_set(MDMTST0, 0x134b); // without PRNG
		cc2400_set(GRMDM,   0x0101); // un-buffered mode, GFSK
		// 0 00 00 0 010 00 0 00 0 1
		//      |  | |   |  +--------> CRC off
		//      |  | |   +-----------> sync word: 8 MSB bits of SYNC_WORD
		//      |  | +---------------> 2 preamble bytes of 01010101
		//      |  +-----------------> not packet mode
			//      +--------------------> un-buffered mode
		cc2400_set(FSDIV,   channel - 1); // 1 MHz IF
		cc2400_set(MDMCTRL, mdmctrl);
	}

	// Set up CS register
	cs_threshold_calc_and_set(channel);

	clkn_start();

	/* Bounded, so a radio that stops answering cannot keep the mode loop
	 * below from ever starting -- and that loop is what services USB, so a
	 * board stuck here answers no control request until it is re-plugged.
	 *
	 * No service callback: this is the setup the mode is about to stream
	 * with, and a control request serviced here could change the channel or
	 * the modulation between the register writes above and the receive
	 * strobe below. The wait happens once per mode start, not once per hop,
	 * so bounding it is enough. */
	cc2400_wait_xosc_stable(NULL);
	cc2400_strobe(SFSON);
	cc2400_wait_fs(1, NULL);
	cc2400_strobe(SRX);
#ifdef UBERTOOTH_ONE
	PAEN_SET;
	HGM_SET;
#endif
}

/* start un-buffered rx */
static void cc2400_rx_sync(u32 sync)
{
	u16 grmdm, mdmctrl;

	if (modulation == MOD_BT_BASIC_RATE) {
		mdmctrl = 0x0029; // 160 kHz frequency deviation
		grmdm = 0x0461; // un-buffered mode, packet w/ sync word detection
		// 0 00 00 1 000 11 0 00 0 1
		//   |  |  | |   |  +--------> CRC off
		//   |  |  | |   +-----------> sync word: 32 MSB bits of SYNC_WORD
		//   |  |  | +---------------> 0 preamble bytes of 01010101
		//   |  |  +-----------------> packet mode
		//   |  +--------------------> un-buffered mode
		//   +-----------------------> sync error bits: 0

	} else if (modulation == MOD_BT_LOW_ENERGY) {
		mdmctrl = 0x0040; // 250 kHz frequency deviation
		grmdm = 0x0561; // un-buffered mode, packet w/ sync word detection
		// 0 00 00 1 010 11 0 00 0 1
		//   |  |  | |   |  +--------> CRC off
		//   |  |  | |   +-----------> sync word: 32 MSB bits of SYNC_WORD
		//   |  |  | +---------------> 2 preamble bytes of 01010101
		//   |  |  +-----------------> packet mode
		//   |  +--------------------> un-buffered mode
		//   +-----------------------> sync error bits: 0

	} else {
		/* oops */
		return;
	}

	cc2400_set(MANAND,  0x7fff);
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

	cc2400_set(FSDIV,   channel - 1); // 1 MHz IF
	cc2400_set(MDMCTRL, mdmctrl);

	// Set up CS register
	cs_threshold_calc_and_set(channel);

	clkn_start();

	/* Bounded without a service callback, as in cc2400_rx(). */
	cc2400_wait_xosc_stable(NULL);
	cc2400_strobe(SFSON);
	cc2400_wait_fs(1, NULL);
	cc2400_strobe(SRX);
#ifdef UBERTOOTH_ONE
	PAEN_SET;
	HGM_SET;
#endif
}

#ifdef TX_ENABLE
/* Polls before the wait for the radio's FIFO to empty gives up. The loop
 * compiles to five instructions, about 80 nanoseconds at 100 MHz, so this is
 * on the order of fifteen milliseconds -- far longer than the 256 microseconds
 * a full FIFO takes to go out at 1 megabit per second, and so out of reach of
 * a radio that is transmitting at all. */
#define FIFO_EMPTY_POLLS 200000

/*
 * Wait, with a bound, for the radio to have sent what is in its FIFO, so the
 * next piece of a packet longer than the FIFO can go in.
 *
 * GIO6 has to be configured as the radio's FIFO_EMPTY signal for this to mean
 * anything; both callers do that before they start writing and put IOCFG back
 * when the packet is out.
 *
 * The bound is what the bare `while (!GIO6);` this replaces lacked: a radio
 * that stops draining -- the synthesizer dropped out, or the transmit strobe
 * was lost -- held the transmit loop forever, and that loop is what services
 * USB, so the board answered no control request until it was re-plugged.
 *
 * A caller that times out stops writing and lets the packet end there, rather
 * than carrying on the way the other bounded radio waits do: the radio still
 * holds what it was given, so the next piece would go past the end of the
 * FIFO. The packet goes out short and the receiver drops it, which is what
 * every alternative costs too, and the board keeps answering the host.
 *
 * These timeouts are not among the counts UBERTOOTH_GET_RADIO_TIMEOUTS
 * reports, so a board that keeps truncating packets reads as a quiet one.
 * Appending a shape to that reply is the way to count them -- cc2400_wait.h
 * says so -- but it changes the reply's length and the host that reads it,
 * which is more than this change needs.
 *
 * Returns 1 if the FIFO emptied, 0 if the wait timed out.
 */
static int fifo_wait_empty(void)
{
	unsigned polls;

	for (polls = FIFO_EMPTY_POLLS; polls != 0; polls--)
		if (GIO6)
			return 1;
	return 0;
}

/* start buffered tx */
static void cc2400_tx_sync(uint32_t sync)
{
	// Bluetooth-like modulation
	cc2400_set(MANAND,  0x7fff);
	cc2400_set(LMTST,   0x2b22);    // LNA and receive mixers test register
	cc2400_set(MDMTST0, 0x134b);    // no PRNG

	cc2400_set(GRMDM,   0x0c01);
	// 0 00 01 1 000 00 0 00 0 1
	//      |  | |   |  +--------> CRC off
	//      |  | |   +-----------> sync word: 8 MSB bits of SYNC_WORD
	//      |  | +---------------> 0 preamble bytes of 01010101
	//      |  +-----------------> packet mode
	//      +--------------------> buffered mode

	cc2400_set(SYNCL,   sync & 0xffff);
	cc2400_set(SYNCH,   (sync >> 16) & 0xffff);

	cc2400_set(FSDIV,   channel);
	// keep the amplifier level set with `ubertooth-util -a` (7 at power-on)
	cc2400_set(FREND,   frend_keep_pa_level(cc2400_get(FREND)));

	if (modulation == MOD_BT_BASIC_RATE) {
		cc2400_set(MDMCTRL, 0x0029);    // 160 kHz frequency deviation
	} else if (modulation == MOD_BT_LOW_ENERGY) {
		cc2400_set(MDMCTRL, 0x0040);    // 250 kHz frequency deviation
	} else {
		/* oops */
		return;
	}

	clkn_start();

	/* Bounded without a service callback, as in cc2400_rx(). */
	cc2400_wait_xosc_stable(NULL);
	cc2400_strobe(SFSON);
	cc2400_wait_fs(1, NULL);

#ifdef UBERTOOTH_ONE
	PAEN_SET;
#endif

	cc2400_wait_fsm(STATE_STROBE_FS_ON, NULL);
	cc2400_strobe(STX);

}
#endif

/* How long a packet le_transmit() can be asked to send. Its length is a u8,
 * so 255 bytes is the worst case, and the buffer it whitens into has to hold
 * that much. This is not a limit on advertising data -- ADV_IND_MAX_DATA is
 * that -- only on what the caller below can hand the radio. */
#define LE_TX_MAX_LEN 255

/*
 * Transmit a BTLE packet with the specified access address.
 *
 * All modulation parameters are set within this function. The data
 * should not be pre-whitened, but the CRC should be calculated and
 * included in the data length.
 *
 */
#ifdef TX_ENABLE
void le_transmit(u32 aa, u8 len, u8 *data)
{
	unsigned int fifo_space, i, j;
	int bit;
	u8 txbuf[LE_TX_MAX_LEN];
	u8 tx_len;
	u8 byte;
	uint32_t sync = rbit(aa);
	u16 gio_save;

	// whiten the data and copy it into the txbuf
	int idx = whitening_index[btle_channel_index(channel)];
	for (i = 0; i < len; ++i) {
		byte = data[i];
		txbuf[i] = 0;
		for (j = 0; j < 8; ++j) {
			bit = (byte & 1) ^ whitening[idx];
			idx = (idx + 1) % sizeof(whitening);
			byte >>= 1;
			txbuf[i] |= bit << (7 - j);
		}
	}

	// Bluetooth-like modulation
	cc2400_set(MANAND,  0x7fff);
	cc2400_set(LMTST,   0x2b22);    // LNA and receive mixers test register
	cc2400_set(MDMTST0, 0x134b);    // no PRNG

	cc2400_set(GRMDM,   0x0ce1);
	// 0 00 01 1 001 11 0 00 0 1
	//      |  | |   |  +--------> CRC off
	//      |  | |   +-----------> sync word: all 32 bits of SYNC_WORD
	//      |  | +---------------> 1 preamble byte of 01010101
	//      |  +-----------------> packet mode
	//      +--------------------> buffered mode

	cc2400_set(FSDIV,   channel);
	// keep the amplifier level set with `ubertooth-util -a` (7 at power-on)
	cc2400_set(FREND,   frend_keep_pa_level(cc2400_get(FREND)));
	cc2400_set(MDMCTRL, 0x0040);    // 250 kHz frequency deviation
	cc2400_set(INT,     0x0010);    // FIFO_THRESHOLD: 16 bytes
	// Nothing waits on that threshold: the loop below refills when the
	// radio reports its FIFO empty, which is a different signal.

	// set sync word to bit-reversed AA
	cc2400_set(SYNCL,   sync & 0xffff);
	cc2400_set(SYNCH,   (sync >> 16) & 0xffff);

	// GIO6 reports the radio's FIFO empty, which is what the write loop
	// below waits on. It must not be FIFO_FULL, which br_transmit() asks
	// for: that is a different signal with the opposite sense.
	gio_save = cc2400_get(IOCFG);
	cc2400_set(IOCFG, (GIO_FIFO_EMPTY << 9) | (gio_save & 0x1ff));

	// turn on the radio
	/* Bounded without a service callback: this sends one packet for the
	 * caller, which is a mode loop that services USB itself. */
	cc2400_wait_xosc_stable(NULL);
	cc2400_strobe(SFSON);
	cc2400_wait_fs(1, NULL);
	TXLED_SET;
#ifdef UBERTOOTH_ONE
	PAEN_SET;
#endif
	cc2400_wait_fsm(STATE_STROBE_FS_ON, NULL);
	/*
	 * The radio's FIFO holds CC2400_FIFO_SIZE bytes, so a longer packet
	 * goes in a piece at a time: wait for the radio to have sent what it
	 * has, write the next piece, strobe it out, repeat. A single longer
	 * write pushes past the end of the FIFO and what goes on the air is
	 * undefined.
	 */
	fifo_space = CC2400_FIFO_SIZE; // the FIFO is empty to start with
	i = 0;
	while (len != 0)
	{
		// Wait for the radio to have sent what is in the FIFO. Bounded,
		// so a radio that stops draining no longer takes the board with
		// it; before the first piece there is nothing to drain, so GIO6
		// already reads high and this returns at once.
		//
		// A wait that gives up ends the packet rather than writing the
		// next piece past the end of a FIFO the radio has not emptied.
		if (!fifo_wait_empty())
			break;
		// Decide how much we should send in this iteration
		if (len >= fifo_space) {
			len -= fifo_space;
			tx_len = fifo_space;
		} else {
			tx_len = len;
			len = 0;
		}
		// Signal FIFO to send data
		cc2400_fifo_write(tx_len, txbuf + i);
		cc2400_strobe(STX);
		i += tx_len;
		// Half a FIFO for every piece after the first, which is what the
		// advertising path has always written. The wait above is on the
		// FIFO being empty, so a whole FIFO would fit; the sizes are left
		// as they were. The only way an advertising packet gets long
		// enough to reach a second pass is advertising data longer than
		// the buffer bt_peripheral_le() builds the packet in, which that
		// function does not refuse and should: such a packet has already
		// overrun that buffer by the time it arrives here.
		fifo_space = CC2400_FIFO_SIZE / 2;
	}
	cc2400_wait_fsm(STATE_STROBE_FS_ON, NULL);
	TXLED_CLR;

	cc2400_strobe(SRFOFF);
	cc2400_wait_fs_unlock();

#ifdef UBERTOOTH_ONE
	PAEN_CLR;
#endif
	// Restore GIO
 	cc2400_set(IOCFG, gio_save);
}
#endif

#ifdef TX_ENABLE
void le_jam(void) {
	cc2400_set(MANAND,  0x7fff);
	cc2400_set(LMTST,   0x2b22);    // LNA and receive mixers test register
	cc2400_set(MDMTST0, 0x234b);    // PRNG, 1 MHz offset

	cc2400_set(GRMDM,   0x0c01);
	// 0 00 01 1 000 00 0 00 0 1
	//      |  | |   |  +--------> CRC off
	//      |  | |   +-----------> sync word: 8 MSB bits of SYNC_WORD
	//      |  | +---------------> 0 preamble bytes of 01010101
	//      |  +-----------------> packet mode
	//      +--------------------> buffered mode

	// cc2400_set(FSDIV,   channel);
	// jam at the amplifier level set with `ubertooth-util -a` (7 at power-on)
	cc2400_set(FREND,   frend_keep_pa_level(cc2400_get(FREND)));
	cc2400_set(MDMCTRL, 0x0040);    // 250 kHz frequency deviation

	/* Bounded without a service callback, as in le_transmit(). */
	cc2400_wait_xosc_stable(NULL);
	cc2400_strobe(SFSON);
	cc2400_wait_fs(1, NULL);
	TXLED_SET;
#ifdef UBERTOOTH_ONE
	PAEN_SET;
#endif
	cc2400_wait_fsm(STATE_STROBE_FS_ON, NULL);
	cc2400_strobe(STX);
}
#endif

/* TODO - return whether hop happened, or should caller have to keep
 * track of this? */
/*
 * Retune to the next channel.
 *
 * `service` is what to do between radio status polls while the synthesizer
 * settles: service_usb() from the loops that poll USB, NULL from the loops
 * that run USB off its interrupt. Those two waits are most of a hop, around
 * 300 microseconds, and in follow mode a hop happens every 625 microseconds.
 * Before this took a callback, that was the gap in endpoint 0 service that
 * stalled roughly one GET_CHANNEL in twenty (see service_usb()).
 *
 * What a request serviced inside those waits can and cannot change:
 *
 *   - The hop mode and the AFH map are read above, before the first wait, so a
 *     change to either takes effect from the next hop. That is also what keeps
 *     the map rebuild safe (see precalc()).
 *   - The channel is not. FSDIV below is written from the global, so a
 *     UBERTOOTH_SET_CHANNEL serviced inside the first wait retunes this hop to
 *     the host's channel instead of the computed one. Nothing ends up
 *     inconsistent -- the channel the board reports and the packet headers it
 *     sends are read from the same global -- and in follow mode the next hop
 *     overwrites it.
 *   - A request that writes a radio register directly, which only the
 *     developer requests behind `ubertooth-util -a` and `ubertooth-debug` do,
 *     can land between the register write and the strobe below and spoil the
 *     one hop.
 */
void hop(void (*service)(void))
{
	do_hop = 0;
	last_hop = clkn;

	// No hopping, if channel is set correctly, do nothing
	if (hop_mode == HOP_NONE) {
		if (cc2400_get(FSDIV) == (channel - 1))
			return;
	}
	/* Slow sweep (100 hops/sec)
	 * only hop to currently used channels if AFH is enabled
	 */
	else if (hop_mode == HOP_SWEEP) {
		do {
			channel += 32;
			if (channel > BT_FREQ_MAX)
				channel -= BT_FREQ_MAX - BT_FREQ_MIN + 1;
		} while ( used_channels != 0 && afh_enabled && !( afh_map[(channel-BT_FREQ_MIN)/8] & 0x1<<((channel-BT_FREQ_MIN)%8) ) );
	}

	/* AFH detection
	 * only hop to currently unused channesl
	 */
	else if (hop_mode == HOP_AFH) {
		do {
			channel += 32;
			if (channel > BT_FREQ_MAX)
				channel -= BT_FREQ_MAX - BT_FREQ_MIN + 1;
		} while( used_channels != NUM_BREDR_CHANNELS && (afh_map[(channel-BT_FREQ_MIN)/8] & 0x1<<((channel-BT_FREQ_MIN)%8)) );
	}

	else if (hop_mode == HOP_BLUETOOTH) {
		channel = next_hop(clkn);
	}

	else if (hop_mode == HOP_BTLE) {
		channel = btle_next_hop(&le);
	}

	else if (hop_mode == HOP_DIRECT) {
		channel = hop_direct_channel;
	}
	/* IDLE mode, but leave amp on, so don't call cc2400_idle(). */
	cc2400_strobe(SRFOFF);
	cc2400_wait_fs(0, service);

	/* Retune */
	if(mode == MODE_TX_SYMBOLS)
		cc2400_set(FSDIV, channel);
	else
		cc2400_set(FSDIV, channel - 1);

	/* Update CS register if hopping.  */
	if (hop_mode > 0) {
		cs_threshold_calc_and_set(channel);
	}

	/* Wait for lock */
	cc2400_strobe(SFSON);
	cc2400_wait_fs(1, service);

	dma_discard = 1;

	if(mode == MODE_TX_SYMBOLS) {
#ifdef TX_ENABLE
		cc2400_strobe(STX);
#endif
	} else {
		cc2400_strobe(SRX);
	}
}

/*
 * Program the demodulator for a modulation the host stated after its mode
 * request, and return the modulation the radio now has.
 *
 * `programmed` is the one cc2400_rx() wrote MDMCTRL for as the mode came up.
 * Every per-capture setting is stated after the mode request, because a mode
 * start puts it back to its default, and the channel survives that order only
 * because every hop writes FSDIV again. MDMCTRL is written once, so without
 * this a modulation stated in the same order reached the global and not the
 * radio: `ubertooth-dump -l` from an idle board dumped Basic Rate records and
 * said nothing.
 *
 * The radio comes off the air for the write and back on afterwards, as a hop
 * does, so the demodulator starts on the new deviation rather than taking it
 * part-way through a packet. The buffer being filled goes with it: part of it
 * was received at the other deviation, and a block marked discarded is a
 * better answer than a block of two halves.
 *
 * Only between the two modulations the Bluetooth receive configures. A generic
 * receive comes into the loop with MOD_NONE and its modem registers set by the
 * host (ubertooth-debug), and those are not this function's to overwrite.
 */
static uint8_t program_stated_modulation(uint8_t programmed)
{
	/* One read of a value the request handler writes, so the register and
	 * the return below cannot be for two different modulations. */
	uint8_t wanted = modulation;

	if (wanted == programmed || !programs_modulation(wanted) ||
	    !programs_modulation(programmed))
		return programmed;

	/* IDLE mode, but leave the amplifier on, as hop() does. */
	cc2400_strobe(SRFOFF);
	cc2400_wait_fs(0, service_usb);

	cc2400_set(MDMCTRL, mdmctrl_deviation(wanted));

	cc2400_strobe(SFSON);
	cc2400_wait_fs(1, service_usb);

	dma_discard = 1;

	cc2400_strobe(SRX);

	return wanted;
}

/* Bluetooth packet monitoring */
void bt_stream_rx()
{
	int8_t rssi;
	int8_t rssi_at_trigger;
	/* The modulation MDMCTRL has been written for, so that one stated after
	 * the mode request reaches the radio; see program_stated_modulation().
	 * Read after cc2400_rx() rather than before, because that is what it
	 * programmed the radio from. This mode polls USB rather than taking it
	 * on an interrupt, so the only places the host can change it are the
	 * service calls below. */
	uint8_t radio_modulation;

	RXLED_CLR;

	usb_queue_init();
	dio_ssp_init();
	dma_init_rx_symbols();
	dio_ssp_start();

	cc2400_rx();
	radio_modulation = modulation;

	cs_trigger_enable();

	/*
	 * Every mode the dispatch runs through this loop has to be listed here,
	 * or the mode returns at once and the board streams nothing while the
	 * dispatch sits in it. That is what MODE_AFH did: it is neither
	 * MODE_RX_SYMBOLS nor MODE_BT_FOLLOW, so the body never ran and
	 * ubertooth-afh's hopping mode was never what received.
	 * test_mode_dispatch.c checks the list against the dispatch.
	 *
	 * Asking for one of these modes while another is streaming is absorbed
	 * here rather than going through the dispatch, so a mode in this list
	 * must be one whose own vendor request sets up everything it needs:
	 * MODE_AFH's does (hop mode, empty map, afh_enabled), as does
	 * MODE_BT_FOLLOW's. Such a request inherits nothing else from the mode
	 * it replaces: each of the three calls reset_absorbed_mode_state()
	 * first, which clears the hop mode and the AFH map the way leaving a
	 * mode would have. That is what makes a plain symbol capture, whose
	 * request sets nothing up, stream plainly here.
	 *
	 * MODE_RX_GENERIC reaches this loop through rx_generic(), which is where
	 * its two receives are told apart and the symbol stream is set up, so it
	 * is listed only while it is the mode the dispatch recorded. A
	 * UBERTOOTH_RX_GENERIC request arriving while another mode streams
	 * therefore ends this loop and goes through the dispatch, instead of
	 * being absorbed here and streaming the running mode's symbols under the
	 * new mode's name.
	 *
	 * The three above are kept out of the generic receive for the same
	 * reason, and it matters more in that direction: this loop was entered
	 * with modulation = MOD_NONE, so a request for one of them absorbed here
	 * would stream unmodulated symbols under its name, with none of the
	 * setup its own request is trusted to have done and no idle reset in
	 * between.
	 *
	 * Re-sending the mode that is already running is absorbed, as it is for
	 * the other three among themselves: a host that reconfigures GRMDM and
	 * asks for MODE_RX_GENERIC again keeps the receive it has, because the
	 * choice is made on the way in and getting back to it means leaving the
	 * mode first.
	 */
	while ( ( requested_mode == MODE_RX_GENERIC
	          && mode == MODE_RX_GENERIC )
	     || ( mode != MODE_RX_GENERIC
	          && ( requested_mode == MODE_RX_SYMBOLS
	            || requested_mode == MODE_BT_FOLLOW
	            || requested_mode == MODE_AFH ) ) )
	{

		RXLED_CLR;

		/* Wait for DMA transfer. TODO - need more work on
		 * RSSI. Should send RSSI indications to host even
		 * when not transferring data. That would also keep
		 * the USB stream going. This loop runs 50-80 times
		 * while waiting for DMA, but RSSI sampling does not
		 * cover all the symbols in a DMA transfer. Can not do
		 * RSSI sampling in CS interrupt, but could log time
		 * at multiple trigger points there. The MAX() below
		 * helps with statistics in the case that cs_trigger
		 * happened before the loop started. */
		rssi_reset();
		rssi_at_trigger = INT8_MIN;
		while (!rx_tc) {
			rssi = (int8_t)(cc2400_get(RSSI) >> 8);
			if (cs_trigger && (rssi_at_trigger == INT8_MIN)) {
				rssi = MAX(rssi,(cs_threshold_cur+54));
				rssi_at_trigger = rssi;
			}
			rssi_add(rssi);

			service_usb();

			/* A modulation the host has just stated, which the mode
			 * start wrote no register for. Here rather than once per
			 * block, so the capture spends microseconds on the
			 * modulation it was not asked for instead of a block. */
			radio_modulation = program_stated_modulation(radio_modulation);

			/* If timer says time to hop, do it. The hop keeps servicing
			 * USB while the synthesizer settles; without that, endpoint 0
			 * went unserviced for the whole retune. */
			if (do_hop) {
				/* A background level belongs to one channel. */
				rssi_noise_reset();
				hop(service_usb);
			} else {
				TXLED_CLR;
			}
			/* TODO - set per-channel carrier sense threshold.
			 * Set by firmware or host. */
		}

		RXLED_SET;

		if (rx_err) {
			status |= DMA_ERROR;
		}

		/* Missed a DMA trasfer? */
		if (rx_tc > 1)
			status |= DMA_OVERFLOW;

		if (dma_discard) {
			status |= DISCARD;
			dma_discard = 0;
		}

		rssi_iir_update(channel);

		/* Set squelch hold if there was either a CS trigger, squelch
		 * is disabled, or if the current rssi_max is above the same
		 * threshold. Currently, this is redundant, but allows for
		 * per-channel or other rssi triggers in the future. */
		if (cs_trigger || cs_no_squelch) {
			status |= CS_TRIGGER;
			cs_trigger = 0;
		}

		if (rssi_max >= (cs_threshold_cur + 54)) {
			status |= RSSI_TRIGGER;
		}

		enqueue(BR_PACKET, (uint8_t*)idle_rxbuf);

		service_usb();
		rx_tc = 0;
		rx_err = 0;
	}

	dio_ssp_stop();
	cs_trigger_disable();
}

static uint8_t reverse8(uint8_t data)
{
	uint8_t reversed = 0;

	for(size_t i=0; i<8; i++)
	{
		reversed |= ((data >> i) & 0x01) << (7-i);
	}

	return reversed;
}

static uint16_t reverse16(uint16_t data)
{
	uint16_t reversed = 0;

	for(size_t i=0; i<16; i++)
	{
		reversed |= ((data >> i) & 0x01) << (15-i);
	}

	return reversed;
}

/*
 * Transmit a BTBR packet with the specified access code.
 *
 * All modulation parameters are set within this function.
 */
#ifdef TX_ENABLE
/* How long before br_transmit()'s transmit point it stops servicing USB, in
 * T0TC's 100 ns ticks. 100 microseconds, which is longer than a typical
 * service pass, so a pass that starts just inside the margin normally finishes
 * before the transmit point.
 *
 * It is not a guarantee: handle_usb() has no deadline of its own, and a single
 * request can be long -- UBERTOOTH_READ_ALL_REGISTERS is 46 bit-banged
 * register reads. A pass that overruns costs this packet its slot, which the
 * host sees as a gap in the transmitted stream, not as a board that stopped
 * answering. The cost of the margin itself is up to 100 microseconds of
 * spinning per packet. */
#define TX_USB_SERVICE_MARGIN 1000

void br_transmit()
{
	uint16_t gio_save;

	uint32_t clkn_saved = 0;

	uint16_t preamble = (target.syncword & 1) == 1 ? 0x5555 : 0xaaaa;
	uint8_t trailer = ((target.syncword >> 63) & 1) == 1 ? 0xaa : 0x55;

	uint8_t data[16] = {
		reverse8((target.syncword >> 0) & 0xFF),
		reverse8((target.syncword >> 8) & 0xFF),
		reverse8((target.syncword >> 16) & 0xFF),
		reverse8((target.syncword >> 24) & 0xFF),
		reverse8((target.syncword >> 32) & 0xFF),
		reverse8((target.syncword >> 40) & 0xFF),
		reverse8((target.syncword >> 48) & 0xFF),
		reverse8((target.syncword >> 56) & 0xFF),
		reverse8(trailer),
		reverse8(0x77),
		reverse8(0x66),
		reverse8(0x55),
		reverse8(0x44),
		reverse8(0x33),
		reverse8(0x22),
		reverse8(0x11)
	};

	cc2400_tx_sync(reverse16(preamble));

	cc2400_set(INT,     0x0014);    // FIFO_THRESHOLD: 20 bytes

	// set GIO to FIFO_FULL
	gio_save = cc2400_get(IOCFG);
	cc2400_set(IOCFG, (GIO_FIFO_FULL << 9) | (gio_save & 0x1ff));

	while ( requested_mode == MODE_TX_SYMBOLS )
	{

		/* Wait for the next pair of slots. This used to spin, so endpoint 0
		 * went unserviced for the whole wait -- most of 625 microseconds,
		 * far longer than the gap that stalls a control request in follow
		 * mode.
		 *
		 * Servicing USB here has to stop before the transmit point. T0TC
		 * counts 100 ns ticks and wraps every 3125, so `T0TC < 2250` is the
		 * last 87.5 microseconds of a tick, and the FIFO write below has to
		 * land in it. So the service stops a margin ahead of 2250 and the
		 * loop spins out the rest: a service pass that begins just inside
		 * the margin still finishes before the transmit point. While the
		 * slot pair has not advanced there is no deadline at all, because
		 * the wait for T0TC starts over on the next tick. */
		while ((clkn >> 1) == (clkn_saved >> 1) || T0TC < 2250) {
			if ((clkn >> 1) == (clkn_saved >> 1)
			    || T0TC < 2250 - TX_USB_SERVICE_MARGIN) {
				service_usb();
			}

			// If timer says time to hop, do it.
			if (do_hop) {
				hop(service_usb);
			}
		}

		clkn_saved = clkn;

		TXLED_SET;

		cc2400_fifo_write(16, data);

		/* The rest of this pass waits on the radio four times: the transmit
		 * finishing, then the retune. Every wait is bounded and keeps
		 * servicing USB, so a radio that stops advancing no longer takes the
		 * board with it: the loop keeps coming back here and keeps answering
		 * the host, which can then stop the mode. */
		cc2400_wait_fsm(STATE_STROBE_FS_ON, service_usb);
		TXLED_CLR;

		cc2400_strobe(SRFOFF);
		cc2400_wait_fs(0, service_usb);

		cc2400_wait_xosc_stable(service_usb);
		cc2400_strobe(SFSON);
		cc2400_wait_fs(1, service_usb);

		cc2400_wait_fsm(STATE_STROBE_FS_ON, service_usb);
		cc2400_strobe(STX);

		service_usb();
	}

#ifdef UBERTOOTH_ONE
	PAEN_CLR;
#endif

	// reset GIO
	cc2400_set(IOCFG, gio_save);
}
#endif

/* set LE access address */
static void le_set_access_address(u32 aa) {
	u32 aa_rev;

	le.access_address = aa;
	aa_rev = rbit(aa);
	le.syncl = aa_rev & 0xffff;
	le.synch = aa_rev >> 16;
}

/*
 * reset le state, called by bt_generic_le and bt_follow_le()
 *
 * This runs when the mode's own loop starts, which is after the request that
 * asked for the mode has been answered. So every field of le_state_t is
 * cleared here except the four that carry what the host asked this capture
 * for: whether to follow connections (do_follow, set by the mode request
 * itself) and the address filter (target, target_mask, target_set, set by a
 * request of their own before it). Clearing those would throw away the
 * command line the capture was started with.
 *
 * The CRC check is the odd one out -- it is a host setting too, but a
 * promiscuous capture recovers CRCInit and turns it on by itself, so it starts
 * off again here and ubertooth-btle(1) sends it after the mode request rather
 * than before.
 *
 * test_le_state_reset.c reads le_state_t and this function and fails if a
 * field is added to the struct without being cleared here or named there as
 * one to keep.
 */
void reset_le() {
	le_set_access_address(0x8e89bed6);     // advertising channel access address
	/* The address above is this reset's own, not one the host chose, so
	 * the next promiscuous pass has to discover one rather than follow
	 * it. The connection timeout resets here too, which is how an
	 * access address the host gave stops being followed after the
	 * connection it belonged to has gone. */
	le.access_address_set = 0;
	le.crc_init  = 0x555555;               // advertising channel CRCInit
	le.crc_init_reversed = 0xAAAAAA;
	le.crc_verify = 0;
	le.last_packet = 0;

	le.link_state = LINK_INACTIVE;

	le.channel_idx = 0;
	le.channel_increment = 0;
	le.chan_sel_2 = 0;
	le.event_counter = 0;

	le.conn_epoch = 0;
	le.interval_timer = 0;
	le.conn_interval = 0;
	le.conn_count = 0;

	le.win_size = 0;
	le.win_offset = 0;

	le.update_pending = 0;
	le.update_instant = 0;
	le.interval_update = 0;
	le.win_size_update = 0;
	le.win_offset_update = 0;

	do_hop = 0;
}

// reset LE Promisc state
void reset_le_promisc(void) {
	memset(&le_promisc, 0, sizeof(le_promisc));
}

/* generic le mode */
void bt_generic_le(u8 active_mode)
{
	u8 hold;
	int i, j;
	int8_t rssi, rssi_at_trigger;
	uint8_t site;

	modulation = MOD_BT_LOW_ENERGY;
	mode = active_mode;

	reset_le();

	/* The loop below searches the whole symbol buffer from the first pass
	 * on, including the half it has not filled yet. That half holds
	 * whatever the other half of the mode left in the shared buffer, so it
	 * is cleared rather than searched as if it were received symbols. */
	memset(le_rx.symbols, 0, sizeof(le_rx.symbols));

	// enable USB interrupts
	ISER0 = ISER0_ISE_USB;

	RXLED_CLR;

	usb_queue_init();
	dio_ssp_init();
	dma_init_rx_symbols();
	dio_ssp_start();
	cc2400_rx();

	cs_trigger_enable();

	hold = 0;

	while (requested_mode == active_mode) {
		watchdog_usb_ready();

		if (requested_channel != 0) {
			cc2400_strobe(SRFOFF);
			cc2400_wait_fs_unlock();

			/* Retune */
			cc2400_set(FSDIV, channel - 1);

			/* Wait for lock */
			cc2400_strobe(SFSON);
			cc2400_wait_fs(1, NULL);

			/* RX mode */
			cc2400_strobe(SRX);

			/* A background level belongs to one channel. */
			rssi_noise_reset();
			requested_channel = 0;
		}

		/* No service callback: this mode runs USB off its interrupt, which
		 * it enabled on the way in, so endpoint 0 is serviced during the
		 * hop without the loop doing anything. */
		if (do_hop) {
			/* A background level belongs to one channel. */
			rssi_noise_reset();
			hop(NULL);
		} else {
			TXLED_CLR;
		}

		RXLED_CLR;

		/* Wait for DMA. Meanwhile keep track of RSSI. */
		rssi_reset();
		rssi_at_trigger = INT8_MIN;
		/* This wait has no deadline: on a quiet channel no packet ever
		 * completes the transfer, so it can last as long as the capture
		 * does. It feeds, because it is the mode loop's own code and runs
		 * below every interrupt handler, exactly like the feed at the top
		 * of the loop -- a handler that stops returning stops this too. */
		site = watchdog_mark(WATCHDOG_SITE_LE_RX);
		while ((rx_tc == 0) && (rx_err == 0))
		{
			watchdog_usb_ready();
			rssi = (int8_t)(cc2400_get(RSSI) >> 8);
			if (cs_trigger && (rssi_at_trigger == INT8_MIN)) {
				rssi = MAX(rssi,(cs_threshold_cur+54));
				rssi_at_trigger = rssi;
			}
			rssi_add(rssi);
		}
		watchdog_mark(site);

		if (rx_err) {
			status |= DMA_ERROR;
		}

		/* No DMA transfer? */
		if (!rx_tc)
			goto rx_continue;

		/* Missed a DMA trasfer? */
		if (rx_tc > 1)
			status |= DMA_OVERFLOW;

		rssi_iir_update(channel);

		/* Set squelch hold if there was either a CS trigger, squelch
		 * is disabled, or if the current rssi_max is above the same
		 * threshold. Currently, this is redundant, but allows for
		 * per-channel or other rssi triggers in the future. */
		if (cs_trigger || cs_no_squelch) {
			status |= CS_TRIGGER;
			hold = CS_HOLD_TIME;
			cs_trigger = 0;
		}

		if (rssi_max >= (cs_threshold_cur + 54)) {
			status |= RSSI_TRIGGER;
			hold = CS_HOLD_TIME;
		}

		/* Hold expired? Ignore data. */
		if (hold == 0) {
			goto rx_continue;
		}
		hold--;

		// copy the previously unpacked symbols to the front of the buffer
		memcpy(le_rx.symbols, le_rx.symbols + DMA_SIZE*8, DMA_SIZE*8);

		// unpack the new packet to the end of the buffer
		for (i = 0; i < DMA_SIZE; ++i) {
			/* output one byte for each received symbol (0x00 or 0x01) */
			for (j = 0; j < 8; ++j) {
				le_rx.symbols[DMA_SIZE*8 + i * 8 + j] = (idle_rxbuf[i] & 0x80) >> 7;
				idle_rxbuf[i] <<= 1;
			}
		}

		int ret = data_cb(le_rx.symbols);
		if (!ret) break;

	rx_continue:
		rx_tc = 0;
		rx_err = 0;
	}

	// disable USB interrupts
	ICER0 = ICER0_ICE_USB;

	// reset the radio completely. bt_promisc_le() runs this mode and
	// bt_le_sync() in turn and starts each half from the state this leaves,
	// so it is not only a teardown; the dispatch idles again on the way out
	// of the mode, which is cheap and idempotent.
	cc2400_idle();
	dio_ssp_stop();
	cs_trigger_disable();
}


/*
 * How long a followed connection may go unheard before it is taken to be over
 * and the mode starts again.
 *
 * Five seconds for a connection the board can hear every event of.
 *
 * A connection recovered by the algorithm-2 search is followed without its
 * channel map, so the board lands on the right channel for only some of its
 * events, and a run of events it does not hear is normal rather than a sign
 * the connection has gone. It is given LE_CSA2_SILENCE_EVENTS connection
 * intervals instead -- see the arithmetic there -- so that a connection it is
 * following correctly is not thrown away and recovered again from the
 * beginning, which at a long interval takes many minutes.
 */
static u32 le_silence_timeout(void)
{
	u32 events;

	if (!le.chan_sel_2 || le.conn_interval == 0)
		return 5 * 10000000u;

	events = LE_CSA2_SILENCE_EVENTS * le.conn_interval * LE_BASECLK;
	return events > 5 * 10000000u ? events : 5 * 10000000u;
}

/*
 * Keeps reads of the low-energy receive buffer after the wait that ended
 * because the DMA had filled it. The controller writes the buffer without the
 * compiler knowing, and the waits watch registers rather than the buffer, so
 * without this a load could legally be hoisted above one of them.
 */
static inline void le_rx_barrier(void)
{
	/* A data barrier, not only a compiler one: the waits are built from
	 * reads of the controller's registers, which are device memory, and
	 * ARMv7-M allows a read of ordinary memory to be reordered against
	 * those. */
	__asm__ volatile ("dmb" ::: "memory");
}

void bt_le_sync(u8 active_mode)
{
	int8_t rssi;
	uint8_t site;
	static int restart_jamming = 0;

	modulation = MOD_BT_LOW_ENERGY;
	mode = active_mode;

	le.link_state = LINK_LISTENING;

	// enable USB interrupts
	ISER0 = ISER0_ISE_USB;

	RXLED_CLR;

	usb_queue_init();
	dio_ssp_init();
	dma_init_le(le_rx.packet + 4, LE_RX_PDU_MAX);
	dio_ssp_start();

	/* The access address promiscuous mode is about to follow, announced
	 * here because the queue reset above empties anything queued earlier:
	 * this is the first point at which the host can be told, and the only
	 * place it is told, so the line cannot arrive twice. Both ways into
	 * the mode come through here, the host-given access address included.
	 *
	 * The clock is stopped and the channel registers are clear at this
	 * point, so the record is stamped channel 0 at clock 0; the host
	 * prints the state line and returns before it reads either. */
	if (active_mode == MODE_BT_PROMISC_LE)
		le_promisc_state(0, &le.access_address, 4);

	cc2400_rx_sync(rbit(le.access_address)); // bit-reversed access address

	while (requested_mode == active_mode) {
		watchdog_usb_ready();

		if (requested_channel != 0) {
			cc2400_strobe(SRFOFF);
			cc2400_wait_fs_unlock();

			/* Retune */
			cc2400_set(FSDIV, channel - 1);

			/* Wait for lock */
			cc2400_strobe(SFSON);
			cc2400_wait_fs(1, NULL);

			/* RX mode */
			cc2400_strobe(SRX);

			/* A background level belongs to one channel. */
			rssi_noise_reset();
			requested_channel = 0;
		}

		RXLED_CLR;

		/* Wait for DMA. Meanwhile keep track of RSSI. */
		rssi_reset();
		/* Nothing bounds this wait: following a connection, the next thing
		 * to end it is the hop at the end of the connection interval, which
		 * BLE allows to be as long as 4 s. It feeds for the reason the wait
		 * in bt_generic_le() does -- it is the mode loop running, below
		 * every interrupt handler. */
		site = watchdog_mark(WATCHDOG_SITE_LE_RX);
		while ((rx_tc == 0) && (rx_err == 0) && (do_hop == 0) && requested_mode == active_mode)
			watchdog_usb_ready();
		watchdog_mark(site);

		/* One reading for the whole packet, taken as it ends: this mode has
		 * the radio deliver whole packets rather than polling through them,
		 * so there is one moment at which the packet's power can be read.
		 * rssi_reset() above has already emptied the per-packet figures. */
		rssi = (int8_t)(cc2400_get(RSSI) >> 8);
		rssi_add(rssi);

		if (requested_mode != active_mode) {
			goto cleanup;
		}

		if (rx_err) {
			status |= DMA_ERROR;
		}

		if (do_hop)
			goto rx_flush;

		/* No packet. A transfer error is the only thing that can end the
		 * wait without one, since a mode change and a hop are both handled
		 * above, so this goes where the error has to be recovered from:
		 * rx_flush drains the symbols the error left in the receiver and
		 * arms the transfer again, which is also the one place rx_err is
		 * cleared.
		 *
		 * `continue` here instead ended the capture. The next pass found
		 * rx_err still set, left the wait at once, arrived here again, and
		 * went round that way forever: nothing was received, the receive
		 * path was never armed again, and the loop went on feeding the
		 * watchdog, so the board answered the host throughout. A capture
		 * that looked alive and recorded nothing until it was killed. */
		if (!rx_tc)
			goto rx_flush;

		/////////////////////
		// process the packet

		/* The packet as the host will see it: the access address the radio
		 * synchronized on, then the bytes the DMA put after it. */
		u8 *p = le_rx.packet;
		/* The DMA writes into that buffer without the compiler knowing, so
		 * a read of it must not be moved above the wait that has just ended
		 * because the bytes are there. The buffer itself is not volatile:
		 * it is read a word at a time in the dewhitening below, and the
		 * symbol half of it is read a byte at a time in a search loop. */
		le_rx_barrier();
		memcpy(p, &le.access_address, 4);

		/* The first four bytes, which is what the DMA's first item has
		 * delivered so far (LE_DMA_HEAD). The length field is among
		 * them, and it is what says how much more to wait for. */
		le_dewhiten_from(p + 4, LE_DMA_HEAD, channel, 0);

		/* The PDU's length field is eight bits wide, and the buffer holds
		 * whatever it can claim, so nothing has to be refused to keep the
		 * capture inside it. Two lengths are still refused, because waiting
		 * for bytes is not free: the radio synchronizes on a 32-bit access
		 * address, so noise does occasionally look like a packet, and the
		 * longest packet takes about 2 ms to arrive -- blind time in which
		 * the next connection event is missed.
		 *
		 * Which lengths those are is le_length_is_possible()'s to say. */
		if (!le_length_is_possible(p[5], le.crc_verify))
			goto rx_flush;

		unsigned len = p[5] + 2;
		unsigned pdu_bytes = len + 3;   /* header, payload and CRC */

		/* Take only the bytes this packet needs, so there is time to resume
		 * receive before the next packet on this channel.
		 *
		 * The wait ends when the controller has the bytes, when it runs out
		 * of packet to deliver them into, on a mode change, or when the
		 * bytes are overdue: a length field corrupted by noise can ask for
		 * the longest packet there is, and a host that stops the mode
		 * meanwhile must not be left waiting for bytes the radio has
		 * stopped sending.
		 *
		 * The deadline is what covers the radio stopping mid-packet while
		 * the transfer stays armed. Nothing else ends the wait then, so
		 * the loop spun here until the host changed mode -- and a host
		 * that is waiting for capture data never does. The board went on
		 * answering requests the whole time, because this mode services
		 * USB from its interrupt, so the capture looked alive and
		 * recorded nothing from that moment on.
		 *
		 * The time is added up step by step rather than compared against
		 * a deadline, because the capture clock briefly runs backwards:
		 * clk100ns_step() says why. */
		unsigned want = 4 * ((pdu_bytes + 3) / 4);
		uint32_t wait_budget = le_rx_byte_wait_clk100ns(want);
		uint32_t waited = 0;
		uint32_t last_tick = T0TC;
		int overdue = 0;
		while (DMACC0DestAddr < (uint32_t)(p + 4) + want
		       && (DMACC0Config & DMACCxConfig_E) && rx_err == 0
		       && requested_mode == active_mode) {
			uint32_t now_tick = T0TC;

			waited += clk100ns_step(last_tick, now_tick);
			last_tick = now_tick;
			if (waited > wait_budget) {
				overdue = 1;
				break;
			}
		}
		/* Whether the whole packet arrived, which is why the wait ended.
		 * The receive buffer is reused, so the bytes a packet cut short by
		 * a transfer error or by the deadline is missing are the previous
		 * packet's: a record built from those would be a packet that was
		 * never on the air. */
		int whole = rx_err == 0 && !overdue &&
		            DMACC0DestAddr >= (uint32_t)(p + 4) + want;
		if (requested_mode != active_mode)
			goto cleanup;
		if (rx_err)
			status |= DMA_ERROR;
		if (overdue)
			cc2400_wait_count_timeout(CC2400_WAIT_RX_BYTES);
		DIO_SSP_DMACR &= ~SSPDMACR_RXDMAE;
		if (!whole)
			goto rx_flush;
		le_rx_barrier();

		// strobe SFSON to allow the resync to occur while we process the packet
		cc2400_strobe(SFSON);

		/* Unwhiten the rest of the packet, carrying on from the four
		 * bytes already done above. */
		le_dewhiten_from(p + 4 + LE_DMA_HEAD, pdu_bytes - LE_DMA_HEAD,
		                 channel, LE_DMA_HEAD);

		if (le.crc_verify) {
			u32 calc_crc = btle_crcgen_lut(le.crc_init_reversed, p + 4, len);
			u32 wire_crc = (p[4+len+2] << 16)
						 | (p[4+len+1] << 8)
						 | (p[4+len+0] << 0);
			if (calc_crc != wire_crc) // skip packets with a bad CRC
				goto rx_flush;
		}


		RXLED_SET;
		packet_cb(p);

		le_pkt_meta meta;
		meta.status = status;
		meta.channel = ubertooth_channel_byte(idle_buf_channel);
		meta.clkn_high = idle_buf_clkn_high;
		meta.clk100ns = idle_buf_clk100ns;
		meta.rssi_min = rssi_min;
		meta.rssi_max = rssi_max;
		meta.rssi_avg = rssi_get_signal();
		meta.rssi_count = rssi_count;
		meta.rssi_noise = 0;
		if (rssi_count != 0) {
			meta.status |= SIGNAL_VALID;
			if (rssi_noise_estimated()) {
				meta.rssi_noise = (uint8_t) rssi_get_noise();
				meta.status |= NOISE_ESTIMATE;
			}
		}

		// hold the USB interrupt off while we touch USB data structures:
		// UBERTOOTH_POLL dequeues from the vendor request handler, which
		// runs from that interrupt in this mode
		u32 usb_irq = usb_irq_hold();
		/* A packet of more than 50 bytes goes out as several records, which
		 * the host puts back together. */
		if (usb_enqueue_le_packet(&meta, p, p + 4, pdu_bytes))
			status = 0;
		else
			status |= FIFO_OVERFLOW;
		usb_irq_release(usb_irq);

		le.last_packet = CLK100NS;

	rx_flush:
		// this might happen twice, but it's safe to do so
		cc2400_strobe(SFSON);

		// flush any excess bytes from the SSP's buffer
		DIO_SSP_DMACR &= ~SSPDMACR_RXDMAE;
		while (SSP1SR & SSPSR_RNE) {
			u8 tmp = (u8)DIO_SSP_DR;
		}

		// timeout - FIXME this is an ugly hack
		u32 now = CLK100NS;
		if (now < le.last_packet)
			now += CLK100NS_PERIOD; // handle rollover
		if  ( // timeout
			((le.link_state == LINK_CONNECTED || le.link_state == LINK_CONN_PENDING)
			&& (now - le.last_packet > le_silence_timeout()))
			// jam finished
			|| (le_jam_count == 1)
			)
		{
			reset_le();
			le_jam_count = 0;
			TXLED_CLR;

			if (jam_mode == JAM_ONCE) {
				jam_mode = JAM_NONE;
				requested_mode = MODE_IDLE;
				goto cleanup;
			}

			// go back to promisc if the connection dies
			if (active_mode == MODE_BT_PROMISC_LE)
				goto cleanup;

			le.link_state = LINK_LISTENING;

			cc2400_strobe(SRFOFF);
			cc2400_wait_fs_unlock();

			/* Retune */
			channel = le_adv_channel != 0 ? le_adv_channel : BT_FREQ_MIN;
			restart_jamming = 1;
		}

		cc2400_set(SYNCL, le.syncl);
		cc2400_set(SYNCH, le.synch);

		/* No service callback, as in bt_generic_le(): USB runs off its
		 * interrupt here. */
		if (do_hop) {
			/* A background level belongs to one channel. */
			rssi_noise_reset();
			hop(NULL);
		}

		/* The terminal count is cleared by dma_init_le() below, before it
		 * arms the channel, so nothing clears it here: a count raised after
		 * arming is this iteration's and has to be kept. */

		// ♪ you can jam but you keep turning off the light ♪
		if (le_jam_count > 0) {
#ifdef TX_ENABLE
			le_jam();
#endif
			--le_jam_count;
		} else {
			/* RX mode */
			dma_init_le(le_rx.packet + 4, LE_RX_PDU_MAX);
			dio_ssp_start();

			if (restart_jamming) {
				cc2400_rx_sync(rbit(le.access_address));
				restart_jamming = 0;
			} else {
				// wait till we're in FSLOCK before strobing RX
				cc2400_wait_fs(1, NULL);
				cc2400_strobe(SRX);
			}
		}
	}

cleanup:

	// disable USB interrupts
	ICER0 = ICER0_ICE_USB;

	// reset the radio completely (as in bt_generic_le(): bt_promisc_le()
	// starts its next pass from this state)
	cc2400_idle();
	dio_ssp_stop();
	cs_trigger_disable();
}



/* How much of a packet the callback below decodes out of the symbol buffer:
 * the four bytes of the access address and 42 bytes of PDU, which is the count
 * it has always decoded. The buffer it writes into is DMA_SIZE (50) bytes, so
 * the last four are left as they were; what matters here is that nothing may
 * be read past the bytes this number covers. */
#define FOLLOW_LE_BYTES 46

_Static_assert(FOLLOW_LE_BYTES <= DMA_SIZE,
               "the connection-follow callback decodes more than its buffer holds");

/* low energy connection following
 * follows a known AA around */
int cb_follow_le() {
	int i, j, k;
	int idx = whitening_index[btle_channel_index(channel)];

	u32 access_address = 0;
	for (i = 0; i < 31; ++i) {
		access_address >>= 1;
		access_address |= (le_rx.symbols[i] << 31);
	}

	for (i = 31; i < DMA_SIZE * 8 + 32; i++) {
		access_address >>= 1;
		access_address |= (le_rx.symbols[i] << 31);
		if (access_address == le.access_address) {
			for (j = 0; j < FOLLOW_LE_BYTES; ++j) {
				u8 byte = 0;
				for (k = 0; k < 8; k++) {
					int offset = k + (j * 8) + i - 31;
					if (offset >= DMA_SIZE*8*2) break;
					int bit = le_rx.symbols[offset];
					if (j >= 4) { // unwhiten data bytes
						bit ^= whitening[idx];
						idx = (idx + 1) % sizeof(whitening);
					}
					byte |= bit << k;
				}
				idle_rxbuf[j] = byte;
			}

			/* The length field is a whole byte in a low-energy
			 * header: the six-bit field belongs to the Basic Rate
			 * packets elsewhere in this file. */
			int len = idle_rxbuf[5] + 2;

			/* A header claiming more than the bytes decoded above
			 * belongs to a packet that arrived cut short. There is
			 * neither a CRC of it to check -- reading one would
			 * read past the buffer -- nor a record of it to send,
			 * because what the host would get is a record claiming
			 * bytes that were never decoded.
			 *
			 * This is the buffer's own bound, not the judgement
			 * le_length_is_possible() makes for the other capture
			 * loop: that one asks whether a length could belong to
			 * a packet at all, and allows a Data Length Extension
			 * payload, which does not fit here. The arithmetic
			 * works out to 37 payload bytes, which is the legacy
			 * maximum, but it is the buffer that says so. */
			if (4 + len + 3 > FOLLOW_LE_BYTES)
				break;

			// verify CRC
			if (le.crc_verify) {
				u32 calc_crc = btle_crcgen_lut(le.crc_init_reversed, (uint8_t*)idle_rxbuf + 4, len);
				u32 wire_crc = (idle_rxbuf[4+len+2] << 16)
				             | (idle_rxbuf[4+len+1] << 8)
				             |  idle_rxbuf[4+len+0];
				if (calc_crc != wire_crc) // skip packets with a bad CRC
					break;
			}

			// send to PC
			enqueue(LE_PACKET, (uint8_t*)idle_rxbuf);
			RXLED_SET;

			packet_cb((uint8_t*)idle_rxbuf);

			break;
		}
	}

	return 1;
}

/**
 * Called when we receive a packet in connection following mode.
 */
void connection_follow_cb(u8 *packet) {
	int i;
	u32 aa = 0;

#define ADV_ADDRESS_IDX 0
#define HEADER_IDX 4
#define DATA_LEN_IDX 5
#define DATA_START_IDX 6

	// u8 *adv_addr = &packet[ADV_ADDRESS_IDX];
	u8 header = packet[HEADER_IDX];
	u8 *data_len = &packet[DATA_LEN_IDX];
	u8 *data = &packet[DATA_START_IDX];
	// u8 *crc = &packet[DATA_START_IDX + *data_len];

	if (le.link_state == LINK_CONN_PENDING) {
		// We received a packet in the connection pending state, so now the device *should* be connected
		le.link_state = LINK_CONNECTED;
		le.conn_epoch = clkn;
		le.interval_timer = le.conn_interval - 1;
		le.conn_count = 0;
		le.update_pending = 0;

		// hue hue hue
		if (jam_mode != JAM_NONE)
			le_jam_count = JAM_COUNT_DEFAULT;

	} else if (le.link_state == LINK_CONNECTED) {
		u8 llid =  header & 0x03;

		// Apply any connection parameter update if necessary
		if (le.update_pending && le.conn_count == le.update_instant) {
			// This is the first packet received in the connection interval for which the new parameters apply
			le.conn_epoch = clkn;
			le.conn_interval = le.interval_update;
			le.interval_timer = le.interval_update - 1;
			le.win_size = le.win_size_update;
			le.win_offset = le.win_offset_update;
			le.update_pending = 0;
		}

		/* A connection update carries twelve bytes: the opcode and the
		 * eleven fields read below. A shorter control packet claiming to
		 * be one would have those fields read from whatever the buffer
		 * held before it, and the connection followed from there. */
		if (llid == 0x03 && *data_len >= 12 && data[0] == 0x00) {
			// This is a CONNECTION_UPDATE_REQ.
			// The host is changing the connection parameters.
			le.win_size_update = packet[7];
			le.win_offset_update = packet[8] + ((u16)packet[9] << 8);
			le.interval_update = packet[10] + ((u16)packet[11] << 8);
			le.update_instant = packet[16] + ((u16)packet[17] << 8);
			if (le.update_instant - le.conn_count < 32767)
				le.update_pending = 1;
		}

	} else if (le.link_state == LINK_LISTENING) {
		u8 pkt_type = packet[4] & 0x0F;
		if (pkt_type == 0x05) {
			uint16_t conn_interval;

			// ignore packets with incorrect length
			if (*data_len != 34)
				return;

			// conn interval must be [7.5 ms, 4.0s] in units of 1.25 ms
			conn_interval = (packet[29] << 8) | packet[28];
			if (conn_interval < LE_MIN_CONN_INTERVAL ||
					conn_interval > LE_MAX_CONN_INTERVAL)
				return;

			// This is a connect packet
			// if we have a target, see if InitA or AdvA matches
			if (le.target_set &&
				memcmp(le.target, &packet[6], 6) &&  // Target address doesn't match Initiator.
				memcmp(le.target, &packet[12], 6)) {  // Target address doesn't match Advertiser.
				return;
			}

			le.link_state = LINK_CONN_PENDING;
			le.crc_verify = 0; // we will drop many packets if we attempt to filter by CRC

			for (i = 0; i < 4; ++i)
				aa |= packet[18+i] << (i*8);
			le_set_access_address(aa);

#define CRC_INIT (2+4+6+6+4)
			le.crc_init = (packet[CRC_INIT+2] << 16)
						| (packet[CRC_INIT+1] << 8)
						|  packet[CRC_INIT+0];
			/* CRCInit is 24 bits wide. rbit() reverses all 32 bits of
			 * the word, so the result has to come down by a byte to be
			 * the reversal of the field: 0x123456 reverses to 0x6a2c48,
			 * not 0x2c4800. */
			le.crc_init_reversed = rbit(le.crc_init) >> 8;

#define WIN_SIZE (2+4+6+6+4+3)
			le.win_size = packet[WIN_SIZE];

#define WIN_OFFSET (2+4+6+6+4+3+1)
			le.win_offset = packet[WIN_OFFSET];

#define CONN_INTERVAL (2+4+6+6+4+3+1+2)
			le.conn_interval = (packet[CONN_INTERVAL+1] << 8)
							 |  packet[CONN_INTERVAL+0];

#define CHANNEL_INC (2+4+6+6+4+3+1+2+2+2+2+5)
			le.channel_increment = packet[CHANNEL_INC] & 0x1f;
			le.channel_idx = le.channel_increment;

			// Hop to the initial channel immediately
			do_hop = 1;
		}
	}
}

void le_phy_main(void);
void bt_follow_le() {
	le_phy_main();

	/* old method
	reset_le();
	packet_cb = connection_follow_cb;
	bt_le_sync(MODE_BT_FOLLOW_LE);
	*/
}

// issue state change message
void le_promisc_state(u8 type, void *data, unsigned len) {
	/* DMA_SIZE because that is what enqueue() copies, and one byte of it is
	 * the type. */
	u8 buf[DMA_SIZE] = { 0 };
	if (len > DMA_SIZE - 1)
		len = DMA_SIZE - 1;

	buf[0] = type;
	memcpy(&buf[1], data, len);
	enqueue(LE_PROMISC, (uint8_t*)buf);
}

/* Works the hop increment out from how long the connection takes to get from
 * 2404 MHz to 2406 MHz, and then follows it. Only channel selection algorithm
 * 1 has an increment to find. This stage is also reached only after the
 * interval stage that precedes it has settled, and on a connection hopping
 * with algorithm 2 that does not happen: algorithm 2 picks each event's
 * channel from the access address, the event counter, and the channel map, so
 * the connection's visits to one channel do not repeat on the 37-event cycle
 * the interval estimator looks for. promisc_try_csa2() takes those
 * connections, from the same sightings. */
void promisc_recover_hop_increment(u8 *packet) {
	/* The interval the span below is divided by. The estimator hands over
	 * nothing outside the range a connection may use, but the division is
	 * unforgiving enough to check anyway: the LPC17xx leaves DIV_0_TRP
	 * clear, so a zero divisor makes UDIV answer 0 rather than fault, and
	 * hop_interval_lut[0] is 0 -- a board that has stopped hopping and
	 * reports both numbers as zero. Nothing is reported instead; the
	 * search below keeps cycling, so a later interval is still picked up.
	 */
	if (le.conn_interval < LE_MIN_CONN_INTERVAL ||
			le.conn_interval > LE_MAX_CONN_INTERVAL) {
		hop_direct_channel = 2404;
		do_hop = 1;
		return;
	}

	if (channel == 2404) {
		le_promisc.first_ts = CLK100NS;
		hop_direct_channel = 2406;
		do_hop = 1;
	} else if (channel == 2406) {
		u32 second_ts = CLK100NS;
		if (second_ts < le_promisc.first_ts)
			second_ts += CLK100NS_PERIOD; // handle rollover
		// Number of channels hopped between previous and current timestamp.
		u32 channels_hopped = LE_DIVIDE_ROUND(second_ts - le_promisc.first_ts,
										   le.conn_interval * LE_BASECLK);
		/* At least one: the two sightings are a whole number of connection
		 * events apart, and a span shorter than half an interval rounds to
		 * none of them. hop_interval_lut[0] is 0, so taking it would be a
		 * hop increment of 0 and a board that stops hopping. */
		if (channels_hopped >= 1 && channels_hopped < LE_HOP_CHANNELS &&
				hop_interval_lut[channels_hopped] >= LE_MIN_HOP_INCREMENT &&
				hop_interval_lut[channels_hopped] <= LE_MAX_HOP_INCREMENT) {
			/* The table turns a number of connection events into the hop
			 * increment that covers them, and only a dozen of its entries
			 * are an increment a connection may use: the rest come from a
			 * span measured against an interval that was not this
			 * connection's. Following one of those chases a sequence no
			 * connection uses, so the search below keeps cycling instead. */
			le.channel_increment = hop_interval_lut[channels_hopped];
			le.interval_timer = le.conn_interval / 2;
			le.conn_count = 0;
			le.conn_epoch = 0;
			do_hop = 0;
			// Move on to regular connection following.
			le.channel_idx = (1 + le.channel_increment) % LE_HOP_CHANNELS;
			le.link_state = LINK_CONNECTED;
			le.crc_verify = 0;
			hop_mode = HOP_BTLE;
			packet_cb = connection_follow_cb;
			le_promisc_state(3, &le.channel_increment, 1);

			if (jam_mode != JAM_NONE)
				le_jam_count = JAM_COUNT_DEFAULT;

			return;
		}
		hop_direct_channel = 2404;
		do_hop = 1;
	}
	else {
		hop_direct_channel = 2404;
		do_hop = 1;
	}
}

/* Add a gap to the end of the ring, dropping the oldest. */
static void promisc_note_gap(u32 gap) {
	if (le_promisc.gap_count < LE_PROMISC_GAPS) {
		le_promisc.gap[le_promisc.gap_count++] = gap;
		return;
	}

	memmove(&le_promisc.gap[0], &le_promisc.gap[1],
			(LE_PROMISC_GAPS - 1) * sizeof(le_promisc.gap[0]));
	le_promisc.gap[LE_PROMISC_GAPS - 1] = gap;
}

/* The gap this many places back from the newest. */
static u32 promisc_gap_back(unsigned back) {
	return le_promisc.gap[le_promisc.gap_count - 1 - back];
}

/* The connection interval the gaps say, in units of 1.25 ms, or 0 when they
 * do not say one that can be believed.
 *
 * A connection returns to this channel at spacings that repeat every trip
 * round the 37 hop positions, and those spacings add up to 37 connection
 * events because the trip visits every position once. A full channel map
 * gives one spacing, the whole trip; a smaller map gives several, because
 * every unused position is remapped onto a used one, so this channel comes up
 * more than once a trip and at uneven spacings. Either way, one repeat of the
 * pattern is 37 events, which is the one piece of arithmetic that holds for
 * both and needs no guess about the map.
 *
 * So: find the shortest pattern the gaps just seen repeat, and take its total.
 * Nothing is reported until the gaps do repeat, which is also what keeps a
 * missed return out of the answer: a return the board did not hear makes one
 * gap the sum of two, and a run with such a gap in it does not repeat.
 */
static u16 promisc_interval(void) {
	unsigned period, i;

	for (period = 1; period <= LE_PROMISC_GAPS / 2; ++period) {
		u32 total = 0;
		u32 interval, unit;

		if (le_promisc.gap_count < 2 * period)
			break;

		/* Two runs of `period` gaps back to back, the newer against the
		 * older. */
		for (i = 0; i < period; ++i)
			if (!le_same_span(promisc_gap_back(i),
					promisc_gap_back(i + period)))
				break;
		if (i < period)
			continue;

		for (i = 0; i < period; ++i)
			total += promisc_gap_back(i);

		/* One repeat is one trip round the channels, so it cannot be
		 * longer than the longest trip. Checked before the division,
		 * because the sum of this many gaps can also be more than a
		 * 32-bit count holds. */
		if (total > LE_PROMISC_MAX_GAP || total < promisc_gap_back(0))
			continue;

		interval = LE_DIVIDE_ROUND(total, LE_HOP_CHANNELS * LE_BASECLK);

		/* Nothing outside the range a connection interval may take is
		 * one. That rules out every pattern totalling less than the
		 * 277.5 ms a trip round 37 channels takes at the shortest
		 * interval, which is where the figures too small to believe
		 * came from. */
		if (interval < LE_MIN_CONN_INTERVAL ||
				interval > LE_MAX_CONN_INTERVAL)
			return 0;

		/* Every gap in the pattern is a whole number of connection
		 * events, so it has to be a whole number of this interval, and
		 * no more events than a trip round the channels holds. A run of
		 * gaps that is part of a longer pattern repeats too -- the last
		 * two of 11, 7, 19, 11, 7, 19 are 19, 11 twice over -- and this
		 * is what tells the part from the whole: a part-pattern's total
		 * is not 37 events, so the interval it gives does not measure
		 * its own gaps. */
		unit = interval * LE_BASECLK;
		for (i = 0; i < period; ++i) {
			u32 gap = promisc_gap_back(i);
			u32 events = LE_DIVIDE_ROUND(gap, unit);

			if (events < 1 || events > LE_HOP_CHANNELS)
				return 0;
			if (!le_same_span(gap, events * unit))
				return 0;
		}

		return (u16)interval;
	}

	return 0;
}

/*
 * How many sightings in a row the estimator above gets to itself before an
 * algorithm-2 search runs, and again between one search and the next.
 *
 * A search leaves the receiver deaf while it runs, so the two sightings either
 * side of one are not neighbours and the gap between them is not a gap between
 * consecutive returns. The estimator above reads the pattern the gaps repeat
 * and needs a run of consecutive ones: two runs of the longest pattern it
 * looks for, which is 32 gaps, and then four more reports that agree, so 36.
 * Both estimators start again after every search, so this has to be more than
 * that or a connection the estimator above could have settled on would never
 * get the chance.
 */
#define LE_PROMISC_CSA2_RUN 40

/*
 * Throw away what both estimators have collected and start again from the
 * next sighting.
 *
 * Both read a run of gaps between consecutive returns to this channel: the one
 * above looks for the pattern they repeat, and the algorithm-2 search adds
 * them up to say how many connection events apart the sightings are. So
 * anything that breaks the run has to empty them, not just leave a gap out.
 * A ring with a gap missing from the middle is still a chain of numbers that
 * add up, and every sighting older than the hole then looks nearer the newest
 * than it was: the search finds the counter that explains those offsets,
 * scores it well, and adopts an answer that puts the follow on the wrong
 * channel for every event. Nothing in the answer says so; the capture simply
 * comes back empty.
 *
 * Clearing the stamp is what makes the next sighting an anchor rather than the
 * end of a gap measured across the break.
 */
static void promisc_restart_estimators(void) {
	le_promisc.gap_count = 0;
	le_promisc.csa2_gap_count = 0;
	le_promisc.sightings = 0;
	le_promisc.consec_intervals = 0;
	le_promisc.seen_a_packet = 0;
	le.conn_interval = 0;
}

/* Add a gap to the algorithm-2 ring, dropping the oldest. */
static void promisc_csa2_note_gap(u32 gap) {
	unsigned i;

	if (le_promisc.csa2_gap_count < LE_CSA2_SIGHTINGS - 1) {
		le_promisc.csa2_gap[le_promisc.csa2_gap_count++] = gap;
		return;
	}

	for (i = 0; i + 1 < LE_CSA2_SIGHTINGS - 1; ++i)
		le_promisc.csa2_gap[i] = le_promisc.csa2_gap[i + 1];
	le_promisc.csa2_gap[LE_CSA2_SIGHTINGS - 2] = gap;
}

/*
 * Search for a connection hopping with channel selection algorithm 2 in the
 * sightings collected so far, and follow it if the search finds one. Returns
 * whether it did.
 *
 * `sighting` is the capture clock at the packet that has just arrived, which
 * is the event the search's answer belongs to. The search takes a couple of
 * hundred milliseconds, so the clock is read again afterwards and the event
 * counter carried forward over the events that went by in the meantime.
 */
static int promisc_try_csa2(u32 sighting) {
	le_csa2_recovery_t rec;
	u32 now, elapsed, unit, to_next_event;
	u16 events_ahead;

	if (!le_csa2_recover(le_promisc.csa2_gap, le_promisc.csa2_gap_count,
			btle_channel_index(channel), le.access_address, &rec))
		return 0;

	unit = (u32)rec.conn_interval * LE_BASECLK;

	now = CLK100NS;
	elapsed = now >= sighting ? now - sighting
			: (CLK100NS_PERIOD - sighting) + now;
	to_next_event = unit - elapsed % unit;

	/* Hop to the channel of the next connection event, a little before it
	 * starts: the receiver then sits on that channel for the whole event.
	 * An event already this close is skipped, because the hop cannot be
	 * scheduled finer than the 1.25 ms the interval timer counts in. */
	events_ahead = 1;
	if (to_next_event < LE_BASECLK) {
		to_next_event += unit;
		events_ahead = 2;
	}

	le.conn_interval = rec.conn_interval;
	le.chan_sel_2 = 1;
	le.event_counter = (u16)(rec.event_counter + elapsed / unit +
			events_ahead);
	/* The follow compares a connection update's instant against this, and
	 * an instant is an event counter, so it starts from the same place and
	 * the timer below keeps the two in step. */
	le.conn_count = le.event_counter;
	le.conn_epoch = clkn;
	le.interval_timer = (u16)(to_next_event / LE_BASECLK);
	do_hop = 0;
	le.link_state = LINK_CONNECTED;
	le.crc_verify = 0;
	hop_mode = HOP_BTLE;
	packet_cb = connection_follow_cb;

	le_promisc_state(2, &le.conn_interval, 2);
	/* The counter the follow starts on, not the one the search answered
	 * with: the search's answer belongs to a sighting a few hundred
	 * milliseconds old, and this is the figure a reader can line up
	 * against the connection. */
	le_promisc_state(4, &le.event_counter, 2);

	if (jam_mode != JAM_NONE)
		le_jam_count = JAM_COUNT_DEFAULT;

	return 1;
}

void promisc_recover_hop_interval(u8 *packet) {
	u32 cur_clk = CLK100NS;
	u32 measured, clk_diff;
	u16 obsv_hop_interval; // observed hop interval

	/* The first packet of a pass is the start of the measurement, not the
	 * end of one. */
	if (!le_promisc.seen_a_packet) {
		le_promisc.seen_a_packet = 1;
		le_promisc.prev_clk = cur_clk;
		return;
	}

	measured = cur_clk;
	if (measured < le_promisc.prev_clk)
		measured += CLK100NS_PERIOD; // handle rollover
	clk_diff = measured - le_promisc.prev_clk;

	/* Shorter than one connection interval, so both packets belong to the
	 * same connection event -- the central's and the peripheral's halves
	 * of one exchange, or a second exchange in the same event. That is not
	 * a visit to this channel, and the sighting does not become the one the
	 * next gap is measured from either: the event's first packet is where
	 * the connection's own grid is. */
	if (clk_diff < LE_MIN_CONN_INTERVAL * LE_BASECLK)
		return;

	/* The clock reading, not the value the rollover was undone on: that
	 * one is above the clock's range, so the next sighting would measure a
	 * gap from a time the clock never shows. */
	le_promisc.prev_clk = cur_clk;

	/* Longer than a trip round the channels at the longest interval the
	 * specification allows. The connection was away, or the clock wrapped
	 * more than once: either way the gap says nothing, for or against, and
	 * the two sightings it lies between are not neighbours. */
	if (clk_diff > LE_PROMISC_MAX_GAP) {
		promisc_restart_estimators();
		return;
	}

	promisc_note_gap(clk_diff);
	promisc_csa2_note_gap(clk_diff);
	++le_promisc.sightings;

	if (le_promisc.sightings >= LE_PROMISC_CSA2_RUN &&
			le_promisc.csa2_gap_count == LE_CSA2_SIGHTINGS - 1) {
		if (promisc_try_csa2(cur_clk))
			return;

		/* The search was deaf while it ran, so the next gap spans it
		 * and the sightings either side of it are not neighbours.
		 * Starting again also means the next search works from
		 * sightings this one has not already turned down. */
		promisc_restart_estimators();
		return;
	}

	obsv_hop_interval = promisc_interval();

	if (obsv_hop_interval == 0) {
		/* Nothing believable: report nothing and let go of anything
		 * already adopted, rather than stand by a figure the gaps no
		 * longer support. */
		le.conn_interval = 0;
		le_promisc.consec_intervals = 0;
		return;
	}

	if (le.conn_interval == obsv_hop_interval) {
		// 5 consecutive hop intervals: consider it legit and move on
		++le_promisc.consec_intervals;
		if (le_promisc.consec_intervals == 5) {
			packet_cb = promisc_recover_hop_increment;
			hop_direct_channel = 2404;
			hop_mode = HOP_DIRECT;
			do_hop = 1;
			le_promisc_state(2, &le.conn_interval, 2);
		}
	} else {
		le.conn_interval = obsv_hop_interval;
		le_promisc.consec_intervals = 0;
	}
}

/*
 * An empty data PDU's header, with the two sequence bits masked off.
 *
 * CRCInit is worked out backwards from the CRC of a packet whose contents are
 * known, and an empty data PDU is the one packet of a connection whose
 * contents can be known without it: two header bytes and nothing else. Its
 * header is LLID 1 with a payload length of zero, and the acknowledgment bits
 * -- next expected sequence number and sequence number -- are whatever the
 * connection's traffic has made them, so they are masked out and the rest is
 * compared.
 *
 * Reading them as part of the header is what the mode did, and it meant
 * waiting for an empty PDU that happened to carry both bits clear. On a
 * connection with data flowing that is one empty PDU in a few hundred: of
 * 2666 the board heard from a 50 ms link between two development boards, ten
 * had both bits clear. So everything the mode recovers after the access
 * address -- the CRCInit, the connection interval, the hop increment, the
 * algorithm-2 search -- waited on luck, and a capture could run for minutes
 * and report nothing more. The access-address half of the mode has always
 * accepted all four headers.
 */
#define LE_EMPTY_PDU_HEADER_MASK 0xf3
#define LE_EMPTY_PDU_HEADER 0x01

void promisc_follow_cb(u8 *packet) {
	int i;

	// get the CRCInit
	if (!le.crc_verify &&
			(packet[4] & LE_EMPTY_PDU_HEADER_MASK) ==
					LE_EMPTY_PDU_HEADER &&
			packet[5] == 0x00) {
		u32 crc = (packet[8] << 16) | (packet[7] << 8) | packet[6];

		le.crc_init = btle_reverse_crc(crc, packet + 4, 2);
		le.crc_init_reversed = 0;
		for (i = 0; i < 24; ++i)
			le.crc_init_reversed |= ((le.crc_init >> i) & 1) << (23 - i);

		le.crc_verify = 1;
		packet_cb = promisc_recover_hop_interval;
		le_promisc_state(1, &le.crc_init, 3);
	}
}

// called when we see an AA, add it to the list
void see_aa(u32 aa) {
	int i, max = -1, killme = -1;
	for (i = 0; i < AA_LIST_SIZE; ++i)
		if (le_promisc.active_aa[i].aa == aa) {
			++le_promisc.active_aa[i].count;
			return;
		}

	// evict someone
	for (i = 0; i < AA_LIST_SIZE; ++i)
		if (le_promisc.active_aa[i].count < max || max < 0) {
			killme = i;
			max = le_promisc.active_aa[i].count;
		}

	le_promisc.active_aa[killme].aa = aa;
	le_promisc.active_aa[killme].count = 1;
}

/* le promiscuous mode */
int cb_le_promisc(char *unpacked) {
	int i, j, k;
	int idx;

	// empty data PDU: 01 00
	char desired[4][16] = {
		{ 1, 0, 0, 0, 0, 0, 0, 0,
		  0, 0, 0, 0, 0, 0, 0, 0, },
		{ 1, 0, 0, 1, 0, 0, 0, 0,
		  0, 0, 0, 0, 0, 0, 0, 0, },
		{ 1, 0, 1, 0, 0, 0, 0, 0,
		  0, 0, 0, 0, 0, 0, 0, 0, },
		{ 1, 0, 1, 1, 0, 0, 0, 0,
		  0, 0, 0, 0, 0, 0, 0, 0, },
	};

	for (i = 0; i < 4; ++i) {
		idx = whitening_index[btle_channel_index(channel)];

		// whiten the desired data
		for (j = 0; j < (int)sizeof(desired[i]); ++j) {
			desired[i][j] ^= whitening[idx];
			idx = (idx + 1) % sizeof(whitening);
		}
	}

	// then look for that bitsream in our receive buffer
	for (i = 32; i < (DMA_SIZE*8*2 - 32 - 16); i++) {
		int ok[4] = { 1, 1, 1, 1 };
		int matching = -1;

		for (j = 0; j < 4; ++j) {
			for (k = 0; k < (int)sizeof(desired[j]); ++k) {
				if (unpacked[i+k] != desired[j][k]) {
					ok[j] = 0;
					break;
				}
			}
		}

		// see if any match
		for (j = 0; j < 4; ++j) {
			if (ok[j]) {
				matching = j;
				break;
			}
		}

		// skip if no match
		if (matching < 0)
			continue;

		// found a match! unwhiten it and send it home
		idx = whitening_index[btle_channel_index(channel)];
		for (j = 0; j < 4+3+3; ++j) {
			u8 byte = 0;
			for (k = 0; k < 8; k++) {
				int offset = k + (j * 8) + i - 32;
				if (offset >= DMA_SIZE*8*2) break;
				int bit = unpacked[offset];
				if (j >= 4) { // unwhiten data bytes
					bit ^= whitening[idx];
					idx = (idx + 1) % sizeof(whitening);
				}
				byte |= bit << k;
			}
			idle_rxbuf[j] = byte;
		}

		u32 aa = (idle_rxbuf[3] << 24) |
				 (idle_rxbuf[2] << 16) |
				 (idle_rxbuf[1] <<  8) |
				 (idle_rxbuf[0]);
		see_aa(aa);

		enqueue(LE_PACKET, (uint8_t*)idle_rxbuf);

	}

	// once we see an AA 5 times, start following it
	for (i = 0; i < AA_LIST_SIZE; ++i) {
		if (le_promisc.active_aa[i].count > 3) {
			le_set_access_address(le_promisc.active_aa[i].aa);
			data_cb = cb_follow_le;
			packet_cb = promisc_follow_cb;
			le.crc_verify = 0;
			// quit using the old stuff and switch to sync mode
			return 0;
		}
	}

	return 1;
}

void bt_promisc_le() {
	while (requested_mode == MODE_BT_PROMISC_LE) {
		reset_le_promisc();

		// jump to a random data channel and turn up the squelch
		if ((channel & 1) == 1)
			channel = 2440;

		// if the PC hasn't given us AA, determine by listening
		if (!le.access_address_set) {
			// cs_threshold_req = -80;
			cs_threshold_calc_and_set(channel);
			data_cb = cb_le_promisc;
			bt_generic_le(MODE_BT_PROMISC_LE);
		}

		// could have got mode change in middle of above
		if (requested_mode != MODE_BT_PROMISC_LE)
			break;

		// bt_generic_le()'s teardown put the channel back to 2441 MHz, which
		// is not a BLE channel; the correction at the top of this loop does
		// not run between the two halves. Until this fork's dispatch rework
		// that teardown also cleared requested_mode, so the break above
		// always fired and the follow half below never ran at all -- the
		// mode ended silently as soon as it had an access address.
		if ((channel & 1) == 1)
			channel = 2440;

		packet_cb = promisc_follow_cb;
		le.crc_verify = 0;
		bt_le_sync(MODE_BT_PROMISC_LE);
	}
}

#ifdef TX_ENABLE
void bt_peripheral_le() {
	u32 calc_crc;
	int i;
	/* Two header bytes, the six address bytes, the advertising data and the
	 * three CRC bytes. A legacy ADV_IND, which is all this sends, carries
	 * at most ADV_IND_MAX_DATA bytes of data, so that is what the buffer
	 * has to hold.
	 *
	 * UBERTOOTH_LE_SET_ADV_DATA refuses anything longer, so the check below
	 * cannot fire from a host request. It stays as the guard on this
	 * buffer: whoever sets le_adv_data next should not be able to overrun
	 * the record built from it. */
	uint8_t adv_ind[2 + 6 + ADV_IND_MAX_DATA + 3] = { 0x00, };
	unsigned adv_ind_len;

	if (le_adv_len > ADV_IND_MAX_DATA) {
		requested_mode = MODE_IDLE;
		return;
	}

	adv_ind_len = 6 + le_adv_len;
	adv_ind[1] = adv_ind_len;

	// copy the user-specified mac address
	for (i = 0; i < 6; ++i)
		adv_ind[i+2] = peripheral_mac_address[5-i];

	// copy in the adv data
	memcpy(adv_ind + 2 + 6, le_adv_data, le_adv_len);

	// total: 2 + 6 + le_adv_len
	adv_ind_len += 2;

	calc_crc = btle_calc_crc(le.crc_init_reversed, adv_ind, adv_ind_len);
	adv_ind[adv_ind_len + 0] = (calc_crc >>  0) & 0xff;
	adv_ind[adv_ind_len + 1] = (calc_crc >>  8) & 0xff;
	adv_ind[adv_ind_len + 2] = (calc_crc >> 16) & 0xff;

	clkn_start();

	// enable USB interrupts due to busy waits
	ISER0 = ISER0_ISE_USB;

	// spam advertising packets
	while (requested_mode == MODE_BT_PERIPHERAL_LE) {
		watchdog_usb_ready();
		le_transmit(0x8e89bed6, adv_ind_len+3, adv_ind);
		msleep(100);
	}

	// disable USB interrupts
	ICER0 = ICER0_ICE_USB;
}
#endif

/*
 * The record generic receive sends: the sync word it matched on, then the
 * bytes read out of the radio's FIFO.
 */
#define RX_GENERIC_SYNCWORD_LEN 4
#define RX_GENERIC_DATA_LEN     32

/*
 * One more byte after the payload: 1 when the radio said this packet's CRC
 * checked out, 0 when it did not.
 *
 * It needs GRMDM to have the CRC on at this end to mean anything, which is
 * the host's doing: with it off the radio called packets carrying no CRC at
 * all good.
 *
 * The length it can be trusted at is the full read below. STATUS_CRC_OK is
 * defined against the read position -- the next two bytes in the FIFO against
 * the CRC16 over the bytes already read out of it (see cc2400_fifo_read(),
 * where a range test that read the wrong number of bytes is what put that
 * note there) -- and this read has no length to go by, because the framing
 * carries none. A 32-byte payload is the case where the read stops where the
 * CRC is, and the case the bench measured corruption at. Eight-byte payloads
 * tracked the transmit's CRC bit as well, so the flag is not scrambled by the
 * shorter read, but nothing has caught a corrupt short packet with it. The
 * receive records the radio's answer; the man pages say where it has been
 * checked.
 */
#define RX_GENERIC_CRC_OFFSET (RX_GENERIC_SYNCWORD_LEN + RX_GENERIC_DATA_LEN)

/*
 * enqueue() is handed a pointer and no length, and copies DMA_SIZE bytes out
 * of it, so a buffer shorter than that has the bytes past its end copied into
 * the record. The buffer below is therefore DMA_SIZE whatever the record uses
 * of it.
 */
_Static_assert(RX_GENERIC_CRC_OFFSET < DMA_SIZE,
               "the generic receive record must fit in what enqueue() copies");

void rx_generic_sync(void) {
	/*
	 * Zeroed because the capture fills only the first
	 * RX_GENERIC_SYNCWORD_LEN + RX_GENERIC_DATA_LEN bytes and the whole
	 * buffer is sent: the tail used to be 14 bytes of whatever was on the
	 * stack below it, read past the end of a 36-byte buffer and shipped to
	 * the host in every record.
	 */
	u8 buf[DMA_SIZE] = { 0 };
	u16 reg_val;

	/* The size the buffer has to be, which is not the same question as the
	 * one above: a 36-byte buffer satisfies that assert, and a 36-byte
	 * buffer is what sent the stack bytes. */
	_Static_assert(sizeof(buf) >= DMA_SIZE,
	               "enqueue() copies DMA_SIZE bytes out of this buffer");

	/* Put syncword at start of buffer
	 * DGS: fix this later, we don't know number of syncword bytes, etc
	 */
	reg_val = cc2400_get(SYNCH);
	buf[0] = (reg_val >> 8) & 0xFF;
	buf[1] = reg_val & 0xFF;
	reg_val = cc2400_get(SYNCL);
	buf[2] = (reg_val >> 8) & 0xFF;
	buf[3] = reg_val & 0xFF;

	usb_queue_init();
	clkn_start();

	/* Bounded without a service callback, as in cc2400_rx(). */
	cc2400_wait_xosc_stable(NULL);
	cc2400_strobe(SFSON);
	cc2400_wait_fs(1, NULL);
	RXLED_SET;
#ifdef UBERTOOTH_ONE
		PAEN_SET;
		HGM_SET;
#endif
	/*
	 * This used to be `while (1)`, with the wait for a sync word below
	 * spinning on the radio and USB serviced only once a packet had arrived.
	 * So on a quiet channel -- which is every channel, until something
	 * transmits the configured sync word -- the board answered no control
	 * request at all and nothing could stop the mode: not even
	 * `ubertooth-util -S`, because the loop never looked at requested_mode.
	 * It needed a re-plug.
	 *
	 * Now the loop ends when the mode does, and the wait services USB while
	 * it polls, so the request that stops the mode gets through.
	 */
	while (requested_mode == MODE_RX_GENERIC) {
		cc2400_wait_fsm(STATE_STROBE_FS_ON, service_usb);
		cc2400_strobe(SRX);
		USRLED_CLR;

		/* Wait for a packet. This one has no deadline -- nothing may be
		 * transmitting the sync word, perhaps for hours, and that is not a
		 * failure -- so instead of a poll bound it services USB and leaves
		 * when the host stops the mode. The main loop's idle reset clears
		 * the LEDs, the amplifier and the radio on the way out. */
		while (!(cc2400_status() & SYNC_RECEIVED)) {
			service_usb();
			if (requested_mode != MODE_RX_GENERIC)
				return;
		}
		USRLED_SET;

		/*
		 * See RX_FIFO_LEAD_BYTES: reading the moment the sync word ends
		 * returns a byte 0 the radio has not written yet.
		 *
		 * The lead is four byte times at 1 Mbit/s. This capture does
		 * not set the modem up, so the rate is whatever the host wrote
		 * to MDMCTRL, and the radio also has a 250 kbit/s mode; at that
		 * rate the same wait is one byte rather than four. Still the
		 * byte the wait exists for, with nothing to spare -- and the
		 * record is wrong for another reason at that rate anyway, since
		 * the read below takes about 14 us a byte and would outrun a
		 * 32 us byte on the air for the whole record.
		 */
		rx_wait_for_fifo();
		cc2400_fifo_read(RX_GENERIC_DATA_LEN,
		                 buf + RX_GENERIC_SYNCWORD_LEN);
		/* What the radio made of the packet's CRC, read after the
		 * payload is out of the FIFO: see RX_GENERIC_CRC_OFFSET. */
		buf[RX_GENERIC_CRC_OFFSET] =
			(cc2400_status() & STATUS_CRC_OK) ? 1 : 0;
		enqueue(BR_PACKET, buf);
		service_usb();
	}
}

void rx_generic(void) {
	/*
	 * Which receive the mode runs follows GRMDM's packet-mode bit, which
	 * whatever set the radio up left there. The test used to be `&& 0x0400`,
	 * so it asked whether GRMDM read non-zero at all: every configured
	 * radio took the packet-mode path and the symbol stream below was dead
	 * code. grmdm_packet_mode() is the bit test it was meant to be.
	 */
	if (grmdm_packet_mode(cc2400_get(GRMDM))) {
		rx_generic_sync();
	} else {
		modulation = MOD_NONE;
		bt_stream_rx();
	}
}

#ifdef TX_ENABLE
/*
 * Transmit one packet and ask for the mode that was running before, which the
 * dispatch passes in: `mode` is already MODE_TX_GENERIC by the time this runs,
 * because the dispatch records the mode it is entering for every mode alike.
 */
void tx_generic(u8 prev_mode) {
	u16 synch, syncl, mdmctrl, mdmtst0, grmdm, fsdiv, frend, gio_save;
	u8 len, sent, chunk;
	/* Read once: the request handler writes it, and a request answered
	 * between packets below must not lengthen the burst running now. It is
	 * at least 1, which the handler saw to. */
	u16 left = tx_pkt_repeat;

	/*
	 * Every radio register this transmit writes is saved here and put back
	 * when the packet is out, because nothing sets the radio up again
	 * afterwards: the generic modes take the radio as the host left it,
	 * through UBERTOOTH_SET_* and UBERTOOTH_WRITE_REGISTERS, and the mode
	 * the board returns to reads what this leaves behind.
	 *
	 * GRMDM is the one that decides something: rx_generic() reads its
	 * packet-mode bit to choose between the packet receive and the raw
	 * symbol stream, and the value written below has that bit set, so a
	 * receive entered after a transmit used to take the packet path
	 * whatever the host had asked for. MDMCTRL, MDMTST0 and FSDIV are the
	 * modem settings and the channel, which the same receive would
	 * otherwise inherit from this packet.
	 */
	synch = cc2400_get(SYNCH);
	syncl = cc2400_get(SYNCL);
	mdmctrl = cc2400_get(MDMCTRL);
	mdmtst0 = cc2400_get(MDMTST0);
	grmdm = cc2400_get(GRMDM);
	fsdiv = cc2400_get(FSDIV);
	frend = cc2400_get(FREND);
	gio_save = cc2400_get(IOCFG);

	cc2400_set(SYNCH, tx_pkt.synch);
	cc2400_set(SYNCL, tx_pkt.syncl);
	/*
	 * About 340 kHz of frequency deviation: twice the Basic Rate one, and
	 * wider than any other mode here sets. A board in the generic receive
	 * demodulates it with its own modem set to either deviation.
	 */
	cc2400_set(MDMCTRL, 0x0057);
	cc2400_set(MDMTST0, 0x134b);
	/*
	 * The framing: buffered mode, packet mode, a preamble of 01010101
	 * (field 6; field 7 received nothing at all on the bench), the whole
	 * 32-bit sync word, and the radio's CRC. The radio adds all of it and
	 * the generic receive programs none of it, so a board meant to read
	 * this transmit has to be set to the same GRMDM value and sync word.
	 *
	 * The CRC is on because nothing else in this framing can tell a
	 * payload with a bit error in it from a payload, and it costs no
	 * detections. The radio reports its verdict rather than dropping the
	 * packet, so the receive writes it down: RX_GENERIC_CRC_OFFSET.
	 *
	 * Packet mode is what puts the payload in the radio's FIFO for the
	 * write loop below. It is also the bit rx_generic() reads to choose
	 * the packet receive, which is why the restore below matters.
	 */
	cc2400_set(GRMDM, 0x0f61 | GRMDM_CRC);
	cc2400_set(FSDIV, tx_pkt.channel);
	/*
	 * The level belongs to this one packet, so it is taken from the request
	 * rather than from the radio, but only its three level bits: bit 3 is
	 * reserved and must be written as 1, and the level set with
	 * `ubertooth-util -a` is put back below.
	 */
	cc2400_set(FREND, FREND_RESERVED_ONE | (tx_pkt.pa_level & FREND_PA_LEVEL));
	/* GIO6 reports the radio's FIFO empty, which is what the write loop
	 * below waits on. It must not be FIFO_FULL, which br_transmit() asks
	 * for: that is a different signal with the opposite sense. */
	cc2400_set(IOCFG, (GIO_FIFO_EMPTY << 9) | (gio_save & 0x1ff));

send_one:
	/* Bounded without a service callback, as in le_transmit(): one packet,
	 * then either the next of the burst below or back to the mode loop. */
	cc2400_wait_xosc_stable(NULL);
	cc2400_strobe(SFSON);
	cc2400_wait_fs(1, NULL);
	TXLED_SET;
#ifdef UBERTOOTH_ONE
		PAEN_SET;
#endif
	cc2400_wait_fsm(STATE_STROBE_FS_ON, NULL);

	/*
	 * The payload goes into the radio's FIFO a FIFO's worth at a time,
	 * waiting for the radio to send what it has between pieces, as
	 * le_transmit() does. The FIFO holds CC2400_FIFO_SIZE bytes and a
	 * generic transmit packet may carry DMA_SIZE, 50 bytes against the
	 * FIFO's 32, so a single write would push the rest past the end of the
	 * FIFO and what went on the air would be undefined.
	 *
	 * The wait is for the FIFO to be empty. The modem takes bytes out of it
	 * at the air rate, so between pieces it has nothing to send for as long
	 * as the next write takes: cc2400_fifo_write() costs about 1 us a byte,
	 * so about 20 us for the 18 bytes a 50-byte packet has left, against
	 * 8 us a byte on the air. Whether the radio carries a packet across
	 * that gap, or the strobe below frames the rest as a second
	 * transmission, is not known: an Ubertooth receiver reads one FIFO's
	 * worth per sync word, so it cannot see the difference. The
	 * alternative, refilling at the threshold in INT before the FIFO runs
	 * dry, needs that measurement and a reading of what the radio asserts
	 * its FIFO signals on, so waiting on empty is what both refill loops
	 * do until someone has those.
	 *
	 * The length is clamped as well as chunked, because this is where a
	 * length reaches the radio: the vendor request already refuses a longer
	 * packet, and a write of tx_pkt.length on its own would read past
	 * tx_pkt.data.
	 *
	 * A zero-length packet still strobes once: the radio sends the preamble
	 * and sync word with no payload.
	 */
	len = MIN(tx_pkt.length, DMA_SIZE);
	sent = 0;
	do {
		/* Nothing to wait for before the first piece: the radio has not
		 * been strobed to transmit, so the FIFO cannot be draining.
		 *
		 * A wait that gives up ends the packet here instead of writing
		 * the rest. The radio still has what it was given, so the next
		 * piece would go past the end of the FIFO -- which is the thing
		 * this loop exists to avoid. Either way this packet is lost;
		 * this way nothing is written past the FIFO to lose it. */
		if (sent != 0 && !fifo_wait_empty())
			break;

		chunk = MIN(len - sent, CC2400_FIFO_SIZE);
		cc2400_fifo_write(chunk, tx_pkt.data + sent);
		cc2400_strobe(STX);
		sent += chunk;
	} while (sent < len);

	cc2400_wait_fsm(STATE_STROBE_FS_ON, NULL);
	TXLED_CLR;

	cc2400_strobe(SRFOFF);
	cc2400_wait_fs_unlock();

#ifdef UBERTOOTH_ONE
	PAEN_CLR;
#endif

	/*
	 * The next packet of the burst, if the request asked for more than one.
	 *
	 * Inside the register save and restore below, because the packet is
	 * framed by registers the restore puts back: a loop around the restore
	 * sent its second and later packets with the Bluetooth registers in
	 * place, and a board set to this framing read one packet of thirty.
	 *
	 * USB is serviced between packets, so a request that ends the mode --
	 * ubertooth-util -S, or any other mode -- still gets through, and a
	 * burst long enough to matter can be stopped. The main loop's own
	 * service pass does not come round until the burst is over.
	 */
	if (--left > 0) {
		service_usb();
		if (requested_mode == MODE_TX_GENERIC)
			goto send_one;
	}

	// Restore state, in the order it was set up above
	cc2400_set(SYNCH, synch);
	cc2400_set(SYNCL, syncl);
	cc2400_set(MDMCTRL, mdmctrl);
	cc2400_set(MDMTST0, mdmtst0);
	cc2400_set(GRMDM, grmdm);
	cc2400_set(FSDIV, fsdiv);
	cc2400_set(FREND, frend_keep_pa_level(frend));
	cc2400_set(IOCFG, gio_save);
	/*
	 * Back to what was running before, unless a request serviced between
	 * packets above asked for something else: overwriting that would tell
	 * the host its mode had started and then drop it.
	 */
	if (requested_mode == MODE_TX_GENERIC)
		requested_mode = prev_mode;
}
#endif

/* spectrum analysis */
void specan()
{
	u16 f;
	u8 i = 0;
	/* Sixteen samples go in a record, three bytes each, and the record is
	 * the DMA_SIZE enqueue() copies: the last two bytes are never written,
	 * and they are sent to the host all the same. Zeroed so what goes out is
	 * not whatever was on the stack. */
	u8 buf[DMA_SIZE] = { 0 };

	RXLED_SET;

	usb_queue_init();
	clkn_start();

#ifdef UBERTOOTH_ONE
	PAEN_SET;
	//HGM_SET;
#endif
	cc2400_set(LMTST,   0x2b22);
	cc2400_set(MDMTST0, 0x134b); // without PRNG
	cc2400_set(GRMDM,   0x0101); // un-buffered mode, GFSK
	cc2400_set(MDMCTRL, 0x0029); // 160 kHz frequency deviation
	//FIXME maybe set RSSI.RSSI_FILT
	/* Bounded: this mode services USB from its own loop below, so a radio
	 * that never reports ready here used to leave the board enumerated and
	 * answering no control request at all. Three of the wedges reported
	 * against this firmware followed a specan run.
	 *
	 * No service callback on these two, as in cc2400_rx(): they run once,
	 * before the loop starts. */
	cc2400_wait_xosc_stable(NULL);
	cc2400_wait_fs_unlock();

	while (requested_mode == MODE_SPECAN) {
		for (f = low_freq; f < high_freq + 1; f++) {
			cc2400_set(FSDIV, f - 1);
			cc2400_strobe(SFSON);
			/* The two waits either side of a sample are where this loop
			 * spends its time, and it only drains endpoint 0 every sixteenth
			 * sample, so they are also where a control request stalls.
			 * Servicing USB between their status polls is what led_specan()
			 * does. This loop pays more for it than that one did, because it
			 * takes a sample at every frequency step rather than cycling
			 * three. What it costs in samples per second has not been
			 * measured; the pass runs while the radio is settling anyway, so
			 * the expectation is that it is small, not that it is nothing.
			 *
			 * The bound is separate and was already here: a sample taken
			 * without a lock is junk, which the host can see, while a wait
			 * without a bound is a board that has to be power-cycled. */
			cc2400_wait_fs(1, service_usb);
			cc2400_strobe(SRX);

			/* give the CC2400 time to acquire RSSI reading */
			service_usb();
			volatile u32 j = 500; while (--j); //FIXME crude delay
			service_usb();
			buf[3 * i] = (f >> 8) & 0xFF;
			buf[(3 * i) + 1] = f  & 0xFF;
			buf[(3 * i) + 2] = cc2400_get(RSSI) >> 8;
			i++;
			if (i == 16) {
				enqueue(SPECAN, buf);
				i = 0;

				service_usb();
			}

			cc2400_strobe(SRFOFF);
			cc2400_wait_fs(0, service_usb);
		}
	}
	RXLED_CLR;
}

/* LED based spectrum analysis */
void led_specan()
{
	int8_t lvl;
	u8 i = 0;
	u16 channels[3] = {2412, 2437, 2462};
	//void (*set[3]) = {TXLED_SET, RXLED_SET, USRLED_SET};
	//void (*clr[3]) = {TXLED_CLR, RXLED_CLR, USRLED_CLR};

#ifdef UBERTOOTH_ONE
	PAEN_SET;
	//HGM_SET;
#endif
	cc2400_set(LMTST,   0x2b22);
	cc2400_set(MDMTST0, 0x134b); // without PRNG
	cc2400_set(GRMDM,   0x0101); // un-buffered mode, GFSK
	cc2400_set(MDMCTRL, 0x0029); // 160 kHz frequency deviation
	cc2400_set(RSSI,    0x00F1); // RSSI Sample over 2 symbols

	/* Bounded, as in specan(): this is before the loop that services USB
	 * has started, so a radio that never reports ready here stops the board
	 * answering altogether. */
	cc2400_wait_xosc_stable(NULL);
	cc2400_wait_fs_unlock();

	while (requested_mode == MODE_LED_SPECAN) {
		cc2400_set(FSDIV, channels[i] - 1);
		cc2400_strobe(SFSON);
		cc2400_wait_fs(1, service_usb);
		cc2400_strobe(SRX);

		/* give the CC2400 time to acquire RSSI reading */
		service_usb();
		volatile u32 j = 500; while (--j); //FIXME crude delay
		service_usb();

		lvl = (int8_t)((cc2400_get(RSSI) >> 8) & 0xff);
		if (rssi_above_threshold(lvl, rssi_threshold)) {
			switch (i) {
				case 0:
					TXLED_SET;
					break;
				case 1:
					RXLED_SET;
					break;
				case 2:
					USRLED_SET;
					break;
			}
		}
		else {
			switch (i) {
				case 0:
					TXLED_CLR;
					break;
				case 1:
					RXLED_CLR;
					break;
				case 2:
					USRLED_CLR;
					break;
			}
		}

		i = (i+1) % 3;

		cc2400_strobe(SRFOFF);
		cc2400_wait_fs(0, service_usb);
	}
}

int main()
{
	// enable all fault handlers (see fault.c)
	SCB_SHCSR = (1 << 18) | (1 << 17) | (1 << 16);

	/* Before anything that could record a wait site: this is what turns what
	 * the last run left in the breadcrumb into a record of why it ended. */
	watchdog_boot();

	ubertooth_init();
	/* Before the USB stack, which reads the serial number for its own
	 * descriptor, and well before a mode could be running: the boot-ROM
	 * calls this makes must not happen while one is. */
	device_id_init();
	/* Here for the same reason, and before the first request can arrive:
	 * hashing the bootloader region takes milliseconds, which is too long
	 * to spend in a request handler that a capture mode answers from the
	 * USB interrupt. */
	bl_report_init(UBERTOOTH_BOOTLOADER_REGION_START);
	clkn_init();
	ubertooth_usb_init(vendor_request_handler);
	cc2400_idle();
	dma_poweron();

	debug_uart_init(0);
	debug_printf("\n\n****UBERTOOTH BOOT****\n%s\n", compile_info);

	/* Last, because from here on a board that goes the watchdog timeout
	 * without a USB service pass resets itself, and the loop below is what
	 * makes those passes. */
	watchdog_start();

	while (1) {
		u8 next_mode, prev_mode;
		u32 usb_irq;

		handle_usb(clkn);

#ifdef STACK_GUARD_TEST
		/* From here, not from the request handler, so the fault is taken
		 * in thread mode where the memory management handler can run. */
		if (overflow_the_stack_now)
			overflow_next(0);
#endif

		/*
		 * Read the request once. It is written by the control request
		 * handler, which runs from the USB interrupt while the
		 * interrupt-driven modes are active, so reading it twice could
		 * dispatch one mode and record another.
		 */
		next_mode = requested_mode;
		if (next_mode == mode)
			continue;

		prev_mode = mode;

		/*
		 * A mode change, and the only place one happens. Leaving a mode
		 * always runs the idle reset, before the next mode starts: LEDs and
		 * the Ubertooth One amplifier off, the radio quiet, the channel back
		 * to 2441 MHz and the squelch back to -120.
		 *
		 * This used to be a dispatch case of its own (`case MODE_IDLE`),
		 * which meant the reset ran only when MODE_IDLE was still the
		 * request by the time the dispatch was reached. A mode loop that
		 * returned for its own reason, or a second request arriving in the
		 * handle_usb() above, carried the old mode's LEDs, amplifier,
		 * channel and squelch straight into the new mode.
		 *
		 * Leaving a mode also drops whatever it had queued for the host and
		 * never got to send. Those packets belong to a capture that has
		 * ended, and the queue only used to be cleared when a mode started,
		 * so they were handed to whoever read the endpoint next -- before
		 * that reader's own mode had started.
		 *
		 * This is the dispatch's job and not cc2400_idle()'s. That reset
		 * runs at boot, and bt_generic_le() and bt_le_sync() call it in the
		 * middle of a mode, so reaching it does not mean a capture has
		 * ended. This is the one place a mode change happens.
		 *
		 * A single-shot jam is the one mode that ends itself while it has a
		 * capture running, and there the drop does take whatever tail the
		 * host had not collected yet: the cost of the guarantee, and the jam
		 * is the last thing that run was for.
		 *
		 * What this cannot reach is the bulk IN endpoint's two hardware
		 * buffers. A packet already handed to them is past the queue, and
		 * flushing them from here would reset the endpoint's data toggle and
		 * leave the host's toggle one ahead. Resynchronizing a pipe is the
		 * host's to do, and libubertooth does it as it starts reading, which
		 * puts both toggles back to DATA0 together.
		 *
		 * The clear runs with the USB interrupt held off because the queue
		 * is also read from that interrupt, by the UBERTOOTH_POLL request.
		 * Every interrupt-driven mode does disable the interrupt before it
		 * returns, so today nothing could race this; the hold makes that a
		 * property of this code rather than of fifteen mode functions, for
		 * one register read.
		 */
		if (prev_mode != MODE_IDLE) {
			cc2400_idle();
			usb_irq = usb_irq_hold();
			usb_queue_init();
			/*
			 * The per-channel averages go with the queued packets, and for
			 * the same reason: they describe the band during a capture that
			 * has ended. This is the one place that is true, which is why
			 * the clear is here and not in cc2400_idle() -- that runs at
			 * boot and in the middle of the two low-energy modes, and a
			 * clear there would keep restarting the filter under a running
			 * capture.
			 *
			 * Inside the same hold, because the averages are read from the
			 * USB interrupt too: a squelch set relative to them recomputes
			 * the threshold from the handler.
			 */
			rssi_iir_reset();
			/* And what the last capture heard with its channels clear, so
			 * the next one's first records report its own measurement or
			 * none. */
			rssi_noise_reset();
			usb_irq_release(usb_irq);

			/*
			 * The E-GO access code goes back to the one
			 * ubertooth-ego(1) documents, for the reason the AFH map
			 * is forgotten above: it belongs to the capture that
			 * asked for it. It used to be set once at boot and kept
			 * until the board was reset, so an `ubertooth-ego -a
			 * 11223344` left 11223344 behind and the next run
			 * captured with it while reporting nothing.
			 */
			ego_capture_reset();
		}

		/*
		 * Record the new mode here rather than in each case. When each case
		 * did it, a case that left it out kept `mode` at the previous mode,
		 * so stopping the mode it was running found requested_mode == mode
		 * and no reset ran at all; and an unknown request kept `mode`
		 * unchanged forever, spinning this loop at full rate.
		 *
		 * Put back what the host stated for this mode -- before its request
		 * or after it -- which the reset above would otherwise have thrown
		 * away. After `mode`, because a channel request reads it, and
		 * before the cases below, whose own setup wins.
		 *
		 * Both with the USB interrupt held off, as the queue clear above is,
		 * and against the same kind of race: the two requests that state a
		 * setting run from that interrupt. One arriving in the middle of
		 * this would have its own copy dropped by the read-then-clear here,
		 * or leave the channel from its request and the hop mode from the
		 * copy. Recording `mode` inside the hold is what ends the window:
		 * a request that arrives after it keeps nothing, because no mode
		 * start is coming for it.
		 */
		usb_irq = usb_irq_hold();
		mode = next_mode;
		apply_kept_settings(next_mode);
		apply_stated_settings(next_mode);
		usb_irq_release(usb_irq);

		/* The breadcrumb's record of what the board was doing. This is the
		 * one place a mode changes, so it is the one place that needs it. */
		watchdog_mode(mode);

		switch (next_mode) {
			case MODE_RESET:
				watchdog_intent(WATCHDOG_INTENT_RESET);
				/* Allow time for the USB command to return correctly */
				wait(1);
				reset();
				break;
			case MODE_ISP:
				/* The on-chip programmer takes over without a chip reset,
				 * so the watchdog stays armed and nothing in there feeds
				 * it. It cannot be switched off before a reset, so the
				 * timeout is stretched to the longest the counter holds
				 * instead. */
				watchdog_intent(WATCHDOG_INTENT_ISP);
				watchdog_stretch();
				/* Allow time for the USB command to return correctly */
				wait(1);
				set_isp();
				/* set_isp() is not supposed to come back -- it hands the
				 * chip to code in ROM. If it ever does, the board must not
				 * be left running for the next hour with a watchdog that
				 * cannot reset it and a wedge that would not be counted,
				 * and it must still take a request: asking for the
				 * programmer again with the board left in this mode would
				 * be a silent no-op.
				 *
				 * cc2400_idle() had this same write removed for being able
				 * to swallow a request that arrived while it ran. Here
				 * nothing can: the case services no USB, so the only
				 * request that could be lost is one arriving during
				 * set_isp(), which does not return. */
				watchdog_unstretch();
				requested_mode = MODE_IDLE;
				break;
			case MODE_AFH:
				/*
				 * AFH detection hops to the channels the piconet has not
				 * been seen on, and the host narrows that down as packets
				 * arrive. UBERTOOTH_AFH asks for the hop mode as well, which
				 * covers a switch into AFH hopping while a stream is already
				 * running; setting it here is what survives the teardown
				 * above, which puts hop_mode back to HOP_NONE and forgets
				 * the AFH map on the way out of whatever ran before.
				 */
				hop_mode = HOP_AFH;
				afh_enabled = 1;
				/* fall through */
			case MODE_RX_SYMBOLS:
				bt_stream_rx();
				break;
			case MODE_BT_FOLLOW:
				/* Hopping with the piconet is the whole of following, and
				 * UBERTOOTH_START_HOPPING asked for it before the teardown
				 * above ran: ask again, as MODE_AFH does. */
				hop_mode = HOP_BLUETOOTH;
				bt_stream_rx();
				break;
#ifdef TX_ENABLE
			case MODE_TX_SYMBOLS:
				/* Same again: UBERTOOTH_TX_SYMBOLS transmits along the
				 * piconet's hopping sequence. */
				hop_mode = HOP_BLUETOOTH;
				br_transmit();
				break;
#endif
			case MODE_BT_FOLLOW_LE:
				bt_follow_le();
				break;
			case MODE_BT_PROMISC_LE:
				bt_promisc_le();
				break;
#ifdef TX_ENABLE
			case MODE_BT_PERIPHERAL_LE:
				bt_peripheral_le();
				break;
			case MODE_TX_TEST:
				cc2400_txtest(&modulation, &channel);
				break;
			case MODE_RANGE_TEST:
				cc2400_rangetest(&channel);
				/*
				 * The range test runs once and then stops, so it
				 * asks for idle on its own behalf -- but only if
				 * the host has not asked for something else
				 * first. It now answers control requests while it
				 * waits for the repeater's reply, so a mode or
				 * reset request can have been accepted while it
				 * ran; overwriting requested_mode here would
				 * report success to the host and then drop it.
				 */
				if (requested_mode == MODE_RANGE_TEST)
					requested_mode = MODE_IDLE;
				break;
			case MODE_REPEATER:
				cc2400_repeater(&channel);
				break;
#endif
			case MODE_SPECAN:
				specan();
				break;
			case MODE_LED_SPECAN:
				led_specan();
				break;
			case MODE_EGO:
				ego_main();
				break;
			case MODE_RX_GENERIC:
				rx_generic();
				break;
#ifdef TX_ENABLE
			case MODE_TX_GENERIC:
				/* one packet, then back to whatever was running */
				tx_generic(prev_mode);
				break;
#endif
			case MODE_XMAS:
				xmas_main();
				break;
			case MODE_IDENTIFY:
				identify_main();
				break;
			case MODE_IDLE:
				/* nothing left to do: the reset above is the whole job */
				break;
			default:
				/* An unknown request. The reset above has already run, so
				 * ask for idle: the next turn records MODE_IDLE, which is
				 * the state the board is actually in, and the loop goes
				 * quiet. Leaving `mode` naming a mode that does not exist
				 * would make re-sending the same unknown request a no-op. */
				requested_mode = MODE_IDLE;
				break;
		}
	}
}
