/*
 * Bounded waits on CC2400 synthesizer state.
 *
 * This file is part of Project Ubertooth and is released under the
 * terms of the GPL. Refer to COPYING for more information.
 */

#include "cc2400_wait.h"

int cc2400_wait_fs_unlock(void)
{
	unsigned i;

	for (i = 0; i < FS_UNLOCK_POLLS; ++i) {
		if (!(cc2400_status() & FS_LOCK))
			return 1;

		/* Re-assert SRFOFF now and then, in case the original strobe was
		 * lost to SPI bus contention with an interrupt handler, or an
		 * interrupt handler turned the synthesizer back on. */
		if ((i % FS_UNLOCK_RESTROBE_INTERVAL) == FS_UNLOCK_RESTROBE_INTERVAL - 1)
			cc2400_strobe(SRFOFF);
	}

	return 0;
}

int cc2400_wait_fs_lock(void)
{
	unsigned i;

	for (i = 0; i < FS_LOCK_POLLS; ++i) {
		if (cc2400_status() & FS_LOCK)
			return 1;

		/* Re-assert SFSON now and then, in case the original strobe was lost
		 * to SPI bus contention with an interrupt handler, or an interrupt
		 * handler turned the synthesizer back off. */
		if ((i % FS_LOCK_RESTROBE_INTERVAL) == FS_LOCK_RESTROBE_INTERVAL - 1)
			cc2400_strobe(SFSON);
	}

	return 0;
}
