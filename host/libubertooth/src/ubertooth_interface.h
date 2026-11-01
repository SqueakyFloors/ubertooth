/*
 * Copyright 2012 Dominic Spill
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

#ifndef __UBERTOOTH_INTERFACE_H
#define __UBERTOOTH_INTERFACE_H

#include <stddef.h>
#include <stdint.h>

/*
 * The firmware API version, reported by the firmware as the USB bcdDevice.
 * The host tools require firmware with exactly this version
 * (ubertooth_check_api()), so host and firmware are released together.
 *
 * Both run-time images report it: bluetooth_rxtx and btbr ship from the same
 * commit and answer the same shared requests, so they carry the same number.
 * btbr used to report a literal 1.06 instead, which nothing bumped and which
 * made every tool refuse a board running that image, whatever revision it was
 * built from, except for the version reports, which read any firmware.
 *
 * During development it does not move. The host tools and the firmware are
 * built, installed and flashed together, and a development build only warns
 * when the two disagree, so an ordinary firmware change -- a new request, a
 * new mode, a fix -- leaves the number where it is. It moves only for a
 * change that really does break an older host tool: a request removed or
 * renumbered, or a reply or record layout an older host would misread. Such a
 * bump also updates CHANGELOG.md, COMPATIBILITY.md, TEST_PLAN.md and the
 * pinned strings in the tests.
 *
 * So a board carrying firmware from an earlier commit is normally accepted,
 * which is why this number is not the revision the `Firmware version:` line
 * shows beside it: that line carries the build's own revision, and this
 * number only says which host tools will work with the board at all.
 *
 * A request the firmware on a board has not got is reported where it is
 * asked for, as a refused request, rather than by this number.
 *
 * A release moves it to the next major version, to mark that its host tools
 * and firmware only work with each other; the first fork release takes 0x0200
 * (2.00). See docs/source/release_procedure.rst.
 *
 * bcdDevice is binary-coded decimal, so 0x0109 is followed by 0x0110 (shown
 * as 1.10), then 0x0111 (1.11), and so on to 0x0119 (1.19); after 0x0119
 * comes 0x0120, not 0x011a.
 */
#define UBERTOOTH_API_VERSION 0x0158

/*
 * The bootloader's bcdDevice, which is fixed on purpose and is the one version
 * in the tree that may never move.
 *
 * ubertooth-dfu has to work against any board, whatever it was flashed with,
 * because reflashing is how a board running the wrong firmware is put right.
 * So nothing checks this number, and raising it with every release would only
 * give some future tool something to refuse a working bootloader over. It is
 * named here so that it is clear it was left behind deliberately rather than
 * forgotten, which is what the bare 0x0100 in bootloader.cpp looked like.
 */
#define UBERTOOTH_BOOTLOADER_API_VERSION 0x0100

/*
 * The build string every image carries, reported by UBERTOOTH_GET_COMPILE_INFO
 * as a one-byte length and then that many characters. A one-byte length cannot
 * describe more than 255 characters, and the host reads at most 1 + 255 bytes
 * of the reply (cmd_get_firmware_compile_info()), so a longer string is cut to
 * this. Cutting it leaves the host a shortened string it can see is shortened.
 * Casting the real length to a byte instead, as the handlers used to, wrapped
 * it modulo 256 and sent that many characters: a 300-character string went out
 * as its first 44, and a 256-character one as nothing at all.
 */
#define UBERTOOTH_COMPILE_INFO_MAX 255

/*
 * The revision alone, reported by UBERTOOTH_GET_REV_NUM behind two zero bytes
 * and a one-byte length. The same limits apply as above: a one-byte length
 * cannot describe more than 255 characters, the host reads at most 2 + 1 + 255
 * bytes of the reply (cmd_get_firmware_revision()), and that is also all the
 * 258-byte reply buffer in each image's USB shim holds.
 */
#define UBERTOOTH_REVISION_MAX 255

/*
 * The firmware info block: what an image says about itself, in one place.
 *
 * Every image carries 64 bytes at a fixed offset from its start, just past the
 * vector table. firmware/common/fw_info.c fills them in and
 * firmware/common/sections.ld puts them there, so an image cannot leave the
 * block out.
 *
 * ubertooth-dfu reads the block straight out of a firmware file at that
 * offset. That is how it names the image and the revision a file carries, and
 * how it refuses a file that is not an image from this tree before it erases
 * anything. The firmware returns the same 64 bytes through
 * UBERTOOTH_GET_FW_INFO, so a board and a file are described the same way.
 *
 * The offset has to clear the vector table. Its eighth word, at offset 0x1c,
 * is the checksum the LPC boot ROM reads over the table, so the block must
 * sit well past that; the table is 204 bytes (51 words) today, and 256 is the
 * next round number above it. sections.ld fails the link if the table ever
 * grows that far.
 */
#define UBERTOOTH_FW_INFO_OFFSET 0x100
/* The 8 bytes a block starts with, no terminator. */
#define UBERTOOTH_FW_INFO_MAGIC "UBTFINFO"
#define UBERTOOTH_FW_INFO_MAGIC_LEN 8
/* The layout below. A block with any other number is not read. */
#define UBERTOOTH_FW_INFO_FORMAT 1
#define UBERTOOTH_FW_INFO_IMAGE_LEN 16
#define UBERTOOTH_FW_INFO_REVISION_LEN 32
#define UBERTOOTH_FW_INFO_LENGTH 64

/*
 * The block as it sits in an image: 64 bytes, no padding, the two 16-bit
 * fields little-endian as the ARM stores them. The strings are padded with
 * zeros and are only terminated when they are shorter than their field, so a
 * reader must not treat them as C strings -- ubertooth_fw_info_parse() below
 * copies them out safely.
 */
struct ubertooth_fw_info_block {
	uint8_t magic[UBERTOOTH_FW_INFO_MAGIC_LEN];
	uint16_t format_version;
	uint16_t api_version;
	uint8_t reserved[4];
	char image[UBERTOOTH_FW_INFO_IMAGE_LEN];
	char revision[UBERTOOTH_FW_INFO_REVISION_LEN];
};

/* Where each field starts, for a reader working on raw bytes. */
#define UBERTOOTH_FW_INFO_OFF_MAGIC 0
#define UBERTOOTH_FW_INFO_OFF_FORMAT 8
#define UBERTOOTH_FW_INFO_OFF_API 10
#define UBERTOOTH_FW_INFO_OFF_RESERVED 12
#define UBERTOOTH_FW_INFO_OFF_IMAGE 16
#define UBERTOOTH_FW_INFO_OFF_REVISION 32

/* The layout is the wire format, so a compiler that padded the struct would
 * silently move every field after the padding. */
typedef char ubertooth_fw_info_block_is_64_bytes[
	sizeof(struct ubertooth_fw_info_block) == UBERTOOTH_FW_INFO_LENGTH ? 1 : -1];

/* The block this image carries, and what UBERTOOTH_GET_FW_INFO returns.
 * Defined in firmware/common/fw_info.c, which every image links. */
extern const struct ubertooth_fw_info_block fw_info_block;

/* A block read out of an image or off a board, with both strings terminated. */
struct ubertooth_fw_info {
	uint16_t format_version;
	uint16_t api_version;
	char image[UBERTOOTH_FW_INFO_IMAGE_LEN + 1];
	char revision[UBERTOOTH_FW_INFO_REVISION_LEN + 1];
};

/* Why a block was not read. The caller names the file or the board, so these
 * say only what was wrong with the 64 bytes. */
enum ubertooth_fw_info_result {
	UBERTOOTH_FW_INFO_OK = 0,
	UBERTOOTH_FW_INFO_SHORT = -1,       /* fewer than 64 bytes to read */
	UBERTOOTH_FW_INFO_NO_MAGIC = -2,    /* no block here */
	UBERTOOTH_FW_INFO_FORMAT_UNKNOWN = -3,
	UBERTOOTH_FW_INFO_BAD_TEXT = -4,    /* an empty or unprintable string */
};

/* Copies length bytes of a field into out (size bytes), stopping at the first
 * zero, and terminates it. Returns 0 when what it copied is non-empty and
 * printable, and -1 otherwise: a block read from the wrong offset, or from a
 * file that is not an image, is mostly unprintable. out is terminated either
 * way, so a caller that ignores the result still has a string. */
static inline int ubertooth_fw_info_text(const uint8_t *field, size_t length,
                                         char *out, size_t size)
{
	size_t i;

	if (out == NULL || size == 0)
		return -1;
	out[0] = '\0';
	if (size < length + 1)
		return -1;
	for (i = 0; i < length && field[i] != 0; i++) {
		if (field[i] < 0x20 || field[i] > 0x7e) {
			out[i] = '\0';
			return -1;
		}
		out[i] = (char)field[i];
	}
	out[i] = '\0';
	return i > 0 ? 0 : -1;
}

/*
 * Reads the 64 bytes at raw into *out: the magic, the format version, and
 * both strings. length is how many bytes are there to read. Returns
 * UBERTOOTH_FW_INFO_OK, or a negative enum ubertooth_fw_info_result, and
 * leaves *out zeroed on failure.
 */
static inline int ubertooth_fw_info_parse(const uint8_t *raw, size_t length,
                                          struct ubertooth_fw_info *out)
{
	static const char magic[] = UBERTOOTH_FW_INFO_MAGIC;
	size_t i;

	if (raw == NULL || out == NULL)
		return UBERTOOTH_FW_INFO_SHORT;
	for (i = 0; i < sizeof(*out); i++)
		((uint8_t *)out)[i] = 0;
	if (length < UBERTOOTH_FW_INFO_LENGTH)
		return UBERTOOTH_FW_INFO_SHORT;
	for (i = 0; i < UBERTOOTH_FW_INFO_MAGIC_LEN; i++) {
		if (raw[UBERTOOTH_FW_INFO_OFF_MAGIC + i] != (uint8_t)magic[i])
			return UBERTOOTH_FW_INFO_NO_MAGIC;
	}
	out->format_version = (uint16_t)(raw[UBERTOOTH_FW_INFO_OFF_FORMAT] |
		(raw[UBERTOOTH_FW_INFO_OFF_FORMAT + 1] << 8));
	if (out->format_version != UBERTOOTH_FW_INFO_FORMAT) {
		uint16_t found = out->format_version;

		for (i = 0; i < sizeof(*out); i++)
			((uint8_t *)out)[i] = 0;
		out->format_version = found;
		return UBERTOOTH_FW_INFO_FORMAT_UNKNOWN;
	}
	out->api_version = (uint16_t)(raw[UBERTOOTH_FW_INFO_OFF_API] |
		(raw[UBERTOOTH_FW_INFO_OFF_API + 1] << 8));
	if (ubertooth_fw_info_text(raw + UBERTOOTH_FW_INFO_OFF_IMAGE,
	                           UBERTOOTH_FW_INFO_IMAGE_LEN,
	                           out->image, sizeof(out->image)) != 0 ||
	    ubertooth_fw_info_text(raw + UBERTOOTH_FW_INFO_OFF_REVISION,
	                           UBERTOOTH_FW_INFO_REVISION_LEN,
	                           out->revision, sizeof(out->revision)) != 0) {
		for (i = 0; i < sizeof(*out); i++)
			((uint8_t *)out)[i] = 0;
		return UBERTOOTH_FW_INFO_BAD_TEXT;
	}
	return UBERTOOTH_FW_INFO_OK;
}

/* One phrase naming what was wrong with a block, for a caller's message. */
static inline const char *ubertooth_fw_info_result_string(int result)
{
	switch (result) {
	case UBERTOOTH_FW_INFO_OK:
		return "the firmware info block was read";
	case UBERTOOTH_FW_INFO_NO_MAGIC:
		return "there is no firmware info block here";
	case UBERTOOTH_FW_INFO_FORMAT_UNKNOWN:
		return "the firmware info block is in a format this host does not know";
	case UBERTOOTH_FW_INFO_BAD_TEXT:
		return "the firmware info block holds no readable image name or revision";
	default:
		return "there are not 64 bytes of firmware info block to read";
	}
}

/*
 * The image check block: what a board needs to tell a whole application from
 * a half-written one.
 *
 * ubertooth-dfu writes an image in an order that leaves nothing startable
 * behind if the transfer stops: erased flash goes where the vector table
 * belongs, the rest of the image follows, and the first 4 KiB sector --
 * vector table and all -- is written last. A bootloader from this tree
 * refuses a vector table that reads as erased flash, so a transfer that
 * stopped leaves a board waiting in DFU for another image.
 *
 * Two things that order cannot settle on its own:
 *
 *   - whether the first block was programmed whole. The checksum word at
 *     offset 0x1c answers that, and the bootloader accepts a zero word as
 *     "no checksum" so that every image built before the word was filled in
 *     still starts. An image whose word is zero therefore gets no
 *     protection.
 *   - whether the fifteen blocks after the vector table arrived. Writing the
 *     region's first block erases the sector it starts
 *     (firmware/bootloader/flash.h, Flash::write), so the vector table has to
 *     be written before the rest of its own sector, and an interruption in
 *     between leaves a startable table in front of a sector that is only
 *     partly written.
 *
 * This block closes both. ubertooth-dfu fills it in as it writes, with flags
 * saying which of the two checks it filled in, how many bytes it wrote, and a
 * checksum over them. firmware/common/image_check.c reserves the 32 bytes and
 * firmware/common/sections.ld puts them at UBERTOOTH_IMAGE_CHECK_OFFSET, just
 * below the firmware info block.
 *
 * The offset is inside the image's first 256 bytes on purpose, so the block
 * reaches the flash in the same program as the vector table it describes:
 * anything written later could be the part an interruption leaves out, and a
 * check the board cannot find is a check it does not make. The host's
 * _Static_assert in ubertooth-dfu.c holds the two together.
 *
 * Backward compatibility, both ways round. An image from an earlier build
 * carries no block, and a bootloader that finds no magic makes neither check,
 * so it starts what it always started. An image built from this tree but
 * written by an earlier ubertooth-dfu carries the block with its flags left
 * at zero, which reads the same way. And a bootloader from an earlier release
 * ignores these 32 bytes, so an image written by this tool still starts on the
 * board it is already on.
 */
#define UBERTOOTH_IMAGE_CHECK_OFFSET 0xe0
/* The 8 bytes a block starts with, no terminator. */
#define UBERTOOTH_IMAGE_CHECK_MAGIC "UBTIMGCK"
#define UBERTOOTH_IMAGE_CHECK_MAGIC_LEN 8
/*
 * The layout below. A block with any other number is not read, and a reader
 * that cannot read the block makes no check: that is what lets a bootloader
 * tolerate an image newer than itself.
 *
 * It is worth knowing what that costs before bumping this. Every bootloader
 * already installed reads format 1 and nothing else, so an image in format 2
 * gets no checks at all on those boards -- and a bootloader is installed with
 * ISP or JTAG, so they will not be updated. A new field belongs in the
 * reserved bytes, or behind a new flag bit, rather than in a new format,
 * unless the point of the change is to stop older bootloaders checking.
 */
#define UBERTOOTH_IMAGE_CHECK_FORMAT 1
#define UBERTOOTH_IMAGE_CHECK_LENGTH 32

/* The checksum word at offset 0x1c is filled in, so the eight words of the
 * vector table add up to zero. A board refuses a flagged image whose words do
 * not, including one whose word is zero. */
#define UBERTOOTH_IMAGE_CHECK_VECTOR 0x0001u
/* length, length_complement and checksum are filled in, so a board can put
 * every byte that was programmed to the checksum before it starts any of
 * them. */
#define UBERTOOTH_IMAGE_CHECK_REGION 0x0002u

/*
 * The block as it sits in an image: 32 bytes, no padding, every multi-byte
 * field little-endian as the ARM stores it.
 *
 * length is how many bytes were programmed from the start of the application
 * region, and checksum is the two's complement of the sum of that many bytes
 * read as little-endian 32-bit words, so the whole of what was written adds up
 * to zero. The checksum word is one of the words in that sum, in the same
 * arrangement as the vector checksum at 0x1c.
 *
 * length_complement is the bitwise complement of length, and both have to
 * agree before a board uses either. A bootloader addresses flash with this
 * length, and a length of its own is one word that a program interrupted part
 * way through could leave at any value; two words that still agree is a
 * different matter.
 */
struct ubertooth_image_check_block {
	uint8_t magic[UBERTOOTH_IMAGE_CHECK_MAGIC_LEN];
	uint16_t format_version;
	uint16_t flags;
	uint32_t length;
	uint32_t length_complement;
	uint32_t checksum;
	uint8_t reserved[8];
};

/* Where each field starts, for a reader working on raw bytes. */
#define UBERTOOTH_IMAGE_CHECK_OFF_MAGIC 0
#define UBERTOOTH_IMAGE_CHECK_OFF_FORMAT 8
#define UBERTOOTH_IMAGE_CHECK_OFF_FLAGS 10
#define UBERTOOTH_IMAGE_CHECK_OFF_LENGTH 12
#define UBERTOOTH_IMAGE_CHECK_OFF_LENGTH_COMPLEMENT 16
#define UBERTOOTH_IMAGE_CHECK_OFF_CHECKSUM 20
#define UBERTOOTH_IMAGE_CHECK_OFF_RESERVED 24

/* The layout is the wire format, so a compiler that padded the struct would
 * silently move every field after the padding. */
typedef char ubertooth_image_check_block_is_32_bytes[
	sizeof(struct ubertooth_image_check_block) == UBERTOOTH_IMAGE_CHECK_LENGTH ? 1 : -1];

/* The block this image carries. Defined in firmware/common/image_check.c,
 * which every run-time image links. */
extern const struct ubertooth_image_check_block image_check_block;

/*
 * The sum the region checksum is worked out from: length bytes read as
 * little-endian 32-bit words, added up and allowed to wrap.
 *
 * ubertooth-dfu calls this over the image it is about to write, and stores the
 * two's complement of what it gets. The bootloader adds the same words up
 * straight out of flash, where they are already 32-bit values in the
 * processor's own order, so it has a loop of its own
 * (firmware/bootloader/app_image.h, app_image_region_sum): at every power-up
 * it reads tens of thousands of words, and reading them a byte at a time
 * would cost four times the work for the same answer.
 *
 * firmware/tests/test_bootloader_app_image.cpp puts the two against each
 * other on the same bytes. They have to agree: a board whose sum differs from
 * the host's refuses the image it was just given.
 *
 * length is rounded down to a whole number of words.
 */
static inline uint32_t ubertooth_image_check_sum(const uint8_t *bytes,
                                                 size_t length)
{
	uint32_t sum = 0;
	size_t i;

	for (i = 0; i + 4 <= length; i += 4)
		sum += (uint32_t)bytes[i] | ((uint32_t)bytes[i + 1] << 8) |
		       ((uint32_t)bytes[i + 2] << 16) |
		       ((uint32_t)bytes[i + 3] << 24);
	return sum;
}

/*
 * The bootloader version block: what a board's bootloader says about itself.
 *
 * A bootloader is installed over ISP or JTAG, not over USB, so it does not
 * change when the application is reflashed and nothing in an ordinary
 * firmware update says which one a board has. The block is
 * UBERTOOTH_BL_VERSION_LENGTH bytes at a fixed offset inside the bootloader's
 * region of flash: magic, the layout version, the bootloader's own version,
 * and the revision it was built from. firmware/bootloader/bl_version.c fills
 * it in and firmware/common/sections.ld puts it there.
 *
 * It sits at UBERTOOTH_FW_INFO_OFFSET, the same offset as the firmware info
 * block, on purpose. No image carries both -- the bootloader carries no
 * firmware info block, and no application carries this one -- so the two
 * share the gap sections.ld already leaves between the vector table and the
 * code, and an ASSERT there refuses an image that somehow carried both. A
 * reader takes the bytes at the offset and tells the two blocks apart by
 * their magic. The image check block above is at its own offset and is in no
 * image this one is in, so the three never overlap.
 *
 * The factory bootloader, and every bootloader built before this block, have
 * their own code at the offset instead, so a reader that finds no magic
 * learns nothing from those bytes beyond which generation they belong to.
 * That is what the hash in UBERTOOTH_GET_BOOTLOADER_INFO is for.
 */
#define UBERTOOTH_BL_VERSION_OFFSET UBERTOOTH_FW_INFO_OFFSET
/* The 8 bytes a block starts with, no terminator. */
#define UBERTOOTH_BL_VERSION_MAGIC "UBTBLVER"
#define UBERTOOTH_BL_VERSION_MAGIC_LEN 8
/* The layout below. A block with any other number is not read. */
#define UBERTOOTH_BL_VERSION_FORMAT 1
#define UBERTOOTH_BL_VERSION_BUILD_LEN 32
#define UBERTOOTH_BL_VERSION_LENGTH 48

/*
 * The version the bootloader puts in its block, binary-coded decimal like the
 * API version above: 0x0200 reads as 2.00.
 *
 * It starts at 2.00 rather than 1.00. The first bootloader to carry a block
 * at all is the one the first fork release ships, which takes 2.00 as the
 * firmware API does, so the numbering says which generation a bootloader
 * belongs to: a board with no block carries one from before the block
 * existed, which is the 1.x generation, and -B says so.
 *
 * This is not the bootloader's USB bcdDevice, which stays at 0x0100
 * (UBERTOOTH_BOOTLOADER_API_VERSION) because that number is the DFU protocol
 * version and ubertooth-dfu has to work against any board. Nothing refuses a
 * board over this number either. It is here so that a released bootloader can
 * be named in one word beside the revision it was built from, and so raising
 * it is the one edit a release needs. Raise it when a release ships a
 * bootloader that behaves differently from the last.
 */
#define UBERTOOTH_BOOTLOADER_VERSION 0x0200

/*
 * The block as it sits in flash: 48 bytes, no padding, the two 16-bit fields
 * little-endian as the ARM stores them. build is padded with zeros and is
 * only terminated when it is shorter than the field, so a reader must not
 * treat it as a C string -- ubertooth_bl_version_parse() below copies it out
 * safely.
 */
struct ubertooth_bl_version_block {
	uint8_t magic[UBERTOOTH_BL_VERSION_MAGIC_LEN];
	uint16_t format_version;
	uint16_t version;
	uint8_t reserved[4];
	char build[UBERTOOTH_BL_VERSION_BUILD_LEN];
};

/* Where each field starts, for a reader working on raw bytes. */
#define UBERTOOTH_BL_VERSION_OFF_MAGIC 0
#define UBERTOOTH_BL_VERSION_OFF_FORMAT 8
#define UBERTOOTH_BL_VERSION_OFF_VERSION 10
#define UBERTOOTH_BL_VERSION_OFF_RESERVED 12
#define UBERTOOTH_BL_VERSION_OFF_BUILD 16

/* The layout is the wire format, so a compiler that padded the struct would
 * silently move every field after the padding. */
typedef char ubertooth_bl_version_block_is_48_bytes[
	sizeof(struct ubertooth_bl_version_block) == UBERTOOTH_BL_VERSION_LENGTH ? 1 : -1];

/* The block this image carries. Defined in firmware/bootloader/bl_version.c,
 * which only the bootloader links. */
extern const struct ubertooth_bl_version_block bl_version_block;

/*
 * The bootloader's region of flash: the 16 KiB below the application, which
 * the running application reads as ordinary memory. BOOTLOADER_OFFSET and
 * BOOTLOADER_SIZE in host/ubertooth-tools/src/dfu.h are the same two numbers
 * seen from the host, and APP_IMAGE_BASE in firmware/bootloader/app_image.h
 * is where the region ends.
 */
#define UBERTOOTH_BOOTLOADER_REGION_START 0x00000000u
#define UBERTOOTH_BOOTLOADER_REGION_LENGTH 0x4000u

/*
 * The code-read-protection word. The LPC boot ROM reads the word at this
 * offset in flash and, for a handful of particular values, locks the part's
 * debug and programming interfaces (NXP LPC176x/5x user manual UM10360,
 * "Code Read Protection"). Nothing in this tree asks for that, so the four
 * bytes must not be written by accident: no block may be placed over them.
 */
#define UBERTOOTH_LPC_CRP_OFFSET 0x2fc
#define UBERTOOTH_LPC_CRP_LENGTH 4

/*
 * The four values that mean something to the boot ROM, from the same chapter
 * of the manual. Any other word, including the 0xffffffff of erased flash and
 * the zero an image that does not use the word leaves, asks for no protection
 * at all.
 *
 * CRP1 and CRP2 shut the debug interface and leave a reduced set of serial
 * programming commands; CRP3 shuts serial programming as well, so the part
 * can only ever run the code already in it; NO_ISP turns the entry pin into
 * an ordinary input, so bridging it no longer reaches the ROM's programmer.
 * None of them can be undone from outside the part: with CRP3 or NO_ISP there
 * is no way back in at all unless the code in flash offers one.
 *
 * That is why these are refused rather than passed on. Everything else in
 * this tree can be put right by writing flash again.
 */
#define UBERTOOTH_LPC_CRP1   0x12345678u
#define UBERTOOTH_LPC_CRP2   0x87654321u
#define UBERTOOTH_LPC_CRP3   0x43218765u
#define UBERTOOTH_LPC_NO_ISP 0x4e697370u

/* Does this word ask the boot ROM for protection? */
static inline int ubertooth_lpc_crp_is_protection(uint32_t word)
{
	return word == UBERTOOTH_LPC_CRP1 || word == UBERTOOTH_LPC_CRP2 ||
	       word == UBERTOOTH_LPC_CRP3 || word == UBERTOOTH_LPC_NO_ISP;
}

/* What to call it in a refusal. NULL for a word that asks for nothing, so a
 * caller cannot print a name for a value that has none. */
static inline const char *ubertooth_lpc_crp_name(uint32_t word)
{
	switch (word) {
	case UBERTOOTH_LPC_CRP1:
		return "CRP1";
	case UBERTOOTH_LPC_CRP2:
		return "CRP2";
	case UBERTOOTH_LPC_CRP3:
		return "CRP3";
	case UBERTOOTH_LPC_NO_ISP:
		return "NO_ISP";
	default:
		return NULL;
	}
}

/*
 * The bootloader updater image: how a host finds the bootloader it carries.
 *
 * The updater is an ordinary application image, written over USB like any
 * firmware. What makes it the updater is its firmware info block, whose image
 * name is UBERTOOTH_BL_UPDATE_IMAGE_NAME, and the 16 KiB of bootloader it
 * carries at a fixed offset from the start of the image.
 *
 * The offset is fixed for the same reason the other blocks' offsets are: a
 * host reads the bootloader out of the file without searching it, so
 * ubertooth-dfu can say what the board's bootloader region will hold before
 * anything is written. firmware/common/sections.ld puts the payload there and
 * firmware/tests/test_bl_update.c reads the built image back to show it is
 * where these numbers say.
 *
 * The payload is the whole region, padded to 16 KiB with the 0xff of erased
 * flash, rather than just the bootloader's own bytes. One length means the
 * updater writes all four sectors the same way, and it means a region
 * installed over USB reads exactly as the same bootloader installed over ISP
 * does, so one bootloader has one hash however it got onto the board.
 */
#define UBERTOOTH_BL_UPDATE_IMAGE_NAME "bl_update"
#define UBERTOOTH_BL_UPDATE_PAYLOAD_OFFSET 0x200
#define UBERTOOTH_BL_UPDATE_PAYLOAD_LENGTH UBERTOOTH_BOOTLOADER_REGION_LENGTH

/*
 * Why an update was refused.
 *
 * The updater on the board and ubertooth-dfu make the same checks over the
 * same bytes -- the host before it writes anything, the board again before it
 * programs -- so they share one list of answers and one set of words for
 * them. A disagreement between the two is then a disagreement about the
 * bytes, not about what the bytes mean.
 *
 * UBERTOOTH_BL_UPDATE_OK is a payload worth writing. UBERTOOTH_BL_UPDATE_SAME
 * is not a refusal: the region already holds these bytes, so there is nothing
 * to write and the board is returned to DFU as if the write had happened.
 */
enum ubertooth_bl_update_result {
	UBERTOOTH_BL_UPDATE_OK = 0,
	UBERTOOTH_BL_UPDATE_SAME = 1,
	/* The payload */
	UBERTOOTH_BL_UPDATE_SHORT = -1,
	UBERTOOTH_BL_UPDATE_NO_VERSION_BLOCK = -2,
	UBERTOOTH_BL_UPDATE_CRP = -3,
	UBERTOOTH_BL_UPDATE_VECTORS = -4,
	/* The part */
	UBERTOOTH_BL_UPDATE_PART_UNREADABLE = -5,
	UBERTOOTH_BL_UPDATE_PART_TOO_SMALL = -6,
	/* The write */
	UBERTOOTH_BL_UPDATE_PREPARE_FAILED = -7,
	UBERTOOTH_BL_UPDATE_ERASE_FAILED = -8,
	UBERTOOTH_BL_UPDATE_WRITE_FAILED = -9,
	UBERTOOTH_BL_UPDATE_READ_BACK_FAILED = -10,
	/* The file, which only the host reads */
	UBERTOOTH_BL_UPDATE_NOT_UPDATER = -11,
	UBERTOOTH_BL_UPDATE_FILE_SHORT = -12,
	/* A caller that asked for the write without lending the RAM it is
	 * programmed from. Its own answer, so a mistake in the one program that
	 * installs a bootloader is not reported to a user as something wrong
	 * with the bootloader. */
	UBERTOOTH_BL_UPDATE_NO_BUFFER = -13,
};

/*
 * The word of a bootloader's vector table the boot ROM reads, and how many
 * words it adds up: the word at offset 0x1c holds the two's complement of the
 * seven in front of it, so all eight come to zero (UM10360, "Criterion for
 * valid user code"). A part whose word at address 0 does not satisfy that
 * starts the ROM's serial programmer instead of the flash.
 *
 * The same arrangement as the application's vector checksum one region higher
 * (firmware/bootloader/app_image.h, and fill_vector_checksum() in
 * ubertooth-dfu.c).
 */
#define UBERTOOTH_BL_UPDATE_CHECKSUM_INDEX 7u
#define UBERTOOTH_BL_UPDATE_CHECKED_WORDS 8u

/*
 * Where a bootloader's initial stack pointer may point. Looser than any part
 * in this family is, for the reason firmware/bootloader/app_image.h gives for
 * the application's version of this check: what it is for is erased flash and
 * zeros, and both are far outside it.
 */
#define UBERTOOTH_BL_UPDATE_RAM_BASE 0x10000000u
#define UBERTOOTH_BL_UPDATE_RAM_LIMIT 0x10010000u

/* The checksum word a bootloader's vector table needs. words is the first
 * UBERTOOTH_BL_UPDATE_CHECKED_WORDS * 4 bytes of the region. */
static inline uint32_t ubertooth_bl_update_vector_checksum(const uint8_t *words)
{
	uint32_t sum = 0;
	unsigned i;

	for (i = 0; i < UBERTOOTH_BL_UPDATE_CHECKSUM_INDEX; i++)
		sum += (uint32_t)words[i * 4] |
		       ((uint32_t)words[i * 4 + 1] << 8) |
		       ((uint32_t)words[i * 4 + 2] << 16) |
		       ((uint32_t)words[i * 4 + 3] << 24);
	/* Unsigned throughout: negating a signed value this large is not
	 * defined. */
	return 0u - sum;
}

/*
 * One byte of the region as it would be programmed: the payload's own byte,
 * except in the checksum word, which is filled in.
 *
 * Both the board and the host need the programmed bytes rather than the
 * file's: the board programs them and reads them back, and the host works out
 * the hash the board will report afterwards. One function so the two cannot
 * differ by that one word.
 */
static inline uint8_t ubertooth_bl_update_byte(const uint8_t *payload,
                                               size_t offset, uint32_t checksum)
{
	const size_t word = UBERTOOTH_BL_UPDATE_CHECKSUM_INDEX * 4;

	if (offset >= word && offset < word + 4)
		return (uint8_t)(checksum >> ((offset - word) * 8));
	return payload[offset];
}

/*
 * Everything that can be asked of a bootloader's 16 KiB before any part is
 * touched: it is the whole region, it carries a version block so the
 * installed bootloader can report itself, it asks for no code read
 * protection, and its vector table is one a part would start.
 *
 * *version, when it is not NULL, gets the version the payload's own version
 * block gives, or 0 when there is none.
 *
 * Whether the board already holds these bytes is a separate question, asked
 * by bl_update_region_matches() in firmware/common/bl_update.c, because that
 * one has to read the board's region at flash address 0 and an address is not
 * a pointer this side of the bus.
 *
 * This is here rather than in the firmware, because the firmware and
 * ubertooth-dfu both make these checks over the same bytes -- the host before
 * it writes the updater to a board, the board again before it programs -- and
 * two implementations of them is the one thing this path must not have.
 */
static inline int ubertooth_bl_update_payload_check(const uint8_t *payload,
                                                    size_t length,
                                                    uint16_t *version)
{
	static const char magic[] = UBERTOOTH_BL_VERSION_MAGIC;
	const uint8_t *block;
	uint32_t stack_pointer, reset_vector, entry;
	size_t i;

	if (version != NULL)
		*version = 0;
	if (payload == NULL || length != UBERTOOTH_BL_UPDATE_PAYLOAD_LENGTH)
		return UBERTOOTH_BL_UPDATE_SHORT;

	/* The protection word first. It is the one mistake with no way back, so
	 * a payload that is wrong in some other way as well is still reported as
	 * the protection it asks for: that is the reason a user has to be
	 * given. */
	if (ubertooth_lpc_crp_is_protection(
			(uint32_t)payload[UBERTOOTH_LPC_CRP_OFFSET] |
			((uint32_t)payload[UBERTOOTH_LPC_CRP_OFFSET + 1] << 8) |
			((uint32_t)payload[UBERTOOTH_LPC_CRP_OFFSET + 2] << 16) |
			((uint32_t)payload[UBERTOOTH_LPC_CRP_OFFSET + 3] << 24)))
		return UBERTOOTH_BL_UPDATE_CRP;

	/* A bootloader with no version block is not refused because it would
	 * not work -- it would -- but because installing one leaves a board that
	 * reports "no version block", which is also what a factory bootloader
	 * reports. An update must not make a board harder to identify than it
	 * was. */
	block = payload + UBERTOOTH_BL_VERSION_OFFSET;
	for (i = 0; i < UBERTOOTH_BL_VERSION_MAGIC_LEN; i++) {
		if (block[i] != (uint8_t)magic[i])
			return UBERTOOTH_BL_UPDATE_NO_VERSION_BLOCK;
	}
	if (version != NULL)
		*version = (uint16_t)(block[UBERTOOTH_BL_VERSION_OFF_VERSION] |
			(block[UBERTOOTH_BL_VERSION_OFF_VERSION + 1] << 8));

	/*
	 * The two words the processor loads first: a word-aligned stack pointer
	 * in RAM, and a Thumb reset vector inside the bootloader's own region,
	 * above the records at the bottom of it. Erased flash and zeros both
	 * fail it.
	 *
	 * The floor is the end of the version block, and it is not arbitrary: a
	 * payload that gets this far carries a version block at
	 * UBERTOOTH_BL_VERSION_OFFSET, checked just above, so no code of its own
	 * can be there or below it. An entry inside the vector table or inside
	 * the block is a reset vector no bootloader has, and the checksum this
	 * code then fills in would make the boot ROM start it anyway -- into
	 * table entries rather than instructions, on a board with no USB left to
	 * recover it.
	 */
	stack_pointer = (uint32_t)payload[0] | ((uint32_t)payload[1] << 8) |
		((uint32_t)payload[2] << 16) | ((uint32_t)payload[3] << 24);
	reset_vector = (uint32_t)payload[4] | ((uint32_t)payload[5] << 8) |
		((uint32_t)payload[6] << 16) | ((uint32_t)payload[7] << 24);
	if (stack_pointer <= UBERTOOTH_BL_UPDATE_RAM_BASE ||
	    stack_pointer > UBERTOOTH_BL_UPDATE_RAM_LIMIT ||
	    (stack_pointer & 3u) != 0)
		return UBERTOOTH_BL_UPDATE_VECTORS;
	if ((reset_vector & 1u) == 0)
		return UBERTOOTH_BL_UPDATE_VECTORS;
	entry = reset_vector & ~1u;
	if (entry < UBERTOOTH_BOOTLOADER_REGION_START +
	            UBERTOOTH_BL_VERSION_OFFSET + UBERTOOTH_BL_VERSION_LENGTH)
		return UBERTOOTH_BL_UPDATE_VECTORS;
	if (entry >= UBERTOOTH_BOOTLOADER_REGION_START +
	             UBERTOOTH_BOOTLOADER_REGION_LENGTH)
		return UBERTOOTH_BL_UPDATE_VECTORS;
	return UBERTOOTH_BL_UPDATE_OK;
}

/* One phrase naming what was found. Written so that it reads after "the
 * update was refused: ". */
static inline const char *ubertooth_bl_update_result_string(int result)
{
	switch (result) {
	case UBERTOOTH_BL_UPDATE_OK:
		return "the bootloader can be written";
	case UBERTOOTH_BL_UPDATE_SAME:
		return "the board already has this bootloader";
	case UBERTOOTH_BL_UPDATE_SHORT:
		return "the bootloader it carries is not 16 KiB";
	case UBERTOOTH_BL_UPDATE_NO_VERSION_BLOCK:
		return "the bootloader it carries has no version block, so a board "
		       "flashed with it could not report which bootloader it has";
	case UBERTOOTH_BL_UPDATE_CRP:
		return "the bootloader it carries asks for code read protection, "
		       "which cannot be undone";
	case UBERTOOTH_BL_UPDATE_VECTORS:
		return "the bootloader it carries has no vector table a part would "
		       "start";
	case UBERTOOTH_BL_UPDATE_PART_UNREADABLE:
		return "the part would not say which part it is";
	case UBERTOOTH_BL_UPDATE_PART_TOO_SMALL:
		return "the part has no flash for the whole bootloader region";
	case UBERTOOTH_BL_UPDATE_PREPARE_FAILED:
		return "the part would not prepare the bootloader sectors";
	case UBERTOOTH_BL_UPDATE_ERASE_FAILED:
		return "the part would not erase the bootloader sectors";
	case UBERTOOTH_BL_UPDATE_WRITE_FAILED:
		return "the part would not program the bootloader sectors";
	case UBERTOOTH_BL_UPDATE_READ_BACK_FAILED:
		return "the bootloader read back from flash is not the one written";
	case UBERTOOTH_BL_UPDATE_NOT_UPDATER:
		return "this image is not a bootloader updater";
	case UBERTOOTH_BL_UPDATE_NO_BUFFER:
		return "there is no buffer to program the bootloader from";
	default:
		return "this file is too short to hold a bootloader updater";
	}
}

/*
 * UBERTOOTH_GET_BOOTLOADER_INFO's reply: the CRC-32 of the whole bootloader
 * region first, as four bytes with the low byte first, then the version block
 * -- or that many zeros when the region carries no block.
 *
 * The hash is always there, which is the point of it: a bootloader with no
 * block is still recognizable, because two boards that answer with the same
 * hash are running the same bootloader. The application computes it over the
 * region as it finds it, so it says nothing about what the bytes mean.
 */
#define UBERTOOTH_BL_REPORT_OFF_CRC 0
#define UBERTOOTH_BL_REPORT_OFF_BLOCK 4
#define UBERTOOTH_BL_REPORT_LENGTH (4 + UBERTOOTH_BL_VERSION_LENGTH)

/* A version block read off a board, with the build string terminated. */
struct ubertooth_bl_version {
	uint16_t format_version;
	uint16_t version;
	char build[UBERTOOTH_BL_VERSION_BUILD_LEN + 1];
};

/* Everything a board says about its bootloader. */
struct ubertooth_bootloader_info {
	uint32_t crc32;     /* of the whole region, always read */
	/* UBERTOOTH_BL_VERSION_OK when version below was read, or a negative
	 * enum ubertooth_bl_version_result saying why it was not. */
	int version_result;
	struct ubertooth_bl_version version;
};

/* Why a block was not read. The caller names the board, so these say only
 * what was wrong with the bytes. */
enum ubertooth_bl_version_result {
	UBERTOOTH_BL_VERSION_OK = 0,
	UBERTOOTH_BL_VERSION_SHORT = -1,    /* too few bytes to read */
	UBERTOOTH_BL_VERSION_NO_MAGIC = -2, /* no block here */
	UBERTOOTH_BL_VERSION_FORMAT_UNKNOWN = -3,
	UBERTOOTH_BL_VERSION_BAD_TEXT = -4, /* an empty or unprintable string */
};

/*
 * Reads the version block at raw into *out: the magic, the format version,
 * the bootloader's version and the build string. length is how many bytes are
 * there to read. Returns UBERTOOTH_BL_VERSION_OK, or a negative enum
 * ubertooth_bl_version_result. On failure *out is zeroed, except that an
 * unknown format keeps the format number it found, for a caller that wants to
 * name it.
 *
 * No magic is the ordinary answer from a bootloader of the 1.x generation,
 * which is every board as shipped, not a fault: the region simply has code
 * where a block would be.
 */
static inline int ubertooth_bl_version_parse(const uint8_t *raw, size_t length,
                                             struct ubertooth_bl_version *out)
{
	static const char magic[] = UBERTOOTH_BL_VERSION_MAGIC;
	size_t i;

	if (raw == NULL || out == NULL)
		return UBERTOOTH_BL_VERSION_SHORT;
	for (i = 0; i < sizeof(*out); i++)
		((uint8_t *)out)[i] = 0;
	if (length < UBERTOOTH_BL_VERSION_LENGTH)
		return UBERTOOTH_BL_VERSION_SHORT;
	for (i = 0; i < UBERTOOTH_BL_VERSION_MAGIC_LEN; i++) {
		if (raw[UBERTOOTH_BL_VERSION_OFF_MAGIC + i] != (uint8_t)magic[i])
			return UBERTOOTH_BL_VERSION_NO_MAGIC;
	}
	out->format_version = (uint16_t)(raw[UBERTOOTH_BL_VERSION_OFF_FORMAT] |
		(raw[UBERTOOTH_BL_VERSION_OFF_FORMAT + 1] << 8));
	if (out->format_version != UBERTOOTH_BL_VERSION_FORMAT) {
		uint16_t found = out->format_version;

		for (i = 0; i < sizeof(*out); i++)
			((uint8_t *)out)[i] = 0;
		out->format_version = found;
		return UBERTOOTH_BL_VERSION_FORMAT_UNKNOWN;
	}
	out->version = (uint16_t)(raw[UBERTOOTH_BL_VERSION_OFF_VERSION] |
		(raw[UBERTOOTH_BL_VERSION_OFF_VERSION + 1] << 8));
	/* The same field reader the firmware info block uses: both blocks pad
	 * their strings with zeros and terminate them only when they are
	 * short. */
	if (ubertooth_fw_info_text(raw + UBERTOOTH_BL_VERSION_OFF_BUILD,
	                           UBERTOOTH_BL_VERSION_BUILD_LEN,
	                           out->build, sizeof(out->build)) != 0) {
		for (i = 0; i < sizeof(*out); i++)
			((uint8_t *)out)[i] = 0;
		return UBERTOOTH_BL_VERSION_BAD_TEXT;
	}
	return UBERTOOTH_BL_VERSION_OK;
}

/*
 * Reads a whole UBERTOOTH_BL_REPORT_LENGTH-byte reply into *out: the hash,
 * then the block behind it. Returns 0 when the hash was read, so that a board
 * with no version block is a success with out->version_result saying so, or
 * UBERTOOTH_BL_VERSION_SHORT when the reply was too short to hold even the
 * hash.
 */
static inline int ubertooth_bootloader_info_parse(const uint8_t *raw,
                                                  size_t length,
                                                  struct ubertooth_bootloader_info *out)
{
	size_t i;

	if (raw == NULL || out == NULL)
		return UBERTOOTH_BL_VERSION_SHORT;
	for (i = 0; i < sizeof(*out); i++)
		((uint8_t *)out)[i] = 0;
	if (length < UBERTOOTH_BL_REPORT_LENGTH) {
		out->version_result = UBERTOOTH_BL_VERSION_SHORT;
		return UBERTOOTH_BL_VERSION_SHORT;
	}
	out->crc32 = (uint32_t)raw[UBERTOOTH_BL_REPORT_OFF_CRC] |
		((uint32_t)raw[UBERTOOTH_BL_REPORT_OFF_CRC + 1] << 8) |
		((uint32_t)raw[UBERTOOTH_BL_REPORT_OFF_CRC + 2] << 16) |
		((uint32_t)raw[UBERTOOTH_BL_REPORT_OFF_CRC + 3] << 24);
	out->version_result =
		ubertooth_bl_version_parse(raw + UBERTOOTH_BL_REPORT_OFF_BLOCK,
		                           length - UBERTOOTH_BL_REPORT_OFF_BLOCK,
		                           &out->version);
	return 0;
}

/* One phrase naming what was found, for a caller's report. The no-magic
 * phrase is what a user sees for a factory bootloader, so it reads as an
 * answer rather than an error. */
static inline const char *ubertooth_bl_version_result_string(int result)
{
	switch (result) {
	case UBERTOOTH_BL_VERSION_OK:
		return "the bootloader version block was read";
	case UBERTOOTH_BL_VERSION_NO_MAGIC:
		return "no version block (factory or earlier, treated as 1.x)";
	case UBERTOOTH_BL_VERSION_FORMAT_UNKNOWN:
		return "the bootloader version block is in a format this host does not know";
	case UBERTOOTH_BL_VERSION_BAD_TEXT:
		return "the bootloader version block holds no readable build string";
	default:
		return "there is not a whole bootloader version block to read";
	}
}

/*
 * LED spectrum analyzer (ubertooth-util -q N): the threshold is -N dBm. The
 * CC2400 measures RSSI from about -120 to -20 dBm, so N outside 20-120 is
 * either never or always reached. The default, -85 dBm, shows Wi-Fi activity
 * without lighting on noise (the original 2011 default, raw 225 = -85 dBm).
 */
#define UBERTOOTH_LED_SPECAN_N_MIN 20
#define UBERTOOTH_LED_SPECAN_N_MAX 120
#define UBERTOOTH_LED_SPECAN_N_DEFAULT 85

/* How often the identify blink toggles the USR/RX/TX LEDs, in milliseconds:
 * about three on-off blinks a second. */
#define UBERTOOTH_IDENTIFY_TOGGLE_MS 167

#define DMA_SIZE 50

/* The most bytes one captured low-energy packet can be: the access address
 * (4), the PDU header (2), a payload as long as the header's 8-bit length
 * field can claim (255) and the CRC (3).
 *
 * Bluetooth 4.2 Data Length Extension allows 251 payload bytes, so a valid
 * packet is at most 260. The firmware captures whatever length the header
 * claims, so the four bytes above that are carried too rather than cut off
 * mid-record; the decoder then ignores them, because it clamps a payload to
 * the 251 the specification allows. */
#define LE_MAX_USB_PACKET_SIZE 264

#define NUM_BREDR_CHANNELS 79

/* The Bluetooth band: channel 0 is 2402 MHz, channel 78 is 2480 MHz. */
#define BT_FREQ_MIN 2402
#define BT_FREQ_MAX (BT_FREQ_MIN + NUM_BREDR_CHANNELS - 1)

/* What usb_pkt_rx.channel says when the board was tuned outside the Bluetooth
 * band, where there is no channel to name. Every host path that indexes a
 * per-channel array with the field drops a record carrying it, as it does any
 * value above channel 78. */
#define CHANNEL_NONE 0xff

/* The channel a record reports, as the one-byte offset from 2402 MHz that
 * usb_pkt_rx carries. That byte only reaches to channel 78, while the channel
 * request accepts any frequency the radio tunes, 2268 to 2794 MHz: an
 * out-of-band frequency subtracted and narrowed would land on some other
 * channel's number, and 2658 to 2736 MHz would land inside the band, where the
 * host would take it for a real channel and index its per-channel arrays with
 * it. CHANNEL_NONE instead, which is outside the band like every value above
 * 78, so the host's own range checks already drop it.
 *
 * Here rather than in one of the firmware images because both of them fill
 * that field, from four places, and the field is defined here. */
static inline uint8_t ubertooth_channel_byte(uint16_t freq)
{
	if (freq < BT_FREQ_MIN || freq > BT_FREQ_MAX)
		return CHANNEL_NONE;
	return (uint8_t)(freq - BT_FREQ_MIN);
}

/* The centre frequency of each low-energy advertising channel, named by the
 * channel index the Bluetooth Core specification gives it. Index 37 sits at
 * the bottom of the band and index 39 at the top, so that interference in the
 * middle of the band cannot hide all three; index 38 sits in the middle, where
 * the data channels leave a gap.
 *
 * Low energy and classic Bluetooth share the band, so 37 and 39 land on the
 * same two frequencies as the classic edges above. That is why they are
 * written as those constants: one band, named once. */
#define BTLE_ADV_CHANNEL_37_MHZ BT_FREQ_MIN
#define BTLE_ADV_CHANNEL_38_MHZ 2426
#define BTLE_ADV_CHANNEL_39_MHZ BT_FREQ_MAX

/*
 * CLK_TUNE_TIME is the duration in units of 100 ns that we reserve for tuning
 * the radio while frequency hopping.  We start the tuning process
 * CLK_TUNE_TIME * 100 ns prior to the start of an upcoming time slot.
*/
#define CLK_TUNE_TIME   2250
#define CLK_TUNE_OFFSET  200

enum ubertooth_usb_commands {
	UBERTOOTH_PING               = 0,
	UBERTOOTH_RX_SYMBOLS         = 1,
	UBERTOOTH_TX_SYMBOLS         = 2,
	UBERTOOTH_GET_USRLED         = 3,
	UBERTOOTH_SET_USRLED         = 4,
	UBERTOOTH_GET_RXLED          = 5,
	UBERTOOTH_SET_RXLED          = 6,
	UBERTOOTH_GET_TXLED          = 7,
	UBERTOOTH_SET_TXLED          = 8,
	UBERTOOTH_GET_1V8            = 9,
	UBERTOOTH_SET_1V8            = 10,
	UBERTOOTH_GET_CHANNEL        = 11,
	UBERTOOTH_SET_CHANNEL        = 12,
	UBERTOOTH_RESET              = 13,
	UBERTOOTH_GET_SERIAL         = 14,
	UBERTOOTH_GET_PARTNUM        = 15,
	UBERTOOTH_GET_PAEN           = 16,
	UBERTOOTH_SET_PAEN           = 17,
	UBERTOOTH_GET_HGM            = 18,
	UBERTOOTH_SET_HGM            = 19,
	UBERTOOTH_TX_TEST            = 20,
	UBERTOOTH_STOP               = 21,
	UBERTOOTH_GET_MOD            = 22,
	UBERTOOTH_SET_MOD            = 23,
	UBERTOOTH_SET_ISP            = 24,
	UBERTOOTH_FLASH              = 25,
	BOOTLOADER_FLASH             = 26,
	UBERTOOTH_SPECAN             = 27,
	UBERTOOTH_GET_PALEVEL        = 28,
	UBERTOOTH_SET_PALEVEL        = 29,
	UBERTOOTH_REPEATER           = 30,
	UBERTOOTH_RANGE_TEST         = 31,
	UBERTOOTH_RANGE_CHECK        = 32,
	UBERTOOTH_GET_REV_NUM        = 33,
	UBERTOOTH_LED_SPECAN         = 34,
	UBERTOOTH_GET_BOARD_ID       = 35,
	UBERTOOTH_SET_SQUELCH        = 36,
	UBERTOOTH_GET_SQUELCH        = 37,
	UBERTOOTH_SET_BDADDR         = 38,
	UBERTOOTH_START_HOPPING      = 39,
	UBERTOOTH_SET_CLOCK          = 40,
	UBERTOOTH_GET_CLOCK          = 41,
	UBERTOOTH_BTLE_SNIFFING      = 42,
	UBERTOOTH_GET_ACCESS_ADDRESS = 43,
	UBERTOOTH_SET_ACCESS_ADDRESS = 44,
	/* UBERTOOTH_DO_SOMETHING = 45 and UBERTOOTH_DO_SOMETHING_REPLY = 46 were
	 * a pair of developer scratch requests that the firmware answered with a
	 * fixed two bytes. They are gone; the numbers are left unused so that a
	 * request number still means the same thing in any capture or log from an
	 * older build. */
	UBERTOOTH_GET_CRC_VERIFY     = 47,
	UBERTOOTH_SET_CRC_VERIFY     = 48,
	UBERTOOTH_POLL               = 49,
	UBERTOOTH_BTLE_PROMISC       = 50,
	UBERTOOTH_SET_AFHMAP         = 51,
	UBERTOOTH_CLEAR_AFHMAP       = 52,
	UBERTOOTH_READ_REGISTER      = 53,
	UBERTOOTH_BTLE_PERIPHERAL    = 54,
	UBERTOOTH_GET_COMPILE_INFO   = 55,
	UBERTOOTH_BTLE_SET_TARGET    = 56,
	UBERTOOTH_BTLE_PHY           = 57,
	UBERTOOTH_WRITE_REGISTER     = 58,
	UBERTOOTH_JAM_MODE           = 59,
	UBERTOOTH_EGO                = 60,
	UBERTOOTH_AFH                = 61,
	UBERTOOTH_HOP                = 62,
	UBERTOOTH_TRIM_CLOCK         = 63,
	// UBERTOOTH_GET_API_VERSION    = 64,
	UBERTOOTH_WRITE_REGISTERS    = 65,
	UBERTOOTH_READ_ALL_REGISTERS = 66,
	UBERTOOTH_RX_GENERIC         = 67,
	UBERTOOTH_TX_GENERIC_PACKET  = 68,
	UBERTOOTH_FIX_CLOCK_DRIFT    = 69,
	UBERTOOTH_CANCEL_FOLLOW      = 70,
	UBERTOOTH_LE_SET_ADV_DATA    = 71,
	UBERTOOTH_RFCAT_SUBCMD       = 72,
	UBERTOOTH_XMAS               = 73,
	UBERTOOTH_IDENTIFY           = 74,
	UBERTOOTH_GET_FW_INFO        = 75,
	UBERTOOTH_GET_WATCHDOG       = 76,
	UBERTOOTH_GET_RADIO_TIMEOUTS = 77,
	UBERTOOTH_GET_BOOTLOADER_INFO = 78,
};

/*
 * The modes the board can be in. The host asks for one with the request that
 * starts it, and reads the mode back out of the watchdog breadcrumb below, so
 * the numbers are part of the interface rather than private to the firmware.
 */
enum operating_modes {
	MODE_IDLE             = 0,
	MODE_RX_SYMBOLS       = 1,
	MODE_TX_SYMBOLS       = 2,
	MODE_TX_TEST          = 3,
	MODE_SPECAN           = 4,
	MODE_RANGE_TEST       = 5,
	MODE_REPEATER         = 6,
	MODE_LED_SPECAN       = 7,
	MODE_BT_FOLLOW        = 8,
	MODE_BT_FOLLOW_LE     = 9,
	MODE_BT_PROMISC_LE    = 10,
	MODE_RESET            = 11,
	MODE_BT_PERIPHERAL_LE = 12,
	MODE_EGO              = 13,
	MODE_AFH              = 14,
	MODE_RX_GENERIC       = 15,
	MODE_TX_GENERIC       = 16,
	MODE_XMAS             = 17,
	MODE_IDENTIFY         = 18,
	/* Handing the chip to its on-chip programmer, which is a mode of its
	 * own because the board answers the host first and goes off the bus a
	 * moment later, the way MODE_RESET does. */
	MODE_ISP              = 19,
	/* The btbr image has its own states and reports no mode at all. */
	MODE_UNKNOWN          = 255,
};

/*
 * How long the board may go without feeding the watchdog before it resets
 * itself.
 *
 * The number has to clear the longest gap the firmware leaves on purpose,
 * which is 1600 ms: xmas mode sleeps in a WFI until one of its three blink
 * timers fires, and each is set up to 400 + 1200 ms ahead (xmas.c), so a run
 * where all three land near the far end feeds only every 1600 ms. Identify
 * mode waits the same way, on a 167 ms blink. The other long waits all feed
 * while they wait: the repeater's 1000 ms turnaround services USB throughout
 * (cc2400_rangetest.c), the BLE modes feed while they wait for a packet or
 * the next hop, and the transmit loops feed once per burst. The second that
 * reset() spends waiting to be reset is excused by the intent instead.
 *
 * So 3000 ms: nearly double the longest deliberate gap, and still fast enough
 * that a wedged board is back before a person would reach for the cable.
 */
#define UBERTOOTH_WATCHDOG_TIMEOUT_MS 3000

/*
 * Why the board last started. The same bits as the part's reset source
 * register, so a value can be read straight out of it.
 */
enum watchdog_reset_cause {
	WATCHDOG_RESET_POWER_ON  = (1 << 0),
	WATCHDOG_RESET_EXTERNAL  = (1 << 1),
	WATCHDOG_RESET_WATCHDOG  = (1 << 2),
	WATCHDOG_RESET_BROWN_OUT = (1 << 3),
};

/*
 * Where the board was waiting. Every bounded wait in the firmware records its
 * own value here as it starts, so a watchdog reset says which wait the board
 * was in and not only which mode.
 */
enum watchdog_site {
	WATCHDOG_SITE_NONE       = 0, /* not in one of the waits below */
	WATCHDOG_SITE_USB        = 1, /* servicing USB */
	WATCHDOG_SITE_FS_LOCK    = 2, /* waiting for the synthesizer to lock */
	WATCHDOG_SITE_FS_UNLOCK  = 3, /* waiting for it to unlock */
	WATCHDOG_SITE_XOSC       = 4, /* waiting for the 16 MHz crystal oscillator */
	WATCHDOG_SITE_FSM        = 5, /* waiting on the radio's state machine */
	WATCHDOG_SITE_LE_RX      = 6, /* waiting for a BLE packet or the next hop */
	/*
	 * The four processor faults, one site each. A fault handler never
	 * returns, so the board is reset by the watchdog a few seconds later
	 * and these are read back after it reconnects.
	 *
	 * One site for all four would say no more than "a fault". Counting the
	 * handler's blink code used to tell you which fault it was, and the
	 * watchdog now cuts that code off part-way, so the breadcrumb carries
	 * what the blinks did.
	 */
	WATCHDOG_SITE_FAULT_HARD  = 7,  /* hard fault */
	WATCHDOG_SITE_FAULT_MEM   = 8,  /* memory management fault */
	WATCHDOG_SITE_FAULT_BUS   = 9,  /* bus fault */
	WATCHDOG_SITE_FAULT_USAGE = 10, /* usage fault */
	/* The btbr image's die(): firmware that found a state it cannot go on
	 * from, such as a message it cannot parse. Not a processor fault. */
	WATCHDOG_SITE_PANIC       = 11,
	/* Inside the low-energy engine's own interrupt handlers, which is not the
	 * same as the mode loop being held up in its USB service pass: before
	 * these, both read as "servicing USB". */
	WATCHDOG_SITE_LE_SSP      = 12, /* emptying the BLE engine's symbol buffer */
	WATCHDOG_SITE_LE_DMA      = 13, /* in the BLE engine's received-byte interrupt */
	WATCHDOG_SITE_LE_TIMER    = 14, /* in the BLE engine's timer interrupt */
	/*
	 * Two loops that never return and are not waits at all.
	 *
	 * The first is the shared default interrupt handler, which NMI, SVC,
	 * PendSV, SysTick and every peripheral interrupt the firmware does not
	 * handle are aliases of. Reaching it means an interrupt fired that
	 * nothing was written for, so it is worth saying rather than leaving
	 * the record naming whatever the board was doing before.
	 *
	 * The second is the debug UART's transfer error path, which stops the
	 * board with the user LED on.
	 */
	WATCHDOG_SITE_UNHANDLED_IRQ = 15, /* an interrupt nothing handles fired */
	WATCHDOG_SITE_DEBUG_DMA     = 16, /* the debug UART's transfer failed */
	/*
	 * Steps of a USB service pass. A board that stops answering stops inside
	 * that pass, and WATCHDOG_SITE_USB alone covers the whole of it: the
	 * handshakes with the USB controller, the control request the pass is
	 * carrying, and the firmware's own handler for that request. Each of
	 * those is bounded to a few milliseconds on paper, so a pass that holds
	 * the board for three seconds is one of them costing far more than its
	 * bound -- or the time going somewhere none of them accounts for.
	 *
	 * These say which. WATCHDOG_SITE_USB stays the reading for the rest of
	 * the pass: scanning the endpoints for one that needs service, and the
	 * register reads around that.
	 */
	WATCHDOG_SITE_USB_BULK    = 17, /* handing a capture packet to the bulk endpoint */
	WATCHDOG_SITE_USB_DEV     = 18, /* handling a USB bus event: reset, suspend, resume */
	WATCHDOG_SITE_USB_EP      = 19, /* reading an endpoint's status */
	WATCHDOG_SITE_USB_SETUP   = 20, /* reading a control request */
	/*
	 * Running a control request: the firmware's own handler for it. For
	 * Ubertooth's own requests that means code which reaches the radio and
	 * never the USB controller, which is what makes this site worth telling
	 * apart from the ones above. Realizing an endpoint is the exception and
	 * has a site of its own below, so it is not counted here.
	 */
	WATCHDOG_SITE_USB_REQUEST = 21,
	WATCHDOG_SITE_USB_REPLY   = 22, /* writing a control reply */
	/* Waiting for the controller to realize an endpoint: the longest wait
	 * there is on it, and the only one a control request can reach. The
	 * stack asks for it at startup and from SET_CONFIGURATION, which the
	 * host sends as it opens the device and after a bus reset. */
	WATCHDOG_SITE_USB_REALIZE = 23,
	WATCHDOG_SITE_COUNT
};

/*
 * UBERTOOTH_GET_WATCHDOG's reply: twenty-two bytes, in the order the fields
 * are listed in watchdog_info below. The first eight are single bytes; after
 * them come a two-byte value, two four-byte values and two more two-byte
 * values, each with its low byte first, so there is no padding to agree about
 * between the board and the host.
 *
 * A watchdog reset the firmware asked for (a reset or a flash request) is not
 * counted and does not overwrite reset_mode/reset_site, so those describe the
 * last time the board stopped servicing USB on its own.
 */
#define UBERTOOTH_WATCHDOG_INFO_LEN 22

/* How many bytes UBERTOOTH_RANGE_CHECK answers with, which is the
 * rangetest_result below written out one byte at a time. Shared, so a field
 * added to it is a compile error on the side that was not updated rather than
 * a short reply on the bench. */
#define UBERTOOTH_RANGE_RESULT_LEN 6

/* In watchdog_info.fault_status: the part's "the fault address register holds
 * this fault's address" bits. One of the two is set when fault_addr means
 * something.
 *
 * Which one depends on the register the board recorded. A memory management
 * or bus fault records its own eight-bit status, where bit 7 is that bit; a
 * hard fault that one of them escalated into records the whole configurable
 * fault status, where the memory management byte is the low one and the bus
 * fault byte the next, so that bit lands at 7 or at 15. Neither is set in the
 * hard fault and usage fault statuses, which have no address register. */
#define UBERTOOTH_FAULT_ADDR_VALID     0x0080
#define UBERTOOTH_FAULT_BUS_ADDR_VALID 0x8000

/*
 * UBERTOOTH_GET_RADIO_TIMEOUTS's reply: how many times each shape of wait on
 * the CC2400 has given up since the board started, in this order, each as two
 * bytes with the low byte first.
 *
 * The waits are bounded so that a radio which never reports the state it was
 * asked for cannot hold the board (firmware/common/cc2400_wait.c). Carrying on
 * is deliberate and silent, though: a half-working radio reads as a quiet
 * channel. These counts are what says which it was. Each stops at 65535 rather
 * than wrapping.
 *
 * Both images answer the request, since both wait on the radio the same way.
 * The last two counts are the exception: only the bluetooth_rxtx image waits
 * for a packet's bytes (its promiscuous low-energy capture) or for the lead
 * before a packet is read out of the radio's FIFO (its generic capture and
 * its range test), so btbr always reports 0 for both.
 */
#define UBERTOOTH_RADIO_TIMEOUTS_LEN 12

typedef struct {
	uint16_t fs_lock;   /* the synthesizer never locked */
	uint16_t fs_unlock; /* it never went back off */
	uint16_t xosc;      /* the 16 MHz crystal never reported stable */
	uint16_t fsm;       /* the state machine never reached a state */
	uint16_t rx_bytes;  /* it stopped delivering a packet's bytes */
	uint16_t rx_lead;   /* the lead before a FIFO read was never measured */
} radio_timeouts;

typedef struct {
	uint8_t reset_cause; /* why the board last started: watchdog_reset_cause bits */
	uint8_t armed;       /* 1 while the watchdog is running */
	uint8_t resets;      /* unasked-for watchdog resets since power-on, stopping at 255 */
	uint8_t boots;       /* boots since power-on, stopping at 255 */
	uint8_t reset_mode;  /* the mode it was in at the last one */
	uint8_t reset_site;  /* and the wait it was in: watchdog_site */
	uint8_t mode;        /* the mode it is in now */
	uint8_t site;        /* and the wait it is in now */
	/* Capture transfers the DMA controller reported an error on since
	 * power-on, stopping at 65535. Each one costs the packet that was
	 * arriving; the capture carries on, which is why the count is worth
	 * reporting. */
	uint16_t transfer_errors;
	/* The last processor fault, which reset_site names: the fault status
	 * register the handler read, and one address. Both zero until a fault
	 * happens.
	 *
	 * Which address depends on the status. With UBERTOOTH_FAULT_ADDR_VALID
	 * or UBERTOOTH_FAULT_BUS_ADDR_VALID set it is the access the processor
	 * refused, from the part's own fault address register. With neither set
	 * the part recorded no access address -- two of the four faults keep no
	 * register for one at all -- and the address is the instruction the
	 * processor was running, read from the exception frame, with zero for
	 * "nothing to report". So the status has to be read before the address
	 * means anything. */
	uint32_t fault_status;
	uint32_t fault_addr;
	/* How deep the stack has ever been since the board booted, and how much
	 * room it has. A 32-byte guard at the bottom of the stack makes an
	 * overflow that walks into it fault, but a single large frame can step
	 * over the guard and write over the data below, so these two are still
	 * what says there is room. Both are since the last boot: the firmware
	 * measures them from a pattern it writes at startup. */
	uint16_t stack_used;
	uint16_t stack_size;
} watchdog_info;

enum rfcat24_subcommands {
	RFCAT_RESERVED               = 0,
	RFCAT_SET_AA                 = 1,
	RFCAT_CAP_LEN                = 2,
};

/*
 * The sync word an E-GO capture looks for when the host sends no access code
 * of its own. A mode start puts the firmware back to it, and ubertooth-ego
 * names it as the default, so both sides read it from here.
 */
#define EGO_DEFAULT_ACCESS_CODE 0x630f9ffeu

/* The most a legacy ADV_IND can carry, which is what the board transmits in
 * MODE_BT_PERIPHERAL_LE: its six-bit length field counts the six address bytes as
 * well, and the specification caps AdvData at 31 bytes.
 *
 * It is also the most UBERTOOTH_LE_SET_ADV_DATA accepts, because that is the
 * only thing the data is ever used for: a longer payload could not be
 * transmitted, so the request says so instead of taking it. */
#define ADV_IND_MAX_DATA 31

enum jam_modes {
	JAM_NONE       = 0,
	JAM_ONCE       = 1,
	JAM_CONTINUOUS = 2,
};

enum modulations {
	MOD_BT_BASIC_RATE = 0,
	MOD_BT_LOW_ENERGY = 1,
	MOD_80211_FHSS    = 2,
	MOD_NONE          = 3
};

enum usb_pkt_types {
	BR_PACKET  = 0,
	LE_PACKET  = 1,
	MESSAGE    = 2,
	KEEP_ALIVE = 3,
	SPECAN     = 4,
	LE_PROMISC = 5,
	EGO_PACKET = 6,
	/* A low-energy packet too long for one record: the first record and the
	 * continuations that follow it, in order. See LE_FRAG_* below. */
	LE_PACKET_START = 7,
	LE_PACKET_CONT  = 8,
};

/* Where a fragment's own metadata sits: the two bytes a fragment record has no
 * other use for, usb_pkt_rx.rssi_noise and usb_pkt_rx.reserved. The first is
 * the low 8 bits of the whole packet's length; bit 0 of the second is its 9th
 * bit, and bits 7..1 are the fragment's position in the chain, counting up
 * from zero. 264 bytes needs nine length bits and six positions, so both fit.
 *
 * Nothing reads a noise figure from a fragment record: the low-energy engine
 * that sends these is the one mode with no background estimate, so
 * NOISE_ESTIMATE is never set on one. */
#define LE_FRAG_TOTAL_HIGH_MASK 0x01
#define LE_FRAG_INDEX_SHIFT 1
#define LE_FRAG_TOTAL_LOW(f) ((f)->rssi_noise)
#define LE_FRAG_FLAGS(f)     ((f)->reserved)

enum hop_mode {
	HOP_NONE      = 0,
	HOP_SWEEP     = 1,
	HOP_BLUETOOTH = 2,
	HOP_BTLE      = 3,
	HOP_DIRECT    = 4,
	HOP_AFH       = 5,
};

enum usb_pkt_status {
	DMA_OVERFLOW  = 0x01,
	DMA_ERROR     = 0x02,
	FIFO_OVERFLOW = 0x04,
	CS_TRIGGER    = 0x08,
	RSSI_TRIGGER  = 0x10,
	DISCARD       = 0x20,
	/*
	 * The two power figures, each set only where the board has that one.
	 *
	 * NOISE_ESTIMATE says the rssi_noise byte holds the board's estimate of
	 * the background on this record's channel. It is an estimate and not a
	 * measurement of clear air: the board cannot time the quiet between
	 * packets, so it goes by level, averaging the readings that sat more
	 * than 10 dB below the loudest it had taken since it last started
	 * counting, which it does afresh at each receive window and at every
	 * hop -- and a mode on a fixed channel still takes the hop timer's
	 * tick, 25 times a second. A faded part of a packet is counted in,
	 * quiet taken
	 * before a packet is not, and the mean is over the loudest 10 dB of
	 * what is admitted, so the figure sits above the floor. The firmware
	 * decides all of this in rssi_add(), where the 10 dB is
	 * RSSI_POWER_SPAN; this header mirrors the number because it cannot
	 * include that one. Without the bit the byte says nothing at all.
	 *
	 * Without SIGNAL_VALID the rssi_avg byte is not this packet's power: it
	 * is either a record that carries no packet, or one from a board or a
	 * `.dump` file older than the measurement, where the field held the
	 * board's running average for the channel instead. A reader of such a
	 * record falls back to rssi_max, which is the nearest thing to a
	 * packet's own level it carries.
	 */
	NOISE_ESTIMATE = 0x40,
	SIGNAL_VALID   = 0x80,
};

/*
 * The 100 ns clock a packet is stamped with (usb_pkt_rx.clk100ns below) comes
 * from one of two counters, so it has one of two periods:
 *
 * CLK100NS is 3125 ticks per CLKN tick over the low 20 bits of CLKN (see
 * CLK100NS in firmware/bluetooth_rxtx/ubertooth_clock.h), so it wraps every
 * 3125 * 2^20 = 3276800000 ticks, about 327.68 s. Classic packets, the LE
 * promiscuous mode and E-GO packets carry it.
 *
 * The low-energy physical-layer engine (firmware/bluetooth_rxtx/le_phy.c, the
 * mode behind ubertooth-btle -f and -n) stamps packets with timer1 instead,
 * which runs free at 100 ns a tick and so wraps every 2^32 ticks, about
 * 429.5 s. Nothing in the packet says which counter stamped it: the host knows
 * from the mode it asked for (ubertooth_t.clk100ns_period).
 */
#define CLK100NS_PER_CLKN 3125
#define CLK100NS_PERIOD   ((uint32_t)CLK100NS_PER_CLKN << 20)
#define CLK100NS_TIMER1_PERIOD (1ull << 32)

/*
 * USB packet for Bluetooth RX (64 total bytes)
 */
typedef struct {
	uint8_t  pkt_type;
	uint8_t  status;
	uint8_t  channel;
	uint8_t  clkn_high;
	uint32_t clk100ns;
	int8_t   rssi_max;   // Max RSSI seen while collecting symbols in this packet
	int8_t   rssi_min;   // Min ...
	/* The packet's own power: the mean of the readings taken while it was on
	 * the air, averaged in power rather than in dBm and over the readings
	 * within 10 dB of the strongest, so the quiet either side of a short
	 * packet does not drag it down. This is what a reader shows as the
	 * signal. */
	int8_t   rssi_avg;
	/* How many readings rssi_avg was taken over. The Basic Rate stream sends
	 * 0 when it took none, so the three bytes above mean nothing; the
	 * low-energy engine sends 0 with an average beside it that does mean
	 * something, so a reader cannot use this byte alone to tell whether the
	 * average is worth showing. */
	uint8_t  rssi_count;
	/* The board's estimate of the background power on this record's
	 * channel, when status carries NOISE_ESTIMATE; meaningless without it.
	 * A raw reading like the three above.
	 *
	 * On the records of a fragmented low-energy packet these two bytes carry
	 * the fragment's own length and index instead (LE_FRAG_* above). */
	uint8_t  rssi_noise;
	uint8_t  reserved;
	uint8_t  data[DMA_SIZE];
} usb_pkt_rx;

typedef struct {
	uint64_t address;
	uint64_t syncword;
} bdaddr;

typedef struct {
	/* 0 before a test has completed, 1 for a result, or 2 plus the index of
	 * the first payload byte of the reply that was not the one sent. */
	uint8_t valid;
	uint8_t request_pa;
	uint8_t request_num;
	uint8_t reply_pa;
	uint8_t reply_num;
	/* Whether this run heard a packet that was not its own reply and
	 * discarded it: 0 for none, otherwise 2 plus the index of the first
	 * payload byte that said so, the encoding `valid` uses. Only the first
	 * such packet of a run is recorded. */
	uint8_t discarded;
} rangetest_result;

typedef struct {
	uint16_t synch;
	uint16_t syncl;
	uint16_t channel;
	uint8_t  length;
	/* Only the low three bits reach FREND's PA_LEVEL, so the board masks a
	 * larger value rather than refusing it as UBERTOOTH_SET_PALEVEL does.
	 * UBERTOOTH_PA_LEVEL_MAX below is the value that means anything. */
	uint8_t  pa_level;
	uint8_t  data[DMA_SIZE];
} generic_tx_packet;

/*
 * The highest CC2400 power level: three bits in FREND's PA_LEVEL field, which
 * is what UBERTOOTH_SET_PALEVEL refuses a larger value against.
 */
#define UBERTOOTH_PA_LEVEL_MAX 7

/*
 * The most a UBERTOOTH_TX_GENERIC_PACKET request can carry, which is what the
 * firmware refuses a longer payload against.
 */
#define GENERIC_TX_MAX_PAYLOAD DMA_SIZE

/*
 * How much of a generic payload goes out as one frame the sender knows the
 * shape of: one load of the radio's FIFO (CC2400_FIFO_SIZE in the firmware's
 * cc2400.h, checked against this in test_tx_generic_bounds.c).
 *
 * A longer payload is written to the radio a FIFO at a time, each piece after
 * the radio has sent the last, and whether the radio carries one packet across
 * that gap or frames the rest separately has not been observed -- an Ubertooth
 * receiver reads one FIFO's worth per sync word either way, so it cannot tell.
 * It is also all a generic receive reports. So this is what a tool offers, and
 * GENERIC_TX_MAX_PAYLOAD above is only what the request will carry.
 */
#define GENERIC_TX_FRAMED_PAYLOAD 32

/* BTBR USB interface */
enum {
	BTUSB_MSG_START = 'S',
	BTUSB_MSG_CONT = 'C',
	BTUSB_EARLY_PRINT = 'P'
};

/* BTCTL interface */
typedef enum btctl_state_e {
	BTCTL_STATE_STANDBY	= 0,
	BTCTL_STATE_INQUIRY	= 1,
	BTCTL_STATE_PAGE	= 2,
	BTCTL_STATE_CONNECTED	= 3,
	BTCTL_STATE_INQUIRY_SCAN = 4,
	BTCTL_STATE_PAGE_SCAN	= 5,
	BTCTL_STATE_COUNT
} btctl_state_t;

typedef enum btctl_reason_e {
	BTCTL_REASON_SUCCESS	= 0,
	BTCTL_REASON_TIMEOUT	= 1,
	/* REASON_PAGED indicates that device entered CONNECTED state,
	 * but the Peripheral did not answer yet */
	BTCTL_REASON_PAGED	= 2
} btctl_reason_t;

typedef enum {
	BTCTL_DEBUG		= 0,
	/* Any -> Device */
	BTCTL_RESET_REQ		= 20,
	BTCTL_IDLE_REQ		= 21,
	/* Host -> Device */
	BTCTL_SET_FREQ_OFF_REQ	= 22,
	BTCTL_SET_BDADDR_REQ	= 23,
	BTCTL_INQUIRY_REQ	= 24,
	BTCTL_PAGING_REQ	= 25,
	BTCTL_SET_MAX_AC_ERRORS_REQ = 26,
	BTCTL_TX_ACL_REQ	= 27,
	BTCTL_INQUIRY_SCAN_REQ	= 28,
	BTCTL_PAGE_SCAN_REQ	= 29,
	BTCTL_SET_EIR_REQ	= 30,// in a btctl_tx_pkt_t
	BTCTL_SET_AFH_REQ	= 31,
	BTCTL_MONITOR_REQ	= 32,
	/* Hand the chip to its on-chip programmer, queued so that the control
	 * transfer that asked for it is answered first; see BTCTL_RESET_REQ. */
	BTCTL_ISP_REQ		= 33,
	/* One packet of a chosen basic-rate format, in a btctl_tx_pkt_req_t */
	BTCTL_TX_PKT_REQ	= 34,
	/* Device -> Host */
	BTCTL_RX_PKT		= 40,
	BTCTL_STATE_RESP	= 41
} btctl_cmd_type_t ;

typedef struct btctl_hdr_s {
	uint8_t type;
	uint8_t padding[3];
	uint8_t data[0];
} __attribute__((packed)) btctl_hdr_t;

typedef struct btctl_paging_req_s {
	uint64_t bdaddr;
} btctl_paging_req_t;

typedef struct btctl_set_reg_req_s {
	uint16_t reg;
} btctl_set_reg_req_t;

typedef struct btctl_set_bdaddr_req_s {
	uint64_t bdaddr;
} btctl_set_bdaddr_req_t;

typedef struct btctl_set_afh_req_s {
	uint32_t instant;
	uint8_t mode;
	uint8_t map[10];
} __attribute__((packed)) btctl_set_afh_req_t;

typedef struct btctl_state_resp_s {
	uint8_t state;
	uint8_t reason;
} btctl_state_resp_t;

typedef struct bthdr_s {
	uint8_t lt_addr;
	uint8_t type;
	uint8_t flags;
	uint8_t hec;
} __attribute__((packed)) bbhdr_t;

#define BTHDR_FLOW 0
#define BTHDR_ARQN 1
#define BTHDR_SEQN 2

#define BBPKT_F_HAS_PKT		0
#define BBPKT_F_HAS_HDR		1
#define BBPKT_F_HAS_CRC		2
#define BBPKT_F_GOOD_CRC	3
#define BBPKT_HAS_PKT(p)	(((p)->flags & (1<<BBPKT_F_HAS_PKT))!=0)
#define BBPKT_HAS_HDR(p)	(((p)->flags & (1<<BBPKT_F_HAS_HDR))!=0)
#define BBPKT_HAS_CRC(p)	(((p)->flags & (1<<BBPKT_F_HAS_CRC))!=0)
#define BBPKT_GOOD_CRC(p)	(((p)->flags & (1<<BBPKT_F_GOOD_CRC))!=0)

#define MAX_ACL_PACKET_SIZE 344

typedef struct {
	uint32_t clkn;		// Clkn sampled at start of rx
	uint8_t chan;		// Channel number
	uint8_t flags;		// Decode flags
	uint16_t data_size;	// Size of bt_data field
	bbhdr_t bb_hdr;		// decoded header
	uint8_t bt_data[0];	// maximum data size for an ACL packet
} __attribute__((packed)) btctl_rx_pkt_t;

typedef struct {
	bbhdr_t bb_hdr;		// header (only lt_addr/type/flags are relevant)
	uint8_t bt_data[0];	// maximum data size for an ACL packet
} __attribute__((packed)) btctl_tx_pkt_t;

/* The basic-rate packet formats the device can be asked to transmit.
 *
 * Named rather than numbered, because a TYPE code in the packet header means
 * different things on an asynchronous link and on a synchronous one
 * (Bluetooth Core, Vol 2 Part B, Table 6.2): HV3 and EV3 are both code 7 and
 * differ only in what the payload holds, so the name is what says which of
 * the two to build. The device knows each format's air layout; the host says
 * which format, how many data bytes, and what the bytes are.
 */
typedef enum btctl_tx_format_e {
	BTCTL_FMT_DM1	= 0,
	BTCTL_FMT_DH1	= 1,
	BTCTL_FMT_DM3	= 2,
	BTCTL_FMT_DH3	= 3,
	BTCTL_FMT_DM5	= 4,
	BTCTL_FMT_DH5	= 5,
	BTCTL_FMT_AUX1	= 6,
	BTCTL_FMT_DV	= 7,
	BTCTL_FMT_HV1	= 8,
	BTCTL_FMT_HV2	= 9,
	BTCTL_FMT_HV3	= 10,
	BTCTL_FMT_EV3	= 11,
	BTCTL_FMT_EV4	= 12,
	BTCTL_FMT_EV5	= 13,
	BTCTL_FMT_COUNT
} btctl_tx_format_t;

/* The most air slots one run of a packet may occupy: the repeat count times
 * the slots the format itself fills, so a five-slot DH5 may be repeated 80
 * times and a one-slot DM1 400.
 *
 * What the bound is for: nothing else leaves the board while the run lasts,
 * so its own ACL and LMP traffic waits, and a peer's link-manager procedures
 * have their own timeouts. The peer keeps the slot after each packet and goes
 * on being heard, so the link itself does not end. At 625 us an air slot, 400
 * of them is a quarter of a second, and the longest run any format can ask
 * for -- 80 DH5s, each five slots with the peer's slot after it -- is 300 ms.
 */
#define BTCTL_TX_PKT_MAX_SLOTS 400

/* One packet to put on the air exactly as given, in the next transmit slot of
 * a connection, and again in each of the next repeat-1 transmit slots. It is
 * never retransmitted on the peer's say-so, so a peer that cannot make sense
 * of it costs nothing: the point is the packet being transmitted, not the
 * peer accepting it. It does carry the link's ARQN and SEQN, like every other
 * packet, which is why the device sends one only while no ACL message of its
 * own is waiting to be acknowledged.
 *
 * data_size counts the bytes after the payload header, which the host writes
 * into data itself for a format that has one. The device adds the CRC where
 * the format has one.
 */
typedef struct {
	uint8_t lt_addr;
	uint8_t format;		// btctl_tx_format_t
	uint16_t repeat;	// transmit slots to spend on it, at least 1
	uint16_t data_size;	// payload bytes supplied in data
	uint8_t data[0];
} __attribute__((packed)) btctl_tx_pkt_req_t;

#endif /* __UBERTOOTH_INTERFACE_H */
