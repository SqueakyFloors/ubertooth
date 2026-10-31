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

#include "ubertooth_dma.h"
#include "irq.h"

volatile uint8_t rxbuf1[DMA_SIZE];
volatile uint8_t rxbuf2[DMA_SIZE];

/*
 * The active buffer is the one with an active DMA transfer.
 * The idle buffer is the one we can read/write between transfers.
 */
volatile uint8_t* volatile active_rxbuf;
volatile uint8_t* volatile idle_rxbuf;

/* rx terminal count and error interrupt counters */
volatile uint32_t rx_tc;
volatile uint32_t rx_err;

/* DMA linked list items */
typedef struct {
	uint32_t src;
	uint32_t dest;
	uint32_t next_lli;
	uint32_t control;
} dma_lli;

dma_lli rx_dma_lli1;
dma_lli rx_dma_lli2;

/* The low-energy receive chain: four bytes, then the rest of the packet. */
dma_lli le_dma_lli[2];


void dma_poweron(void) {
	// refer to UM10360 LPC17xx User Manual Ch 31 Sec 31.6.1, PDF page 616
	PCONP |= PCONP_PCGPDMA;

	// enable DMA interrupts at lowest priority
	IPR6 |= IPR6_IP_DMA; // hack, sets it to 31 (lowest)
	ISER0 = ISER0_ISE_DMA;

	// disable active channels
	DMACC0Config = 0;
	DMACC1Config = 0;
	DMACC2Config = 0;
	DMACC3Config = 0;
	DMACC4Config = 0;
	DMACC5Config = 0;
	DMACC6Config = 0;
	DMACC7Config = 0;
	DMACIntTCClear = 0xFF;
	DMACIntErrClr = 0xFF;

	/* enable DMA globally */
	DMACConfig = DMACConfig_E;
	while (!(DMACConfig & DMACConfig_E));
}

void dma_poweroff(void) {
	// Disable the DMA controller by writing 0 to the DMA Enable bit in the DMACConfig
	// register.
	DMACConfig &= ~DMACConfig_E;
	while (DMACConfig & DMACConfig_E);

	ICER0 = ICER0_ICE_DMA;

	PCONP &= ~PCONP_PCGPDMA;
}

void dma_clear_interrupts(unsigned channel) {
	DMACIntTCClear = 1 << channel;
	DMACIntErrClr  = 1 << channel;
}

void dma_init_rx_symbols(void) {
	dma_clear_interrupts(0);

	/* DMA linked lists */
	rx_dma_lli1.src = (uint32_t)&(DIO_SSP_DR);
	rx_dma_lli1.dest = (uint32_t)&rxbuf1[0];
	rx_dma_lli1.next_lli = (uint32_t)&rx_dma_lli2;
	rx_dma_lli1.control = (DMA_SIZE) |
			(1 << 12) |        /* source burst size = 4 */
			(1 << 15) |        /* destination burst size = 4 */
			(0 << 18) |        /* source width 8 bits */
			(0 << 21) |        /* destination width 8 bits */
			DMACCxControl_DI | /* destination increment */
			DMACCxControl_I;   /* terminal count interrupt enable */

	rx_dma_lli2.src = (uint32_t)&(DIO_SSP_DR);
	rx_dma_lli2.dest = (uint32_t)&rxbuf2[0];
	rx_dma_lli2.next_lli = (uint32_t)&rx_dma_lli1;
	rx_dma_lli2.control = (DMA_SIZE) |
			(1 << 12) |        /* source burst size = 4 */
			(1 << 15) |        /* destination burst size = 4 */
			(0 << 18) |        /* source width 8 bits */
			(0 << 21) |        /* destination width 8 bits */
			DMACCxControl_DI | /* destination increment */
			DMACCxControl_I;   /* terminal count interrupt enable */

	/* configure DMA channel 0 */
	DMACC0SrcAddr = rx_dma_lli1.src;
	DMACC0DestAddr = rx_dma_lli1.dest;
	DMACC0LLI = rx_dma_lli1.next_lli;
	DMACC0Control = rx_dma_lli1.control;
	DMACC0Config = DIO_SSP_SRC
	               | (0x2 << 11)       /* peripheral to memory */
	               | DMACCxConfig_IE   /* allow error interrupts */
	               | DMACCxConfig_ITC; /* allow terminal count interrupts */

	rx_tc = 0;
	rx_err = 0;

	active_rxbuf = &rxbuf1[0];
	idle_rxbuf = &rxbuf2[0];

	// enable channel
	DMACC0Config |= 1;
}

void dma_init_le(volatile uint8_t* dest, unsigned len) {
	int i;
	/* The first four bytes on their own, then everything after them. */
	const unsigned size[2] = { LE_DMA_HEAD, len - LE_DMA_HEAD };

	dma_clear_interrupts(0);

	for (i = 0; i < 2; ++i) {
		le_dma_lli[i].src = (uint32_t)&(DIO_SSP_DR);
		le_dma_lli[i].dest = (uint32_t)(dest + (i == 0 ? 0 : LE_DMA_HEAD));
		le_dma_lli[i].next_lli = i == 0 ? (uint32_t)&le_dma_lli[1] : 0;
		le_dma_lli[i].control = size[i] |
				(1 << 12) |        /* source burst size = 4 */
				(0 << 15) |        /* destination burst size = 1 */
				(0 << 18) |        /* source width 8 bits */
				(0 << 21) |        /* destination width 8 bits */
				DMACCxControl_DI | /* destination increment */
				/* Only the first item interrupts: it is what tells the
				 * mode loop that a packet has started and that its
				 * length byte has arrived. The rest of the packet is
				 * followed by reading DMACC0DestAddr, so an interrupt
				 * per four bytes would buy nothing. */
				(i == 0 ? DMACCxControl_I : 0);
	}

	/* Before the channel is armed, as dma_init_rx_symbols() does. Exactly
	 * one terminal count is raised per packet, and a mode loop reads it as
	 * "a packet has started", so a count left over from whatever ran before
	 * would be read as a packet that is not there -- and the buffer it would
	 * be taken from holds something else entirely. */
	rx_tc = 0;
	rx_err = 0;

	/* configure DMA channel 0 */
	DMACC0SrcAddr = le_dma_lli[0].src;
	DMACC0DestAddr = le_dma_lli[0].dest;
	DMACC0LLI = le_dma_lli[0].next_lli;
	DMACC0Control = le_dma_lli[0].control;
	DMACC0Config =
			DIO_SSP_SRC |
			(0x2 << 11) |     /* peripheral to memory */
			DMACCxConfig_IE | /* allow error interrupts */
			DMACCxConfig_ITC; /* allow terminal count interrupts */
}

/*
 * Arming receive sets one bit in each of three registers that carry other
 * settings too, so each is a read-modify-write, and three contexts reach them
 * while the low-energy engine runs: that mode's loop, its timer interrupt on
 * every hop, and its byte interrupt at the end of every packet. The timer
 * interrupt preempts the byte interrupt, so the guard is what stops a write
 * carrying a value another context has moved on from.
 *
 * The receive request is the bit that costs a capture if it goes missing: the
 * radio is strobed into receive right after this, and the receiver then clocks
 * symbols into the SSP with nothing asking the controller to collect them, so
 * no byte reaches a buffer and no interrupt is raised while the loop goes on
 * answering the host.
 *
 * The chip select pin stays outside the guard: it has its own set and clear
 * registers, so writing it reads nothing back.
 */
void dio_ssp_start()
{
	uint32_t primask;

	/* make sure the (active low) chip select signal is not active */
	DIO_SSEL_SET;

	primask = irq_save_disable();
	/* enable rx DMA on DIO_SSP */
	DIO_SSP_DMACR |= SSPDMACR_RXDMAE;
	DIO_SSP_CR1 |= SSPCR1_SSE;

	// enable channel
	DMACC0Config |= 1;
	irq_restore(primask);

	/* activate chip select pin */
	DIO_SSEL_CLR;
}

/* Stop the SSP asking the controller for bytes, leaving the SSP and the
 * channel as they are. Its own function because the low-energy engine does
 * this much on its own, wherever it stops receiving. */
void dio_ssp_rx_dma_disable(void)
{
	uint32_t primask = irq_save_disable();

	DIO_SSP_DMACR &= ~SSPDMACR_RXDMAE;
	irq_restore(primask);
}

void dio_ssp_stop()
{
	uint32_t primask;

	// disable CC2400's output (active low)
	DIO_SSEL_SET;

	// disable DMA on SSP; disable SSP ; disable DMA channel
	primask = irq_save_disable();
	DIO_SSP_DMACR &= ~SSPDMACR_RXDMAE;
	DIO_SSP_CR1 &= ~SSPCR1_SSE;
	irq_restore(primask);
	DMACC0Config = 0;
	dma_clear_interrupts(0);

	// TODO flush SSP
	/*
	while (SSP1SR & SSPSR_RNE) {
		u8 tmp = (u8)DIO_SSP_DR;
	}
	*/
}
