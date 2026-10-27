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
 * constraint and for the same reason as the routine above: it reads systime
 * and infile.
 *
 * Two readings that come from the same arrival are still placed by separate
 * clock reads, and a packet is stamped per record while a capture's times
 * count board ticks from one anchor, so the two drift apart over a long
 * capture and the agreement above is only ever to within that second. */
uint32_t ubertooth_packet_systime(uint64_t arrival_ns);

void cb_afh_initial(ubertooth_t* ut, void* args);
void cb_afh_monitor(ubertooth_t* ut, void* args);
void cb_afh_r(ubertooth_t* ut, void* args);
void cb_btle(ubertooth_t* ut, void* args);
void cb_ego(ubertooth_t* ut, void* args __attribute__((unused)));
void cb_rx(ubertooth_t* ut, void* args);
void cb_scan(ubertooth_t* ut, void* args);

#endif /* __UBERTOOTH_CALLBACK_H__ */
