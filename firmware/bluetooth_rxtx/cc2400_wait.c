/*
 * Bounded waits on CC2400 synthesizer state.
 *
 * This file is part of Project Ubertooth and is released under the
 * terms of the GPL. Refer to COPYING for more information.
 */

#include "cc2400_wait.h"

int cc2400_wait_fs(int locked, void (*service)(void))
{
	unsigned polls    = locked ? FS_LOCK_POLLS : FS_UNLOCK_POLLS;
	unsigned interval = locked ? FS_LOCK_RESTROBE_INTERVAL
	                           : FS_UNLOCK_RESTROBE_INTERVAL;
	u8 strobe         = locked ? SFSON : SRFOFF;
	unsigned i;

	for (i = 0; i < polls; ++i) {
		if (!!(cc2400_status() & FS_LOCK) == !!locked)
			return 1;

		if (service != NULL)
			service();

		/* Re-assert the strobe now and then, in case the original was lost
		 * to SPI bus contention with an interrupt handler, or an interrupt
		 * handler put the synthesizer back the other way. */
		if ((i % interval) == interval - 1)
			cc2400_strobe(strobe);
	}

	return 0;
}

int cc2400_wait_fs_unlock(void)
{
	return cc2400_wait_fs(0, NULL);
}

int cc2400_wait_xosc_stable(void)
{
	unsigned i;

	for (i = 0; i < XOSC_STABLE_POLLS; ++i) {
		if (cc2400_status() & XOSC16M_STABLE)
			return 1;
	}

	return 0;
}
