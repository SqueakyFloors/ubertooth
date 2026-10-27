/*
 * Bounded waits on CC2400 radio state.
 *
 * This file is part of Project Ubertooth and is released under the
 * terms of the GPL. Refer to COPYING for more information.
 */

#include "cc2400_wait.h"
#include "watchdog.h"

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

	watchdog_mark(site);
	return 0;
}
