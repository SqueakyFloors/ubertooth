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
 * The answer every tool gives for -V: the host tools' version, then the
 * firmware version and build of the attached board. One place, so -V reads
 * the same whichever tool was asked, and a script can grep one line out of it
 * whatever the tool.
 *
 * It lives in its own file, away from the device code, so a test of any
 * caller can compile the real answer instead of a copy of it.
 */

#include <stdio.h>
#include <string.h>

#include "ubertooth.h"
#include "ubertooth_interface.h"

/* "Firmware version: ", or "Firmware version (0): " where several boards are
 * reported: a line read on its own then says both what it is and which board
 * it came from, and says it the same way whether one board or several were
 * read. index below 0 is the single-board form. */
static void print_label(FILE *out, const char *label, int index)
{
	if (index < 0)
		fprintf(out, "%s: ", label);
	else
		fprintf(out, "%s (%d): ", label, index);
}

int ubertooth_firmware_version_text(ubertooth_t *ut, char *text, size_t size)
{
	struct ubertooth_fw_info info;
	char revision[UBERTOOTH_VERSION_TEXT_LEN];
	uint16_t api_version = 0;
	size_t used;
	int r;

	if (text == NULL || size == 0)
		return 1;
	text[0] = '\0';
	/*
	 * The image and the revision come from the firmware info block, which is
	 * also what ubertooth-dfu reads out of a firmware file, so a board and
	 * the file it was flashed from are named the same way.
	 *
	 * Firmware older than the block stalls the request. This is the one
	 * report that has to work on any firmware a board might be running, so
	 * the revision alone is read instead, which is what this line always
	 * carried.
	 */
	r = cmd_get_firmware_info(ut->devh, &info);
	if (r == 0)
		snprintf(text, size, "%s %s", info.image, info.revision);
	else if (r == LIBUSB_ERROR_PIPE) {
		revision[0] = '\0';
		if (cmd_get_firmware_revision(ut->devh, revision,
		                              sizeof(revision)) < 0)
			return 1;
		revision[sizeof(revision) - 1] = '\0';
		snprintf(text, size, "%s", revision);
	} else {
		/* The library has already reported the transfer error. */
		return 1;
	}
	/*
	 * The API version comes from neither: it is the USB bcdDevice, which is
	 * the number the other tools check a board against. The block carries
	 * its own copy, and the two can differ -- firmware older than this tree
	 * reported a fixed bcdDevice for the btbr image, whatever the block
	 * said -- so showing the descriptor's is what explains why the other
	 * tools accept or refuse a board.
	 */
	used = strlen(text);
	if (ubertooth_get_api(ut, &api_version) < 0)
		return 1;
	/* A revision long enough to fill the buffer would leave no room for the
	 * API version. The line then carries what was read, and the caller is
	 * told part of it is missing rather than being handed a version that
	 * looks whole. */
	if (snprintf(text + used, size - used, " (API:%x.%02x)",
	             (api_version >> 8) & 0xFF, api_version & 0xFF)
	    >= (int)(size - used))
		return 1;
	return 0;
}

/*
 * One board's two lines of -V. A read that failed says so on the line itself,
 * because -V always answers: a tool asked for the version and got none is
 * what the line has to say, and the library has put the reason on standard
 * error already.
 *
 * Returns 0, or 1 if anything could not be read.
 */
static int print_board_versions(FILE *out, ubertooth_t *ut, int index)
{
	char text[UBERTOOTH_VERSION_TEXT_LEN];
	int status;

	status = ubertooth_firmware_version_text(ut, text, sizeof(text));
	print_label(out, UBERTOOTH_VERSION_LABEL_FIRMWARE, index);
	if (text[0] == '\0') {
		/* Nothing at all was read, so the board is not answering: the build
		 * string would fail the same way, and saying it twice says nothing
		 * more. */
		fprintf(out, "read failed\n");
		return 1;
	}
	fprintf(out, "%s\n", text);

	text[0] = '\0';
	if (cmd_get_firmware_compile_info(ut->devh, text, sizeof(text)) < 0)
		status = 1;
	print_label(out, UBERTOOTH_VERSION_LABEL_BUILD, index);
	if (text[0] == '\0') {
		/* Nothing to show, whether the read failed or the board answered an
		 * empty string: the line and the status have to agree, or a script
		 * reading one of them is misled by the other. */
		fprintf(out, "read failed\n");
		return 1;
	}
	fprintf(out, "%s\n", text);
	return status;
}

/* The board a tool chose could not be opened: why, on the line the version
 * would have been on. The open paths have already put the matching "error: "
 * line on standard error. */
static void print_unavailable(FILE *out, int index)
{
	print_label(out, UBERTOOTH_VERSION_LABEL_FIRMWARE, index);
	fprintf(out, "unavailable, %s\n", ubertooth_open_failure_reason());
}

int ubertooth_print_versions(FILE *out, const char *selector)
{
	ubertooth_t *ut;
	int count, status = 0, i;

	ubertooth_print_host_version(out);
	/*
	 * The test switch is read here rather than left to ubertooth_count(),
	 * which announces it: that note belongs to a command that wanted a
	 * board to work on, and -V only reports what is attached. The line
	 * below is the whole answer either way.
	 */
	count = ubertooth_devices_hidden() ? 0 : ubertooth_count();
	if (count < 0) {
		/* The bus could not be listed, so nothing is known about what is
		 * attached. libusb's own error is already on standard error. */
		print_label(out, UBERTOOTH_VERSION_LABEL_FIRMWARE, -1);
		fprintf(out, "unavailable, the USB bus could not be listed\n");
		return 1;
	}
	/* Nothing attached is an answer, not a failure: -V is the one command
	 * that has to work with no board on the bus, because it is how a user
	 * reports which host tools they are running. */
	if (count == 0) {
		print_label(out, UBERTOOTH_VERSION_LABEL_FIRMWARE, -1);
		fprintf(out, "no Ubertooth attached\n");
		/* A board in the bootloader is not counted, and answers nothing, so
		 * the line above is true -- but it is also the one thing the user
		 * would want to know here, and -V is how an unknown board is
		 * identified. The selection paths note it for the same reason. */
		ubertooth_note_dfu_bootloader();
		return 0;
	}

	/* One board, or the one the user named with -U: the single-board form,
	 * with no index to disambiguate. An empty -U value names no board, as it
	 * does in every other selection, so it lists them all. */
	if ((selector != NULL && selector[0] != '\0') || count == 1) {
		ut = ubertooth_start_select(selector);
		if (ut == NULL) {
			print_unavailable(out, -1);
			return 1;
		}
		status = print_board_versions(out, ut, -1);
		/* Closed without a STOP, so asking a board its version does not end
		 * the mode it is running. */
		ubertooth_close(ut);
		ubertooth_free(ut);
		return status;
	}

	/* Several boards and no -U: every board, indexed, rather than a refusal
	 * to pick one. A board that cannot be opened gets its own line and the
	 * rest are still reported. */
	for (i = 0; i < count; i++) {
		ut = ubertooth_start(i);
		if (ut == NULL) {
			print_unavailable(out, i);
			status = 1;
			continue;
		}
		if (print_board_versions(out, ut, i) != 0)
			status = 1;
		ubertooth_close(ut);
		ubertooth_free(ut);
	}
	return status;
}
