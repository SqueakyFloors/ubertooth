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
 * source tree", and the decision of whether that stops the command. It lives
 * in its own file, away from the device code, so a test of any caller can
 * compile the real message instead of a copy of it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ubertooth.h"

/* The build normally passes the library's version in; ubertooth.c keeps the
 * same fallback, so a build that does not still compiles. */
#ifndef VERSION
#define VERSION "unknown"
#endif

/* Set by the build: 1 for a release build, 0 for a build from a working
 * checkout. A build that does not set it is treated as a release, so the
 * stricter behaviour is the one nobody has to remember to ask for. */
#ifndef UBERTOOTH_RELEASE_BUILD
#define UBERTOOTH_RELEASE_BUILD 1
#endif

#define ALLOW_VAR UBERTOOTH_ALLOW_API_MISMATCH_VAR

/* The firmware API version of the open board, and which board it came from. A
 * request a board refuses is reported with both versions beside each other,
 * because firmware from another commit is the usual reason, and the usual fix.
 *
 * The owner is kept so that this cannot become another board's version: it is
 * dropped when that board is released, so a process that works through
 * several boards in turn -- which is what ubertooth-util's listing does --
 * never quotes the one before. Before any board's version has been read there
 * is nothing to quote, and the report gives only its own half.
 *
 * One board at a time, which is all any tool here opens: a process holding two
 * at once would have the second board's version quoted for the first. The one
 * slot is also why no lock is needed around it, even though the reports are
 * written from the capture's poll thread as well: a board's version is
 * recorded while it is being opened and dropped while it is being released,
 * and the poll thread exists only between those two. */
static const void* board_api_version_owner;
static uint16_t board_api_version;

/* The two halves of an API version as the tools print it: 0x0119 is 1.19.
 * The firmware reports it as its USB bcdDevice, which is binary-coded
 * decimal, so each byte is read as its own decimal number. */
#define API_MAJOR(v) (((v) >> 8) & 0xFF)
#define API_MINOR(v) ((v) & 0xFF)

void ubertooth_note_api_version(const void* board, uint16_t version)
{
	board_api_version_owner = board;
	board_api_version = version;
}

void ubertooth_forget_api_version(const void* board)
{
	if (board_api_version_owner == board)
		board_api_version_owner = NULL;
}

/* Whether a mismatch may be run through rather than refused, and, where it
 * may, the phrase that says what allowed it. The environment variable decides
 * when it is set to exactly "1" (run on) or "0" (refuse), so either behaviour
 * can be asked for whatever the build is -- the override a release build
 * needs for debugging, and the strictness a test harness wants from a build
 * made out of a checkout. Anything else, including unset and empty, leaves
 * the build's own default. */
static int mismatch_allowed(const char** why)
{
	const char* setting = getenv(ALLOW_VAR);

	*why = NULL;
	if (setting != NULL && strcmp(setting, "1") == 0) {
		*why = ALLOW_VAR " is set";
		return 1;
	}
	if (setting != NULL && strcmp(setting, "0") == 0)
		return 0;
	if (UBERTOOTH_RELEASE_BUILD)
		return 0;
	*why = "this is a development build";
	return 1;
}

int ubertooth_report_api_mismatch(uint16_t version, const char* selector)
{
	const char* why = NULL;
	int allowed = mismatch_allowed(&why);

	fprintf(stderr, "%s: Ubertooth firmware API version %x.%02x found, libubertooth %s requires %x.%02x.\n",
	        allowed ? "warning" : "error",
	        API_MAJOR(version), API_MINOR(version), VERSION,
	        API_MAJOR(UBERTOOTH_API_VERSION), API_MINOR(UBERTOOTH_API_VERSION));
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

	if (!allowed)
		return -1;

	/* Why the command is not being refused, and what that costs: a record
	 * whose layout changed between the two versions is decoded by the wrong
	 * rules, which is wrong data rather than a failure, and a request this
	 * firmware has not got comes back as a refusal further down. */
	fprintf(stderr, "note: %s, so the command runs on; what it reports may "
	        "be wrong, and a request this firmware has not got will fail\n",
	        why);
	return 0;
}

void ubertooth_report_unsupported_request(const char* what, int code)
{
	/* A board that has left the USB bus can stall a request on its way out,
	 * and then this is not about firmware at all: the board whose version
	 * the note would quote is not there any more. Saying it is too old
	 * sends the user to reflash a board that was never the problem, so the
	 * line says what really happened and stops there. Asked of the board
	 * the version was read from, which is the board these requests go to:
	 * only one is open at a time. The bus is listed rather than the board
	 * asked anything, so a board that is merely silent is not disturbed. */
	if (board_api_version_owner != NULL
	    && ubertooth_device_gone(board_api_version_owner)) {
		fprintf(stderr, "error: could not %s: the board is no longer "
		        "attached\n", what);
		return;
	}

	/* A board refuses a request it does not implement by stalling it, and
	 * the stall is all the host sees: no reason comes back. So the line
	 * names the request, and the note puts the board's firmware API version
	 * next to the one these tools were built for -- equal versions say the
	 * request is simply not in this firmware, different ones say which
	 * reflash fixes it. */
	if (code != 0)
		fprintf(stderr, "error: could not %s: the board's firmware does "
		        "not support it (stalled, libusb %d)\n", what, code);
	else
		fprintf(stderr, "error: could not %s: the board's firmware does "
		        "not support it (stalled)\n", what);

	if (board_api_version_owner == NULL)
		return;

	fprintf(stderr, "note: the board reports firmware API version "
	        "%x.%02x, libubertooth %s requires %x.%02x\n",
	        API_MAJOR(board_api_version), API_MINOR(board_api_version),
	        VERSION, API_MAJOR(UBERTOOTH_API_VERSION),
	        API_MINOR(UBERTOOTH_API_VERSION));

	/* The other reason a request stalls, which the note above cannot be told
	 * apart from: a wedged board stalls everything sent to it, whatever its
	 * firmware implements, so a report that names the firmware alone sends
	 * the user to reflash a board that is not short of the request at all.
	 * The question that does separate the two is whether any request gets
	 * through, and ubertooth-util -r is the recovery for the wedge: it
	 * resets the board's USB connection and then the board, with nobody at
	 * the bench. "On the same board" rather than "with the same -U": the
	 * board this is about is the one the command was working on, which a
	 * run with one board attached named with no -U at all.
	 *
	 * Under the same condition as the note above, which is what says an
	 * application firmware answered: a board in the DFU bootloader runs
	 * none, and ubertooth-util cannot reach one. */
	fprintf(stderr, "note: a board that stalls every request is wedged "
	        "rather than short of this one; reset it with ubertooth-util -r "
	        "on the same board\n");
}
