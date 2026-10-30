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

#ifndef __UBERTOOTH_DMA_H
#define __UBERTOOTH_DMA_H value

#include "inttypes.h"
#include "ubertooth.h"

extern volatile uint8_t rxbuf1[DMA_SIZE];
extern volatile uint8_t rxbuf2[DMA_SIZE];

/*
 * The active buffer is the one with an active DMA transfer.
 * The idle buffer is the one we can read/write between transfers.
 */
extern volatile uint8_t* volatile active_rxbuf;
extern volatile uint8_t* volatile idle_rxbuf;

/* rx terminal count and error interrupt counters */
extern volatile uint32_t rx_tc;
extern volatile uint32_t rx_err;

void dma_poweron();
void dma_poweroff();
void dma_clear_interrupts(unsigned channel);
void dma_init_rx_symbols();

/* How many bytes the first item of the low-energy receive chain covers. Its
 * terminal count is what wakes the mode loop, and by then the PDU header has
 * arrived, so the loop can read how long the packet is going to be. */
#define LE_DMA_HEAD 4

/* Sets the low-energy receive DMA up to put `len` received bytes at `dest`,
 * which must be at least LE_DMA_HEAD long. A whole packet fits in one go, so
 * the mode loop never has to re-arm the controller mid-packet. */
void dma_init_le(volatile uint8_t* dest, unsigned len);
void dio_ssp_start();
void dio_ssp_stop();

/* Stop the SSP raising a receive transfer request, leaving the SSP and the
 * channel as they are. See the comment on it: every partial write of these
 * registers holds interrupts off, because the capture engine's loop and both
 * of its interrupt handlers reach them. */
void dio_ssp_rx_dma_disable(void);

#endif
