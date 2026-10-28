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

/*
 * The capture ring buffer, shared by exactly two threads.
 *
 * The producer is the thread that collects records off the board: it fills
 * the slot fifo_get_write_element() hands out and publishes it with
 * fifo_inc_write_ptr(), or does both at once with fifo_push(). The consumer
 * is the capture's own loop, which takes records with fifo_pop(). Each owns
 * one pointer -- the producer write_ptr, the consumer read_ptr -- and only
 * ever reads the other's.
 *
 * Who owns a slot follows from the two pointers. The slots from read_ptr up
 * to write_ptr hold published records and are the consumer's; the rest,
 * write_ptr included, are the producer's. Advancing a pointer is what hands
 * a slot over, so each advance is a release store and each load of the other
 * thread's pointer is an acquire load:
 *
 *   - publishing (write_ptr) releases the record and its arrival reading, so
 *     a consumer that sees the new value sees the whole record. Without that
 *     pairing nothing but the processor's own ordering stops the consumer
 *     reading a record the producer had not finished writing.
 *   - releasing a slot (read_ptr) releases the consumer's reads of it, so a
 *     producer that sees the new value will not write that slot until the
 *     copy out of it is done. fifo_pop() therefore copies the record and its
 *     reading before it advances read_ptr, and not after: a transfer handed
 *     a slot the consumer was still copying gave one corrupted packet.
 *
 * The ring always leaves one slot empty, which is what keeps the slot a
 * transfer is reading into out of the consumer's range: a publish that would
 * make write_ptr meet read_ptr is refused and the record is discarded.
 */
#include "ubertooth_fifo.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

fifo_t* fifo_init()
{
	/* Zeroed rather than merely allocated: a slot read before any record
	 * was published into it then reads as zeros, the same on every run,
	 * instead of as whatever the allocation happened to hold. */
	fifo_t* fifo = (fifo_t*)calloc(1, sizeof(fifo_t));

	if (fifo == NULL)
		return NULL;

	atomic_init(&fifo->read_ptr, 0);
	atomic_init(&fifo->write_ptr, 0);

	return fifo;
}

void fifo_inc_write_ptr(fifo_t* fifo, uint64_t arrival_ns)
{
	/* Relaxed: this thread is the only one that writes write_ptr. */
	size_t write = atomic_load_explicit(&fifo->write_ptr,
	                                    memory_order_relaxed);
	size_t next = (write + 1) % FIFO_SIZE;

	/* Acquire, so that a slot the consumer has just finished with is taken
	 * back only once its reads of that slot are complete here. A read_ptr
	 * read a moment stale only refuses a publish this ring had room for. */
	if (atomic_load_explicit(&fifo->read_ptr, memory_order_acquire)
	    == next) {
		fprintf(stderr, "warning: FIFO overflow, packet discarded\n");
		return;
	}

	/* Written with the record, before the store that publishes the slot,
	 * in the same order and on the same terms as the record itself. */
	fifo->arrival_ns[write] = arrival_ns;

	/* Release: whoever sees this value sees the record and the reading. */
	atomic_store_explicit(&fifo->write_ptr, next, memory_order_release);
}

uint8_t fifo_empty(fifo_t* fifo)
{
	/* Relaxed: the consumer, which is who asks, is the only thread that
	 * writes read_ptr. */
	size_t read = atomic_load_explicit(&fifo->read_ptr,
	                                   memory_order_relaxed);
	/* Acquire: this is the matching half of the publish, so the pop that a
	 * waiting record leads to sees the whole record. A write_ptr read a
	 * moment stale only reports a ring that has just filled as empty. */
	size_t write = atomic_load_explicit(&fifo->write_ptr,
	                                    memory_order_acquire);

	return (read == write);
}

void fifo_push(fifo_t* fifo, const usb_pkt_rx* packet, uint64_t arrival_ns)
{
	size_t write = atomic_load_explicit(&fifo->write_ptr,
	                                    memory_order_relaxed);

	/* The slot is the producer's until the publish below, so filling it
	 * first is safe even when the ring turns out to be full: the record is
	 * then discarded where it lies. */
	memcpy(&(fifo->packets[write]), packet, sizeof(usb_pkt_rx));

	fifo_inc_write_ptr(fifo, arrival_ns);
}

usb_pkt_rx fifo_pop(fifo_t* fifo)
{
	/* Relaxed: this thread is the only one that writes read_ptr. */
	size_t selected = atomic_load_explicit(&fifo->read_ptr,
	                                       memory_order_relaxed);
	usb_pkt_rx packet = fifo->packets[selected];

	/* Kept rather than read back out of the ring later: once the slot is
	 * handed back below, the producer may write to it. */
	fifo->popped_arrival_ns = fifo->arrival_ns[selected];

	/* Both reads are done, so the slot goes back to the producer. Release:
	 * a producer that sees this value sees those reads as finished. */
	atomic_store_explicit(&fifo->read_ptr, (selected + 1) % FIFO_SIZE,
	                      memory_order_release);

	return packet;
}

uint64_t fifo_popped_arrival_ns(const fifo_t* fifo)
{
	return fifo->popped_arrival_ns;
}

usb_pkt_rx* fifo_get_write_element(fifo_t* fifo)
{
	/* Relaxed: this thread is the only one that writes write_ptr. */
	size_t write = atomic_load_explicit(&fifo->write_ptr,
	                                    memory_order_relaxed);

	return &(fifo->packets[write]);
}
