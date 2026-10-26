/*
 * Copyright 2010, 2011 Michael Ossmann
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

#ifndef __FLASH_H_
#define __FLASH_H_

#include "iap.h"

/*
 * The bound for a part the table below has no entry for and the sector probe
 * got no answer out of: 512 KiB, which is what the bootloader allowed on every
 * part before the table existed.
 *
 * Keeping the old figure cannot leave such a board worse off than it is
 * today. A part with at least 512 KiB behaves as it always did. A part with
 * less faults on a read past its end, as it always did. Bounding a part
 * nobody can place at the smallest part in the family would instead refuse a
 * read-back, and an image, that work today.
 */
#define LPC_FLASH_SIZE_FALLBACK 0x80000u

/*
 * The sector map of the family: sixteen 4 KiB sectors over the first 64 KiB,
 * then 32 KiB sectors upwards. The largest part in the family has 512 KiB, so
 * sector 29 is the highest sector there is (NXP LPC176x/5x user manual
 * UM10360, "Flash memory sectors").
 */
#define LPC_SMALL_SECTOR_SIZE  0x1000u
#define LPC_SMALL_SECTOR_COUNT 16u
#define LPC_LARGE_SECTOR_SIZE  0x8000u
#define LPC_LARGE_SECTOR_BASE  (LPC_SMALL_SECTOR_SIZE * LPC_SMALL_SECTOR_COUNT)
#define LPC_TOP_SECTOR         29u

/* The first address in a sector. One past the top sector gives the top of
 * flash on the largest part in the family. */
static inline uint32_t lpc_sector_start(const uint32_t sector) {
    if( sector < LPC_SMALL_SECTOR_COUNT ) {
        return sector * LPC_SMALL_SECTOR_SIZE;
    }
    return LPC_LARGE_SECTOR_BASE +
           (sector - LPC_SMALL_SECTOR_COUNT) * LPC_LARGE_SECTOR_SIZE;
}

/* The sector an address is in. */
static inline uint32_t lpc_sector_number(const uint32_t address) {
    if( address < LPC_LARGE_SECTOR_BASE ) {
        return address / LPC_SMALL_SECTOR_SIZE;
    }
    return LPC_SMALL_SECTOR_COUNT +
           (address - LPC_LARGE_SECTOR_BASE) / LPC_LARGE_SECTOR_SIZE;
}

/* The size of the sector an address is in. */
static inline uint32_t lpc_sector_size(const uint32_t address) {
    return (address < LPC_LARGE_SECTOR_BASE) ? LPC_SMALL_SECTOR_SIZE
                                             : LPC_LARGE_SECTOR_SIZE;
}

/*
 * The flash the part with this identification number has, in bytes, or 0 for
 * a part with no entry here.
 *
 * Reading flash a part has not got is a precise bus fault on LPC17xx, and the
 * fault does not return to the bootloader's polled USB loop: the board
 * answers nothing more, not even a state request, until it is power-cycled.
 * Writing flash a part has not got is milder -- In-Application Programming
 * refuses it and the bootloader can report that -- but it stops part-way
 * through an image, leaving an application missing its tail. Either way the
 * bound has to come from the part rather than from a constant, because the
 * boards in the field are a mix: the Ubertooth One schematic specifies an
 * LPC1754 with 128 KiB, and others read back more.
 *
 * The identification numbers and sizes are from the NXP LPC176x/5x user
 * manual (UM10360, "Part identification numbers"). The host keeps the same
 * figures, with the part names it prints, in DFU_PARTS in
 * host/ubertooth-tools/src/dfu_recovery.c, so that it does not ask for flash
 * a board has not got; a firmware unit test reads both tables and fails if
 * they disagree.
 */
static inline uint32_t lpc_part_flash_size(const uint32_t part_id) {
    struct entry {
        uint32_t part_id;
        uint32_t flash_size;
    };
    static const entry table[] = {
        { 0x25001110,  32 * 1024 },  /* LPC1751 */
        { 0x25001118,  32 * 1024 },  /* LPC1751 */
        { 0x25001121,  64 * 1024 },  /* LPC1752 */
        { 0x25011722, 128 * 1024 },  /* LPC1754 */
        { 0x25011723, 256 * 1024 },  /* LPC1756 */
        { 0x25013F37, 512 * 1024 },  /* LPC1758 */
        { 0x25113737, 512 * 1024 },  /* LPC1759 */
        { 0x26012033, 256 * 1024 },  /* LPC1763 */
        { 0x26011922, 128 * 1024 },  /* LPC1764 */
        { 0x26013733, 256 * 1024 },  /* LPC1765 */
        { 0x26013F33, 256 * 1024 },  /* LPC1766 */
        { 0x26012837, 512 * 1024 },  /* LPC1767 */
        { 0x26013F37, 512 * 1024 },  /* LPC1768 */
        { 0x26113F37, 512 * 1024 },  /* LPC1769 */
    };

    for( unsigned i = 0; i < sizeof(table) / sizeof(table[0]); i++ ) {
        if( table[i].part_id == part_id ) {
            return table[i].flash_size;
        }
    }
    return 0;
}

class Flash {
public:
    Flash() :
        iap(),
        part_id_read(0),
        flash_bound_found(0),
        probe_walked(false),
        probe_walked_part_id(0) {
    }

    bool write(const uint32_t flash_address, uint32_t source_address, uint32_t length) {
        IAP::ReturnCode result = IAP::CMD_SUCCESS;
        
        const uint32_t sector = lpc_sector_number(flash_address);
        if( at_sector_boundary(flash_address) ) {
            if( result == IAP::CMD_SUCCESS) {
                result = iap.prepare_sectors_for_write_operation(sector, sector);
            }
            if( result == IAP::CMD_SUCCESS) {
                result = iap.erase_sectors(sector, sector, cclk_khz);
            }
        }
        
        if( result == IAP::CMD_SUCCESS) {
            result = iap.prepare_sectors_for_write_operation(sector, sector);
        }
        if( result == IAP::CMD_SUCCESS ) {
            /* Reported, not discarded: a program the part refuses has to
             * reach the host as a failure, or a block that never made it
             * into flash is answered as written. */
            result = iap.copy_ram_to_flash(flash_address, source_address, length, cclk_khz);
        }
        
        return (result == IAP::CMD_SUCCESS);
    }
    
    /* Is there flash behind this address on the part this is running on? */
    bool valid_address(const uint32_t address) const {
        return (address < flash_bound());
    }

    /*
     * One past the last address with flash behind it on this part.
     *
     * The table is the fast answer. A part with no entry is asked which
     * sectors it has instead, so that nothing here depends on a part being
     * listed. If neither answers, the old fixed 512 KiB stands.
     *
     * The first answer, from either source, is remembered: the part cannot
     * change while the bootloader runs, and a transfer checks two addresses
     * for every 256-byte block, so asking again each time would make it
     * crawl. While neither has answered, the table is looked up on every
     * check, because that costs one command, so a part whose identification
     * number only reads later still ends up bounded by its own flash.
     *
     * The probe is not retried on the same reading of the part, because it is
     * up to 30 commands: a part that failed it would otherwise be re-walked
     * twice per block, which is a transfer nobody can wait out. It is walked
     * again once the identification number reads for the first time, since
     * that is a part answering commands it was not answering before, and
     * there are only ever the two readings -- unreadable, then whatever the
     * ROM gave -- so the walk can happen at most twice.
     */
    uint32_t flash_bound() const {
        if( flash_bound_found == 0 ) {
            const uint32_t id = part_id();
            const uint32_t from_table = lpc_part_flash_size(id);
            if( from_table != 0 ) {
                flash_bound_found = from_table;
            } else if( !probe_walked || (id != probe_walked_part_id) ) {
                probe_walked = true;
                probe_walked_part_id = id;
                flash_bound_found = probe_flash_bound();
            }
        }
        return (flash_bound_found != 0) ? flash_bound_found
                                        : LPC_FLASH_SIZE_FALLBACK;
    }
    
private:
    IAP iap;
    /* The part identification number, read once and then remembered. A
     * read-back checks two addresses for every 256-byte block it sends, and
     * the part cannot change while the bootloader runs, so there is nothing
     * to gain from asking the ROM again.
     *
     * Zero means not read yet. No part has that number, so a read that fails
     * leaves the bound to the sector probe and is asked again on the next
     * request. */
    mutable uint32_t part_id_read;
    
    /* The bound flash_bound() worked out, or 0 for not worked out yet. No
     * part has a top of flash at address 0, so there is no answer to confuse
     * with "no answer yet". */
    mutable uint32_t flash_bound_found;

    /* Has the sector probe been walked, and under which reading of the part
     * identification number (0 for a reading that failed)? A walk that gave
     * no answer is not repeated for the same reading. */
    mutable bool probe_walked;
    mutable uint32_t probe_walked_part_id;

    static const uint32_t cclk_khz = 100000;
    
    /*
     * The top of flash on a part the table has no entry for, found by asking
     * the part which sectors it has. 0 when the walk got no usable answer,
     * which includes a part that answered for the sectors below and then
     * something unexpected: those answers give a floor, not a bound, so the
     * walk is discarded rather than taken as the top of flash.
     *
     * The blank check is the one In-Application Programming command that asks
     * about a sector without addressing it: a sector the part has not got
     * answers INVALID_SECTOR, and a sector it has answers CMD_SUCCESS or
     * SECTOR_NOT_BLANK depending on its contents. That is what makes the
     * probe safe, since reading an address the part has not got is the bus
     * fault this bound exists to avoid.
     *
     * Sectors run from 0 with no gaps, so the first sector the part refuses
     * is the end of its flash. A part that refuses sector 0 is answering
     * nonsense, and the 0 that comes back sends it to the fallback.
     */
    uint32_t probe_flash_bound() const {
        for( uint32_t sector = 0; sector <= LPC_TOP_SECTOR; sector++ ) {
            const IAP::ReturnCode result = iap.blank_check_sectors(sector, sector);
            if( result == IAP::INVALID_SECTOR ) {
                return lpc_sector_start(sector);
            }
            if( (result != IAP::CMD_SUCCESS) &&
                (result != IAP::SECTOR_NOT_BLANK) ) {
                return 0;
            }
        }
        return lpc_sector_start(LPC_TOP_SECTOR + 1);
    }

    uint32_t part_id() const {
        if( part_id_read == 0 ) {
            uint32_t id = 0;
            if( iap.read_part_identification_number(id) == IAP::CMD_SUCCESS ) {
                part_id_read = id;
            }
        }
        return part_id_read;
    }

    bool at_sector_boundary(const uint32_t address) const {
        return (address & (lpc_sector_size(address) - 1)) == 0;
    }
};

#endif//__FLASH_H_

