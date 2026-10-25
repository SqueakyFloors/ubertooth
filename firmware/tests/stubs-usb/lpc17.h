/*
 * A fake LPC17xx register file, for running the USB hardware layer on the host.
 *
 * firmware/common/lpcusb/target/usbhw_lpc.c reaches the USB controller only
 * through the register macros in firmware/common/lpc17.h, so replacing that
 * header with this one compiles the same code against ordinary memory. The
 * registers the waits poll are functions rather than variables, so the test
 * can decide what the controller does: answer, or stop answering.
 *
 * Only the registers usbhw_lpc.c touches are here. This file is part of
 * Project Ubertooth and is released under the terms of the GPL. Refer to
 * COPYING for more information.
 */

#ifndef __UBERTOOTH_TESTS_FAKE_LPC17_H
#define __UBERTOOTH_TESTS_FAKE_LPC17_H

#include <stdint.h>

/* ---- registers the test drives ---- */

/* Device interrupt status: how the protocol engine reports a command done. */
volatile uint32_t *fake_devintst(void);
/* Received packet length, with PKT_RDY: how a readable packet is reported. */
volatile uint32_t *fake_rxplen(void);
/* Transmit data: counts the words written, and can clear WR_EN after some. */
volatile uint32_t *fake_txdata(void);
/* Receive data: serves the bytes of the packet the test queued, a word at a
 * time, the way the controller hands them over. */
volatile uint32_t *fake_rxdata_reg(void);

#define USBDevIntSt  (*fake_devintst())
#define USBRxPLen    (*fake_rxplen())
#define USBTxData    (*fake_txdata())
#define USBRxData    (*fake_rxdata_reg())

/* ---- plain registers ---- */

extern volatile uint32_t fake_devintclr;
extern volatile uint32_t fake_devintset;
extern volatile uint32_t fake_devinten;
extern volatile uint32_t fake_devintpri;
extern volatile uint32_t fake_epintst;
extern volatile uint32_t fake_epinten;
extern volatile uint32_t fake_epintclr;
extern volatile uint32_t fake_epintpri;
extern volatile uint32_t fake_reep;
extern volatile uint32_t fake_epin;
extern volatile uint32_t fake_maxpsize;
extern volatile uint32_t fake_txplen;
extern volatile uint32_t fake_ctrl;
extern volatile uint32_t fake_cmdcode;
extern volatile uint32_t fake_cmddata;
extern volatile uint32_t fake_epdmaen;
extern volatile uint32_t fake_epdmadis;
extern volatile uint32_t fake_udcah;
extern volatile uint32_t fake_clkctrl;
extern volatile uint32_t fake_clkst;
extern volatile uint32_t fake_pconp;
extern volatile uint32_t fake_pinsel1;
extern volatile uint32_t fake_pinsel3;
extern volatile uint32_t fake_pinsel4;

#define USBDevIntClr fake_devintclr
#define USBDevIntSet fake_devintset
#define USBDevIntEn  fake_devinten
#define USBDevIntPri fake_devintpri
#define USBEpIntSt   fake_epintst
#define USBEpIntEn   fake_epinten
#define USBEpIntClr  fake_epintclr
#define USBEpIntPri  fake_epintpri
#define USBReEp      fake_reep
#define USBEpIn      fake_epin
#define USBMaxPSize  fake_maxpsize
#define USBTxPLen    fake_txplen
#define USBCtrl      fake_ctrl
#define USBCmdCode   fake_cmdcode
#define USBCmdData   fake_cmddata
#define USBEpDMAEn   fake_epdmaen
#define USBEpDMADis  fake_epdmadis
#define USBUDCAH     fake_udcah
#define USBClkCtrl   fake_clkctrl
#define USBClkSt     fake_clkst
#define PCONP        fake_pconp
#define PINSEL1      fake_pinsel1
#define PINSEL3      fake_pinsel3
#define PINSEL4      fake_pinsel4

/* ---- bit definitions, as on the part ---- */

#define USBClkCtrl_DEV_CLK_EN (0x1 << 1)
#define USBClkCtrl_AHB_CLK_EN (0x1 << 4)
#define USBClkSt_DEV_CLK_ON   (0x1 << 1)
#define USBClkSt_AHB_CLK_ON   (0x1 << 4)
#define PCONP_PCUSB           (0x1 << 31)

#endif /* __UBERTOOTH_TESTS_FAKE_LPC17_H */
