/*
 * Copyright 2026 Project Ubertooth contributors
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

#ifndef __BL_UPDATE_H
#define __BL_UPDATE_H

#include <stddef.h>
#include <stdint.h>

/*
 * Installing a bootloader: the checks, and the order the four sectors are
 * written in.
 *
 * A bootloader cannot write itself -- it refuses USB writes below 0x4000 --
 * so the bootloader updater is an ordinary application image that the
 * bootloader starts like any other. It carries the 16 KiB of the bootloader
 * region it is to install (firmware/bl_update), and this file is everything
 * it does with those bytes that is not a register write: the refusals, the
 * vector checksum, the write order, and the read-back.
 *
 * Every In-Application Programming call is reached through bl_update_ops, so
 * firmware/tests/test_bl_update.c drives the whole of it against a model of a
 * part's flash. That matters more here than elsewhere: this is the one code
 * path in the tree whose mistakes a user cannot put right over USB, and no
 * board can be used to find them.
 */

/* The four sectors of the bootloader region, each 4 KiB (NXP LPC176x/5x user
 * manual UM10360, "Flash memory sectors"): the region is the bottom 16 KiB of
 * flash, so it is sectors 0 to 3 on every part in the family. */
#define BL_UPDATE_FIRST_SECTOR 0u
#define BL_UPDATE_LAST_SECTOR 3u
#define BL_UPDATE_SECTOR_COUNT 4u
#define BL_UPDATE_SECTOR_LENGTH 0x1000u

/*
 * The In-Application Programming commands this needs, and the one question
 * about the part. Every one answers 0 for success and anything else for a
 * failure, so the caller's wrapper can pass the ROM's own return code
 * straight through and a test can hand back whatever it wants to model.
 *
 * program() and verify() are given a RAM address, because the ROM programs
 * from RAM and compares against it: the payload itself is in flash, so the
 * caller lends a buffer a sector long and this file fills it.
 */
struct bl_update_ops {
	/* The part identification number. */
	int (*read_part)(void *context, uint32_t *part_id);
	/* Has the part got this sector? 1 for yes, 0 for no, and a negative
	 * value when it would not say. The blank check is the one command that
	 * asks about a sector without addressing it, so this costs no read of
	 * flash a part may not have. */
	int (*sector_present)(void *context, uint32_t sector);
	int (*prepare)(void *context, uint32_t first_sector, uint32_t last_sector);
	int (*erase)(void *context, uint32_t first_sector, uint32_t last_sector);
	int (*program)(void *context, uint32_t flash_address,
	               const uint8_t *ram, uint32_t length);
	int (*verify)(void *context, uint32_t flash_address,
	              const uint8_t *ram, uint32_t length);
};

/* What a run found. result is an enum ubertooth_bl_update_result. */
struct bl_update_report {
	int result;
	/* The part, once read; 0 when it would not answer. */
	uint32_t part_id;
	/* The version the payload's own version block gives, or 0 when it
	 * carries none -- which is a refusal, so this is 0 only then. */
	uint16_t bootloader_version;
	/* How many of the four sectors were programmed, and how many read back
	 * as written. Both are 0 for a run that refused before writing. */
	unsigned sectors_written;
	unsigned sectors_verified;
};

/*
 * ubertooth_bl_update_payload_check() over the payload, and then the region
 * the board has now, which answers UBERTOOTH_BL_UPDATE_SAME when it already
 * holds the payload and there is nothing to write. The answer and the version
 * the payload gives go into *report.
 *
 * region is an address rather than a pointer, because the region this is for
 * starts at flash address 0: a pointer parameter would invite a null check
 * that every real call would trip over, and a cast of 0 to a pointer is
 * exactly the argument such a check would turn away. The same reason, and the
 * same shape, as bl_report_crc32() in bl_report.h.
 *
 * The region is always read, so there is no way to ask for the write without
 * the comparison: that would be an erase and a program of the bootloader
 * region for a board that already had it.
 */
int bl_update_check(const uint8_t *payload, size_t length,
                    uintptr_t region, struct bl_update_report *report);

/*
 * The checks above, then the part's own, then the write.
 *
 * buffer is BL_UPDATE_SECTOR_LENGTH bytes of RAM, word-aligned, which the
 * programming and the read-back are given a sector at a time.
 *
 * The order is the whole of the risk here, so it is written down once:
 *
 *   1. prepare and erase sectors 0 to 3 in one command;
 *   2. program them in turn, lowest first, preparing each immediately before
 *      its program, as the ROM requires;
 *   3. read all four back and compare them with what was sent.
 *
 * Erasing the whole region before programming any of it is deliberate. A run
 * that stops part way leaves erased flash where the vector table belongs, and
 * a part whose vector checksum does not add up starts the boot ROM's serial
 * programmer rather than the flash: an interrupted update leaves one known
 * state, recoverable over the serial programming interface. Programming the
 * region's first sector last would instead leave the previous bootloader's
 * vector table in front of a different bootloader's code, which is a board
 * that starts something nobody has described.
 *
 * Returns the result, which is UBERTOOTH_BL_UPDATE_OK only when all four
 * sectors were programmed and read back as sent.
 */
int bl_update_run(const struct bl_update_ops *ops, void *context,
                  const uint8_t *payload, size_t length,
                  uintptr_t region, uint8_t *buffer,
                  struct bl_update_report *report);

/*
 * One sector of the payload as it is to be programmed: the payload's bytes,
 * with the vector checksum filled in where the first sector carries it.
 *
 * Exposed because the read-back needs the same bytes a second time. sector is
 * 0 to BL_UPDATE_LAST_SECTOR and out is BL_UPDATE_SECTOR_LENGTH bytes.
 */
void bl_update_sector(const uint8_t *payload, unsigned sector, uint8_t *out);

#endif /* __BL_UPDATE_H */
