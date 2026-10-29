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

#ifndef __APP_IMAGE_H_
#define __APP_IMAGE_H_

#include <stdbool.h>
#include <stdint.h>

/* The first address of the application region: the bootloader has the flash
 * below it, and an application starts with its vector table here. The same
 * address the host writes from, BOOTLOADER_OFFSET + BOOTLOADER_SIZE in
 * host/ubertooth-tools/src/dfu.h. */
#define APP_IMAGE_BASE 0x4000u

/*
 * Where an initial stack pointer may point: the local SRAM of this family,
 * 0x10000000 upwards. The largest part has 32 KiB of it and the smallest
 * 8 KiB (NXP LPC176x/5x user manual UM10360, "Memory map"); the images in
 * this tree are linked for 16 KiB and put the stack at the top, a little
 * under 0x10004000 (firmware/common/sections.ld, _StackTop).
 *
 * The limit here is 64 KiB, looser than any part in the family has, on
 * purpose. A stack pointer this refuses leaves a board waiting in DFU, so
 * every address a real image might use has to be inside it: a part or a
 * linker script this code does not know about must not be refused. What the
 * check is for is erased flash and zeros, and both are far outside even this.
 */
#define APP_RAM_BASE  0x10000000u
#define APP_RAM_LIMIT 0x10010000u

/*
 * Is there an application at APP_IMAGE_BASE that the bootloader can start?
 *
 * stack_pointer and reset_vector are the first two words of the application:
 * the stack pointer the processor loads and the address it jumps to. flash_top
 * is one past the last flash address that could hold application code.
 *
 * This is a loose check and is meant to be. It is not here to tell a good
 * image from a bad one -- it cannot, since nothing in the image says how long
 * it should be or what it should add up to. It is here to tell an image from
 * no image: erased flash reads as 0xFFFFFFFF in both words, and a download
 * that stopped part way through now leaves exactly that at the start of the
 * region (host/ubertooth-tools/src/ubertooth-dfu.c, download()). Starting
 * that is what left boards faulting on every power-up until someone bridged
 * the entry pin at the bench; refusing it leaves the bootloader running, so
 * the next image can be written over USB.
 *
 * What it asks:
 *
 *   - the stack pointer is inside RAM, and word-aligned, which is what the
 *     processor needs of it before the first push;
 *   - the reset vector is a Thumb address -- the low bit set, as every
 *     function pointer in a Cortex-M vector table has -- inside the flash
 *     above the bootloader.
 *
 * flash_top is a parameter so that a caller may be as strict as it can
 * afford. The bootloader passes the largest part in the family, not the flash
 * the part in front of it has, so a reset vector pointing into flash this
 * part has not got is accepted and jumped into. Reading the part's own bound
 * means In-Application Programming calls into the ROM on every power-up,
 * before any clock is set up, in code that cannot be exercised on hardware
 * from this tree -- a bootloader is installed with ISP or JTAG, not over USB
 * -- and a check that hung there would take the board with it. The looser
 * bound costs nothing against what this is for: a reset vector above the
 * part's flash cannot come from a host and a bootloader from this tree, since
 * both already bound a download by the part's flash.
 *
 * The LPC vector checksum at offset 0x1c is not asked for. The boot ROM reads
 * it over the image at address 0, which is the bootloader itself; the
 * application's own table leaves that word zero (firmware/common/
 * LPC17xx_Interrupts.c) and no host tool fills it in, so requiring it would
 * refuse every image this tree builds, including the one a user would flash
 * to recover a board. It would add nothing anyway: the checksum covers the
 * first eight words, all of them in the same 256-byte block as the two words
 * above, so it is written or not written with them.
 */
static inline bool app_image_startable(const uint32_t stack_pointer,
                                       const uint32_t reset_vector,
                                       const uint32_t flash_top)
{
    if( (stack_pointer <= APP_RAM_BASE) ||
        (stack_pointer > APP_RAM_LIMIT) ||
        ((stack_pointer & 3u) != 0) ) {
        return false;
    }
    if( (reset_vector & 1u) == 0 ) {
        return false;
    }
    const uint32_t entry = reset_vector & ~1u;
    return (entry >= APP_IMAGE_BASE) && (entry < flash_top);
}

#endif//__APP_IMAGE_H_
