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

#ifndef __BL_REPORT_H
#define __BL_REPORT_H

#include <stddef.h>
#include <stdint.h>

/*
 * What a running application says about the bootloader under it:
 * UBERTOOTH_GET_BOOTLOADER_INFO's reply, built here so that both images
 * answer the request with the same bytes.
 *
 * firmware/tests/test_bl_version.c runs both functions below.
 */

/*
 * The CRC-32 of the length bytes of memory starting at start: the usual
 * reflected CRC-32 (polynomial 0xedb88320, starting at all ones, inverted at
 * the end), the one zlib, PNG and Ethernet use, so a value can be checked
 * against any other implementation of it.
 *
 * The region is an address rather than a pointer because the region this is
 * for starts at flash address 0. A pointer parameter would invite a null
 * check that every real call would trip over; an address cannot be mistaken
 * for one. The bytes are read through a volatile pointer, so a compiler
 * cannot fold a read at address 0 away.
 */
uint32_t bl_report_crc32(uintptr_t start, size_t length);

/*
 * Works out the report once, at startup: the hash of the whole bootloader
 * region starting at region, then the version block that region carries, or
 * zeros in place of the block when there is none there.
 *
 * This is where the cost is. Hashing 16 KiB takes about 2 ms, and the request
 * that asks for the report is answered from the USB interrupt in several
 * capture modes, so doing it there would be by some margin the longest thing
 * in the tree to happen inside one -- long enough to cost a capture transfer.
 * Here it happens once, before any mode can be running, for the same reason
 * device_id_init() does.
 *
 * Working it out early is sound because the answer cannot change while the
 * application runs: an application never writes the flash below itself, and
 * the bootloader only does so after a reset has handed the board back to it.
 *
 * The region's address is a parameter, which each image passes
 * UBERTOOTH_BOOTLOADER_REGION_START for. That lets the test above hand it a
 * region of its own to read, since no host can read the board's address 0.
 */
void bl_report_init(uintptr_t region);

/*
 * Copies the UBERTOOTH_BL_REPORT_LENGTH bytes bl_report_init() worked out to
 * out: what the request handler answers with, and a copy of that size and
 * nothing more. Zeros before bl_report_init() has run.
 */
void bl_report_build(uint8_t *out);

#endif /* __BL_REPORT_H */
