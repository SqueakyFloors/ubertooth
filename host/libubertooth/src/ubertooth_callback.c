/*
 * Copyright 2015 Hannes Ellinger
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ubertooth_callback.h"

extern unsigned int packet_counter_max;

/* An access code is 64 symbols long. The search reads all 64 from every start
 * position it tries, so a search allowed to start anywhere in the bank reads
 * past the symbols that were captured. */
#define AC_SYMBOLS 64

/* How many start positions a search for an access code may try, which is what
 * libbtbb's btbb_find_ac() takes as its search length: it tries positions 0 to
 * search_length - 1. The last position whose whole access code lies inside the
 * bank begins AC_SYMBOLS symbols before the bank's end, so counting from zero
 * to that position inclusive is one more than the difference between them. */
#define AC_SEARCH_POSITIONS (BANK_LEN - AC_SYMBOLS + 1)

/* How long a bank of symbols is in the 100 ns ticks the firmware stamps it
 * with. One symbol is one microsecond of air, so the 400-symbol bank a record
 * carries is 400 µs, and the firmware takes its stamp at the END of the bank:
 * the bank began this many ticks earlier. */
#define BANK_100NS (BANK_LEN * 10)

/* 100 ns ticks in a 625 µs Bluetooth slot, which is two CLKN ticks. A packet's
 * offset from the clock's rising edge is measured within one slot, and the
 * firmware's clock trim carries a correction within one slot. */
#define CLK100NS_PER_SLOT (2 * CLK100NS_PER_CLKN)

/* CLKN is CLK27-0: a counter of 2^28 312.5 µs ticks, which runs for 23 hours
 * and starts over. */
#define CLKN_MASK 0x0fffffffu

/* CLKN ticks in a second, and the parts per million one 100 ns tick of error
 * per CLKN tick comes to (1000000 / 3125). */
#define CLKN_PER_SECOND 3200
#define PPM_PER_100NS_PER_CLKN 320

static int8_t cc2400_rssi_to_dbm( const int8_t rssi )
{
	/* models the cc2400 datasheet fig 22 for 1M as piece-wise linear */
	if (rssi < -48) {
		return -120;
	}
	else if (rssi <= -45) {
		return 6*(rssi+28);
	}
	else if (rssi <= 30) {
		return (int8_t) ((99*((int)rssi-62))/110);
	}
	else if (rssi <= 35) {
		return (int8_t) ((60*((int)rssi-35))/11);
	}
	else {
		return 0;
	}
}

#define RSSI_HISTORY_LEN 10

/* The ten most recent RSSI maxima of each Basic Rate channel, newest last, and
 * whether the table has been filled with the value that stands for a slot no
 * packet has written yet.
 *
 * That value is INT8_MIN, because determine_signal_and_noise() reports the
 * largest of a channel's ten entries and a slot nothing has written must not
 * be able to win. The fill happens on first use: C can only initialise a whole
 * array to zero, and `= {{INT8_MIN}}` sets the first entry of the first
 * channel and leaves the other 789 at zero. Raw 0 is about -55 dBm, so every
 * channel used to report at least that for its first ten packets - a packet
 * whose own maximum was -91 dBm read -55 dBm, in the signal field of the
 * record written to a capture file as well as on screen, and the
 * signal-to-noise ratio printed beside it was out by the same amount. */
static int8_t rssi_history[NUM_BREDR_CHANNELS][RSSI_HISTORY_LEN];
static int rssi_history_filled = 0;

static void determine_signal_and_noise( usb_pkt_rx *rx, int8_t * sig, int8_t * noise )
{
	int8_t * channel_rssi_history = rssi_history[rx->channel];
	int8_t rssi;
	int i;

	if (!rssi_history_filled) {
		/* One byte per entry, so a byte fill reaches all of them. */
		memset(rssi_history, INT8_MIN, sizeof(rssi_history));
		rssi_history_filled = 1;
	}

	/* Shift rssi max history and append current max */
	memmove(channel_rssi_history,
	        channel_rssi_history+1,
	        RSSI_HISTORY_LEN-1);
	channel_rssi_history[RSSI_HISTORY_LEN-1] = rx->rssi_max;

#if 0
	/* Signal starts in oldest bank, but may cross into second
	 * oldest bank.  Take the max or the 2 maxs. */
	rssi = MAX(channel_rssi_history[0], channel_rssi_history[1]);
#else
	/* Alternatively, use all banks in history. */
	rssi = channel_rssi_history[0];
	for (i = 1; i < RSSI_HISTORY_LEN; i++)
		rssi = MAX(rssi, channel_rssi_history[i]);
#endif
	*sig = cc2400_rssi_to_dbm( rssi );

	/* Noise is an IIR of averages */
	/* FIXME: currently bogus */
	*noise = cc2400_rssi_to_dbm( rx->rssi_avg );
}

static uint64_t clock_ns( clockid_t clock_id )
{
	struct timespec ts = { 0, 0 };
	(void) clock_gettime( clock_id, &ts );
	return (1000000000ull*(uint64_t) ts.tv_sec) + (uint64_t) ts.tv_nsec;
}

/* The host time a packet arrived, from the two clocks read together and the
 * monotonic reading the packet itself arrived with: the time that has passed
 * since the packet arrived, measured on the monotonic clock, taken off the
 * current host time.
 *
 * Three readings are no use and give back the current host time instead: a
 * packet with no arrival reading at all, one that claims to have arrived after
 * the monotonic reading just taken, and one so far back that taking it off
 * would land before the host clock's epoch. A host whose clock has never been
 * set reaches that last one: its current time is only a few seconds past the
 * epoch, while the monotonic clock has been counting since this boot, which
 * happened earlier than that. */
static uint64_t host_ns_at_arrival( uint64_t real_ns, uint64_t mono_ns,
                                    uint64_t arrival_ns )
{
	uint64_t waited_ns;

	if (arrival_ns == 0 || arrival_ns > mono_ns)
		return real_ns;
	waited_ns = mono_ns - arrival_ns;
	if (waited_ns > real_ns)
		return real_ns;
	return real_ns - waited_ns;
}

/* The host time a packet arrived, worked out from the reading the record it
 * came from arrived with: for a replay the host time stored with that record,
 * so a capture written with -q/-r while replaying keeps the recorded times,
 * and for a live capture the current time less however long the packet has
 * been waiting. The first packet's is where a capture's timestamps start.
 *
 * It is the reading passed in that decides, never the record the caller
 * happens to be holding. A packet that arrived as a chain of records is
 * placed where the chain started, and reassembly hands back that record's
 * reading for it; a replay that read the host time of the chain's last record
 * into systime must not move the packet to it.
 *
 * A live capture cannot simply read the clock here, because the poll thread
 * fills the ring buffer well ahead of the loop that drains it: a capture
 * written to a pipe nothing is reading yet, or taken by a process that was
 * descheduled, reaches its first packet long after the packet arrived, and
 * the clock read here would start every timestamp in the capture that late.
 * What says how long ago the packet arrived is the monotonic reading the ring
 * buffer stamped it with as its transfer completed, and reading the two
 * clocks together is what relates that reading to a host time. */
static uint64_t packet_host_ns( uint64_t arrival_ns )
{
	uint64_t real_ns, mono_ns;

	/* A replayed record's reading is the host time it was recorded with,
	 * in nanoseconds: that is what ubertooth_packet_arrival_ns() pushed
	 * into the ring buffer with it. */
	if (infile != NULL)
		return arrival_ns;
	/* The host clock first, so the monotonic reading is the later of the
	 * two and the wait is never understated: the error that leaves is a
	 * packet time fractionally early, not late. */
	real_ns = clock_ns( CLOCK_REALTIME );
	mono_ns = clock_ns( CLOCK_MONOTONIC );
	return host_ns_at_arrival( real_ns, mono_ns, arrival_ns );
}

uint32_t ubertooth_packet_systime( uint64_t arrival_ns )
{
	return (uint32_t) (packet_host_ns( arrival_ns ) / 1000000000ull);
}

uint64_t ubertooth_packet_arrival_ns( void )
{
	if (infile != NULL)
		return 1000000000ull * (uint64_t) systime;
	return clock_ns( CLOCK_MONOTONIC );
}

void ubertooth_capture_clock_reset( ubertooth_t* ut )
{
	ut->abs_start_ns = 0;
	ut->start_clk100ns = 0;
	ut->last_clk100ns = 0;
	ut->last_clkn_high = 0;
	ut->clk100ns_upper = 0;
	ut->clk100ns_started = 0;
	ut->clk100ns_counted = 0;
	ut->last_arrival_ns = 0;
	ut->clk100ns_anchor_open = 0;
	ut->clk100ns_gap_warned = 0;
}

/* How far the host's measure of the gap between two packets may fall short of
 * the time a wrap would account for and still be read as that wrap. It is
 * sized to how imprecisely that gap is known, not to the clock's period: a
 * replayed record's host time is a whole second, so two of them can misstate
 * the gap by just under a second either way, and a live capture reads the host
 * as each packet's transfer completes, a packet's latency apart. Two seconds
 * covers both, and a genuine wrap's own figure is good to well inside it.
 *
 * Whichever way the decision goes wrong it costs a whole period, so this does
 * not bound the error; what it bounds is how far the two measures have to
 * disagree before the decision flips. Small is still the safer direction. Too
 * small loses a genuine wrap between the first two packets: the times come out
 * a period short, which the host clock catches and reports below. Too large
 * reads the leftover packet as a wrap: they come out a period long, and nothing
 * says so, because that report only looks for times behind the host clock. */
#define CLK100NS_WRAP_SLACK_NS 2000000000ull

/* Whether the period count the capture's first packet carried belonged to the
 * mode before it, that packet being one left over from it. The firmware resets
 * CLKN when a mode starts, so the capture's own first count drops to whatever
 * the new mode has reached; read as a difference from the leftover's, the drop
 * is time that never passed. Where the mode before had run 200 periods that
 * came to 56 periods forwards, five hours into every time from there on, and
 * where it had run fewer than 128 the difference read backwards and was
 * rejected, which left the stale count as the baseline and stopped the capture
 * counting its own periods at all.
 *
 * What says so is the host's own clock, as it does for a wrap above: the two
 * packets' counts and readings together claim how much time passed between
 * them, and only a count from another mode claims an amount the host never
 * saw. The slack is the wrap test's, for the same reason - it is how far the
 * host's measure of the gap may be out - plus a whole period, because the
 * firmware latches a packet's count and its clock reading with separate reads
 * of CLKN: a packet caught as the count ticks over carries one a period from
 * its own reading, which is legitimate and has to be believed. One period is
 * as far as that can take a single packet, and one period is also as far as it
 * can take a pair of them. The two reads happen at one place in the firmware
 * per capture, in a fixed order - a counted capture's packets are stamped by
 * the receive interrupt, which reads the clock before the count - so whichever
 * way that order leans, both packets lean the same way and the difference
 * between their counts is out by at most the one period. Two packets leaning
 * opposite ways would have to come from two different places, which is a mode
 * change: the case below.
 *
 * The two allowances share one budget, and the budget cannot be widened: a
 * leftover count two periods from the capture's own already claims only about
 * two periods, so anything wider would stop catching it. A packet whose count
 * really was latched a period from its reading therefore leaves only the gap
 * slack to cover error in the host's measure of the gap, which is why that
 * measure is taken as each packet's transfer completes rather than where the
 * reconstruction is reached: a capture whose output is blocked on a slow pipe
 * can be minutes behind by the time it reads its first packet, and measured
 * there the gap before the second would be almost nothing, rejecting a count
 * the firmware legitimately latched and losing the whole gap between the two.
 *
 * What is left unsettled is a leftover count a period from the capture's own,
 * which looks exactly like that latch, or two periods from it where the two
 * readings are also within the slack of a whole period apart. Either costs
 * those one or two periods, as it did before, and no more: the capture runs at
 * the right rate from there, a fixed amount behind. ubertooth-rx(1) says so. */
static int count_came_from_the_mode_before( const ubertooth_t* ut,
                                            uint32_t clk100ns, uint8_t periods,
                                            uint64_t since_last_ns )
{
	/* The periods the count moved, as a signed difference: a count that
	 * reads lower is time going backwards, not 255 periods forwards. The
	 * byte's top half is read as that, rather than cast to a signed type,
	 * which C leaves to the compiler for a value that does not fit. */
	uint8_t stepped = (uint8_t) (periods - ut->last_clkn_high);
	int64_t moved = (stepped < 128) ? (int64_t) stepped :
	                                  (int64_t) stepped - 256;
	int64_t claim_ns = 100 * (moved * (int64_t) ut->clk100ns_period +
	                          (int64_t) clk100ns -
	                          (int64_t) ut->last_clk100ns);
	int64_t gap_ns = (int64_t) since_last_ns;
	int64_t apart_ns = (claim_ns > gap_ns) ? claim_ns - gap_ns :
	                                         gap_ns - claim_ns;

	return apart_ns > (int64_t) (100 * ut->clk100ns_period +
	                             CLK100NS_WRAP_SLACK_NS);
}

static void track_clk100ns( ubertooth_t* ut, const usb_pkt_rx* rx,
                            uint64_t host_ns, uint64_t arrival_ns )
{
	uint32_t clk100ns = le32toh( rx->clk100ns );
	/* How many times the clock stamping this packet has started over, as
	 * the packet itself reports it: clkn_high is the byte of CLKN directly
	 * above the 20 bits CLK100NS counts through, so it is that clock's
	 * period count. It is only read for a capture stamped with that clock:
	 * the low-energy physical-layer modes stamp from timer1, whose periods
	 * CLKN does not count, and they send a zero in the field. */
	uint8_t periods = (ut->clk100ns_period == CLK100NS_PERIOD) ?
	                  rx->clkn_high : 0;
	uint8_t advance;
	/* How much time the host saw pass since the last packet, and how much a
	 * wrap here would account for. The latter is only meaningful where the
	 * clock reads lower than it did. */
	uint64_t wrap_ns, since_last_ns;
	/* Whether this is the packet right after the capture's first, the only
	 * one where what the record says has a rival reading: the packet before
	 * it can be one left over from the mode before. Taken once and cleared
	 * here, so no later packet is judged that way. */
	uint8_t anchor_open = ut->clk100ns_anchor_open;

	ut->clk100ns_anchor_open = 0;

	/* A count has to arrive before it can be believed: a mode that sends
	 * none leaves the field zero, which is also what the first period of a
	 * capture that does send one looks like. From the first non-zero count
	 * on, the count is the authority and the clock is not second-guessed. */
	if (periods != 0)
		ut->clk100ns_counted = 1;

	if (!ut->clk100ns_started) {
		ut->clk100ns_started = 1;
		ut->last_clk100ns = ut->start_clk100ns = clk100ns;
		ut->last_clkn_high = periods;
		ut->abs_start_ns = host_ns;
		ut->last_arrival_ns = arrival_ns;
		ut->clk100ns_anchor_open = 1;
		return;
	}

	since_last_ns = (arrival_ns > ut->last_arrival_ns) ?
	                arrival_ns - ut->last_arrival_ns : 0;

	if (ut->clk100ns_counted) {
		/* The count is one byte, read as a difference from the last
		 * packet's, so the byte's own wrap costs nothing. Only a
		 * forward difference is time passing: half the byte's range is
		 * kept for that, covering a gap of up to 127 periods, so a
		 * count that reads backwards - a record out of order or edited
		 * - is ignored rather than read as nearly 23 hours forwards. A
		 * longer gap cannot be counted at all; the host clock is what
		 * reports the loss. A rejected count is not kept either, or the
		 * climb back from it would be counted as time of its own. The
		 * cost of that is a capture past the covered range: its counts
		 * stay rejected until they come back around to the one last
		 * believed, so its times slide further behind rather than
		 * settling a fixed amount behind. An out-of-order record is
		 * much the likelier of the two inputs, and the host clock
		 * reports the other. */
		advance = (uint8_t) (periods - ut->last_clkn_high);
		if (anchor_open && count_came_from_the_mode_before( ut, clk100ns,
		                                                    periods,
		                                                    since_last_ns )) {
			/* The count the capture was anchored on was the
			 * leftover's, so the difference from it is not time.
			 * This packet's count becomes the baseline and the
			 * anchor moves to its reading, which is where the
			 * capture starts. The host time the anchor carries
			 * stays as it was, because that is when the capture
			 * started, which is what its times are measured from.
			 * Where some of the gap between the two packets really
			 * did pass - a quiet start, or a gap too long for the
			 * count to cover - those absolute times come out that
			 * much early. Nothing here can recover it: the count
			 * is the only record of such a gap and it is the count
			 * being rejected. The message below reports it. */
			ut->last_clkn_high = periods;
			ut->start_clk100ns = clk100ns;
		} else if (advance < 128) {
			ut->clk100ns_upper += advance;
			ut->last_clkn_high = periods;
		}
	} else if (clk100ns < ut->last_clk100ns) {
		/* Without a count a wrap can only be inferred from the clock
		 * reading lower than it did, which misses one that falls in a
		 * gap longer than a single period.
		 *
		 * Between the capture's first two packets, and only there, that
		 * inference has a rival. The first packet of a capture can be
		 * one left over from the mode before, carrying that mode's
		 * clock; then the second reads lower not because the clock
		 * started over but because the first was never part of this
		 * capture. A packet out of place further in reads lower too,
		 * and is still read as a wrap. The gap each packet arrived
		 * with, which is what settles the first two, is known for
		 * every packet; what the same test still lacks further in is a
		 * way to read that gap as a number of wraps rather than as one
		 * wrap or none, because by then a capture can have been quiet
		 * for longer than a period.
		 *
		 * What separates the two is how much time passed. A wrap says
		 * how much: the clock ran from where the first packet read,
		 * round the end of its range, to where this one reads. A
		 * leftover packet was taken in moments before this one, so
		 * hardly any time passed. The host's own clock, which kept
		 * running either way, says which of those happened: the wrap
		 * stands unless it claims more time than the host saw, by more
		 * than the two could disagree by on their own. */
		wrap_ns = 100 * (ut->clk100ns_period -
		                 (ut->last_clk100ns - clk100ns));

		if (anchor_open &&
		    wrap_ns > since_last_ns + CLK100NS_WRAP_SLACK_NS) {
			/* The packet the capture was anchored on was the
			 * leftover. The anchor moves down to this packet, so
			 * the capture starts at its own first reading and
			 * nothing after it lands before the capture began. The
			 * host time the anchor carries stays as it was: it is
			 * when the capture started, and this packet was taken
			 * in within a packet's latency of it. */
			ut->start_clk100ns = clk100ns;
		} else {
			ut->clk100ns_upper += 1;
		}
	}

	ut->last_clk100ns = clk100ns;
	ut->last_arrival_ns = arrival_ns;
}

/* A wrap that cannot be counted - inferred from the clock going backwards and
 * falling in a gap longer than one period, or past the range of the count a
 * packet carries - leaves nothing in the capture to show it: the times simply
 * come out short from then on. The host clock does show it, because it kept
 * running, so it ends up ahead of the reconstructed times. What it is compared
 * against is the host time this packet arrived, not the time the loop reached
 * it, so a loop working through a backlog is not mistaken for lost time.
 *
 * Said once, and not used to correct them: it says that time was lost, not
 * where, and a host clock that is stepped during a capture must not move
 * packet times, which is why the reading that decides a wrap above is taken
 * from the monotonic clock instead. A step is also why this message does not
 * claim a lost wrap as the cause: a host clock jumped forwards looks the same
 * from here. */
static void warn_if_a_wrap_was_lost( ubertooth_t* ut, uint64_t packet_ns,
                                     uint64_t host_ns )
{
	uint64_t period_ns = 100ull * ut->clk100ns_period;

	if (ut->clk100ns_gap_warned)
		return;
	/* Half a period, not a whole one: a single lost wrap leaves the times
	 * exactly one period short, and the host time a replayed record carries
	 * is a whole second, so a whole-period test would turn on sub-second
	 * truncation and say nothing about the commonest case. A capture that
	 * keeps up is a packet's latency behind, far inside half a period. */
	if (host_ns < packet_ns || host_ns - packet_ns <= period_ns / 2)
		return;

	ut->clk100ns_gap_warned = 1;
	fprintf(stderr, "warning: packet times are behind the host clock by more "
	        "than half the capture clock's %.2f s period: a quiet gap lost a "
	        "clock wrap, or the host clock was stepped forwards\n",
	        period_ns / 1e9);
}

/* The capture clock of a packet, as 100 ns ticks since the board started
 * counting: each wrap seen adds the period of the clock this mode stamps with
 * (ut->clk100ns_period). Adding 2^32 instead, as if every clock were a 32-bit
 * counter, put 101.8 s into every packet after a wrap of the CLKN-derived
 * clock, which wraps at 3125 * 2^20. */
static uint64_t clk100ns_extended( const ubertooth_t* ut, const usb_pkt_rx* rx )
{
	return ut->clk100ns_upper * ut->clk100ns_period + le32toh( rx->clk100ns );
}

/* arrival_ns is when this packet arrived, read as its transfer completed and
 * carried with it through the ring buffer (fifo_popped_arrival_ns()). Reading
 * that clock here instead would say when this call was reached, which for a
 * packet taken from a backlog is no measure of the gap before it, nor of when
 * the capture itself began. */
static uint64_t now_ns_from_clk100ns( ubertooth_t* ut, const usb_pkt_rx* rx,
                                      uint64_t arrival_ns )
{
	uint64_t packet_ns;
	/* The host time this packet arrived, read once and used both to anchor
	 * the capture and to report a wrap that could not be told either way. */
	uint64_t host_ns = packet_host_ns( arrival_ns );

	track_clk100ns( ut, rx, host_ns, arrival_ns );
	/* The 100 ns ticks since the first packet, as nanoseconds: the whole
	 * difference is scaled, not just the current tick count. Signed,
	 * because a packet can read a little before the one a capture started
	 * on: the firmware latches clk100ns and the period count with separate
	 * reads of CLKN, so a packet caught as the count ticks over carries one
	 * that disagrees with its own clock reading by a period, in whichever
	 * direction that mode's two reads happen to be ordered. */
	packet_ns = (uint64_t) ((int64_t) ut->abs_start_ns + 100 *
	            ((int64_t) clk100ns_extended( ut, rx ) -
	             (int64_t) ut->start_clk100ns));
	warn_if_a_wrap_was_lost( ut, packet_ns, host_ns );
	return packet_ns;
}

/* The CLKN of the first bit of a packet's access code, from the stamp the
 * record carries, the period count in the same record, and where in the bank
 * the access code was found.
 *
 * The stamp is taken at the end of the bank, so the bank's first symbol is
 * BANK_100NS earlier and the access code is `offset` symbols after that. CLKN
 * is a counter that starts over, and the three parts are put together the way
 * the counter itself works: clkn_high is CLKN's top eight bits and clk100ns
 * counts 100 ns ticks through the bottom twenty, so moving back by the bank
 * borrows from the period count when the stamp is less than 400 µs into a
 * period, and a borrow from period zero wraps round to the top of CLKN's
 * range.
 *
 * Both callers used to subtract in plain unsigned arithmetic, which read that
 * borrow as about 2^32 ticks and put the packet 1374388 ticks - seven minutes
 * - ahead of the clock. It is reachable in the first 400 µs after a mode
 * starts and in a 400 µs window at every 327.68 s wrap of clk100ns. */
static uint32_t clkn_at_access_code( uint8_t periods, uint32_t clk100ns,
                                     int offset )
{
	/* Signed: this is the one subtraction that can go below zero, and it
	 * can only do so by less than one bank. */
	int64_t at = (int64_t) clk100ns + 10 * (int64_t) offset - BANK_100NS;
	uint32_t borrowed = 0;

	if (at < 0) {
		at += CLK100NS_PERIOD;
		borrowed = 1;
	}
	return ((((uint32_t) periods - borrowed) << 20) +
	        (uint32_t) (at / CLK100NS_PER_CLKN)) & CLKN_MASK;
}

/* Sniff for LAPs. If a piconet is provided, use the given LAP to
 * search for UAP.
 */
void ubertooth_count_queue_gap(ubertooth_t* ut, const usb_pkt_rx* rx)
{
	if (ut == NULL || rx == NULL)
		return;

	/* A keep-alive is one byte on the wire -- the record's type and nothing
	 * else -- so its other fields hold whatever the record before it left in
	 * the buffer, this flag among them. Counting one would repeat a gap that
	 * was already counted. */
	if (rx->pkt_type == KEEP_ALIVE)
		return;

	if (!(rx->status & FIFO_OVERFLOW))
		return;

	ut->queue_gaps++;
	if (ut->queue_gap_warned)
		return;

	ut->queue_gap_warned = 1;
	fprintf(stderr, "warning: the board's packet queue filled up and it "
	        "dropped at least one packet; this capture has a gap\n");
}

uint32_t ubertooth_queue_gaps(ubertooth_t* ut)
{
	return ut != NULL ? ut->queue_gaps : 0;
}

void ubertooth_report_queue_gaps(ubertooth_t* ut)
{
	uint32_t gaps = ubertooth_queue_gaps(ut);

	if (gaps == 0)
		return;

	fprintf(stderr, "warning: the capture has %u gap%s where the board's "
	        "queue filled up and it dropped packets\n",
	        gaps, gaps == 1 ? "" : "s");
}

void cb_scan(ubertooth_t* ut, void* args __attribute__((unused)))
{
	btbb_packet* pkt = NULL;
	int8_t signal_level;
	int8_t noise_level;
	int8_t snr;
	int offset;
	uint32_t clkn;

	usb_pkt_rx usb = fifo_pop(ut->fifo);
	usb_pkt_rx* rx = &usb;
	uint64_t arrival_ns = fifo_popped_arrival_ns(ut->fifo);
	char syms[BANK_LEN];
	ubertooth_unpack_symbols((uint8_t*)rx->data, syms);

	/* Before the checks below: a record this callback goes on to discard
	 * still carries the board's word that packets were lost before it. */
	ubertooth_count_queue_gap(ut, rx);

	/* Sanity check */
	if (rx->channel > (NUM_BREDR_CHANNELS-1))
		goto out;

	determine_signal_and_noise( rx, &signal_level, &noise_level );
	snr = signal_level - noise_level;

	/* Pass packet-pointer-pointer so that
	 * packet can be created in libbtbb. */
	offset = btbb_find_ac(syms, AC_SEARCH_POSITIONS, LAP_ANY, max_ac_errors, &pkt);
	if (offset < 0)
		goto out;

	/* Once offset is known for a valid packet, copy in symbols
	 * and other rx data. CLKN here is the 312.5us CLK27-0. The
	 * btbb library can shift it be CLK1 if needed. */
	clkn = clkn_at_access_code(rx->clkn_high, le32toh(rx->clk100ns), offset);
	btbb_packet_set_data(pkt, syms + offset, BANK_LEN - offset,
	                     rx->channel, clkn);

	/* The host time this record arrived. A replayed record keeps the one it
	 * was recorded with. */
	systime = ubertooth_packet_systime( arrival_ns );

	printf("systime=%u ch=%2d LAP=%06x err=%u clk100ns=%u clk1=%u s=%d n=%d snr=%d\n",
	       systime,
	       btbb_packet_get_channel(pkt),
	       btbb_packet_get_lap(pkt),
	       btbb_packet_get_ac_errors(pkt),
	       rx->clk100ns,
	       btbb_packet_get_clkn(pkt),
	       signal_level,
	       noise_level,
	       snr);

	btbb_process_packet(pkt, NULL);

out:
	if (pkt)
		btbb_packet_unref(pkt);
}

/* Fills a single unused channel next to one a packet just arrived on. This is
 * a guess, not a deduction: nothing stops a piconet from leaving out one
 * channel on its own, but a lone gap in a run of used channels is more often a
 * channel no packet has landed on yet. `neighbor` is the channel either side
 * of the packet's, `past` the one beyond it, and the gap counts as single only
 * when `past` is already marked used.
 *
 * Both have to name a real channel. Basic Rate has 79 of them, 0 to 78, so
 * looking outwards from a packet on channel 1 there is no channel below 0 to
 * judge the gap by, and from one on channel 77 no channel above 78: there the
 * rule has nothing to work from and does not apply. The bounds matter because
 * libbtbb reports a channel off the end of the band as used, which read as
 * evidence of a gap and marked channel 0 used on a packet from channel 1, and
 * channel 78 used on one from channel 77, with no packet ever seen on
 * either. */
static void afh_fill_single_gap(btbb_piconet* pn, int neighbor, int past)
{
	if (neighbor < 0 || neighbor > NUM_BREDR_CHANNELS - 1)
		return;
	if (past < 0 || past > NUM_BREDR_CHANNELS - 1)
		return;

	if (btbb_piconet_get_channel_seen(pn, (uint8_t)neighbor))
		return;
	if (!btbb_piconet_get_channel_seen(pn, (uint8_t)past))
		return;

	fprintf(stderr, "note: activating additional channel %d\n", neighbor);
	btbb_piconet_set_channel_seen(pn, (uint8_t)neighbor);
}

void cb_afh_initial(ubertooth_t* ut, void* args)
{
	btbb_piconet* pn = (btbb_piconet*)args;
	btbb_packet* pkt = NULL;
	int channel;

	usb_pkt_rx usb = fifo_pop(ut->fifo);
	usb_pkt_rx* rx = &usb;
	char syms[BANK_LEN];
	ubertooth_unpack_symbols((uint8_t*)rx->data, syms);

	/* Before the checks below: a record this callback goes on to discard
	 * still carries the board's word that packets were lost before it. */
	ubertooth_count_queue_gap(ut, rx);

	/* Sanity check: the channel names a bit of the AFH map below */
	if (rx->channel > (NUM_BREDR_CHANNELS-1))
		goto out;

	if( btbb_find_ac(syms, AC_SEARCH_POSITIONS, btbb_piconet_get_lap(pn), max_ac_errors, &pkt) < 0 )
		goto out;

	/* detect AFH map
	 * set current channel as used channel and send updated AFH
	 * map to ubertooth */
	channel = rx->channel;
	if(btbb_piconet_set_channel_seen(pn, (uint8_t)channel)) {

		afh_fill_single_gap(pn, channel+1, channel+2);
		afh_fill_single_gap(pn, channel-1, channel-2);

		cmd_set_afh_map(ut->devh, btbb_piconet_get_afh_map(pn));
		btbb_print_afh_map(pn);
	}
	cmd_hop(ut->devh);

out:
	if (pkt)
		btbb_packet_unref(pkt);
}

void cb_afh_monitor(ubertooth_t* ut, void* args)
{
	btbb_piconet* pn = (btbb_piconet*)args;
	btbb_packet* pkt = NULL;
	uint8_t channel;
	int i;

	usb_pkt_rx usb = fifo_pop(ut->fifo);
	usb_pkt_rx* rx = &usb;
	char syms[BANK_LEN];
	ubertooth_unpack_symbols((uint8_t*)rx->data, syms);

	/* Before the checks below: a record this callback goes on to discard
	 * still carries the board's word that packets were lost before it. */
	ubertooth_count_queue_gap(ut, rx);

	static unsigned long last_seen[NUM_BREDR_CHANNELS] = {0};
	static unsigned long counter = 0;

	/* Sanity check: the channel indexes last_seen below */
	if (rx->channel > (NUM_BREDR_CHANNELS-1))
		goto out;

	if( btbb_find_ac(syms, AC_SEARCH_POSITIONS, btbb_piconet_get_lap(pn), max_ac_errors, &pkt) < 0 )
		goto out;

	counter++;
	channel = rx->channel;
	last_seen[channel]=counter;

	if(btbb_piconet_set_channel_seen(pn, channel)) {
		fprintf(stderr, "note: + channel %2d is used now\n", channel);
		btbb_print_afh_map(pn);
	// } else {
	// 	printf("channel %d is already used\n", channel);
	}

	for(i=0; i<NUM_BREDR_CHANNELS; i++) {
		if((counter - last_seen[i] >= packet_counter_max)) {
			if(btbb_piconet_clear_channel_seen(pn, i)) {
				fprintf(stderr, "note: - channel %2d is not used any more\n",
				        i);
				btbb_print_afh_map(pn);
			}
		}
	}
	cmd_hop(ut->devh);

out:
	if (pkt)
		btbb_packet_unref(pkt);
}

void cb_afh_r(ubertooth_t* ut, void* args)
{
	btbb_piconet* pn = (btbb_piconet*)args;
	btbb_packet* pkt = NULL;
	uint8_t channel;
	int i;

	usb_pkt_rx usb = fifo_pop(ut->fifo);
	usb_pkt_rx* rx = &usb;
	char syms[BANK_LEN];
	ubertooth_unpack_symbols((uint8_t*)rx->data, syms);

	/* Before the checks below: a record this callback goes on to discard
	 * still carries the board's word that packets were lost before it. */
	ubertooth_count_queue_gap(ut, rx);

	static unsigned long last_seen[NUM_BREDR_CHANNELS] = {0};
	static unsigned long counter = 0;

	/* Sanity check: the channel indexes last_seen below */
	if (rx->channel > (NUM_BREDR_CHANNELS-1))
		goto out;

	if( btbb_find_ac(syms, AC_SEARCH_POSITIONS, btbb_piconet_get_lap(pn), max_ac_errors, &pkt) < 0 )
		goto out;


	counter++;
	channel = rx->channel;
	last_seen[channel]=counter;

	btbb_piconet_set_channel_seen(pn, channel);

	for(i=0; i<NUM_BREDR_CHANNELS; i++) {
		if((counter - last_seen[i] >= packet_counter_max)) {
			btbb_piconet_clear_channel_seen(pn, i);
		}
	}
	cmd_hop(ut->devh);

out:
	if (pkt)
		btbb_packet_unref(pkt);
}


/*
 * Sniff Bluetooth Low Energy packets.
 */
void cb_btle(ubertooth_t* ut, void* args)
{
	lell_packet* pkt;
	btle_options* opts = (btle_options*) args;
	int i;
	usb_pkt_rx usb = fifo_pop(ut->fifo);
	const usb_pkt_rx* rx = &usb;
	uint64_t arrival_ns = fifo_popped_arrival_ns(ut->fifo);
	/* The whole captured packet and its length, once reassembly has it,
	 * and when the record its readings come from arrived. */
	const uint8_t* le_data;
	uint16_t le_data_len;
	const usb_pkt_rx* le_metadata;
	uint64_t le_arrival_ns;
	// u32 access_address = 0; // Build warning

	static uint64_t prev_ts = 0;
	uint32_t refAA;
	int8_t sig, noise;

	/* A keep-alive only says a quiet capture is still alive. It carries no
	 * packet, so there is nothing to decode and nothing to record: a replay
	 * of an older capture file can still hold one. */
	if (rx->pkt_type == KEEP_ALIVE)
		return;

	// display LE promiscuous mode state changes
	if (rx->pkt_type == LE_PROMISC) {
		u8 state = rx->data[0];
		const void *val = &rx->data[1];
		/* These four lines are what promiscuous mode is for: a capture
		 * written to a file has no other way to say which connection it
		 * is of. So they go to the stream the tool reports on, which is
		 * standard error once a capture has taken standard output.
		 *
		 * A caller that sets no stream gets standard output, which is
		 * where a plain run wants them -- unless that caller also said a
		 * capture had taken standard output, in which case standard
		 * output is /dev/null and these lines would reach nobody. */
		FILE* out = stdout;

		if (opts != NULL)
			out = opts->report_stream != NULL ? opts->report_stream
			    : opts->capture_took_stdout  ? stderr : stdout;

		fprintf(out, "--------------------\n");
		fprintf(out, "LE Promisc - ");
		switch (state) {
			case 0:
				fprintf(out, "Access Address: %08x\n", *(const uint32_t *)val);
				break;
			case 1:
				fprintf(out, "CRC Init: %06x\n", *(const uint32_t *)val);
				break;
			case 2:
				fprintf(out, "Hop interval: %g ms\n", *(const uint16_t *)val * 1.25);
				break;
			case 3:
				fprintf(out, "Hop increment: %u\n", *(const uint8_t *)val);
				break;
			default:
				fprintf(out, "Unknown %u\n", state);
				break;
		};
		fprintf(out, "\n");

		return;
	}

	/* The host time this record arrived, which the dump below stores with
	 * it: per record, because that is what the dump's fixed-size format
	 * holds. A replayed record keeps the one it was recorded with. The
	 * packet's own time is set again once reassembly has the whole of it. */
	systime = ubertooth_packet_systime( arrival_ns );

	/* Sanity check */
	if (rx->channel > (NUM_BREDR_CHANNELS-1))
		return;

	/* Dump to sumpfile if specified. One record at a time, as received, so a
	 * dump keeps its fixed-size format: a long packet is several records
	 * there, and a replay puts them back together below exactly as a live
	 * capture does. */
	if (dumpfile) {
		uint32_t systime_be = htobe32(systime);
		fwrite(&systime_be, sizeof(systime_be), 1, dumpfile);
		fwrite(rx, sizeof(usb_pkt_rx), 1, dumpfile);
		fflush(dumpfile);
	}

	/* A packet too long for one record arrives as several. Nothing below
	 * runs until the whole packet is here: a chain still arriving returns,
	 * and so does a broken one, whose records are dropped. */
	if (le_usb_reassemble(&ut->le_reassembly, rx, arrival_ns, &le_data,
	                      &le_data_len, &le_metadata,
	                      &le_arrival_ns) != LE_REASSEMBLY_COMPLETE)
		return;
	/* The channel, timestamp and signal readings of the packet as a whole,
	 * which for a chain are the first record's. */
	rx = le_metadata;

	/* The host time of the packet, rather than of the record that completed
	 * it: the first record of a chain, which is where the capture below
	 * places the packet, so the column and the capture agree. */
	systime = ubertooth_packet_systime( le_arrival_ns );

	/* After reassembly, so the clock sees one timestamp per packet rather
	 * than the same one once per record. The arrival reading is the one
	 * belonging to the record the timestamp came from, so the two always
	 * describe the same moment. */
	uint64_t nowns = now_ns_from_clk100ns( ut, rx, le_arrival_ns );

	/* The decoder is told how many bytes were captured, so it reads no
	 * further than that. */
	lell_allocate_and_decode_n(le_data, le_data_len,
	                           rx->channel + 2402, rx->clk100ns, &pkt);
	/* The decoder hands back nothing if it could not allocate the packet. */
	if (pkt == NULL) {
		fprintf(stderr, "error: unable to allocate memory\n");
		return;
	}

	/* do nothing further if filtered due to bad AA */
	if (opts &&
	    (opts->allowed_access_address_errors <
	     lell_get_access_address_offenses(pkt))) {
		lell_packet_unref(pkt);
		return;
	}

	/* Dump to PCAP/PCAPNG if specified */
	refAA = lell_packet_is_data(pkt) ? 0 : 0x8e89bed6;
	sig = cc2400_rssi_to_dbm( rx->rssi_max );
	noise = INT8_MIN; // FIXME - keep track of this

	if (ut->h_pcap_le) {
		/* only one of these two will succeed, depending on
		 * whether PCAP was opened with DLT_PPI or not */
		lell_pcap_append_packet(ut->h_pcap_le, nowns,
					sig, noise,
					refAA, pkt);
		// read the above comment: this function may silently fail
		lell_pcap_append_ppi_packet(ut->h_pcap_le, nowns,
		                            rx->clkn_high,
		                            rx->rssi_min, rx->rssi_max,
		                            rx->rssi_avg, rx->rssi_count,
		                            pkt);
	}
	if (ut->h_pcapng_le) {
		lell_pcapng_append_packet(ut->h_pcapng_le, nowns,
		                          sig, noise,
		                          refAA, pkt);
	}

	/* A capture on "-" owns standard output: this process's own standard
	 * output is /dev/null for the whole run, so the per-packet dump below
	 * would reach nobody. Skipping it also keeps the terminal clean,
	 * because the dump ends in lell_print(), which reports a field it
	 * cannot read on standard error -- and standard error is not
	 * redirected. */
	if (opts != NULL && opts->capture_took_stdout) {
		lell_packet_unref(pkt);
		return;
	}

	// rollover
	uint64_t rx_ts = le32toh( rx->clk100ns );
	if (rx_ts < prev_ts)
		rx_ts += ut->clk100ns_period;
	uint64_t ts_diff = rx_ts - prev_ts;
	prev_ts = le32toh( rx->clk100ns );
	printf("systime=%u freq=%d addr=%08x delta_t=%.03f ms rssi=%d\n",
	       systime, rx->channel + 2402, lell_get_access_address(pkt),
	       ts_diff / 10000.0, rx->rssi_min - 54);

	/* The bytes of the packet itself: the PDU header, the payload the
	 * decoder found and the CRC, after the four-byte access address. Never
	 * more than was captured, so a header claiming a longer payload than
	 * arrived prints only what arrived. */
	unsigned len = lell_get_payload_length(pkt) + 4 + 2 + 3;
	if (len > le_data_len)
		len = le_data_len;

	for (i = 4; i < (int)len; ++i)
		printf("%02x ", le_data[i]);
	printf("\n");

	lell_print(pkt);
	printf("\n");

	lell_packet_unref(pkt);

	fflush(stdout);
}
/*
 * Sniff E-GO packets
 */
void cb_ego(ubertooth_t* ut, void* args)
{
	int i;
	uint8_t len = *(uint8_t *)args;
	static uint64_t prev_ts = 0;
	usb_pkt_rx usb = fifo_pop(ut->fifo);
	usb_pkt_rx* rx = &usb;

	ubertooth_count_queue_gap(ut, rx);

	/* Wide enough to hold a stamp plus a whole clock period, as the
	 * low-energy callback above is: in 32 bits that sum wraps for any stamp
	 * above 1018167295, and the delta is only right afterwards because the
	 * subtraction wraps back. The stamp itself comes off the wire
	 * little-endian. */
	uint64_t rx_time = le32toh(rx->clk100ns);
	if (rx_time < prev_ts)
		rx_time += CLK100NS_PERIOD; // rollover
	uint64_t ts_diff = rx_time - prev_ts;
	prev_ts = le32toh(rx->clk100ns);
	printf("time=%u delta_t=%.06f ms freq=%d \n",
	       le32toh(rx->clk100ns), ts_diff / 10000.0,
	       rx->channel + 2402);

	for (i = 0; i < len; ++i)
		printf("%02x ", rx->data[i]);
	printf("\n\n");

	fflush(stdout);
}

#define CLOCK_TRIM_THRESHOLD 2

/* What the firmware's clock trim can ask for, in 100 ns ticks. The request
 * carries one unsigned 16-bit field holding a correction modulo one slot: the
 * firmware reads 0 to 3124 as a delay of that many ticks, and 3125 to 6249 as
 * an advance of one slot less the value. So the largest delay is one tick
 * short of a CLKN tick and the largest advance is a whole one. A field of 6250
 * or more is a delay again, which is the arithmetic the old "advance" branch
 * leant on; nothing sends one now. */
#define CLOCK_TRIM_MAX_DELAY   (CLK100NS_PER_CLKN - 1)
#define CLOCK_TRIM_MAX_ADVANCE CLK100NS_PER_CLKN

/* The largest drift the firmware's correction can express, in parts per
 * million. It adds or removes one 100 ns tick every 320/PPM CLKN ticks, so
 * 320 PPM is one tick every tick; beyond that its division gives nothing,
 * which it reads as correcting on every tick, saturated at 320 PPM. This
 * bounds each request, not the total: the firmware adds each one to a running
 * figure of its own, which nothing bounds. */
#define DRIFT_PPM_MAX PPM_PER_100NS_PER_CLKN

/* How far the board's clock has to move for the next packet's access code to
 * arrive CLK_TUNE_TIME after a CLKN edge: positive to delay the clock,
 * negative to advance it.
 *
 * The offset the packet was measured at runs through one slot and starts over,
 * so an error of +3999 ticks and one of -2251 land the clock in the same
 * place. The shorter way round is the one to take: it perturbs the clock less,
 * and it is the only one of the two that the firmware's field can carry, which
 * is why nothing here needs clamping. The result is always between one whole
 * CLKN tick of advance and one tick less a 100 ns tick of delay, as long as
 * CLK_TUNE_TIME is no more than a CLKN tick; the test walks every offset a
 * packet can be measured at and checks that. */
static int32_t clock_correction_100ns( uint16_t clk_offset )
{
	int32_t error = (int32_t) clk_offset - CLK_TUNE_TIME;

	if (error > CLOCK_TRIM_MAX_DELAY)
		error -= CLK100NS_PER_SLOT;
	return error;
}

/* The field to send for such a correction. An advance goes out as one slot
 * less the amount, which is how the firmware reads the field.
 *
 * A negative correction used to be passed straight to the request's unsigned
 * parameter: -150 went out as 65386, which the firmware added to its tick
 * reload, so that CLKN tick lasted 6.85 ms while the count advanced by three -
 * leaving the clock 5.91 ms behind where it should have moved forward by
 * 15 µs. The other branch sent a value above one slot in the same way. */
static uint16_t clock_trim_field( int32_t correction_100ns )
{
	if (correction_100ns < 0)
		return (uint16_t) (CLK100NS_PER_SLOT + correction_100ns);
	return (uint16_t) correction_100ns;
}

/* How fast the board's clock is running against the piconet's, in parts per
 * million, from the correction this trim needed and the CLKN ticks since the
 * last one. Negative means the board is slow, which is the sign the firmware
 * reads as "speed up".
 *
 * Signed throughout. The division used to be by an unsigned tick count, which
 * converted a negative correction first: a real -15 PPM came out as 1342162,
 * was printed as that, and was narrowed to 31442 for the request. A positive
 * figure makes the firmware lengthen every tick, so a board already running
 * 15 PPM slow was slowed by a further 320 PPM - the same direction as the
 * error - for the rest of the run. The result is clamped to what the firmware
 * can act on, and a trim in the same CLKN tick as the last one reports nothing
 * rather than dividing by zero. */
static int drift_ppm( int32_t correction_100ns, uint32_t clkn_ticks )
{
	int32_t ppm;

	if (clkn_ticks == 0)
		return 0;
	ppm = correction_100ns * PPM_PER_100NS_PER_CLKN / (int32_t) clkn_ticks;
	if (ppm > DRIFT_PPM_MAX)
		ppm = DRIFT_PPM_MAX;
	if (ppm < -DRIFT_PPM_MAX)
		ppm = -DRIFT_PPM_MAX;
	return (int) ppm;
}

void cb_rx(ubertooth_t* ut, void* args)
{
	btbb_packet* pkt = NULL;
	btbb_piconet* pn = (btbb_piconet *)args;
	/* As many symbols as a record carries, and no more: the unpacking
	 * below fills exactly BANK_LEN of them. A buffer ten times that size
	 * left the decoder reading the zeros the rest of it started as. */
	char syms[BANK_LEN];
	int offset;
	uint16_t clk_offset;
	uint32_t clkn;
	int r;
	uint32_t lap = LAP_ANY;
	uint8_t uap = UAP_ANY;

	static int trim_counter = 0;
	static int calibrated = 0;
	static uint32_t clkn_trim = 0;

	usb_pkt_rx usb = fifo_pop(ut->fifo);
	usb_pkt_rx* rx = &usb;
	uint64_t arrival_ns = fifo_popped_arrival_ns(ut->fifo);
	ubertooth_unpack_symbols((uint8_t*)rx->data, syms);

	/* Before the checks below: a record this callback goes on to discard
	 * still carries the board's word that packets were lost before it. */
	ubertooth_count_queue_gap(ut, rx);

	if (rx->pkt_type != BR_PACKET) {
		goto out;
	}

	if (rx->status & DISCARD) {
		goto out;
	}

	// /* Sanity check */
	if (rx->channel > (NUM_BREDR_CHANNELS-1))
		goto out;

	/* The host time this record arrived, which the dump below stores with
	 * it. A replayed record keeps the one it was recorded with. */
	systime = ubertooth_packet_systime( arrival_ns );

	uint64_t nowns = now_ns_from_clk100ns( ut, rx, arrival_ns );

	int8_t signal_level = rx->rssi_max;
	int8_t noise_level = rx->rssi_min;
	determine_signal_and_noise( rx, &signal_level, &noise_level );
	int8_t snr = signal_level - noise_level;

	/* Look for packets with specified LAP, if given. Otherwise
	 * search for any packet. */
	if (pn) {
		lap = btbb_piconet_get_flag(pn, BTBB_LAP_VALID) ? btbb_piconet_get_lap(pn) : LAP_ANY;
	}

	/* Pass packet-pointer-pointer so that
	 * packet can be created in libbtbb. */
	offset = btbb_find_ac(syms, AC_SEARCH_POSITIONS, lap, max_ac_errors, &pkt);
	if (offset < 0)
		goto out;

	/* calculate the offset between the first bit of the AC and the rising edge of CLKN */
	clk_offset = (le32toh(rx->clk100ns) + offset*10 +
	              CLK100NS_PER_SLOT - BANK_100NS) % CLK100NS_PER_SLOT;

	btbb_packet_set_modulation(pkt, BTBB_MOD_GFSK);
	btbb_packet_set_transport(pkt, BTBB_TRANSPORT_ANY);

	/* Once offset is known for a valid packet, copy in symbols
	 * and other rx data. CLKN here is the 312.5us CLK27-0. The
	 * btbb library can shift it be CLK1 if needed. */
	clkn = clkn_at_access_code(rx->clkn_high, le32toh(rx->clk100ns), offset);
	btbb_packet_set_data(pkt, syms + offset, BANK_LEN - offset,
	                     rx->channel, clkn);

	printf("systime=%u ch=%2d LAP=%06x err=%u clkn=%u clk_offset=%u s=%d n=%d snr=%d\n",
	       systime,
	       btbb_packet_get_channel(pkt),
	       btbb_packet_get_lap(pkt),
	       btbb_packet_get_ac_errors(pkt),
	       clkn,
	       clk_offset,
	       signal_level,
	       noise_level,
	       snr
	);

	/* calibrate Ubertooth clock such that the first bit of the AC
	 * arrives CLK_TUNE_TIME after the rising edge of CLKN */
	if (pn != NULL && infile == NULL) {
		/* The correction this packet calls for. Its sign, and nothing
		 * else, decides which form the trim takes; the two were chosen
		 * by trim_counter instead, so a calibrated follow reached the
		 * "delay" form with a correction that was really an advance,
		 * and vice versa. */
		int32_t correction = clock_correction_100ns( clk_offset );
		/* Forward CLKN ticks since the last trim, masked because CLKN
		 * starts over. */
		uint32_t since_trim = (clkn - clkn_trim) & CLKN_MASK;

		if (trim_counter < -CLOCK_TRIM_THRESHOLD
		    || trim_counter > CLOCK_TRIM_THRESHOLD
		    || (correction != 0 && !calibrated)) {
			fprintf(stderr, "note: offset %s CLK_TUNE_TIME\n",
			        clk_offset < CLK_TUNE_TIME ? "<" : ">");
			fprintf(stderr, "note: CLK100ns trim: %d\n", correction);
			cmd_trim_clock(ut->devh, clock_trim_field(correction));
			trim_counter = 0;
			if (calibrated) {
				int ppm = drift_ppm(correction, since_trim);

				fprintf(stderr, "note: clock drifted %d in %f s, "
				        "%d PPM too %s\n",
				       correction,
				       (double)since_trim/CLKN_PER_SECOND,
				       ppm, ppm < 0 ? "slow" : "fast");
				cmd_fix_clock_drift(ut->devh, (int16_t) ppm);
			}
			clkn_trim = clkn;
			calibrated = 1;
			goto out;
		}

		if (clk_offset < CLK_TUNE_TIME - CLK_TUNE_OFFSET) {
			trim_counter--;
			goto out;
		} else if (clk_offset > CLK_TUNE_TIME + CLK_TUNE_OFFSET) {
			trim_counter++;
			goto out;
		} else {
			trim_counter = 0;
		}
	}

	/* If dumpfile is specified, write out all banks to the
	 * file. There could be duplicate data in the dump if more
	 * than one LAP is found within the span of NUM_BANKS. */
	if (dumpfile) {
		uint32_t systime_be = htobe32(systime);
		fwrite(&systime_be, sizeof(systime_be), 1, dumpfile);
		fwrite(rx, sizeof(usb_pkt_rx), 1, dumpfile);
		fflush(dumpfile);
	}

	r = btbb_process_packet(pkt, pn);

	/* The reference upper address part (UAP) the capture records alongside
	 * this packet, read now that the decoder has had the packet. A packet
	 * whose CRC is what finally settles the piconet's UAP settles it inside
	 * the call above, so reading it beforehand left that one packet -- the
	 * first evidence a reader has for the UAP, and the record that most
	 * needs it -- written as if nothing was known, with its CRC left with
	 * nothing to check it against. */
	if (pn && btbb_piconet_get_flag(pn, BTBB_UAP_VALID))
		uap = btbb_piconet_get_uap(pn);

	/* The packet's own 18-bit header, for the capture record below. Nothing
	 * else reads it while the decoder is still working the piconet's clock
	 * out: the UAP search unwhitens the header into a scratch buffer, takes
	 * the UAP out of its check bits and throws the header away. Such a
	 * record carried 18 zero bits, which a reader cannot tell from a header
	 * of zeros that was really sent.
	 *
	 * Reading it needs the piconet's clock rather than the Ubertooth's. The
	 * two differ by the offset the UAP search worked out, which counts in
	 * 625 us slots and so in two clock ticks each, until the whole clock is
	 * known. From then on libbtbb reads the header itself and the offset
	 * the piconet reports counts in clock ticks, hence the guard: those
	 * packets are the decoder's.
	 *
	 * A header the error correction could decode but whose check bits do
	 * not match this piconet's UAP -- a packet of another piconet whose
	 * access code matched, say -- is written into the packet before the
	 * decoder compares them, and goes into the record. A reader finds that
	 * out the way it checks any recorded header, by recomputing the check
	 * bits from the reference UAP in the same record. */
	if (pn && btbb_piconet_get_flag(pn, BTBB_UAP_VALID)
	       && btbb_piconet_get_flag(pn, BTBB_CLK6_VALID)
	       && !btbb_piconet_get_flag(pn, BTBB_CLK27_VALID)
	       && btbb_header_present(pkt)) {
		/* Setting the data again is what carries that clock into the
		 * packet, and leaves it there: the capture writers below do not
		 * read it, and the Ubertooth's own reading has already been
		 * printed. The UAP goes in as well, so that the decoder's
		 * comparison is against the piconet's UAP rather than against
		 * nothing; the record carries the header bits either way. */
		uint32_t piconet_clkn = (clkn + 2 * (uint32_t)
		        btbb_piconet_get_clk_offset(pn)) & CLKN_MASK;

		btbb_packet_set_uap(pkt, uap);
		btbb_packet_set_flag(pkt, BTBB_CLK6_VALID, 1);
		btbb_packet_set_data(pkt, syms + offset, BANK_LEN - offset,
		                     rx->channel, piconet_clkn);
		btbb_decode_header(pkt);
	}

	/* Dump to PCAP/PCAPNG if specified */
	if (ut->h_pcap_bredr) {
		btbb_pcap_append_packet(ut->h_pcap_bredr, nowns,
		                        signal_level, noise_level,
		                        lap, uap, pkt);
	}
	if (ut->h_pcapng_bredr) {
		btbb_pcapng_append_packet(ut->h_pcapng_bredr, nowns,
		                          signal_level, noise_level,
		                          lap, uap, pkt);
	}

	if(infile == NULL && r < 0) {
		cmd_start_hopping(ut->devh, btbb_piconet_get_clk_offset(pn), 0);
		calibrated = 0;
	}

out:
	if (pkt)
		btbb_packet_unref(pkt);
}
