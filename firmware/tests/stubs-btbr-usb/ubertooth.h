/*
 * Host-test stand-in for firmware/common/ubertooth.h, for the test that
 * compiles the btbr image's USB shim.
 *
 * It is the shared hardware-free stub plus a fake interrupt controller. The
 * shim holds the USB interrupt off through NVIC ISER0 and ICER0, so those two
 * registers have to behave as they do on the part -- ISER0 is write-1-to-set
 * and reads back the enable mask, ICER0 is write-1-to-clear -- or a test
 * cannot tell whether the firmware put the interrupt back. The test supplies
 * the two accessors below.
 */
#ifndef UBERTOOTH_TEST_BTBR_USB_STUB_H
#define UBERTOOTH_TEST_BTBR_USB_STUB_H

#include "../stubs/ubertooth.h"

/* Interrupt set-enable: a read gives the enable mask, a write sets bits. */
volatile uint32_t *fake_nvic_iser(void);
/* Interrupt clear-enable: a write clears bits from the enable mask. */
volatile uint32_t *fake_nvic_icer(void);

#define ISER0 (*fake_nvic_iser())
#define ICER0 (*fake_nvic_icer())

#define ISER0_ISE_USB (0x1 << 24)
#define ICER0_ICE_USB (0x1 << 24)

/* The shim reads the part's unique serial to build the serial descriptor. */
void get_device_serial(uint8_t *buffer, int *len);

#endif /* UBERTOOTH_TEST_BTBR_USB_STUB_H */
