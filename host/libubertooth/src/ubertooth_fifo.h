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

#ifndef __cplusplus
#include <stdatomic.h>
#endif

/* Slots in the ring, 1000000 of them or 72 MB. A test can build the ring
 * small instead (-DFIFO_SIZE=8), which is the only way a unit test reaches
 * the full-ring paths; every source linked into one binary must then be
 * built with the same value, because this is the size of fifo_t. */
#ifndef FIFO_SIZE
#define FIFO_SIZE 1000000
#endif

/* One of the ring's two pointers. Each is written by one thread and read by
 * the other, and the store that advances it is what hands a slot over, so it
 * is an atomic rather than a plain integer: see the top of ubertooth_fifo.c.
 *
 * A C++ translation unit that includes this header (the Kismet plugin does)
 * gets the plain integer the atomic is built on, because _Atomic is not C++.
 * Nothing outside ubertooth_fifo.c touches these two fields, and nothing in
 * C++ needs the size of a fifo_t: a handle holds only a pointer to one. */
#ifdef __cplusplus
typedef size_t fifo_ptr_t;
#else
typedef _Atomic size_t fifo_ptr_t;
#endif

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
	/* The reading of the record fifo_pop() returned last, copied out of
	 * the ring while that slot was still the consumer's. */
	uint64_t popped_arrival_ns;
	fifo_ptr_t read_ptr;
	fifo_ptr_t write_ptr;
} fifo_t;

fifo_t* fifo_init();

/* Publishes the slot fifo_get_write_element() handed out, stamped with when
 * its record arrived. */
void fifo_inc_write_ptr(fifo_t* fifo, uint64_t arrival_ns);

void fifo_push(fifo_t* fifo, const usb_pkt_rx* packet, uint64_t arrival_ns);

/* The oldest record waiting, as a copy, and its slot goes back to the
 * producer. Ask fifo_empty() first: a pop of an empty ring returns whatever
 * the next slot holds, and that question is also what makes a record the
 * producer has just published visible to this thread. */
usb_pkt_rx fifo_pop(fifo_t* fifo);

/* When the record fifo_pop() returned last arrived. The answer is taken
 * during that pop and kept, so another pop replaces it. */
uint64_t fifo_popped_arrival_ns(const fifo_t* fifo);

/* The slot the next record is to be read into, for a caller that fills a
 * slot in place (a USB transfer writes into it) and then publishes it with
 * fifo_inc_write_ptr(). Never a slot the consumer can be reading. */
usb_pkt_rx* fifo_get_write_element(fifo_t* fifo);

/* Whether the ring holds no record the consumer has not taken. For the
 * consumer to ask. */
uint8_t fifo_empty(fifo_t* fifo);

#endif /* __UBERTOOTH_FIFO_H__ */
