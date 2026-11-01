/*
 * Host-test stand-in for firmware/common/ubertooth.h, for watchdog.c.
 *
 * stubs/ubertooth.h, plus the registers watchdog.c drives: the watchdog's own
 * four, the reset source register, and the NVIC pair behind the USB interrupt
 * hold the feed takes. They are plain variables here, so a test can set what
 * the part came up with and read back what the code wrote -- including the
 * feed register, which the test watches as a sequence rather than a value.
 */
#include "../stubs/ubertooth.h"

#ifndef UBERTOOTH_FW_TESTS_STUB_WATCHDOG_UBERTOOTH_H
#define UBERTOOTH_FW_TESTS_STUB_WATCHDOG_UBERTOOTH_H

extern uint32_t fake_wdmod;
extern uint32_t fake_wdtc;
extern uint32_t fake_wdclksel;
extern uint32_t fake_rsid;
extern uint32_t fake_iser0;
extern uint32_t fake_icer0;

/* The interrupt control and state register, whose low nine bits say which
 * exception the processor is in. A test sets it to put the code under test in
 * an interrupt handler. */
extern uint32_t fake_scb_icsr;

/* Every write to the feed register, in order: the pair 0xAA, 0x55 is what the
 * part accepts, and the test checks that is what it gets. */
#define FAKE_WDFEED_MAX 64
extern uint32_t fake_wdfeed_writes[FAKE_WDFEED_MAX];
extern unsigned fake_wdfeed_count;
void fake_wdfeed_write(uint32_t value);

#define WDMOD    fake_wdmod
#define WDTC     fake_wdtc
#define WDCLKSEL fake_wdclksel
#define RSID     fake_rsid
#define ISER0    fake_iser0
#define ICER0    fake_icer0
#define SCB_ICSR fake_scb_icsr

#define WDFEED_SEQUENCE fake_wdfeed_write(0xAA); fake_wdfeed_write(0x55)

#define WDMOD_WDEN    (0x1u << 0)
#define WDMOD_WDRESET (0x1u << 1)
#define WDMOD_WDTOF   (0x1u << 2)
#define WDMOD_WDINT   (0x1u << 3)

#define RSID_POR  (0x1u << 0)
#define RSID_EXTR (0x1u << 1)
#define RSID_WDTR (0x1u << 2)
#define RSID_BODR (0x1u << 3)

#define ISER0_ISE_USB (0x1u << 24)
#define ICER0_ICE_USB (0x1u << 24)

#endif /* UBERTOOTH_FW_TESTS_STUB_WATCHDOG_UBERTOOTH_H */
