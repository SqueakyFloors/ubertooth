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

#ifndef __UBERTOOTH_CALLBACK_H__
#define __UBERTOOTH_CALLBACK_H__


#include "ubertooth_control.h"
#include "ubertooth.h"

/* Returns the capture clock tracking to the state a capture starts from. A
 * handle used for a second capture must not inherit the first one's wrap
 * count, or every time it reconstructs would be a period or more too high.
 * Which clock stamps the packets (clk100ns_period) is the tool's choice, not
 * tracking state, and is left as the tool set it. */
void ubertooth_capture_clock_reset(ubertooth_t* ut);

/* Reads the clock the capture clock tracking measures gaps between records
 * with, to be called as each record arrives and carried with it through the
 * ring buffer. A live capture reads the monotonic clock, which no step of the
 * host clock can move, so a step cannot change how a record's clock reading is
 * read; the time written into the capture comes from the host clock instead. A
 * replay has only the host time each record carries, which the caller has read
 * into systime by the time the record is pushed.
 *
 * A live capture calls this from the thread its transfers complete on, which
 * is safe because a capture that has one is never a replay: infile is NULL for
 * the whole of it, so nothing here reads systime, which that thread does not
 * own. A replay has no such thread. Reading infile itself is safe either way,
 * because the one tool that sets it does so before any thread exists. */
uint64_t ubertooth_packet_arrival_ns(void);

/* The host time a record arrived, in whole seconds: what the systime= column
 * prints, what a -d dump stores with the record, and what a replay of that
 * dump anchors its capture on. Takes the reading the record arrived with, the
 * one the ring buffer carries, and places it the same way the capture's own
 * timestamps are placed, so a dump and a PCAP written side by side in one run
 * put the packet at the same moment, to within the whole second a dump
 * stores. Reading the host clock here instead would carry however long the
 * capture's loop took to reach the record into the column, into the dump, and
 * into every time a replay of that dump reconstructs. A replayed record keeps
 * the host time it was recorded with.
 *
 * Only the thread that drains the ring buffer may call this, the same
 * constraint and for the same reason as the routine above: it reads infile.
 * For a replay the reading passed in is itself the host time the record was
 * recorded with, so a packet reassembled from several records is placed where
 * its first record was, live and replayed alike.
 *
 * A record's two times are placed by separate clock reads, microseconds
 * apart, and a packet is stamped per record while a capture's times count
 * board ticks from one anchor, so the two drift apart over a long capture:
 * the agreement above is only ever to within that second. */
uint32_t ubertooth_packet_systime(uint64_t arrival_ns);

/* How long since cb_rx() heard a packet of the piconet whose clock this
 * capture is following, in milliseconds, and -1 when it is following no clock
 * the library gave it. A clock just handed over reads as no time at all,
 * because nothing has been heard on it yet.
 *
 * For a capture tool to tell a follow that has gone quiet from one that is
 * working. A clock that is not the piconet's does not fail: clock discovery
 * only offers candidates that carry the same CLK1-6, so a wrong CLK1-27
 * dewhitens and checks out every header it receives, and what it gets wrong is
 * which channel to listen on. Measured in libbtbb over the wrong locks of a
 * 400-value sweep, such a receiver lands on the piconet's channel in 5.7% of
 * slot pairs against 100% for the right clock, so it is heard from about one
 * packet in eighteen and no run of failing headers ever arrives for the
 * library to act on.
 *
 * Measured here, on the monotonic clock, so a caller compares one reading
 * against its own threshold rather than two clocks against each other. How
 * long a silence is long enough is the caller's: only the tool knows it asked
 * the board to hop with the piconet, and what its user wants of a piconet that
 * has stopped talking. */
long ubertooth_follow_quiet_ms(void);

/* How long the clock search has been running in this capture, in
 * milliseconds, and -1 when no search is running: nothing of the piconet has
 * reached the search yet, or a clock of the piconet's has been confirmed at
 * some point in this run. The reading starts at the first packet of the
 * piconet the search had to work with, so it is the span the search has had,
 * not the time since the last packet, and a band with no traffic on it reads
 * -1 however long the capture runs.
 *
 * For a tool that reports what the search cannot reach. The search does not
 * fail when it cannot reach a clock, it starts over, so nothing it prints
 * tells a user that it is not going to finish. A confirmed clock ends the
 * reading for the rest of the run, because a search that is running again
 * after one was given up has confirmed a clock before.
 *
 * How long is long enough is the caller's, as it is above. */
long ubertooth_clock_search_ms(void);

/* Gives up the clock this capture is following, for a caller that has decided
 * the follow has gone quiet, and leaves the capture as a clock the library
 * gives up itself does: the warning once on standard error, the board back on
 * the plain symbol capture and the channel this capture asked for, and
 * discovery running again.
 *
 * Returns 1 if a clock was given up, and 0 if there was none to give up: this
 * capture is not following one, or it is following a clock its own tool set,
 * which the library leaves alone because it has nothing better to fall back
 * on. Nothing is printed in that case, so a caller may ask whenever it thinks
 * the follow is quiet. Of the tools sharing this callback, only
 * ubertooth-follow sets a clock of its own, and it does not watch for silence,
 * so today that answer is for a caller that does not exist yet.
 *
 * Either way the reading above starts again, so a caller that asks on a wait
 * asks once per wait and not once per record. A board that refuses to come off
 * the old offset says so and is asked again at the caller's next wait, without
 * the warning being repeated. */
int ubertooth_follow_drop_clock(ubertooth_t* ut, btbb_piconet* pn);

void cb_afh_initial(ubertooth_t* ut, void* args);
void cb_afh_monitor(ubertooth_t* ut, void* args);
void cb_afh_r(ubertooth_t* ut, void* args);
void cb_btle(ubertooth_t* ut, void* args);
void cb_ego(ubertooth_t* ut, void* args __attribute__((unused)));
void cb_rx(ubertooth_t* ut, void* args);
void cb_scan(ubertooth_t* ut, void* args);

#endif /* __UBERTOOTH_CALLBACK_H__ */
