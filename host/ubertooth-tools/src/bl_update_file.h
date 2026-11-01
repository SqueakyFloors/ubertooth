/*
 * What a bootloader updater image says about the bootloader it carries.
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

#ifndef UBERTOOTH_BL_UPDATE_FILE_H
#define UBERTOOTH_BL_UPDATE_FILE_H

#include <stdint.h>
#include <stdio.h>

/*
 * ubertooth-dfu --update-bootloader writes a bootloader by writing the
 * updater image that carries it, so everything it can say about the
 * bootloader before the board is touched it has to read out of that file.
 *
 * It reads the image's firmware info block to see that this is an updater at
 * all, then the 16 KiB of bootloader region at
 * UBERTOOTH_BL_UPDATE_PAYLOAD_OFFSET, and puts those bytes to the same checks
 * the updater will make on the board
 * (ubertooth_bl_update_payload_check() in libubertooth's
 * ubertooth_interface.h). A bootloader the board would refuse is therefore
 * refused here, with nothing written and no board touched -- which is the
 * whole point of checking twice, since the refusal that matters most is a
 * code read protection value, and the board cannot report one after the fact.
 *
 * It also works out the hash the board will report once the bootloader is
 * installed, so the user has a number to check the result against:
 * `ubertooth-util -B` after the update prints the same value.
 */

struct bl_update_file {
	/* An enum ubertooth_bl_update_result. UBERTOOTH_BL_UPDATE_OK is a file
	 * worth writing; everything negative is a refusal. */
	int result;
	/* The version the bootloader's own version block gives, 0 when the
	 * payload carries none. */
	uint16_t bootloader_version;
	/* The word at the code read protection offset, so a refusal can quote
	 * it alongside the name ubertooth_lpc_crp_name() gives. */
	uint32_t crp_word;
	/* The CRC-32 the board's bootloader report will carry once this
	 * bootloader is installed: the hash of the whole region as it will be
	 * programmed, with the vector checksum filled in. Only meaningful when
	 * result is UBERTOOTH_BL_UPDATE_OK. */
	uint32_t region_crc32;
};

/*
 * Reads and checks the bootloader an updater image carries.
 *
 * file is the image, open for reading and left seeked wherever this finishes;
 * image_name is the name from its firmware info block, which the caller has
 * already read (check_download_image() does). Either being NULL is
 * UBERTOOTH_BL_UPDATE_NOT_UPDATER.
 *
 * Returns out->result, and fills in whatever it got far enough to read. It
 * prints nothing: bl_update_file_report() does that.
 */
int bl_update_file_check(FILE *file, const char *image_name,
                         struct bl_update_file *out);

/*
 * The lines a user sees about the file, on standard error with the rest of the
 * progress: what bootloader it carries, or why it was refused.
 *
 * name is the file name to quote. Returns 0 for a file worth writing and 1
 * for one that was refused, so a caller can exit on it.
 */
int bl_update_file_report(FILE *out, const char *name,
                          const struct bl_update_file *info);

#endif /* UBERTOOTH_BL_UPDATE_FILE_H */
