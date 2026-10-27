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

#include "ubertooth_fifo.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

fifo_t* fifo_init()
{
	/* Zeroed, so that a reading read back from a slot no record was ever
	 * published into is 0 rather than whatever the allocation held. */
	fifo_t* fifo = (fifo_t*)calloc(1, sizeof(fifo_t));

	if (fifo == NULL)
		return NULL;

	fifo->read_ptr = 0;
	fifo->write_ptr = 0;

	return fifo;
}

void fifo_inc_write_ptr(fifo_t* fifo, uint64_t arrival_ns)
{
	if (fifo->read_ptr != (fifo->write_ptr+1) % FIFO_SIZE) {
		/* Stamped before the slot is published, so the consumer never
		 * sees a record without the reading that belongs to it. */
		fifo->arrival_ns[fifo->write_ptr] = arrival_ns;
		fifo->write_ptr = (fifo->write_ptr+1) % FIFO_SIZE;
	} else {
		fprintf(stderr, "warning: FIFO overflow, packet discarded\n");
	}
}

uint8_t fifo_empty(fifo_t* fifo)
{
	return (fifo->read_ptr == fifo->write_ptr);
}

void fifo_push(fifo_t* fifo, const usb_pkt_rx* packet, uint64_t arrival_ns)
{
	memcpy(&(fifo->packets[fifo->write_ptr]), packet, sizeof(usb_pkt_rx));

	fifo_inc_write_ptr(fifo, arrival_ns);
}

usb_pkt_rx fifo_pop(fifo_t* fifo)
{
	size_t selected = fifo->read_ptr;

	fifo->read_ptr = (fifo->read_ptr+1) % FIFO_SIZE;

	return fifo->packets[selected];
}

uint64_t fifo_popped_arrival_ns(const fifo_t* fifo)
{
	/* The slot the last pop left behind, counting back round the ring. */
	size_t popped = (fifo->read_ptr + FIFO_SIZE - 1) % FIFO_SIZE;

	return fifo->arrival_ns[popped];
}

usb_pkt_rx* fifo_get_write_element(fifo_t* fifo)
{
	return &(fifo->packets[fifo->write_ptr]);
}
