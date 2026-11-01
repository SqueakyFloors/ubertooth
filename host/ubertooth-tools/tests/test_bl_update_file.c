/*
 * What ubertooth-dfu --update-bootloader reads out of an updater image, and
 * what it refuses.
 *
 * These run over files this test writes, so every refusal is exercised
 * without a board and without an ARM toolchain. The refusals themselves are
 * ubertooth_bl_update_payload_check() in libubertooth's
 * ubertooth_interface.h, which the firmware uses as well
 * (firmware/tests/test_bl_update.c drives the same function against a model
 * of a part's flash); what is under test here is the part only the host does:
 * finding the bootloader in a firmware file, refusing a file that is not an
 * updater at all, and working out the hash the board will report afterwards.
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../firmware/tests/minunit.h"
#include "bl_update_file.h"
#include "dfu_recovery.h"
#include "ubertooth_interface.h"

#define PAYLOAD_LENGTH UBERTOOTH_BL_UPDATE_PAYLOAD_LENGTH

static uint8_t payload[PAYLOAD_LENGTH];

static void write_le32(uint8_t *bytes, uint32_t value)
{
	bytes[0] = (uint8_t)(value & 0xff);
	bytes[1] = (uint8_t)((value >> 8) & 0xff);
	bytes[2] = (uint8_t)((value >> 16) & 0xff);
	bytes[3] = (uint8_t)((value >> 24) & 0xff);
}

/* A bootloader a part would take: a plausible vector table, a version block,
 * no protection word, and bytes elsewhere that are not all the same. */
static void build_good_payload(void)
{
	static const char magic[] = UBERTOOTH_BL_VERSION_MAGIC;
	size_t i;

	for (i = 0; i < PAYLOAD_LENGTH; i++)
		payload[i] = (uint8_t)(i * 31u + 7u);
	write_le32(payload, 0x10003fe0u);
	write_le32(payload + 4, 0x00000301u);
	write_le32(payload + UBERTOOTH_BL_UPDATE_CHECKSUM_INDEX * 4, 0);
	write_le32(payload + UBERTOOTH_LPC_CRP_OFFSET, 0xffffffffu);
	memcpy(payload + UBERTOOTH_BL_VERSION_OFFSET, magic,
	       UBERTOOTH_BL_VERSION_MAGIC_LEN);
	write_le32(payload + UBERTOOTH_BL_VERSION_OFFSET +
	           UBERTOOTH_BL_VERSION_OFF_VERSION, 0x0102u);
}

/*
 * An updater image as a file: filler up to the payload offset, then the
 * payload, then a little code after it, which is the shape the real image has.
 * The file is a temporary one, removed by the caller.
 */
static FILE *write_image(size_t payload_bytes, size_t tail_bytes)
{
	FILE *file = tmpfile();
	uint8_t filler[UBERTOOTH_BL_UPDATE_PAYLOAD_OFFSET];
	uint8_t tail[256];
	size_t i;

	if (file == NULL)
		return NULL;
	for (i = 0; i < sizeof(filler); i++)
		filler[i] = (uint8_t)(i | 0x80);
	memset(tail, 0xa5, sizeof(tail));
	if (fwrite(filler, 1, sizeof(filler), file) != sizeof(filler) ||
	    fwrite(payload, 1, payload_bytes, file) != payload_bytes)
		return NULL;
	while (tail_bytes > 0) {
		const size_t chunk = tail_bytes < sizeof(tail) ? tail_bytes
		                                               : sizeof(tail);

		if (fwrite(tail, 1, chunk, file) != chunk)
			return NULL;
		tail_bytes -= chunk;
	}
	rewind(file);
	return file;
}

static int check(FILE *file, struct bl_update_file *out)
{
	return bl_update_file_check(file, UBERTOOTH_BL_UPDATE_IMAGE_NAME, out);
}

MU_TEST(an_updater_image_is_read)
{
	struct bl_update_file info;
	FILE *file;

	build_good_payload();
	file = write_image(PAYLOAD_LENGTH, 512);
	mu_assert_fatal(file != NULL, "the test image would not be written");
	mu_assert_int_eq(UBERTOOTH_BL_UPDATE_OK, check(file, &info));
	mu_assert_int_eq(0x0102, info.bootloader_version);
	fclose(file);
}

/*
 * Only an image that says it is the updater. Without this, --update-bootloader
 * given an ordinary firmware image would read 16 KiB of that image's code and
 * write it over the bootloader region, which is a board that does not start.
 */
MU_TEST(any_other_image_is_refused)
{
	struct bl_update_file info;
	FILE *file;

	build_good_payload();
	file = write_image(PAYLOAD_LENGTH, 512);
	mu_assert_fatal(file != NULL, "the test image would not be written");
	mu_assert_int_eq(UBERTOOTH_BL_UPDATE_NOT_UPDATER,
	                 bl_update_file_check(file, "bluetooth_rxtx", &info));
	mu_assert_int_eq(UBERTOOTH_BL_UPDATE_NOT_UPDATER,
	                 bl_update_file_check(file, "", &info));
	mu_assert_int_eq(UBERTOOTH_BL_UPDATE_NOT_UPDATER,
	                 bl_update_file_check(file, NULL, &info));
	mu_assert_int_eq(UBERTOOTH_BL_UPDATE_NOT_UPDATER,
	                 bl_update_file_check(NULL,
	                                      UBERTOOTH_BL_UPDATE_IMAGE_NAME,
	                                      &info));
	fclose(file);
}

/* A file that stops inside the payload holds no bootloader to install. */
MU_TEST(a_file_too_short_for_the_payload_is_refused)
{
	struct bl_update_file info;
	FILE *file;

	build_good_payload();
	file = write_image(PAYLOAD_LENGTH - 1, 0);
	mu_assert_fatal(file != NULL, "the test image would not be written");
	mu_assert_int_eq(UBERTOOTH_BL_UPDATE_FILE_SHORT, check(file, &info));
	fclose(file);

	file = write_image(0, 0);
	mu_assert_fatal(file != NULL, "the test image would not be written");
	mu_assert_int_eq(UBERTOOTH_BL_UPDATE_FILE_SHORT, check(file, &info));
	fclose(file);
}

/*
 * The refusal that matters most, made before the updater is written to a
 * board: a code read protection value cannot be undone from outside the part,
 * and with CRP3 or NO_ISP there is no way back in at all.
 */
MU_TEST(every_code_read_protection_value_is_refused)
{
	static const uint32_t values[] = {
		UBERTOOTH_LPC_CRP1, UBERTOOTH_LPC_CRP2,
		UBERTOOTH_LPC_CRP3, UBERTOOTH_LPC_NO_ISP,
	};
	struct bl_update_file info;
	unsigned i;

	for (i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
		FILE *file;

		build_good_payload();
		write_le32(payload + UBERTOOTH_LPC_CRP_OFFSET, values[i]);
		file = write_image(PAYLOAD_LENGTH, 512);
		mu_assert_fatal(file != NULL, "the test image would not be written");
		mu_assert_int_eq(UBERTOOTH_BL_UPDATE_CRP, check(file, &info));
		/* The word is kept so the refusal can quote it by value and by
		 * name, which is what tells a user which protection was asked
		 * for. */
		mu_assert_int_eq((int)values[i], (int)info.crp_word);
		fclose(file);
	}
}

MU_TEST(a_payload_the_board_would_refuse_is_refused_here)
{
	struct bl_update_file info;
	FILE *file;

	build_good_payload();
	payload[UBERTOOTH_BL_VERSION_OFFSET] ^= 0xff;
	file = write_image(PAYLOAD_LENGTH, 512);
	mu_assert_fatal(file != NULL, "the test image would not be written");
	mu_assert_int_eq(UBERTOOTH_BL_UPDATE_NO_VERSION_BLOCK,
	                 check(file, &info));
	fclose(file);

	build_good_payload();
	write_le32(payload, 0xffffffffu);
	write_le32(payload + 4, 0xffffffffu);
	file = write_image(PAYLOAD_LENGTH, 512);
	mu_assert_fatal(file != NULL, "the test image would not be written");
	mu_assert_int_eq(UBERTOOTH_BL_UPDATE_VECTORS, check(file, &info));
	fclose(file);
}

/*
 * The hash is of the region as the board will hold it, which is the payload
 * with the vector checksum filled in -- not of the file's bytes. A user
 * compares it with what `ubertooth-util -B` reports afterwards, so it has to
 * be the number the board will answer with.
 */
MU_TEST(the_hash_is_of_the_region_the_board_will_hold)
{
	struct bl_update_file info;
	uint8_t region[PAYLOAD_LENGTH];
	uint32_t checksum;
	size_t i;
	FILE *file;

	build_good_payload();
	file = write_image(PAYLOAD_LENGTH, 512);
	mu_assert_fatal(file != NULL, "the test image would not be written");
	mu_assert_int_eq(UBERTOOTH_BL_UPDATE_OK, check(file, &info));
	fclose(file);

	checksum = ubertooth_bl_update_vector_checksum(payload);
	memcpy(region, payload, sizeof(region));
	write_le32(region + UBERTOOTH_BL_UPDATE_CHECKSUM_INDEX * 4, checksum);
	mu_assert_int_eq((int)~dfu_image_crc32(region, sizeof(region)),
	                 (int)info.region_crc32);

	/* And it is not the hash of the file's bytes, which differ in that one
	 * word: a payload whose checksum word is already zero would otherwise
	 * give a number the board never reports. */
	mu_assert(info.region_crc32 != ~dfu_image_crc32(payload, PAYLOAD_LENGTH),
	          "the hash was taken over the file's bytes rather than the "
	          "region the board will hold");

	/* The eight words of the installed region add up to zero, which is what
	 * the boot ROM requires of code it is to start. */
	checksum = 0;
	for (i = 0; i < UBERTOOTH_BL_UPDATE_CHECKED_WORDS; i++)
		checksum += (uint32_t)region[i * 4] |
			((uint32_t)region[i * 4 + 1] << 8) |
			((uint32_t)region[i * 4 + 2] << 16) |
			((uint32_t)region[i * 4 + 3] << 24);
	mu_assert_int_eq(0, (int)checksum);
}

/*
 * The hash is the usual reflected CRC-32 -- the one zlib, PNG and Ethernet
 * use -- so a user can check it against any other implementation. The DFU
 * suffix uses the same CRC without the final inversion, which is why there is
 * one function here and an inversion at the one call that needs it. The
 * check-value of that CRC over "123456789" is 0xcbf43926.
 */
MU_TEST(the_hash_is_the_usual_crc32)
{
	static const uint8_t digits[] = "123456789";

	mu_assert_int_eq((int)0xcbf43926u,
	                 (int)~dfu_image_crc32(digits, sizeof(digits) - 1));
}

/* A refusal says what was wrong, and a protection value also says which one
 * and where, because that is the one refusal a user must not work around. */
MU_TEST(a_refusal_says_which_protection_was_asked_for)
{
	struct bl_update_file info = {0};
	char *buffer = NULL;
	size_t size = 0;
	FILE *out = open_memstream(&buffer, &size);

	mu_assert_fatal(out != NULL, "the memory stream would not open");
	info.result = UBERTOOTH_BL_UPDATE_CRP;
	info.crp_word = UBERTOOTH_LPC_CRP3;
	mu_assert_int_eq(1, bl_update_file_report(out, "bl_update.dfu", &info));
	fclose(out);
	mu_assert(strstr(buffer, "bl_update.dfu") != NULL,
	          "the refusal does not name the file");
	mu_assert(strstr(buffer, "CRP3") != NULL,
	          "the refusal does not name the protection that was asked for");
	mu_assert(strstr(buffer, "0x43218765") != NULL,
	          "the refusal does not quote the word it found");
	mu_assert(strstr(buffer, "0x2fc") != NULL,
	          "the refusal does not say where the word is");
	mu_assert(strncmp(buffer, "error: ", 7) == 0,
	          "the refusal is not an error line");
	free(buffer);
}

/* An accepted file reports the bootloader it carries and the hash to expect,
 * and says so without an error prefix. */
MU_TEST(an_accepted_file_reports_what_it_carries)
{
	struct bl_update_file info = {0};
	char *buffer = NULL;
	size_t size = 0;
	FILE *out = open_memstream(&buffer, &size);

	mu_assert_fatal(out != NULL, "the memory stream would not open");
	info.result = UBERTOOTH_BL_UPDATE_OK;
	info.bootloader_version = 0x0102;
	info.region_crc32 = 0x2f3c2f8f;
	mu_assert_int_eq(0, bl_update_file_report(out, "bl_update.dfu", &info));
	fclose(out);
	mu_assert(strstr(buffer, "error") == NULL,
	          "an accepted file was reported as an error");
	mu_assert(strstr(buffer, "1.02") != NULL,
	          "the report does not give the bootloader's version");
	mu_assert(strstr(buffer, "0x2f3c2f8f") != NULL,
	          "the report does not give the hash to expect");
	free(buffer);
}

int main(void)
{
	MU_RUN(an_updater_image_is_read);
	MU_RUN(any_other_image_is_refused);
	MU_RUN(a_file_too_short_for_the_payload_is_refused);
	MU_RUN(every_code_read_protection_value_is_refused);
	MU_RUN(a_payload_the_board_would_refuse_is_refused_here);
	MU_RUN(the_hash_is_of_the_region_the_board_will_hold);
	MU_RUN(the_hash_is_the_usual_crc32);
	MU_RUN(a_refusal_says_which_protection_was_asked_for);
	MU_RUN(an_accepted_file_reports_what_it_carries);
	return mu_report();
}
