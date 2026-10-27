/*
 * A fake LPC17xx register file, for running firmware/bluetooth_rxtx/le_phy.c
 * on the host.
 *
 * The real firmware/common/lpc17.h turns each register name into a write to a
 * fixed address, which on the host is a segmentation fault. This header
 * includes the real one -- so every bit definition under test is the part's own
 * number, not a copy that could drift -- and then replaces the registers
 * le_phy.c touches with ordinary variables. A register the file never names
 * keeps the real address and is never read or written.
 *
 * The variables are what a test reads to see what the code did: which matches
 * timer1 has armed, what the interrupt controller was told, which LEDs are on.
 *
 * This file is part of Project Ubertooth and is released under the terms of
 * the GPL. Refer to COPYING for more information.
 */
#ifndef UBERTOOTH_FW_TESTS_FAKE_LPC17_LEPHY_H
#define UBERTOOTH_FW_TESTS_FAKE_LPC17_LEPHY_H

#include <stdint.h>

#include "../../common/lpc17.h"

/* timer1: the counter le_phy.c measures every deadline against (NOW is T1TC),
 * its prescaler and control register, the four match registers, the match
 * control register that arms them and the interrupt register that reports
 * them. */
extern volatile uint32_t fake_t1ir;
extern volatile uint32_t fake_t1tcr;
extern volatile uint32_t fake_t1tc;
extern volatile uint32_t fake_t1pr;
extern volatile uint32_t fake_t1mcr;
extern volatile uint32_t fake_t1mr0;
extern volatile uint32_t fake_t1mr1;
extern volatile uint32_t fake_t1mr2;
extern volatile uint32_t fake_t1mr3;

#undef T1IR
#undef T1TCR
#undef T1TC
#undef T1PR
#undef T1MCR
#undef T1MR0
#undef T1MR1
#undef T1MR2
#undef T1MR3
#define T1IR  fake_t1ir
#define T1TCR fake_t1tcr
#define T1TC  fake_t1tc
#define T1PR  fake_t1pr
#define T1MCR fake_t1mcr
#define T1MR0 fake_t1mr0
#define T1MR1 fake_t1mr1
#define T1MR2 fake_t1mr2
#define T1MR3 fake_t1mr3

/* The interrupt controller: set-enable, clear-enable and clear-pending. A
 * write is a bitmask of the lines it affects, so a test reads which bits
 * arrived. */
extern volatile uint32_t fake_iser0;
extern volatile uint32_t fake_icer0;
extern volatile uint32_t fake_icpr0;

#undef ISER0
#undef ICER0
#undef ICPR0
#define ISER0 fake_iser0
#define ICER0 fake_icer0
#define ICPR0 fake_icpr0

/* DMA channel 0 and the controller's interrupt registers. The status
 * registers are what le_DMA_IRQHandler() reads to decide a byte has
 * arrived, so a test sets them. */
extern volatile uint32_t fake_dmac_int_stat;
extern volatile uint32_t fake_dmac_int_tc_stat;
extern volatile uint32_t fake_dmac_int_tc_clear;
extern volatile uint32_t fake_dmac_int_err_stat;
extern volatile uint32_t fake_dmac_int_err_clr;
extern volatile uint32_t fake_dmacc0_src;
extern volatile uint32_t fake_dmacc0_dest;
extern volatile uint32_t fake_dmacc0_lli;
extern volatile uint32_t fake_dmacc0_control;
extern volatile uint32_t fake_dmacc0_config;

#undef DMACIntStat
#undef DMACIntTCStat
#undef DMACIntTCClear
#undef DMACIntErrStat
#undef DMACIntErrClr
#undef DMACC0SrcAddr
#undef DMACC0DestAddr
#undef DMACC0LLI
#undef DMACC0Control
#undef DMACC0Config
#define DMACIntStat    fake_dmac_int_stat
#define DMACIntTCStat  fake_dmac_int_tc_stat
#define DMACIntTCClear fake_dmac_int_tc_clear
#define DMACIntErrStat fake_dmac_int_err_stat
#define DMACIntErrClr  fake_dmac_int_err_clr
#define DMACC0SrcAddr  fake_dmacc0_src
#define DMACC0DestAddr fake_dmacc0_dest
#define DMACC0LLI      fake_dmacc0_lli
#define DMACC0Control  fake_dmacc0_control
#define DMACC0Config   fake_dmacc0_config

/* The SSP the radio's bytes arrive on: data register, status register (bit
 * RNE says a byte is waiting) and the DMA control register. le_phy.c takes
 * the data register's address to point DMA at it, so it has to be a real
 * object. The status register stays 0 here, which is an empty receive FIFO:
 * no test pretends bytes arrived from a radio that is not there. */
extern volatile uint32_t fake_ssp1dr;
extern volatile uint32_t fake_ssp1sr;
extern volatile uint32_t fake_ssp1dmacr;

#undef SSP1DR
#undef SSP1SR
#undef SSP1DMACR
#define SSP1DR    fake_ssp1dr
#define SSP1SR    fake_ssp1sr
#define SSP1DMACR fake_ssp1dmacr

/* The GPIO ports behind the LED macros and, on an Ubertooth One, the
 * amplifier's enable and gain pins. le_phy.c only writes them: a pin mask
 * written to SET turns those pins on and one written to CLR turns them off,
 * so the last mask written to each is what a test reads. */
extern volatile uint32_t fake_fio1pin;
extern volatile uint32_t fake_fio1set;
extern volatile uint32_t fake_fio1clr;
extern volatile uint32_t fake_fio2pin;
extern volatile uint32_t fake_fio2set;
extern volatile uint32_t fake_fio2clr;

#undef FIO1PIN
#undef FIO1SET
#undef FIO1CLR
#undef FIO2PIN
#undef FIO2SET
#undef FIO2CLR
#define FIO1PIN fake_fio1pin
#define FIO1SET fake_fio1set
#define FIO1CLR fake_fio1clr
#define FIO2PIN fake_fio2pin
#define FIO2SET fake_fio2set
#define FIO2CLR fake_fio2clr

#endif /* UBERTOOTH_FW_TESTS_FAKE_LPC17_LEPHY_H */
