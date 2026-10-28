#ifndef __BTPHY_RF_H
#define __BTPHY_RF_H
#include <stdint.h>
#include "ubertooth.h"
#include "cc2400_wait.h"
#include "watchdog.h"
#include <ubtbr/btphy.h>

/* Num of bytes left in fifo when FIFO_EMPTY int trigger 64usec */
#define PHY_MIN_FIFO_BYTES 16
#define PHY_FIFO_THRESHOLD (32-PHY_MIN_FIFO_BYTES)

/* Wait for a radio state with btphy_rf_wait_fsm(state, due) in a slot task, or
 * cc2400_wait_fsm(state, NULL) outside the slot machinery. There is
 * deliberately no spin-on-FSMSTATE macro here: the tasks that wait run from
 * TIMER0_IRQHandler, and USB in this image is interrupt-driven at the same
 * priority, so an unbounded wait blocks the USB handler and the board answers
 * nothing until it is re-plugged. */

#define FS_TUNED() (cc2400_status() & FS_LOCK)

#define RF_EXPECTED_RX_CLKN_OFFSET 610	// (29usec of rf warmup + 32usec of sw)

/* No wait in a slot task may run past the end of the slot. The slot clock
 * restarts there, so a deadline beyond it would never be reached, and the task
 * has to be out of the timer interrupt by then in any case. Stop 10usec short
 * of the restart, as rx_task.c's own deadlines do, which still leaves the loop
 * time to see the deadline pass. */
#define RF_WAIT_LATEST (CLKN_OFFSET_RESET_VAL-100)

/*
 * Wait for the radio's state machine to reach `state`, until the slot clock
 * reaches `due` -- a position in the slot in 100ns steps, as tx_task.c and
 * rx_task.c write their deadlines. Returns 1 if the state was reached, 0 if
 * the deadline passed first.
 *
 * This is cc2400_wait_fsm() with the bound the slot tasks need. A poll count
 * cannot serve them: these waits run inside TIMER0_IRQHandler, which fires
 * every 312.5usec and counts one slot per interrupt, so a wait of
 * FSM_POLLS -- about 9ms -- holds the handler for around 28 slots and the
 * piconet clock loses every one of those counts. A deadline on the slot clock
 * costs at most the slot it is in, whatever the radio does, because the clock
 * passes the deadline on its own.
 *
 * The radio is polled at least once even if the deadline has already passed,
 * so a task that gets here late still sees a radio that is in the state, and
 * a passed deadline is not reported as a radio that never answered.
 *
 * A wait that gives up counts as the same shape of give-up as
 * cc2400_wait_fsm(), so ubertooth-util -b reads the two together: what the
 * count is for is whether the radio reached the states it was asked for, which
 * does not depend on which bound ended the wait.
 */
static inline int btphy_rf_wait_fsm(uint8_t state, uint16_t due)
{
	/* Says which wait this is while it runs, so a board the watchdog resets
	 * says where it was stuck. These run in an interrupt handler, where
	 * watchdog_mark() records the site without claiming it as the one the
	 * mode is in. */
	uint8_t site = watchdog_mark(WATCHDOG_SITE_FSM);

	if (due > RF_WAIT_LATEST)
		due = RF_WAIT_LATEST;

	do {
		/* FSMSTATE's low five bits are the state; the rest is
		 * breakpoint configuration. */
		if ((cc2400_get(FSMSTATE) & 0x1f) == state) {
			watchdog_mark(site);
			return 1;
		}
	} while (CLKN_OFFSET < due);

	cc2400_wait_count_timeout(CC2400_WAIT_FSM);
	watchdog_mark(site);
	return 0;
}

typedef void (*btbr_int_cb_t)(void *arg);

#define MAX_AC_ERRORS_DEFAULT	1
typedef struct {
	uint16_t freq_off_reg;
	uint16_t max_ac_errors;
	btbr_int_cb_t int_handler;
	void *int_arg;
} rf_state_t;

extern volatile rf_state_t rf_state;

void btphy_rf_init(void);
void btphy_rf_off(void);
void btphy_rf_set_freq_off(uint8_t off);
void btphy_rf_set_max_ac_errors(uint8_t max_ac_errors);
void btphy_rf_cfg_sync(uint32_t sync);
void btphy_rf_tune_chan(uint16_t channel, int tx);
void btphy_rf_fifo_write(uint8_t *data, unsigned len);

void btphy_rf_enable_int(btbr_int_cb_t cb, void*cb_arg, int tx);
void btphy_rf_disable_int(void);

static inline void btphy_rf_idle(void)
{
	cc2400_strobe(SRFOFF);
	TXLED_CLR;
	RXLED_CLR;
}

static inline void btphy_rf_tx(void)
{
	cc2400_strobe(STX);
	TXLED_SET;
}
static inline void btphy_rf_rx(void)
{
	cc2400_strobe(SRX);
	RXLED_SET;
}

/* cc2400 configure for un-buffered rx */
static inline void btphy_rf_cfg_rx(void)
{
	/* un-buffered mode, packet w/ sync word detection */
	cc2400_set(GRMDM,   0x4E1|(rf_state.max_ac_errors<<13));
	// 0 XX 00 1 001 11 0 00 0 1
	//   |  |  | |   |  +--------> CRC off
	//   |  |  | |   +-----------> sync word: 32 MSB bits of SYNC_WORD
	//   |  |  | +---------------> 1 preamble bytes of (0)1010101
	//   |  |  +-----------------> packet mode
	//   |  +--------------------> un-buffered mode // use sync word to trigger 
	//   +-----------------------> sync error bits allowed: N
	cc2400_set(IOCFG, 0x170|(GIO_PKT<<9));
}

/* cc2400 configure for buffered tx */
static inline void btphy_rf_cfg_tx(void)
{
	cc2400_set(GRMDM,   0x0CE1);
	// 0 00 01 1 001 11 0 00 0 1
	//      |  | |   |  +--------> CRC off
	//      |  | |   +-----------> sync word: 32 MSB bits of SYNC_WORD
	//      |  | +---------------> 1 preamble bytes of (0)1010101
	//      |  +-----------------> packet mode / sync word detection
	//      +--------------------> buffered mode
	cc2400_set(IOCFG, 0x170|(GIO_FIFO_EMPTY<<9));
}
#endif
