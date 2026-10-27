/*
 * Copyright 2026 Great Scott Gadgets
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

/*
 * The host's one wording for "this board is not running firmware from this
 * source tree". It lives in its own file, away from the device code, so a test
 * of either caller can compile the real message instead of a copy of it.
 */

#include <stdio.h>

#include "ubertooth.h"

void ubertooth_report_api_mismatch(uint16_t version)
{
	fprintf(stderr, "error: Ubertooth firmware API version %x.%02x found, libubertooth %s requires %x.%02x.\n",
	        (version >> 8) & 0xFF, version & 0xFF, VERSION,
	        (UBERTOOTH_API_VERSION >> 8) & 0xFF, UBERTOOTH_API_VERSION & 0xFF);
	/* Whichever side is behind is the one to replace. */
	if (version < UBERTOOTH_API_VERSION)
		fprintf(stderr, "note: flash the firmware built from the same "
		        "source tree, for example: ./install.sh "
		        "--flash-device all\n");
	else
		fprintf(stderr, "note: install the host tools built from the "
		        "same source tree as the firmware\n");
}
