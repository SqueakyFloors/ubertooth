/*
 * Copyright 2026 Great Scott Gadgets
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

/*
 * Putting a long Bluetooth low-energy packet back together.
 *
 * A record on the wire holds 50 bytes of packet, so a packet longer than that
 * arrives as several records: one LE_PACKET_START and then LE_PACKET_CONT
 * records in order, each saying how long the whole packet is and where it
 * sits in the chain. This collects them until the packet is complete.
 *
 * The state belongs to one board, so two boards captured at once never mix
 * their packets.
 */
#ifndef __UBERTOOTH_LE_FRAG_H__
#define __UBERTOOTH_LE_FRAG_H__

#include <stdint.h>

#include "ubertooth_interface.h"

typedef struct {
	uint8_t data[LE_MAX_USB_PACKET_SIZE];
	/* How long the chain being collected says the packet is, and how much
	 * of it has arrived. */
	uint16_t expected;
	uint16_t received;
	/* The position the next continuation record must carry. */
	uint8_t next_index;
	/* Whether a chain is being collected at all. */
	uint8_t active;
	/* The first record of the chain, which carries the channel, timestamp
	 * and signal readings of the packet as a whole, and when that record
	 * arrived. The timestamp and the arrival reading are compared against
	 * each other, so both have to come from the same record: the records
	 * that follow in a chain arrive later and are not it. */
	usb_pkt_rx metadata;
	uint64_t metadata_arrival_ns;
} le_usb_reassembly;

enum le_usb_reassembly_result {
	LE_REASSEMBLY_ERROR = -1,
	LE_REASSEMBLY_WAIT = 0,
	LE_REASSEMBLY_COMPLETE = 1,
};

/*
 * Takes one record and says whether a whole packet is now in hand.
 *
 * A record of the plain LE_PACKET type is a whole packet already, so it
 * completes at once. A chain completes on the record that brings the last of
 * the declared length. COMPLETE fills in data, its length, the record whose
 * metadata describes the packet, and when that record arrived; the first
 * three stay valid until the next call with this state.
 *
 * arrival_ns is when the record passed in arrived. It is reported back as
 * metadata_arrival_ns for the record the metadata came from, which for a
 * chain is the first record rather than the one that completed it.
 *
 * WAIT means more records of this chain are still to come. ERROR means the
 * record cannot be part of a packet -- a continuation with no chain started,
 * out of order, or disagreeing about the length -- and the chain is dropped:
 * half a packet is never handed on.
 */
int le_usb_reassemble(le_usb_reassembly *state, const usb_pkt_rx *frame,
                      uint64_t arrival_ns, const uint8_t **data,
                      uint16_t *length, const usb_pkt_rx **metadata,
                      uint64_t *metadata_arrival_ns);

/* Forgets a chain in progress, so a handle starting a capture never carries
 * half a packet in from the last one. */
void le_usb_reassembly_reset(le_usb_reassembly *state);

#endif /* __UBERTOOTH_LE_FRAG_H__ */
