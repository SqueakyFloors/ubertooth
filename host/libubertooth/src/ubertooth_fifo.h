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

#ifndef __UBERTOOTH_FIFO_H__
#define __UBERTOOTH_FIFO_H__

#include "ubertooth_control.h"

// set fifo size to 1000000 elements or 64 MByte
#define FIFO_SIZE 1000000

typedef struct {
	usb_pkt_rx packets[FIFO_SIZE];
	/* When each record arrived, one reading per slot, on whichever clock
	 * the caller measures elapsed time with. The producer fills this ring
	 * as transfers complete and can run far ahead of the consumer that
	 * drains it, so a reading taken where a record is consumed says when
	 * the consumer got round to it, not when it came in: a consumer that
	 * stalls understates every gap it then catches up on by the length of
	 * the stall. A reading per record does not drift that way.
	 *
	 * Only a slot that has been published carries a reading, which is why
	 * the two calls that publish one take it and fifo_pop() does not have
	 * to: a record nothing stamped would silently carry the reading of
	 * whichever record held the slot a whole ring ago. Reading it back is
	 * optional, because a consumer that does not need it loses nothing. */
	uint64_t arrival_ns[FIFO_SIZE];
	size_t read_ptr;
	size_t write_ptr;
} fifo_t;

fifo_t* fifo_init();

/* Publishes the slot fifo_get_write_element() handed out, stamped with when
 * its record arrived. */
void fifo_inc_write_ptr(fifo_t* fifo, uint64_t arrival_ns);

void fifo_push(fifo_t* fifo, const usb_pkt_rx* packet, uint64_t arrival_ns);
usb_pkt_rx fifo_pop(fifo_t* fifo);

/* When the record fifo_pop() returned last arrived. The ring keeps one
 * reading per slot, so call it for the record just popped: another pop
 * replaces the answer. */
uint64_t fifo_popped_arrival_ns(const fifo_t* fifo);

usb_pkt_rx* fifo_get_write_element(fifo_t* fifo);

uint8_t fifo_empty(fifo_t* fifo);

#endif /* __UBERTOOTH_FIFO_H__ */
