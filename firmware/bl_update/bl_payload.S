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
 * The bootloader this updater installs, carried in the image.
 *
 * The bootloader's own binary is included byte for byte and padded to the
 * whole 16 KiB of the bootloader region with 0xff, the value erased flash
 * reads. The padding is what makes a region installed from here read exactly
 * as the same bootloader installed over the serial programming interface, so
 * one bootloader has one hash however it reached the board.
 *
 * ../common/sections.ld places this section at
 * UBERTOOTH_BL_UPDATE_PAYLOAD_OFFSET, so ubertooth-dfu reads the same bytes
 * out of the image file without searching it. The Makefile beside this file
 * puts the built image's bytes at that offset against the bootloader binary
 * they came from.
 *
 * A bootloader larger than the region would make the .space below negative.
 * The assembler only warns about that and emits nothing, so what catches it
 * is the ASSERT in ../common/sections.ld that holds this section to exactly
 * 0x4000 bytes: the link fails. That is the same 16 KiB the bootloader's own
 * linker script gives it.
 */
	.section .bl_payload, "a", %progbits
	.align 2
	.global bl_payload
	.type bl_payload, %object
bl_payload:
	.incbin "../bootloader/bootloader.bin"
	.space 0x4000 - (. - bl_payload), 0xff
	.size bl_payload, . - bl_payload
