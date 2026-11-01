/*
 * The three calls btctl.c makes into the board itself.
 *
 * On the board they are declared by firmware/common/ubertooth.h and by
 * ubtbr/debug.h, which pulls in ubtbr/ubertooth_usb.h. This test replaces the
 * second of those with stubs-btbr/ubtbr/debug.h, and compiles the first for
 * the host, so neither declaration arrives. None of them is reached by a
 * transmit request; the test defines all three, two of them as failures.
 */
#ifndef UBERTOOTH_FW_TESTS_BTBR_TX_BOARD_CALLS_H
#define UBERTOOTH_FW_TESTS_BTBR_TX_BOARD_CALLS_H

int usb_work(void);
void reset(void);
void set_isp(void);

#endif /* UBERTOOTH_FW_TESTS_BTBR_TX_BOARD_CALLS_H */
