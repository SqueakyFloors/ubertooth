/*
 * Bounded waits on CC2400 radio state.
 *
 * This file is part of Project Ubertooth and is released under the
 * terms of the GPL. Refer to COPYING for more information.
 */

#include "cc2400_wait.h"
#include "watchdog.h"

/*
 * How many times each shape of wait has given up since the board started.
 *
 * Read with UBERTOOTH_GET_RADIO_TIMEOUTS, so that comparing a run against a
 * known-good one says whether the radio reached the states it was asked for,
 * which carrying on quietly otherwise hides.
 *
 * volatile because these waits run from interrupt handlers as well as from the
 * mode loops. A count can still be lost that way: the update below is a read
 * and a write, and a handler whose own wait times out in between is overwritten
 * by the write. That is left as it is -- there is no interrupt guard shared by
 * both images here, and a diagnostic counter that is occasionally one short
 * still answers the question it is for.
 */
static volatile uint16_t timeouts[CC2400_WAIT_SHAPE_COUNT];

static void wait_timed_out(enum cc2400_wait_shape shape)
{
	/* One read, one write, and the write is at most one above what the read
	 * saw, so an interrupt in between can cost a count but can never carry
	 * the counter past its limit. Testing the array again instead of this
	 * local would: a handler that reached the limit between the test and the
	 * increment would leave the increment to wrap the counter to zero, and a
	 * board that had been giving up for hours would read as a healthy one. */
	uint16_t count = timeouts[shape];

	if (count != CC2400_WAIT_TIMEOUTS_MAX)
		timeouts[shape] = count + 1;
}

void cc2400_wait_report(uint8_t *data)
{
	unsigned i;

	for (i = 0; i < CC2400_WAIT_SHAPE_COUNT; ++i) {
		uint16_t count = timeouts[i];

		data[2 * i]     = count & 0xff;
		data[2 * i + 1] = (count >> 8) & 0xff;
	}
}

int cc2400_wait_fs(int locked, void (*service)(void))
{
	unsigned polls    = locked ? FS_LOCK_POLLS : FS_UNLOCK_POLLS;
	unsigned interval = locked ? FS_LOCK_RESTROBE_INTERVAL
	                           : FS_UNLOCK_RESTROBE_INTERVAL;
	u8 strobe         = locked ? SFSON : SRFOFF;
	/* Say which wait this is while it runs, so a board the watchdog resets
	 * says where it was stuck and not only which mode it was in. The site
	 * that was recorded before is put back on the way out: these waits nest
	 * inside a mode that may itself have recorded one. */
	uint8_t site = watchdog_mark(locked ? WATCHDOG_SITE_FS_LOCK
	                                    : WATCHDOG_SITE_FS_UNLOCK);
	unsigned i;

	for (i = 0; i < polls; ++i) {
		if (!!(cc2400_status() & FS_LOCK) == !!locked) {
			watchdog_mark(site);
			return 1;
		}

		if (service != NULL)
			service();

		/* Re-assert the strobe now and then, in case an interrupt handler
		 * put the synthesizer back the other way after the first strobe. */
		if ((i % interval) == interval - 1)
			cc2400_strobe(strobe);
	}

	wait_timed_out(locked ? CC2400_WAIT_FS_LOCK : CC2400_WAIT_FS_UNLOCK);
	watchdog_mark(site);
	return 0;
}

int cc2400_wait_fs_unlock(void)
{
	return cc2400_wait_fs(0, NULL);
}

int cc2400_wait_xosc_stable(void (*service)(void))
{
	uint8_t site = watchdog_mark(WATCHDOG_SITE_XOSC);
	unsigned i;

	for (i = 0; i < XOSC_STABLE_POLLS; ++i) {
		if (cc2400_status() & XOSC16M_STABLE) {
			watchdog_mark(site);
			return 1;
		}

		if (service != NULL)
			service();
	}

	wait_timed_out(CC2400_WAIT_XOSC);
	watchdog_mark(site);
	return 0;
}

int cc2400_wait_fsm(u8 state, void (*service)(void))
{
	uint8_t site = watchdog_mark(WATCHDOG_SITE_FSM);
	unsigned i;

	for (i = 0; i < FSM_POLLS; ++i) {
		/* FSMSTATE's low five bits are the state; the rest is breakpoint
		 * configuration. */
		if ((cc2400_get(FSMSTATE) & 0x1f) == state) {
			watchdog_mark(site);
			return 1;
		}

		if (service != NULL)
			service();
	}

	wait_timed_out(CC2400_WAIT_FSM);
	watchdog_mark(site);
	return 0;
}
