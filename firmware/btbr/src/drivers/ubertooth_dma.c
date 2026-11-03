/*
 * Copyright 2015 Hannes Ellinger
 * Copyright 2020 Etienne Helluy-Lafont, Univ. Lille, CNRS.
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

#include <ubertooth.h>
#include <ubtbr/cfg.h>
#include <ubtbr/ubertooth_dma.h>
#include <ubtbr/debug.h>

volatile uint8_t *dma_user_rx_buf = (void*)0;

void DMA_IRQHandler(void)
{
	cprintf ("DMAI %x\n", DMACIntStat);
	DMACIntTCClear = 0xFF;
	DMACIntErrClr = 0xFF;
}

void dma_poweron(void) {
	// refer to UM10360 LPC17xx User Manual Ch 31 Sec 31.6.1, PDF page 616
	PCONP |= PCONP_PCGPDMA;

	// enable DMA interrupts at lowest priority
	//IPR6 |= IPR6_IP_DMA; // hack, sets it to 31 (lowest)
	IPR6 |= 31<<19; // pri 
	ISER0 = ISER0_ISE_DMA;

	/* Disable DMA interrupt */
	//ICER0 = ICER0_ICE_DMA;

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

void dma_clear_interrupts(unsigned channel) {
	DMACIntTCClear = 1 << channel;
	DMACIntErrClr  = 1 << channel;
}

void dio_ssp_stop(void)
{
	// disable CC2400's output (active low)
	DIO_SSEL_SET;

	// disable DMA on SSP; disable SSP ; disable DMA channel
	DIO_SSP_CR1 &= ~SSPCR1_SSE;
	DIO_SSP_DMACR = 0;
	DMACC0Config = 0;

	dma_clear_interrupts(0);
	dma_clear_interrupts(1);

	// flush SSP
	while (SSP1SR & SSPSR_RNE) {
		cprintf("ssp ne\n");
		volatile uint8_t tmp = (uint8_t)DIO_SSP_DR;
	}
}

void dio_ssp_start_rx(void)
{
	/* make sure the (active low) chip select signal is not active */
	DIO_SSEL_SET;
	//SCLK_SET_IN;

	/* Configure RX DMA on DIO_SSP (SSP1 for Ubertooth One) */
	DIO_SSP_DMACR = SSPDMACR_RXDMAE ;
	/* Configuration of DMA : SSPCR1_MS, SSPCR1_SOD
		DIO_SSP_CR1 = SSPCR1_MS | SSPCR1_SOD;
	DIO_SSP_CR1 = SSPCR1_MS | SSPCR1_SOD;
	 */
	DIO_SSP_CR1 = SSPCR1_MS | SSPCR1_SOD;

	/* Enable ssp */
	DIO_SSP_CR1 |= SSPCR1_SSE;

	// enable channel
	DMACC0Config |= 1;

	/* activate chip select pin */
	DIO_SSEL_CLR;
}

void dma_init_rx_single(volatile void *buf, unsigned buf_size)
{
	dma_user_rx_buf = buf;

	/* configure DMA channel 0: Rx single slot */
	DMACC0SrcAddr	= (uint32_t)&(DIO_SSP_DR);
	DMACC0DestAddr	= (uint32_t)buf;
	DMACC0LLI	= 0;
	// FIXME
	//DMACC0Control	= 4 |
	DMACC0Control	= buf_size |
			(1 << 12) |        /* source burst size = 1 */
			(0 << 15) |        /* destination burst size = 1 */
			(0 << 18) |        /* source width 8 bits */
			(0 << 21) |        /* destination width 8 bits */
			DMACCxControl_DI  /* destination increment */
			;

	DMACC0Config	= DIO_SSP_SRC	/* Src periph (3 for Ubertooth One) */
			| (0x2 << 11)	/* peripheral to memory */
			;
}

unsigned long dma_get_rx_offset(void)
{
	return (unsigned long)DMACC0DestAddr - (unsigned long)dma_user_rx_buf;
}

