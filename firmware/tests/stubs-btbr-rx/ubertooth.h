/*
 * Host-test stand-in for firmware/common/ubertooth.h, for the test that
 * compiles the BR/EDR image's receive task.
 *
 * It is the shared hardware-free stub plus the CC2400 register numbers and the
 * three pieces of hardware the receive task reads: the radio accessors, the
 * slot clock, and the packet strobe. The test supplies all of them, so a wait
 * on the slot clock can be driven byte by byte without a board.
 *
 * The slot clock is a function call rather than a variable, so the test can
 * advance it on every read: the firmware's waits only read it, and a loop that
 * polls a clock which never moves would not terminate here.
 */
#ifndef UBERTOOTH_TEST_BTBR_RX_STUB_H
#define UBERTOOTH_TEST_BTBR_RX_STUB_H

#include "../stubs/ubertooth.h"
#include "../../common/cc2400.h"

u8 cc2400_status(void);
u8 cc2400_strobe(u8 reg);
u16 cc2400_get(u8 reg);
void cc2400_set(u8 reg, u16 val);

/* TIMER0's counter, which counts the slot clock in 100ns steps. Reading it
 * moves it on, by the step the test set. */
uint32_t fake_clock_read(void);
#define T0TC fake_clock_read()

/* TIMER0's match register: where the slot clock restarts, one less than the
 * tick's length. A plain variable, so a test can shorten the tick the way
 * btphy_adj_clkn_delay() does. */
extern uint32_t fake_clock_match;
#define T0MR0 fake_clock_match

/* The CC2400's packet strobe: 0 means a sync word was detected. */
int fake_gio6(void);
#define GIO6 fake_gio6()

/* The receive task turns the LEDs on and off around a packet. */
#define RXLED_SET do { } while (0)
#define RXLED_CLR do { } while (0)
#define TXLED_SET do { } while (0)
#define TXLED_CLR do { } while (0)

#endif /* UBERTOOTH_TEST_BTBR_RX_STUB_H */
