/* SPDX-License-Identifier: GPL-2.0-or-later */

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
 * The region is the flash below the image, so this is for an image linked
 * with a bootloader under it -- every shipped one. An image linked at 0
 * instead (LPC17xx_Linker_Script_without_bootloader.ld) would hash its own
 * first 16 KiB and report it as a 1.x bootloader, so such an image should not
 * call this at all.
 *
 * The region's address is a parameter, which each image passes
 * UBERTOOTH_BOOTLOADER_REGION_START for. That lets the test above hand it a
 * region of its own to read, since no host can read the board's address 0.
 */
void bl_report_init(uintptr_t region);

/*
 * Copies the UBERTOOTH_BL_REPORT_LENGTH bytes bl_report_init() worked out to
 * out: what the request handler answers with, and a copy of that size and
 * nothing more.
 *
 * Returns 1 once there is a report to copy, or 0 before bl_report_init() has
 * run, in which case out is left alone and the handler refuses the request.
 * Nothing can reach it that early today -- each image works the report out
 * before it brings the USB stack up -- but a zeroed report would read as a
 * board with no version block and a hash of zero, and a wrong answer given
 * confidently is worse than a refusal.
 */
int bl_report_build(uint8_t *out);

#endif /* __BL_REPORT_H */
