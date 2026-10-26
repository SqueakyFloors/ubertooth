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

/* Ignore packets with a SNR lower than this in order to reduce
 * processor load.  TODO: this should be a command line parameter. */

static int8_t rssi_history[NUM_BREDR_CHANNELS][RSSI_HISTORY_LEN] = {{INT8_MIN}};

static void determine_signal_and_noise( usb_pkt_rx *rx, int8_t * sig, int8_t * noise )
{
	int8_t * channel_rssi_history = rssi_history[rx->channel];
	int8_t rssi;
	int i;

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

static uint64_t now_ns( void )
{
	struct timespec ts = { 0, 0 };
	(void) clock_gettime( CLOCK_REALTIME, &ts );
	return (1000000000ull*(uint64_t) ts.tv_sec) + (uint64_t) ts.tv_nsec;
}

/* The host time a packet arrived: the current time for a live capture, and
 * the host time stored with the replayed record for a replay, so a capture
 * written with -q/-r while replaying keeps the recorded times. The first
 * packet's is where a capture's timestamps start. */
static uint64_t packet_host_ns( void )
{
	if (infile != NULL)
		return 1000000000ull * (uint64_t) systime;
	return now_ns( );
}

/* A second reading of the host, used only to measure how long passed between
 * two packets. A live capture reads the monotonic clock, which no step of the
 * host clock can move, so a step cannot change how a packet's clock reading is
 * read; the time written into the capture still comes from packet_host_ns()
 * above. A replay has only the host time each record carries. */
static uint64_t packet_elapsed_ns( void )
{
	struct timespec ts = { 0, 0 };

	if (infile != NULL)
		return 1000000000ull * (uint64_t) systime;
	(void) clock_gettime( CLOCK_MONOTONIC, &ts );
	return (1000000000ull*(uint64_t) ts.tv_sec) + (uint64_t) ts.tv_nsec;
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
	ut->last_elapsed_ns = 0;
	ut->clk100ns_anchor_open = 0;
	ut->clk100ns_gap_warned = 0;
}

/* How far the host's measure of the gap between two packets may fall short of
 * the time a wrap would account for and still be read as that wrap. It is
 * sized to how imprecisely that gap is known, not to the clock's period: a
 * replayed record's host time is a whole second, so two of them can misstate
 * the gap by just under a second either way, and a live capture reads the host
 * as it takes each packet in, a packet's latency apart. Two seconds covers
 * both, and a genuine wrap's own figure is good to well inside it.
 *
 * Whichever way the decision goes wrong it costs a whole period, so this does
 * not bound the error; what it bounds is how far the two measures have to
 * disagree before the decision flips. Small is still the safer direction. Too
 * small loses a genuine wrap between the first two packets: the times come out
 * a period short, which the host clock catches and reports below. Too large
 * reads the leftover packet as a wrap: they come out a period long, and nothing
 * says so, because that report only looks for times behind the host clock. */
#define CLK100NS_WRAP_SLACK_NS 2000000000ull

static void track_clk100ns( ubertooth_t* ut, const usb_pkt_rx* rx,
                            uint64_t host_ns, uint64_t elapsed_ns )
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
	/* How much time a wrap here would account for, against how much the
	 * host saw pass. Only meaningful where the clock reads lower than it
	 * did. */
	uint64_t wrap_ns, since_last_ns;
	/* Whether this is the packet right after the capture's first, the only
	 * one where a lower clock reading has a rival reading. Taken once and
	 * cleared here, so no later packet is judged that way. */
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
		ut->last_elapsed_ns = elapsed_ns;
		ut->clk100ns_anchor_open = 1;
		return;
	}

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
		if (advance < 128) {
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
		 * capture. A packet out of place further in reads lower too, and
		 * is still read as a wrap. Settling those the same way would
		 * need the gap between two packets known as each arrived rather
		 * than as the host got round to it: a packet taken from a
		 * backlog is processed moments after the one before it however
		 * long the real gap was, so judging every drop this way refuses
		 * genuine wraps. That reading is not kept per packet today.
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
		since_last_ns = (elapsed_ns > ut->last_elapsed_ns) ?
		                elapsed_ns - ut->last_elapsed_ns : 0;

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
	ut->last_elapsed_ns = elapsed_ns;
}

/* A wrap that cannot be counted - inferred from the clock going backwards and
 * falling in a gap longer than one period, or past the range of the count a
 * packet carries - leaves nothing in the capture to show it: the times simply
 * come out short from then on. The host clock does show it, because it kept
 * running, so it ends up ahead of the reconstructed times. Said once, and not
 * used to correct them: it says that time was lost, not where, and a host
 * clock that is stepped during a capture must not move packet times, which is
 * why the reading that decides a wrap above is taken from the monotonic clock
 * instead. A step is also why this message does not claim a lost wrap as the
 * cause: a host clock jumped forwards looks the same from here. */
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

static uint64_t now_ns_from_clk100ns( ubertooth_t* ut, const usb_pkt_rx* rx )
{
	uint64_t packet_ns;
	/* The time this packet is stamped with, read once and used both to
	 * start the capture and to report a wrap that could not be told either
	 * way, and the separate reading that measures how long passed since the
	 * last packet. */
	uint64_t host_ns = packet_host_ns( );
	uint64_t elapsed_ns = packet_elapsed_ns( );

	track_clk100ns( ut, rx, host_ns, elapsed_ns );
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

/* Sniff for LAPs. If a piconet is provided, use the given LAP to
 * search for UAP.
 */
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
	char syms[BANK_LEN];
	ubertooth_unpack_symbols((uint8_t*)rx->data, syms);

	/* Sanity check */
	if (rx->channel > (NUM_BREDR_CHANNELS-1))
		goto out;

	determine_signal_and_noise( rx, &signal_level, &noise_level );
	snr = signal_level - noise_level;

	/* Pass packet-pointer-pointer so that
	 * packet can be created in libbtbb. */
	offset = btbb_find_ac(syms, BANK_LEN - 64, LAP_ANY, max_ac_errors, &pkt);
	if (offset < 0)
		goto out;

	/* Once offset is known for a valid packet, copy in symbols
	 * and other rx data. CLKN here is the 312.5us CLK27-0. The
	 * btbb library can shift it be CLK1 if needed. */
	clkn = (rx->clkn_high << 20) + (le32toh(rx->clk100ns) + offset*10) / CLK100NS_PER_CLKN;
	btbb_packet_set_data(pkt, syms + offset, BANK_LEN - offset,
	                     rx->channel, clkn);

	/* A replayed record carries the host time it was recorded with (read
	 * into systime by the caller); a live one is stamped now. */
	if (infile == NULL)
		systime = time(NULL);

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

void cb_afh_initial(ubertooth_t* ut, void* args)
{
	btbb_piconet* pn = (btbb_piconet*)args;
	btbb_packet* pkt = NULL;
	uint8_t channel;

	usb_pkt_rx usb = fifo_pop(ut->fifo);
	usb_pkt_rx* rx = &usb;
	char syms[BANK_LEN];
	ubertooth_unpack_symbols((uint8_t*)rx->data, syms);


	if( btbb_find_ac(syms, BANK_LEN - 64, btbb_piconet_get_lap(pn), max_ac_errors, &pkt) < 0 )
		goto out;

	/* detect AFH map
	 * set current channel as used channel and send updated AFH
	 * map to ubertooth */
	channel = rx->channel;
	if(btbb_piconet_set_channel_seen(pn, channel)) {

		/* Don't allow single unused channels */
		if (!btbb_piconet_get_channel_seen(pn, channel+1) &&
		    btbb_piconet_get_channel_seen(pn, channel+2))
		{
			fprintf(stderr, "note: activating additional channel %d\n",
			        channel+1);
		    btbb_piconet_set_channel_seen(pn, channel+1);
		}
		if (!btbb_piconet_get_channel_seen(pn, channel-1) &&
		    btbb_piconet_get_channel_seen(pn, channel-2))
		{
			fprintf(stderr, "note: activating additional channel %d\n",
			        channel-1);
			btbb_piconet_set_channel_seen(pn, channel-1);
		}

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

	static unsigned long last_seen[79] = {0};
	static unsigned long counter = 0;


	if( btbb_find_ac(syms, BANK_LEN - 64, btbb_piconet_get_lap(pn), max_ac_errors, &pkt) < 0 )
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

	for(i=0; i<79; i++) {
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

	static unsigned long last_seen[79] = {0};
	static unsigned long counter = 0;

	if( btbb_find_ac(syms, BANK_LEN - 64, btbb_piconet_get_lap(pn), max_ac_errors, &pkt) < 0 )
		goto out;


	counter++;
	channel = rx->channel;
	last_seen[channel]=counter;

	btbb_piconet_set_channel_seen(pn, channel);

	for(i=0; i<79; i++) {
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
	usb_pkt_rx* rx = &usb;
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
		void *val = &rx->data[1];

		printf("--------------------\n");
		printf("LE Promisc - ");
		switch (state) {
			case 0:
				printf("Access Address: %08x\n", *(uint32_t *)val);
				break;
			case 1:
				printf("CRC Init: %06x\n", *(uint32_t *)val);
				break;
			case 2:
				printf("Hop interval: %g ms\n", *(uint16_t *)val * 1.25);
				break;
			case 3:
				printf("Hop increment: %u\n", *(uint8_t *)val);
				break;
			default:
				printf("Unknown %u\n", state);
				break;
		};
		printf("\n");

		return;
	}

	/* When reading from file, the caller read the record's host time into
	 * systime before calling this routine, so do not overwrite it. */
	if (infile == NULL)
		systime = time(NULL);

	uint64_t nowns = now_ns_from_clk100ns( ut, rx );

	/* Sanity check */
	if (rx->channel > (NUM_BREDR_CHANNELS-1))
		return;

	/* Dump to sumpfile if specified */
	if (dumpfile) {
		uint32_t systime_be = htobe32(systime);
		fwrite(&systime_be, sizeof(systime_be), 1, dumpfile);
		fwrite(rx, sizeof(usb_pkt_rx), 1, dumpfile);
		fflush(dumpfile);
	}

	/* The decoder always reads 64 bytes from the pointer it is handed, and
	 * a received packet is only 50, so it cannot be given rx->data. */
	uint8_t symbols[LE_SYM_LEN];
	ubertooth_le_symbols(rx, &symbols);
	lell_allocate_and_decode(symbols, rx->channel + 2402, rx->clk100ns, &pkt);

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

	// rollover
	uint64_t rx_ts = le32toh( rx->clk100ns );
	if (rx_ts < prev_ts)
		rx_ts += ut->clk100ns_period;
	uint64_t ts_diff = rx_ts - prev_ts;
	prev_ts = le32toh( rx->clk100ns );
	printf("systime=%u freq=%d addr=%08x delta_t=%.03f ms rssi=%d\n",
	       systime, rx->channel + 2402, lell_get_access_address(pkt),
	       ts_diff / 10000.0, rx->rssi_min - 54);

	int len = (rx->data[5] & 0x3f) + 6 + 3;
	if (len > 50) len = 50;

	for (i = 4; i < len; ++i)
		printf("%02x ", rx->data[i]);
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
	static u32 prev_ts = 0;
	usb_pkt_rx usb = fifo_pop(ut->fifo);
	usb_pkt_rx* rx = &usb;

	u32 rx_time = rx->clk100ns;
	if (rx_time < prev_ts)
		rx_time += CLK100NS_PERIOD; // rollover
	u32 ts_diff = rx_time - prev_ts;
	prev_ts = rx->clk100ns;
	printf("time=%u delta_t=%.06f ms freq=%d \n",
	       rx->clk100ns, ts_diff / 10000.0,
	       rx->channel + 2402);

	for (i = 0; i < len; ++i)
		printf("%02x ", rx->data[i]);
	printf("\n\n");

	fflush(stdout);
}

#define CLOCK_TRIM_THRESHOLD 2

void cb_rx(ubertooth_t* ut, void* args)
{
	btbb_packet* pkt = NULL;
	btbb_piconet* pn = (btbb_piconet *)args;
	char syms[BANK_LEN*10] = {0};
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
	ubertooth_unpack_symbols((uint8_t*)rx->data, syms);

	if (rx->pkt_type != BR_PACKET) {
		goto out;
	}

	if (rx->status & DISCARD) {
		goto out;
	}

	// /* Sanity check */
	if (rx->channel > (NUM_BREDR_CHANNELS-1))
		goto out;

	/* When reading from file, caller will read
	 * systime before calling this routine, so do
	 * not overwrite. Otherwise, get current time. */
	if (infile == NULL)
		systime = time(NULL);

	uint64_t nowns = now_ns_from_clk100ns( ut, rx );

	int8_t signal_level = rx->rssi_max;
	int8_t noise_level = rx->rssi_min;
	determine_signal_and_noise( rx, &signal_level, &noise_level );
	int8_t snr = signal_level - noise_level;

	/* Look for packets with specified LAP, if given. Otherwise
	 * search for any packet. */
	if (pn) {
		lap = btbb_piconet_get_flag(pn, BTBB_LAP_VALID) ? btbb_piconet_get_lap(pn) : LAP_ANY;
		uap = btbb_piconet_get_flag(pn, BTBB_UAP_VALID) ? btbb_piconet_get_uap(pn) : UAP_ANY;
	}

	/* Pass packet-pointer-pointer so that
	 * packet can be created in libbtbb. */
	offset = btbb_find_ac(syms, BANK_LEN, lap, max_ac_errors, &pkt);
	if (offset < 0)
		goto out;

	/* calculate the offset between the first bit of the AC and the rising edge of CLKN */
	clk_offset = (le32toh(rx->clk100ns) + offset*10 + 6250 - 4000) % 6250;

	btbb_packet_set_modulation(pkt, BTBB_MOD_GFSK);
	btbb_packet_set_transport(pkt, BTBB_TRANSPORT_ANY);

	/* Once offset is known for a valid packet, copy in symbols
	 * and other rx data. CLKN here is the 312.5us CLK27-0. The
	 * btbb library can shift it be CLK1 if needed. */
	clkn = (le32toh(rx->clkn_high) << 20) + (le32toh(rx->clk100ns) + offset*10 - 4000) / CLK100NS_PER_CLKN;
	btbb_packet_set_data(pkt, syms + offset, BANK_LEN*10 - offset,
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
		if (trim_counter < -CLOCK_TRIM_THRESHOLD
		    || ((clk_offset < CLK_TUNE_TIME) && !calibrated)) {
			fprintf(stderr, "note: offset < CLK_TUNE_TIME\n");
			fprintf(stderr, "note: CLK100ns trim: %d\n",
			        6250 + clk_offset - CLK_TUNE_TIME);
			cmd_trim_clock(ut->devh, 6250 + clk_offset - CLK_TUNE_TIME);
			trim_counter = 0;
			if (calibrated) {
				fprintf(stderr, "note: clock drifted %d in %f s, "
				        "%d PPM too slow\n",
				       (clk_offset-CLK_TUNE_TIME),
				       (double)(clkn-clkn_trim)/3200,
				       (clk_offset-CLK_TUNE_TIME) * 320 / (int32_t)(clkn-clkn_trim));
				cmd_fix_clock_drift(ut->devh, (clk_offset-CLK_TUNE_TIME) * 320 / (int32_t)(clkn-clkn_trim));
			}
			clkn_trim = clkn;
			calibrated = 1;
			goto out;
		} else if (trim_counter > CLOCK_TRIM_THRESHOLD
		           || ((clk_offset > CLK_TUNE_TIME) && !calibrated)) {
			fprintf(stderr, "note: offset > CLK_TUNE_TIME\n");
			fprintf(stderr, "note: CLK100ns trim: %d\n",
			        clk_offset - CLK_TUNE_TIME);
			cmd_trim_clock(ut->devh, clk_offset - CLK_TUNE_TIME);
			trim_counter = 0;
			if (calibrated) {
				fprintf(stderr, "note: clock drifted %d in %f s, "
				        "%d PPM too fast\n",
				       (clk_offset-CLK_TUNE_TIME),
				       (double)(clkn-clkn_trim)/3200,
				       (clk_offset-CLK_TUNE_TIME) * 320 / (clkn-clkn_trim));
				cmd_fix_clock_drift(ut->devh, (clk_offset-CLK_TUNE_TIME) * 320 / (clkn-clkn_trim));
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
