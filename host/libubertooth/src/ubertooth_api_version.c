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

/* The build normally passes the library's version in; ubertooth.c keeps the
 * same fallback, so a build that does not still compiles. */
#ifndef VERSION
#define VERSION "unknown"
#endif

void ubertooth_report_api_mismatch(uint16_t version, const char* selector)
{
	fprintf(stderr, "error: Ubertooth firmware API version %x.%02x found, libubertooth %s requires %x.%02x.\n",
	        (version >> 8) & 0xFF, version & 0xFF, VERSION,
	        (UBERTOOTH_API_VERSION >> 8) & 0xFF, UBERTOOTH_API_VERSION & 0xFF);
	/* Whichever side is behind is the one to replace. */
	if (version < UBERTOOTH_API_VERSION) {
		/* The example reflashes the board this message is about and no
		 * other: install.sh takes the same index or serial after
		 * --flash-device as the tool took after -U. Where the user
		 * named no board, --flash takes the one that is attached and
		 * refuses when there is more than one, so neither form ever
		 * reflashes a board nobody asked about. */
		if (selector != NULL && selector[0] != '\0')
			fprintf(stderr, "note: flash the firmware built from the "
			        "same source tree, for example: ./install.sh "
			        "--flash-device %s\n", selector);
		else
			fprintf(stderr, "note: flash the firmware built from the "
			        "same source tree, for example: ./install.sh "
			        "--flash\n");
	} else {
		fprintf(stderr, "note: install the host tools built from the "
		        "same source tree as the firmware\n");
	}
}
