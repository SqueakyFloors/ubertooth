/* -*- c -*- */
/*
 * Copyright 2015 Dominic Spill
 * 
 * This file is part of Project Ubertooth
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
 * along with libbtbb; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#define REQ_DETACH 0
#define REQ_DNLOAD 1
#define REQ_UPLOAD 2
#define REQ_GETSTATUS 3
#define REQ_CLRSTATUS 4
#define REQ_GETSTATE 5
#define REQ_ABORT 6

#define STATE_APP_IDLE 0
#define STATE_APP_DETACH 1
#define STATE_DFU_IDLE 2
#define STATE_DFU_DNLOAD_SYNC 3
#define STATE_DFU_DNBUSY 4
#define STATE_DFU_DNLOAD_IDLE 5
#define STATE_DFU_MANIFEST_SYNC 6
#define STATE_DFU_MANIFEST 7
#define STATE_DFU_MANIFEST_WAIT_RESET 8
#define STATE_DFU_UPLOAD_IDLE 9
#define STATE_DFU_ERROR 10

#define STATUS_OK 0x0
#define STATUS_ERR_TARGET 0x1
#define STATUS_ERR_FILE 0x2
#define STATUS_ERR_WRITE 0x3
#define STATUS_ERR_ERASE 0x4
#define STATUS_ERR_CHECK_ERASED 0x5
#define STATUS_ERR_PROG 0x6
#define STATUS_ERR_VERIFY 0x7
#define STATUS_ERR_ADDRESS 0x8
#define STATUS_ERR_NOTDONE 0x9
#define STATUS_ERR_FIRMWARE 0xA
#define STATUS_ERR_VENDOR 0xB
#define STATUS_ERR_USBR 0xC
#define STATUS_ERR_POR 0xD
#define STATUS_ERR_UNKNOWN 0xE
#define STATUS_ERR_STALLEDPKT 0xF

/* The bootloader's USB ids. Device selection needs them too, so that a board
 * being flashed is not reported as a board that went away, and it has its own
 * copy in libubertooth's ubertooth_control.h: that header declares libusb
 * types, and this one has to compile without libusb, for the DFU tool's own
 * sources and for the packaging check in tools/check-dfu-package.sh.
 *
 * Both copies reach ubertooth-dfu.c, which includes both headers, so changing
 * one and not the other is a macro redefined with a different value: the
 * compiler says so, and the two cannot drift apart in silence. */
#define U1_DFU_VENDORID  0x1d50
#define U1_DFU_PRODUCTID 0x6003

/* A DFU 1.1 suffix carries idVendor/idProduct so a host can refuse firmware
 * meant for another device; 0xffff matches any device. */
#define DFU_ID_WILDCARD 0xffff

/* The suffix as it sits in the file: 16 little-endian bytes at the end, in
 * this order:
 *   bcdDevice, idProduct, idVendor, bcdDFU, ucDfuSig[3], bLength, dwCRC.
 * ucDfuSig holds "DFU" stored backwards, so those three bytes read
 * 0x55 0x46 0x44 ('U', 'F', 'D'). dwCRC covers every byte before it. */
#define DFU_SUFFIX_LENGTH 16
#define DFU_SUFFIX_OFF_BCDDEVICE 0
#define DFU_SUFFIX_OFF_IDPRODUCT 2
#define DFU_SUFFIX_OFF_IDVENDOR 4
#define DFU_SUFFIX_OFF_BCDDFU 6
#define DFU_SUFFIX_OFF_UCDFUSIG 8
#define DFU_SUFFIX_OFF_BLENGTH 11
#define DFU_SUFFIX_OFF_DWCRC 12

/* The only DFU specification versions this tool knows about. */
#define DFU_VERSION_1_0 0x0100
#define DFU_VERSION_1_1 0x0101

#define BOOTLOADER_OFFSET 0x0
#define BOOTLOADER_SIZE 0x4000
/* 512 KiB: the most flash any part in the family has, and so the highest
 * address any bootloader will read back or write.
 *
 * A bootloader built from this tree stops at the flash the attached part has,
 * which is less than this on most parts: it takes the figure from the part
 * identification number, falls back to asking the part which sectors it has,
 * and only then to 512 KiB, which is what older bootloaders allow on every
 * part (firmware/bootloader/flash.h, Flash::flash_bound). Both the read-back
 * and the download are bounded by it.
 *
 * Nothing above this can be read or written on any part, so it also bounds
 * the largest image there is any point sending. */
#define BOOTLOADER_FLASH_LIMIT 0x80000u
#define BLOCK_SIZE (1<<8)
#define SECTOR_SIZE (1<<12)
