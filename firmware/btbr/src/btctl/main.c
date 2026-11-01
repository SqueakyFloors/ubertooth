/* Ubtbr main
 *
 * Copyright 2020 Etienne Helluy-Lafont, Univ. Lille, CNRS.
 *
 * This file is part of Project Ubertooth.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */
#include <ubertooth.h>
#include <ubtbr/cfg.h>
#include <ubtbr/btctl_intf.h>
#include <ubtbr/vendor_request_handler.h>
#include <ubtbr/btphy.h>
#include <watchdog.h>
#include <bl_report.h>
#include <ubertooth_interface.h>

int main()
{
	unsigned clk, clk_prev = 0;

	// enable all fault handlers (see fault.c)
	SCB_SHCSR = (1 << 18) | (1 << 17) | (1 << 16);

	watchdog_boot();
	/* This image runs its own state machine rather than the mode the other
	 * image's breadcrumb records, so it says so instead of claiming to be
	 * idle. */
	watchdog_mode(MODE_UNKNOWN);

	ubertooth_init();
	/* Before the USB stack, which reads the serial number for its own
	 * descriptor, and well before this image is running a connection: the
	 * boot-ROM calls this makes must not happen then. */
	device_id_init();
	/* Here for the same reason, and before the first request can arrive:
	 * hashing the bootloader region takes milliseconds, which is too long
	 * to spend in a request handler answered from the USB interrupt. */
	bl_report_init(UBERTOOTH_BOOTLOADER_REGION_START);
	ubertooth_usb_init(vendor_request_handler);

	/* Initialize memory */
	mem_pool_init();

	/* Initialize control interface */
	btctl_init();

	/* Initialize sync & phy */
	btphy_init();

	/* Last, as in the other image: from here on a board that goes the
	 * watchdog timeout without a USB service pass resets itself. */
	watchdog_start();

	while (1) {
		/* This image services USB from the USB interrupt, which it leaves
		 * enabled for as long as it runs, so there is no service pass of its
		 * own to feed the watchdog from when the host is quiet. Reaching the
		 * top of this loop is the feed instead: it runs below every
		 * interrupt handler, so a request arriving now would be answered.
		 * See watchdog_usb_ready() in firmware/common/watchdog.h. */
		watchdog_usb_ready();

		/* Flush console once per second. Elapsed ticks, not an absolute
		 * deadline: clk_prev is always a reading already taken, and the
		 * difference carries across the free-running counter's wrap,
		 * where an addition would stop the flush for a whole period. */
		if ((clk = CENTRAL_CLKN) - clk_prev >= CLKN_RATE)
		{
			console_flush();
			clk_prev = clk;
		}
		/* Handle usb async work */
		usb_work();
		/* Handle host messages */
		btctl_work();
	}
}
