/*
 * Copyright 2026 Project Ubertooth
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
 * The 32 bytes a board reads to tell a whole application from a half-written
 * one: a magic that says they are here, and room for the flags, the length
 * and the checksum that ubertooth-dfu fills in as it writes the image.
 *
 * The image carries them empty. An image is not checked until a host has
 * written it, because only the host knows how many bytes reached the flash,
 * and what the checksum over them comes to; a bootloader that reads flags of
 * zero makes no check, which is what keeps an image written by an earlier
 * host tool startable.
 *
 * ../common.mk compiles this into every run-time image, so no image can leave
 * the block out, and ../common/sections.ld puts it at a fixed offset from the
 * start of the image. That offset is inside the image's first 256 bytes,
 * which is what makes the block useful: the host writes those bytes in the
 * same program as the vector table, so a board that finds a vector table
 * finds this block with it.
 *
 * The layout, the offset and the magic are in
 * host/libubertooth/src/ubertooth_interface.h, which the host reads them from
 * too.
 */
#include "ubertooth_interface.h"

/*
 * `used` keeps the block through --gc-sections: nothing in the firmware refers
 * to it, so the linker would otherwise drop the section it is the only member
 * of. sections.ld also KEEPs it.
 *
 * The named section is only asked for on ELF, which is every target this
 * firmware is built for. ../tests/test_fw_info.c compiles this file with the
 * host compiler to check the bytes it produces, and a Mach-O host rejects a
 * section name with no segment in front of it; the section placement is not
 * what that test is about.
 */
#ifdef __ELF__
#define IMAGE_CHECK_PLACEMENT __attribute__((section(".image_check"), used))
#else
#define IMAGE_CHECK_PLACEMENT __attribute__((used))
#endif

IMAGE_CHECK_PLACEMENT
const struct ubertooth_image_check_block image_check_block = {
	/* UBERTOOTH_IMAGE_CHECK_MAGIC, byte by byte: the field holds no
	 * terminator, and a string literal that exactly fills it draws a warning
	 * from newer compilers. The firmware test puts these bytes against the
	 * macro. */
	{ 'U', 'B', 'T', 'I', 'M', 'G', 'C', 'K' },
	UBERTOOTH_IMAGE_CHECK_FORMAT,
	/* flags: no check is asked for until a host has written the image. */
	0,
	/* length, its complement, and the checksum over that many bytes. */
	0,
	0,
	0,
	{ 0, 0, 0, 0, 0, 0, 0, 0 },
};
