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

/* The image check block an image carries and ubertooth-dfu fills in: the
 * layout, the offset, the flags and what the checksum covers are all there,
 * and the host reads them from the same header. */
#include "ubertooth_interface.h"

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
 * The local limit here is 64 KiB, looser than any part in the family has, on
 * purpose, and the AHB SRAM at 0x2007c000 is accepted as well -- two 16 KiB
 * banks on the larger parts, where a linker script outside this tree could
 * put a stack. A stack pointer this refuses leaves a board waiting in DFU, so
 * every address a real image might use has to be inside one of the two: a
 * part or a linker script this code does not know about must not be refused.
 * What the check is for is erased flash and zeros, and both are far outside
 * either.
 */
#define APP_RAM_BASE      0x10000000u
#define APP_RAM_LIMIT     0x10010000u
#define APP_AHB_RAM_BASE  0x2007c000u
#define APP_AHB_RAM_LIMIT 0x20084000u

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
 * means an In-Application Programming call into the ROM at every power-up,
 * before any clock is set up. The DFU path makes that same call, so it is not
 * new code -- what would be new is making it on the path every ordinary boot
 * takes, in an image that cannot be put on a board from this tree at all: a
 * bootloader is installed with ISP or JTAG, not over USB, so nothing here can
 * show that call is safe that early. A check that hung there would take the
 * board with it, and the looser bound costs little against what this is for:
 * a reset vector above the part's flash cannot come from a host and a
 * bootloader from this tree, since both already bound a download by the
 * part's flash.
 *
 * The LPC vector checksum is asked for separately, by
 * app_image_vector_checksum_ok() below, and the two together are what
 * app_image_table_startable() puts a vector table to. The bootloader calls
 * app_image_region_startable(), which adds the checks the image check block
 * asks for on top of those.
 */
static inline bool app_image_startable(const uint32_t stack_pointer,
                                       const uint32_t reset_vector,
                                       const uint32_t flash_top)
{
    const bool stack_in_local_ram = (stack_pointer > APP_RAM_BASE) &&
                                    (stack_pointer <= APP_RAM_LIMIT);
    const bool stack_in_ahb_ram = (stack_pointer > APP_AHB_RAM_BASE) &&
                                  (stack_pointer <= APP_AHB_RAM_LIMIT);

    if( (!stack_in_local_ram && !stack_in_ahb_ram) ||
        ((stack_pointer & 3u) != 0) ) {
        return false;
    }
    if( (reset_vector & 1u) == 0 ) {
        return false;
    }
    const uint32_t entry = reset_vector & ~1u;
    return (entry >= APP_IMAGE_BASE) && (entry < flash_top);
}

/*
 * The LPC vector checksum: the word at offset 0x1c holds the two's complement
 * of the seven words in front of it, so all eight add up to zero (NXP
 * LPC176x/5x user manual UM10360, "Criterion for valid user code"). It is the
 * eight words of the vector table immediately above APP_IMAGE_BASE.
 */
#define APP_IMAGE_CHECKSUM_INDEX 7u
#define APP_IMAGE_CHECKED_WORDS  8u

/*
 * Do the first eight words of the application add up the way a checksummed
 * vector table does?
 *
 * ubertooth-dfu fills this word in as it writes the application's first block
 * (host/ubertooth-tools/src/ubertooth-dfu.c, download()), so an image written
 * by it carries a checksum over the words the board starts from.
 *
 * What that catches is a first block the part did not finish programming. A
 * download that stops between blocks writes nothing of the block it was on --
 * the bootloader is handed a block only once all 256 bytes of it have arrived
 * (firmware/common/lpcusb/target/usbcontrol.c) and programs it in one call --
 * and the two words above already refuse the erased flash that leaves. The
 * window the checksum covers is narrower and real: a board that loses power
 * part way through that one program, which on a bus-powered board is what a
 * pulled cable does. Erased flash leaves 0xFFFFFFFF in the word, which fails
 * the sum as well.
 *
 * A zero word is no checksum rather than a wrong one unless required says
 * otherwise. That is what every image this tree builds has in the file -- the
 * application's own vector table leaves the word zero (firmware/common/
 * LPC17xx_Interrupts.c) -- and what a board flashed by an earlier host tool
 * carries. Refusing those would leave a board waiting in DFU for an image it
 * already has, which is the one outcome this check must not produce.
 *
 * required is for an image that says for itself that the word is filled in:
 * its image check block carries UBERTOOTH_IMAGE_CHECK_VECTOR, so a zero word
 * is not an image that predates the checksum but one whose eight words do not
 * add up, and it is refused like any other wrong sum.
 *
 * The checksum covers the first block alone, so it says nothing about the
 * rest of the image: that is what the region checksum below is for.
 */
static inline bool app_image_vector_checksum_ok(const uint32_t* const vectors,
                                                const bool required)
{
    uint32_t sum = 0;

    if( !required && (vectors[APP_IMAGE_CHECKSUM_INDEX] == 0) ) {
        return true;
    }
    for( unsigned i = 0; i < APP_IMAGE_CHECKED_WORDS; i++ ) {
        sum += vectors[i];
    }
    return (sum == 0);
}

/*
 * Everything a vector table on its own can be asked: vectors points at the
 * eight words at APP_IMAGE_BASE, flash_top is one past the last flash address
 * that could hold application code, and checksum_required is the flag above.
 */
static inline bool app_image_table_startable(const uint32_t* const vectors,
                                             const uint32_t flash_top,
                                             const bool checksum_required)
{
    return app_image_startable(vectors[0], vectors[1], flash_top) &&
           app_image_vector_checksum_ok(vectors, checksum_required);
}

/*
 * The shortest image there could be a region checksum over: the image check
 * block ends at the top of the first 256 bytes, so an image shorter than that
 * does not hold the block the length came out of.
 */
#define APP_IMAGE_MIN_LENGTH 0x100u

/* Does this look like an image check block, rather than whatever else those
 * 32 bytes might hold? An image built before the block existed, an image from
 * another tree, and erased flash all read as absent here, and an absent block
 * asks for no checks at all. */
static inline bool app_image_check_present(
    const struct ubertooth_image_check_block* const check)
{
    static const char magic[] = UBERTOOTH_IMAGE_CHECK_MAGIC;

    for( unsigned i = 0; i < UBERTOOTH_IMAGE_CHECK_MAGIC_LEN; i++ ) {
        if( check->magic[i] != (uint8_t)magic[i] ) {
            return false;
        }
    }
    return (check->format_version == UBERTOOTH_IMAGE_CHECK_FORMAT);
}

/* The block an image carries, read out of the region it is part of. */
static inline const struct ubertooth_image_check_block* app_image_check_block(
    const uint32_t* const region)
{
    return (const struct ubertooth_image_check_block*)
        ((const uint8_t*)region + UBERTOOTH_IMAGE_CHECK_OFFSET);
}

/*
 * Add words words of the region up, letting the sum wrap.
 *
 * The same sum the host works out over the image it is about to write
 * (ubertooth_image_check_sum() in ubertooth_interface.h, which says why there
 * are two of these), with the checksum word among the words added, so the
 * whole of what was written comes to zero.
 */
static inline uint32_t app_image_region_sum(const uint32_t* const region,
                                            const uint32_t words)
{
    uint32_t sum = 0;

    for( uint32_t i = 0; i < words; i++ ) {
        sum += region[i];
    }
    return sum;
}

/*
 * Does every byte that was programmed add up?
 *
 * This is the check the vector checksum cannot make. The host writes the
 * region's first block before the rest of the sector it starts, because
 * writing that block is what erases the sector (firmware/bootloader/flash.h,
 * Flash::write), so an interruption in between leaves a vector table that
 * reads perfectly in front of fifteen blocks of erased flash. Everything
 * above that sector is already written by then, so the one thing left to ask
 * is whether the whole of what the host said it wrote is still there. Erased
 * flash reads 0xFFFFFFFF, which does not add up, so a sector that is only
 * partly written is refused and the board waits in DFU for another image.
 *
 * The length comes out of the image, which is why it is read carefully. It
 * decides how much flash is read, and a part has less flash than the family's
 * largest, so a length nothing checked could send this reading addresses the
 * part has not got. So: both copies of it have to agree, it has to be a whole
 * number of words, it has to be long enough to hold the block it came from,
 * and it has to fit the region. What is left is a length that was programmed
 * whole, and the host only ever writes one the part it is talking to has the
 * flash for.
 */
static inline bool app_image_region_checksum_ok(
    const uint32_t* const region,
    const struct ubertooth_image_check_block* const check,
    const uint32_t flash_top)
{
    const uint32_t length = check->length;

    if( (length ^ check->length_complement) != 0xffffffffu ) {
        return false;
    }
    if( (length < APP_IMAGE_MIN_LENGTH) || ((length & 3u) != 0) ) {
        return false;
    }
    if( length > (flash_top - APP_IMAGE_BASE) ) {
        return false;
    }
    return (app_image_region_sum(region, length / 4u) == 0);
}

/*
 * Everything the bootloader asks of an application before it starts it.
 *
 * region points at the start of the application region, APP_IMAGE_BASE, and
 * flash_top is one past the last flash address that could hold application
 * code.
 *
 * The vector table is read first and on its own terms, so a region that holds
 * no image at all costs two words and no more. Then come the checks the image
 * asks for itself: an image that carries no image check block, or carries one
 * with no flags set, is startable on the two words and the zero-checksum rule
 * alone, which is what every image written before this existed relies on.
 */
static inline bool app_image_region_startable(const uint32_t* const region,
                                              const uint32_t flash_top)
{
    const struct ubertooth_image_check_block* const check =
        app_image_check_block(region);
    const bool present = app_image_check_present(check);
    const uint16_t flags = present ? check->flags : 0;

    if( !app_image_table_startable(region, flash_top,
                                   (flags & UBERTOOTH_IMAGE_CHECK_VECTOR) != 0) ) {
        return false;
    }
    if( (flags & UBERTOOTH_IMAGE_CHECK_REGION) != 0 ) {
        return app_image_region_checksum_ok(region, check, flash_top);
    }
    return true;
}

#endif//__APP_IMAGE_H_
