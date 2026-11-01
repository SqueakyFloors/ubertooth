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
 * The bootloader report an application answers UBERTOOTH_GET_BOOTLOADER_INFO
 * with. See bl_report.h for what the two functions are and why the region is
 * an address.
 */
#include "bl_report.h"
#include "ubertooth_interface.h"

/*
 * A nibble at a time, from a table of sixteen words rather than the usual two
 * hundred and fifty-six. The hash runs inside a USB control request, so it
 * has to be quick: the 16 KiB region takes a couple of milliseconds this way
 * against four or five bit by bit, and the table costs 64 bytes of flash
 * instead of a kilobyte.
 *
 * Each entry is the CRC of one nibble value, so two steps consume a byte.
 */
static const uint32_t crc32_nibbles[16] = {
	0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac,
	0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
	0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
	0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
};

uint32_t bl_report_crc32(uintptr_t start, size_t length)
{
	const volatile uint8_t *byte = (const volatile uint8_t *)start;
	uint32_t crc = 0xffffffff;
	size_t i;

	for (i = 0; i < length; i++) {
		crc ^= byte[i];
		crc = (crc >> 4) ^ crc32_nibbles[crc & 0x0f];
		crc = (crc >> 4) ^ crc32_nibbles[crc & 0x0f];
	}
	return ~crc;
}

void bl_report_build(uint8_t *out, uintptr_t region)
{
	const volatile uint8_t *block = (const volatile uint8_t *)
		(region + UBERTOOTH_BL_VERSION_OFFSET);
	static const char magic[] = UBERTOOTH_BL_VERSION_MAGIC;
	uint32_t crc = bl_report_crc32(region,
	                               UBERTOOTH_BOOTLOADER_REGION_LENGTH);
	int have_block = 1;
	size_t i;

	out[UBERTOOTH_BL_REPORT_OFF_CRC + 0] = (uint8_t)(crc & 0xff);
	out[UBERTOOTH_BL_REPORT_OFF_CRC + 1] = (uint8_t)((crc >> 8) & 0xff);
	out[UBERTOOTH_BL_REPORT_OFF_CRC + 2] = (uint8_t)((crc >> 16) & 0xff);
	out[UBERTOOTH_BL_REPORT_OFF_CRC + 3] = (uint8_t)((crc >> 24) & 0xff);

	/* A bootloader with no block has its own code at the offset. Those
	 * bytes say nothing about it, so zeros go out in their place rather
	 * than a few bytes of someone's bootloader: the host reads the absence
	 * from the magic either way, and the hash is what identifies the
	 * bootloader in that case. */
	for (i = 0; i < UBERTOOTH_BL_VERSION_MAGIC_LEN; i++) {
		if (block[UBERTOOTH_BL_VERSION_OFF_MAGIC + i] != (uint8_t)magic[i]) {
			have_block = 0;
			break;
		}
	}
	for (i = 0; i < UBERTOOTH_BL_VERSION_LENGTH; i++)
		out[UBERTOOTH_BL_REPORT_OFF_BLOCK + i] =
			have_block ? block[i] : 0;
}
