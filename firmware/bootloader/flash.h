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
 * The ceiling used for a part with no entry in the table below: what the
 * bootloader enforced on every part, whatever flash it had.
 *
 * It is not a figure any part is known to have; it is the status quo, chosen
 * so that a part with no entry cannot end up worse off than it is now. Such a
 * part either has at least this much flash, and reads back as it always did,
 * or has less and faults on a read past its end as it always did. Bounding it
 * at the smallest part in the family instead would refuse a read-back that
 * works today, which is a regression on a board that is fine.
 */
#define LPC_FLASH_SIZE_FALLBACK 0x80000u

/*
 * The flash the part with this identification number has, in bytes, or 0 for
 * a part with no entry here.
 *
 * Reading flash a part has not got is a precise bus fault on LPC17xx, and the
 * fault does not return to the bootloader's polled USB loop: the board
 * answers nothing more, not even a state request, until it is power-cycled.
 * The read-back bound therefore has to come from the part rather than from a
 * constant, because the boards in the field are a mix -- the Ubertooth One
 * schematic specifies an LPC1754 with 128 KiB, and others read back more.
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

/* One past the last address with flash behind it on this part, or the
 * fallback above for a part with no entry in the table. */
static inline uint32_t lpc_part_flash_bound(const uint32_t part_id) {
    const uint32_t flash_size = lpc_part_flash_size(part_id);
    return (flash_size != 0) ? flash_size : LPC_FLASH_SIZE_FALLBACK;
}

class Flash {
public:
    Flash() :
        iap(),
        part_id_read(0) {
    }
    
    bool write(const uint32_t flash_address, uint32_t source_address, uint32_t length) {
        IAP::ReturnCode result = IAP::CMD_SUCCESS;
        
        const uint32_t sector = sector_number(flash_address);
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
            iap.copy_ram_to_flash(flash_address, source_address, length, cclk_khz);
        }
        
        return (result == IAP::CMD_SUCCESS);
    }
    
    /* Is there flash behind this address on the part this is running on? */
    bool valid_address(const uint32_t address) const {
        return (address < lpc_part_flash_bound(part_id()));
    }
    
private:
    IAP iap;
    /* The part, read once and remembered: the read-back asks about every
     * 256-byte block twice and the answer cannot change while the bootloader
     * runs. Zero means not read yet, which matches no entry in the table, so
     * a read that fails is retried and bounded by the fallback meanwhile. */
    mutable uint32_t part_id_read;
    
    static const uint32_t cclk_khz = 100000;
    
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
        return (address & (sector_size(address) - 1)) == 0;
    }
    
    uint32_t sector_size(const uint32_t address) const {
        if( address < 0x10000 ) {
            return 4096;
        } else {
            return 32768;
        }
    }
                              
    uint32_t sector_number(const uint32_t address) const {
        if( address < 0x10000 ) {
            return address >> 12;
        } else {
            return 14 + (address >> 15);
        }
    }
};

#endif//__FLASH_H_

