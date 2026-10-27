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

#include "ubertooth_le_frag.h"

#include <string.h>

/* The whole packet's length, as this record states it. */
static uint16_t fragment_total(const usb_pkt_rx *frame)
{
	return (uint16_t) frame->reserved[0] |
	       ((uint16_t) (frame->reserved[1] & LE_FRAG_TOTAL_HIGH_MASK) << 8);
}

/* This record's position in the chain, counting up from zero. */
static uint8_t fragment_index(const usb_pkt_rx *frame)
{
	return frame->reserved[1] >> LE_FRAG_INDEX_SHIFT;
}

/* Copies this record's bytes onto the end of what has arrived so far, and
 * says whether that completes the packet. The last record of a chain is
 * usually short, so only the bytes still wanted are taken. */
static int append_fragment(le_usb_reassembly *state, const usb_pkt_rx *frame)
{
	uint16_t remaining = state->expected - state->received;
	uint16_t chunk = remaining < DMA_SIZE ? remaining : DMA_SIZE;

	memcpy(state->data + state->received, frame->data, chunk);
	state->received += chunk;
	return state->received == state->expected;
}

void le_usb_reassembly_reset(le_usb_reassembly *state)
{
	if (state)
		state->active = 0;
}

int le_usb_reassemble(le_usb_reassembly *state, const usb_pkt_rx *frame,
                      const uint8_t **data, uint16_t *length,
                      const usb_pkt_rx **metadata)
{
	uint16_t total;

	if (!state || !frame || !data || !length || !metadata)
		return LE_REASSEMBLY_ERROR;

	*data = NULL;
	*length = 0;
	*metadata = NULL;

	if (frame->pkt_type == LE_PACKET) {
		/* A whole packet in one record. It also means any chain that was
		 * being collected will never be finished. */
		state->active = 0;
		*data = frame->data;
		*length = DMA_SIZE;
		*metadata = frame;
		return LE_REASSEMBLY_COMPLETE;
	}

	if (frame->pkt_type == LE_PACKET_START) {
		total = fragment_total(frame);
		/* A first record has to be the first of a chain, and the chain
		 * has to be one the firmware would have sent: a packet of 50
		 * bytes or less travels in a single LE_PACKET record, and no
		 * packet is longer than LE_MAX_USB_PACKET_SIZE. */
		if (fragment_index(frame) != 0 || total <= DMA_SIZE ||
		    total > LE_MAX_USB_PACKET_SIZE) {
			state->active = 0;
			return LE_REASSEMBLY_ERROR;
		}
		state->metadata = *frame;
		/* The reassembled packet is handed on as an ordinary one, so it
		 * carries neither a fragment type nor fragment metadata. */
		state->metadata.pkt_type = LE_PACKET;
		state->metadata.reserved[0] = 0;
		state->metadata.reserved[1] = 0;
		state->expected = total;
		state->received = 0;
		state->next_index = 1;
		state->active = 1;
	} else if (frame->pkt_type == LE_PACKET_CONT) {
		total = fragment_total(frame);
		/* A continuation belongs to the chain being collected only if it
		 * is the next one and agrees about the length. Anything else
		 * means a record was lost or the stream is out of step, and the
		 * packet can no longer be trusted. */
		if (!state->active || total != state->expected ||
		    fragment_index(frame) != state->next_index) {
			state->active = 0;
			return LE_REASSEMBLY_ERROR;
		}
		++state->next_index;
	} else {
		/* Not a low-energy packet record at all. The caller handles the
		 * record types that mean something else; anything left is not a
		 * packet, and a chain in progress is left alone because the
		 * firmware sends a chain's records one after another. */
		return LE_REASSEMBLY_ERROR;
	}

	if (!append_fragment(state, frame))
		return LE_REASSEMBLY_WAIT;

	state->active = 0;
	*data = state->data;
	*length = state->expected;
	*metadata = &state->metadata;
	return LE_REASSEMBLY_COMPLETE;
}
