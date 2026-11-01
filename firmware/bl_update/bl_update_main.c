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
 * The bootloader updater: the image that installs a bootloader over USB.
 *
 * A bootloader refuses USB writes below 0x4000, so it cannot replace itself.
 * This image is written to the application region like any firmware, and the
 * bootloader starts it. It carries the 16 KiB of bootloader region to install
 * (bl_payload.S), programs it with the boot ROM's In-Application Programming
 * commands, reads it back, and resets into DFU so the next image can be
 * written over this one.
 *
 * Everything that decides whether to write, and in what order, is in
 * firmware/common/bl_update.c, driven through function pointers so that
 * firmware/tests/test_bl_update.c runs all of it on a host. This file is the
 * part that needs the part: the ROM calls, the clock, the LEDs and the reset.
 *
 * It has no USB of its own on purpose. The host already made every check this
 * makes, over the same bytes, before it wrote this image to the board
 * (host/ubertooth-tools/src/bl_update_file.c), and it reads the result
 * afterwards from the bootloader report the next application answers
 * (ubertooth-util -B). A USB stack here would be a second implementation of
 * the one path in the tree that must not have a second implementation.
 */
#include "bl_update.h"
#include "irq.h"
#include "ubertooth.h"

/*
 * The payload: the whole bootloader region, padded to 16 KiB with the 0xff of
 * erased flash. bl_payload.S puts the bootloader's own binary here, and
 * sections.ld places the section at UBERTOOTH_BL_UPDATE_PAYLOAD_OFFSET so the
 * host finds the same bytes in the image file.
 */
extern const uint8_t bl_payload[UBERTOOTH_BL_UPDATE_PAYLOAD_LENGTH];

/*
 * The buffer the ROM programs from and compares against. The commands read
 * RAM, and the payload is in flash, so one sector at a time goes through
 * here.
 *
 * Four KiB of the part's 16 KiB of main SRAM, which this image can spare: it
 * has no capture buffers, no radio and no USB. Word-aligned because the ROM
 * requires a word-aligned source.
 */
static uint32_t sector_buffer[BL_UPDATE_SECTOR_LENGTH / 4];

/*
 * The boot ROM's In-Application Programming commands used here (NXP
 * LPC176x/5x user manual UM10360, "IAP commands"). The entry point itself is
 * iap_entry in ubertooth.h, which every image already has; the bootloader
 * wraps the same address in a C++ class of its own
 * (firmware/bootloader/iap.cpp), and this image is C.
 */
#define IAP_PREPARE_SECTORS 50u
#define IAP_COPY_RAM_TO_FLASH 51u
#define IAP_ERASE_SECTORS 52u
#define IAP_BLANK_CHECK_SECTORS 53u
#define IAP_READ_PART_ID 54u
#define IAP_COMPARE 56u

#define IAP_CMD_SUCCESS 0u
#define IAP_INVALID_COMMAND 1u
#define IAP_INVALID_SECTOR 7u
#define IAP_SECTOR_NOT_BLANK 8u

/* The clock figure the ROM times an erase and a program by, in kHz. It has to
 * be the clock the part is really running at, which clock_start() has just
 * set up: CCLK_HZ in ubertooth.h is what that comes to for this board. */
#define IAP_CCLK_KHZ (CCLK_HZ / 1000u)

/*
 * One ROM call, with interrupts off.
 *
 * Flash is not readable while it is being erased or programmed, so an
 * interrupt taken in the middle would fetch its handler out of flash that is
 * not there. Nothing in this image enables an interrupt, so this is a
 * guarantee rather than a fix, and it costs four instructions.
 *
 * output[0] is the ROM's return code, and later words are the command's
 * answer. The array is written whole each time, so a command that returns
 * nothing cannot be read as one that did.
 */
static uint32_t iap_call(uint32_t *input, uint32_t *output)
{
	uint32_t primask;

	output[0] = IAP_INVALID_COMMAND;
	output[1] = 0;
	primask = irq_save_disable();
	iap_entry(input, output);
	irq_restore(primask);
	return output[0];
}

static int ops_read_part(void *context, uint32_t *part_id)
{
	uint32_t input[1];
	uint32_t output[2];

	(void)context;
	input[0] = IAP_READ_PART_ID;
	if (iap_call(input, output) != IAP_CMD_SUCCESS)
		return -1;
	*part_id = output[1];
	return 0;
}

static int ops_sector_present(void *context, uint32_t sector)
{
	uint32_t input[3];
	uint32_t output[3];
	uint32_t result;

	(void)context;
	input[0] = IAP_BLANK_CHECK_SECTORS;
	input[1] = sector;
	input[2] = sector;
	output[2] = 0;
	result = iap_call(input, output);
	/* A sector the part has answers one of these two, depending on what is
	 * in it; a sector it has not got is refused. Anything else is the ROM
	 * declining to say, which is not the same as a part that is too small.
	 */
	if (result == IAP_CMD_SUCCESS || result == IAP_SECTOR_NOT_BLANK)
		return 1;
	if (result == IAP_INVALID_SECTOR)
		return 0;
	return -1;
}

static int ops_prepare(void *context, uint32_t first_sector,
                       uint32_t last_sector)
{
	uint32_t input[3];
	uint32_t output[2];

	(void)context;
	input[0] = IAP_PREPARE_SECTORS;
	input[1] = first_sector;
	input[2] = last_sector;
	return iap_call(input, output) == IAP_CMD_SUCCESS ? 0 : -1;
}

static int ops_erase(void *context, uint32_t first_sector,
                     uint32_t last_sector)
{
	uint32_t input[4];
	uint32_t output[2];

	(void)context;
	input[0] = IAP_ERASE_SECTORS;
	input[1] = first_sector;
	input[2] = last_sector;
	input[3] = IAP_CCLK_KHZ;
	return iap_call(input, output) == IAP_CMD_SUCCESS ? 0 : -1;
}

static int ops_program(void *context, uint32_t flash_address,
                       const uint8_t *ram, uint32_t length)
{
	uint32_t input[5];
	uint32_t output[2];

	(void)context;
	input[0] = IAP_COPY_RAM_TO_FLASH;
	input[1] = flash_address;
	input[2] = (uint32_t)ram;
	input[3] = length;
	input[4] = IAP_CCLK_KHZ;
	return iap_call(input, output) == IAP_CMD_SUCCESS ? 0 : -1;
}

static int ops_verify(void *context, uint32_t flash_address,
                      const uint8_t *ram, uint32_t length)
{
	uint32_t input[4];
	uint32_t output[3];

	(void)context;
	/* The ROM's own compare, which reads the flash that was just written
	 * and the RAM it came from. A difference comes back as COMPARE_ERROR
	 * with the offset in output[1], which nothing here can report, so it is
	 * the return code that matters. */
	input[0] = IAP_COMPARE;
	input[1] = flash_address;
	input[2] = (uint32_t)ram;
	input[3] = length;
	output[2] = 0;
	return iap_call(input, output) == IAP_CMD_SUCCESS ? 0 : -1;
}

static const struct bl_update_ops board_ops = {
	ops_read_part,
	ops_sector_present,
	ops_prepare,
	ops_erase,
	ops_program,
	ops_verify,
};

/*
 * Roughly a quarter of a second, counted rather than timed: no timer is set
 * up in this image and the blink below is the only thing that waits. The
 * figure is approximate on purpose -- it is a blink a person watches, not a
 * measurement -- and `volatile` keeps the loop from being optimized away.
 */
static void blink_delay(void)
{
	volatile uint32_t i;

	for (i = 0; i < CCLK_HZ / 40u; i++)
		;
}

/*
 * What a refusal looks like on the board: the USR LED blinks the refusal's
 * number, pauses, and repeats a few times.
 *
 * This is a diagnostic of last resort. The host makes every one of these
 * checks over the same bytes before it writes this image to a board, so a
 * refusal here means the bytes on the board are not the bytes in the file --
 * a transfer that went wrong -- and the board is returned to DFU either way.
 * Like btbr's fault blink, it is there so that something is visible when
 * nothing can be reported.
 */
static void blink_refusal(int result)
{
	unsigned count = (unsigned)(-result);
	unsigned round;
	unsigned i;

	for (round = 0; round < 3u; round++) {
		for (i = 0; i < count; i++) {
			USRLED_SET;
			blink_delay();
			USRLED_CLR;
			blink_delay();
		}
		blink_delay();
		blink_delay();
	}
}

int main(void)
{
	struct bl_update_report report;
	int result;

	/* The pins and the clock, and nothing else: no radio, and no USB. The
	 * ROM times an erase and a program by the figure IAP_CCLK_KHZ gives it,
	 * so the clock has to be set up before any of them and must be the one
	 * that figure names. */
	gpio_init();
	clock_start();

	/* All three LEDs on while the region is being written, so that a board
	 * in the middle of it is not mistaken for one that has finished. */
	USRLED_SET;
	RXLED_SET;
	TXLED_SET;

	result = bl_update_run(&board_ops, NULL, bl_payload, sizeof(bl_payload),
	                       UBERTOOTH_BOOTLOADER_REGION_START,
	                       (uint8_t *)sector_buffer, &report);

	USRLED_CLR;
	RXLED_CLR;
	TXLED_CLR;

	if (result < 0)
		blink_refusal(result);

	/*
	 * Back to DFU, whatever happened.
	 *
	 * This image is in the application region, so leaving the board running
	 * it would leave a board that cannot be flashed. DFU is where the next
	 * image is written from, and it is also where the host goes to look for
	 * the board once this is done.
	 *
	 * A board that is power-cycled instead of reset comes up in the
	 * bootloader, which starts this image again: the region then already
	 * holds the payload, so the run answers "the board already has this
	 * bootloader" and returns to DFU without writing anything. That is why
	 * the run compares before it writes.
	 */
	bootloader_ctrl = DFU_MODE;
	reset();

	/* reset() does not return. */
	while (1)
		;
}
