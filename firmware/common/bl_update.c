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
/*
 * Installing a bootloader. See bl_update.h for what each function is for and
 * why the write order is the one it is.
 *
 * The checks over the payload itself are not here: they are
 * ubertooth_bl_update_payload_check() in ubertooth_interface.h, because
 * ubertooth-dfu makes the same ones over the same bytes before it writes this
 * image to a board.
 */
#include "bl_update.h"
#include "ubertooth_interface.h"

void bl_update_sector(const uint8_t *payload, unsigned sector, uint8_t *out)
{
	const size_t base = (size_t)sector * BL_UPDATE_SECTOR_LENGTH;
	const uint32_t checksum = ubertooth_bl_update_vector_checksum(payload);
	size_t i;

	for (i = 0; i < BL_UPDATE_SECTOR_LENGTH; i++)
		out[i] = ubertooth_bl_update_byte(payload, base + i, checksum);
}

/*
 * Does the region already hold this bootloader?
 *
 * Compared against the bytes that would be programmed rather than the payload
 * as it sits in the file, so the checksum word this code fills in does not
 * make a board differ from the bootloader it is already running.
 *
 * The bytes are read through a volatile pointer because the board's region is
 * at address 0, which a compiler may otherwise fold a read of away;
 * bl_report_crc32() reads the same region the same way.
 */
static int region_matches(const uint8_t *payload, uintptr_t region)
{
	const volatile uint8_t *installed = (const volatile uint8_t *)region;
	const uint32_t checksum = ubertooth_bl_update_vector_checksum(payload);
	size_t i;

	for (i = 0; i < UBERTOOTH_BL_UPDATE_PAYLOAD_LENGTH; i++) {
		if (installed[i] != ubertooth_bl_update_byte(payload, i, checksum))
			return 0;
	}
	return 1;
}

int bl_update_check(const uint8_t *payload, size_t length,
                    uintptr_t region, struct bl_update_report *report)
{
	report->part_id = 0;
	report->sectors_written = 0;
	report->sectors_verified = 0;
	report->result = ubertooth_bl_update_payload_check(
		payload, length, &report->bootloader_version);
	if (report->result == UBERTOOTH_BL_UPDATE_OK &&
	    region_matches(payload, region))
		report->result = UBERTOOTH_BL_UPDATE_SAME;
	return report->result;
}

/* Is the part one that can hold the region at all? The identification number
 * has to read, because a ROM that will not answer that is a ROM the
 * programming commands are about to be sent to, and sector 3 has to be a
 * sector the part has: nothing in this family is that small, so a part that
 * says otherwise is not a part these four sectors belong to. */
static int part_refused(const struct bl_update_ops *ops, void *context,
                        struct bl_update_report *report)
{
	uint32_t part_id = 0;
	int present;

	if (ops->read_part(context, &part_id) != 0 || part_id == 0) {
		report->result = UBERTOOTH_BL_UPDATE_PART_UNREADABLE;
		return 1;
	}
	report->part_id = part_id;
	present = ops->sector_present(context, BL_UPDATE_LAST_SECTOR);
	if (present < 0) {
		report->result = UBERTOOTH_BL_UPDATE_PART_UNREADABLE;
		return 1;
	}
	if (present == 0) {
		report->result = UBERTOOTH_BL_UPDATE_PART_TOO_SMALL;
		return 1;
	}
	return 0;
}

int bl_update_run(const struct bl_update_ops *ops, void *context,
                  const uint8_t *payload, size_t length,
                  uintptr_t region, uint8_t *buffer,
                  struct bl_update_report *report)
{
	unsigned sector;

	if (bl_update_check(payload, length, region, report) !=
	    UBERTOOTH_BL_UPDATE_OK)
		return report->result;
	if (ops == NULL || buffer == NULL) {
		report->result = UBERTOOTH_BL_UPDATE_SHORT;
		return report->result;
	}
	if (part_refused(ops, context, report))
		return report->result;

	/* One prepare and one erase over the whole region: see bl_update.h for
	 * why the region is emptied before any of it is programmed. */
	if (ops->prepare(context, BL_UPDATE_FIRST_SECTOR,
	                 BL_UPDATE_LAST_SECTOR) != 0) {
		report->result = UBERTOOTH_BL_UPDATE_PREPARE_FAILED;
		return report->result;
	}
	if (ops->erase(context, BL_UPDATE_FIRST_SECTOR,
	               BL_UPDATE_LAST_SECTOR) != 0) {
		report->result = UBERTOOTH_BL_UPDATE_ERASE_FAILED;
		return report->result;
	}

	for (sector = BL_UPDATE_FIRST_SECTOR; sector <= BL_UPDATE_LAST_SECTOR;
	     sector++) {
		const uint32_t address = sector * BL_UPDATE_SECTOR_LENGTH;

		bl_update_sector(payload, sector, buffer);
		/* The ROM clears a sector's preparation when it programs it, so
		 * each program needs its own. */
		if (ops->prepare(context, sector, sector) != 0) {
			report->result = UBERTOOTH_BL_UPDATE_PREPARE_FAILED;
			return report->result;
		}
		if (ops->program(context, address, buffer,
		                 BL_UPDATE_SECTOR_LENGTH) != 0) {
			report->result = UBERTOOTH_BL_UPDATE_WRITE_FAILED;
			return report->result;
		}
		report->sectors_written++;
	}

	/* The read-back, over the whole region once all of it is written: a
	 * sector checked before the next one is programmed would not catch a
	 * later write that disturbed an earlier sector. */
	for (sector = BL_UPDATE_FIRST_SECTOR; sector <= BL_UPDATE_LAST_SECTOR;
	     sector++) {
		const uint32_t address = sector * BL_UPDATE_SECTOR_LENGTH;

		bl_update_sector(payload, sector, buffer);
		if (ops->verify(context, address, buffer,
		                BL_UPDATE_SECTOR_LENGTH) != 0) {
			report->result = UBERTOOTH_BL_UPDATE_READ_BACK_FAILED;
			return report->result;
		}
		report->sectors_verified++;
	}

	report->result = UBERTOOTH_BL_UPDATE_OK;
	return report->result;
}
