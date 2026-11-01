/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The bootloader report an application answers UBERTOOTH_GET_BOOTLOADER_INFO
 * with. See bl_report.h for what the two functions are and why the region is
 * an address.
 */
#include "bl_report.h"
#include "ubertooth_interface.h"

/*
 * Put the user flash at the bottom of the address map, where bl_report_build()
 * reads it from.
 *
 * FW_HOST_TEST is the host build of this file: firmware/tests/
 * test_bl_version.c runs the report against a region of its own and has no
 * registers to write, so the store compiles to nothing there and the test
 * reads this file as text to show it is still made.
 */
#ifdef FW_HOST_TEST
static void bl_report_map_user_flash(void) { }
#else
#include "lpc17.h"
static void bl_report_map_user_flash(void)
{
	MEMMAP = MEMMAP_USER_FLASH;
}
#endif

/*
 * A nibble at a time, from a table of sixteen words rather than the usual two
 * hundred and fifty-six: the 16 KiB region takes a couple of milliseconds
 * this way against four or five bit by bit, for 64 bytes of flash instead of
 * a kilobyte. The hash is worked out at startup, where neither cost is
 * pressing, but it is startup all the same.
 *
 * Each entry is the CRC of one nibble value, so two steps consume a byte.
 */
static const uint32_t crc32_nibbles[16] = {
	0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac,
	0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
	0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
	0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
};

uint32_t bl_report_crc32(uintptr_t start, size_t length)
{
	const volatile uint8_t *byte = (const volatile uint8_t *)start;
	uint32_t crc = 0xffffffff;
	size_t i;

	for (i = 0; i < length; i++) {
		crc ^= byte[i];
		crc = (crc >> 4) ^ crc32_nibbles[crc & 0x0f];
		crc = (crc >> 4) ^ crc32_nibbles[crc & 0x0f];
	}
	return ~crc;
}

/* The answer, worked out once by bl_report_init(), and whether it is there
 * yet: a handler reached before startup finished refuses rather than copying
 * zeros. */
static uint8_t bl_report[UBERTOOTH_BL_REPORT_LENGTH];
static int bl_report_ready;

void bl_report_init(uintptr_t region)
{
	const volatile uint8_t *block = (const volatile uint8_t *)
		(region + UBERTOOTH_BL_VERSION_OFFSET);
	static const char magic[] = UBERTOOTH_BL_VERSION_MAGIC;
	uint8_t *out = bl_report;
	uint32_t crc;
	int have_block = 1;
	size_t i;

	/*
	 * Ask for the user flash at the bottom of the address map before
	 * reading it. The part maps either the flash or the boot ROM over the
	 * lowest addresses (firmware/common/lpc17.h, MEMMAP), and the window
	 * covers the first 512 bytes -- which is where the version block sits.
	 * A Cortex-M3 finds its vector table through VTOR, so this image never
	 * needed the register and never set it: the mapping is whatever the
	 * boot ROM and the bootloader left behind. With the boot ROM mapped,
	 * this read would hash 512 bytes of ROM and find no block on a board
	 * that has one.
	 *
	 * Setting it costs one store and changes nothing else in this image,
	 * which reads nothing at the bottom of the map. It is also the state a
	 * running application should already be in, since the bootloader
	 * itself executes from there.
	 */
	bl_report_map_user_flash();

	crc = bl_report_crc32(region, UBERTOOTH_BOOTLOADER_REGION_LENGTH);

	out[UBERTOOTH_BL_REPORT_OFF_CRC + 0] = (uint8_t)(crc & 0xff);
	out[UBERTOOTH_BL_REPORT_OFF_CRC + 1] = (uint8_t)((crc >> 8) & 0xff);
	out[UBERTOOTH_BL_REPORT_OFF_CRC + 2] = (uint8_t)((crc >> 16) & 0xff);
	out[UBERTOOTH_BL_REPORT_OFF_CRC + 3] = (uint8_t)((crc >> 24) & 0xff);

	/* A bootloader with no block has its own code at the offset. Those
	 * bytes say nothing about it, so zeros go out in their place rather
	 * than a few bytes of someone's bootloader: the host reads the absence
	 * from the magic either way, and the hash is what identifies the
	 * bootloader in that case. */
	for (i = 0; i < UBERTOOTH_BL_VERSION_MAGIC_LEN; i++) {
		if (block[UBERTOOTH_BL_VERSION_OFF_MAGIC + i] != (uint8_t)magic[i]) {
			have_block = 0;
			break;
		}
	}
	for (i = 0; i < UBERTOOTH_BL_VERSION_LENGTH; i++)
		out[UBERTOOTH_BL_REPORT_OFF_BLOCK + i] =
			have_block ? block[i] : 0;

	/* Last, so nothing can read a half-filled report. */
	bl_report_ready = 1;
}

int bl_report_build(uint8_t *out)
{
	size_t i;

	if (!bl_report_ready)
		return 0;
	for (i = 0; i < UBERTOOTH_BL_REPORT_LENGTH; i++)
		out[i] = bl_report[i];
	return 1;
}
