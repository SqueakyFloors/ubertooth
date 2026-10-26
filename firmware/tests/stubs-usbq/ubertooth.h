/*
 * Host-test stand-in for firmware/common/ubertooth.h, for ubertooth_usb.c.
 *
 * stubs/ubertooth.h, plus the two things ubertooth_usb.c and its header reach
 * for that only exist on the board: the serial number, which comes from the
 * LPC17xx's IAP, and the NVIC registers behind the USB interrupt hold. The
 * registers are plain variables here, so a test can set the interrupt enabled
 * or disabled and see what the code under test did with it.
 */
#include "../stubs/ubertooth.h"

#ifndef UBERTOOTH_FW_TESTS_STUB_USBQ_UBERTOOTH_H
#define UBERTOOTH_FW_TESTS_STUB_USBQ_UBERTOOTH_H

void get_device_serial(uint8_t *buffer, int *len);

extern uint32_t fake_iser0;
extern uint32_t fake_icer0;

#define ISER0 fake_iser0
#define ICER0 fake_icer0

#define ISER0_ISE_USB (0x1u << 24)
#define ICER0_ICE_USB (0x1u << 24)

#endif /* UBERTOOTH_FW_TESTS_STUB_USBQ_UBERTOOTH_H */
