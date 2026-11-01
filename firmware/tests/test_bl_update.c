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
 * Installing a bootloader: every refusal, and the order the four sectors are
 * written in.
 *
 * firmware/common/bl_update.c reaches the part only through bl_update_ops, so
 * all of it runs here against a model of a part's flash that records what it
 * was asked to do. That matters more for this file than for the others:
 * writing the bootloader region is the one thing in the tree a user cannot
 * put right over USB, and no board can be used to find a mistake in it.
 *
 * The last section reads the linker script, the image's makefile and the
 * shared header as text, because the offset the host reads the payload from
 * is agreed between all three and nothing at run time would notice them
 * drifting apart.
 */
#include <stdarg.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "minunit.h"
#include "source_scan.h"
#include "bl_update.h"
#include "ubertooth_interface.h"

#define PAYLOAD_LENGTH UBERTOOTH_BL_UPDATE_PAYLOAD_LENGTH

static uint8_t payload[PAYLOAD_LENGTH];
static uint8_t installed[PAYLOAD_LENGTH];
static uint8_t buffer[BL_UPDATE_SECTOR_LENGTH];

/*
 * The model of a part.
 *
 * Every call the code under test makes is appended to a log as one line, so a
 * case can read the whole sequence back as text and see the order rather than
 * only the outcome. flash holds what was programmed, prepared says which
 * sectors are prepared, and the fail_* fields make one command answer a
 * failure so the refusal it produces can be checked.
 */
#define LOG_MAX 4096

struct fake_part {
	char log[LOG_MAX];
	uint8_t flash[PAYLOAD_LENGTH];
	int prepared[BL_UPDATE_SECTOR_COUNT];
	uint32_t part_id;
	int part_readable;
	/* The answer sector_present() gives for the sector asked about. */
	int top_sector_present;
	int fail_prepare_at;  /* 1-based call number to fail, 0 for none */
	int prepare_calls;
	int fail_erase;
	int fail_program_sector; /* 0-based sector to fail, -1 for none */
	int fail_verify_sector;
	/* Bytes of flash to corrupt after programming, so the read-back has
	 * something to catch: a length of 0 corrupts nothing. */
	uint32_t corrupt_offset;
	uint32_t corrupt_length;
	int programmed_without_prepare;
};

static struct fake_part part;

static void note(const char *format, ...)
{
	va_list args;
	size_t used = strlen(part.log);

	va_start(args, format);
	vsnprintf(part.log + used, sizeof(part.log) - used, format, args);
	va_end(args);
}

static int fake_read_part(void *context, uint32_t *part_id)
{
	(void)context;
	note("read_part\n");
	if (!part.part_readable)
		return -1;
	*part_id = part.part_id;
	return 0;
}

static int fake_sector_present(void *context, uint32_t sector)
{
	(void)context;
	note("sector_present %u\n", sector);
	return part.top_sector_present;
}

static int fake_prepare(void *context, uint32_t first, uint32_t last)
{
	uint32_t s;

	(void)context;
	note("prepare %u-%u\n", first, last);
	part.prepare_calls++;
	if (part.fail_prepare_at == part.prepare_calls)
		return -1;
	for (s = first; s <= last && s < BL_UPDATE_SECTOR_COUNT; s++)
		part.prepared[s] = 1;
	return 0;
}

static int fake_erase(void *context, uint32_t first, uint32_t last)
{
	uint32_t s;

	(void)context;
	note("erase %u-%u\n", first, last);
	if (part.fail_erase)
		return -1;
	for (s = first; s <= last && s < BL_UPDATE_SECTOR_COUNT; s++) {
		if (!part.prepared[s])
			return -1;
		memset(part.flash + s * BL_UPDATE_SECTOR_LENGTH, 0xff,
		       BL_UPDATE_SECTOR_LENGTH);
		/* The part clears a sector's preparation when it writes it. */
		part.prepared[s] = 0;
	}
	return 0;
}

static int fake_program(void *context, uint32_t address, const uint8_t *ram,
                        uint32_t length)
{
	const uint32_t sector = address / BL_UPDATE_SECTOR_LENGTH;

	(void)context;
	note("program 0x%04x %u\n", address, length);
	if (sector < BL_UPDATE_SECTOR_COUNT && !part.prepared[sector])
		part.programmed_without_prepare = 1;
	if ((int)sector == part.fail_program_sector)
		return -1;
	memcpy(part.flash + address, ram, length);
	if (sector < BL_UPDATE_SECTOR_COUNT)
		part.prepared[sector] = 0;
	if (part.corrupt_length != 0) {
		uint32_t i;

		for (i = 0; i < part.corrupt_length; i++)
			part.flash[part.corrupt_offset + i] ^= 0xff;
		part.corrupt_length = 0;
	}
	return 0;
}

static int fake_verify(void *context, uint32_t address, const uint8_t *ram,
                       uint32_t length)
{
	const uint32_t sector = address / BL_UPDATE_SECTOR_LENGTH;

	(void)context;
	note("verify 0x%04x %u\n", address, length);
	if ((int)sector == part.fail_verify_sector)
		return -1;
	return memcmp(part.flash + address, ram, length) == 0 ? 0 : -1;
}

static const struct bl_update_ops fake_ops = {
	fake_read_part,
	fake_sector_present,
	fake_prepare,
	fake_erase,
	fake_program,
	fake_verify,
};

/* A part that answers everything, with flash full of some other bootloader. */
static void reset_part(void)
{
	memset(&part, 0, sizeof(part));
	part.part_id = 0x25011722u;  /* LPC1754, the part the schematic names */
	part.part_readable = 1;
	part.top_sector_present = 1;
	part.fail_program_sector = -1;
	part.fail_verify_sector = -1;
	memset(part.flash, 0x5a, sizeof(part.flash));
}

static void write_le32(uint8_t *bytes, uint32_t value)
{
	bytes[0] = (uint8_t)(value & 0xff);
	bytes[1] = (uint8_t)((value >> 8) & 0xff);
	bytes[2] = (uint8_t)((value >> 16) & 0xff);
	bytes[3] = (uint8_t)((value >> 24) & 0xff);
}

static uint32_t read_le32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
	       ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

/*
 * A payload a part would take: a plausible vector table, a version block, no
 * protection word, and bytes elsewhere that are not all the same, so a
 * misplaced copy shows up.
 */
static void build_good_payload(void)
{
	static const char magic[] = UBERTOOTH_BL_VERSION_MAGIC;
	size_t i;

	for (i = 0; i < PAYLOAD_LENGTH; i++)
		payload[i] = (uint8_t)(i * 31u + 7u);
	write_le32(payload, 0x10003fe0u);      /* stack pointer, top of SRAM */
	write_le32(payload + 4, 0x00000301u);  /* reset vector, Thumb */
	write_le32(payload + UBERTOOTH_BL_UPDATE_CHECKSUM_INDEX * 4, 0);
	write_le32(payload + UBERTOOTH_LPC_CRP_OFFSET, 0xffffffffu);
	memcpy(payload + UBERTOOTH_BL_VERSION_OFFSET, magic,
	       UBERTOOTH_BL_VERSION_MAGIC_LEN);
	write_le32(payload + UBERTOOTH_BL_VERSION_OFFSET +
	           UBERTOOTH_BL_VERSION_OFF_VERSION, 0x0102u);
	memset(installed, 0x5a, sizeof(installed));
}

/*
 * The installed region is handed over as an address, which is what the board
 * does with flash address 0; the test's own array stands in for it. A pointer
 * parameter would have needed a null check, and a cast of 0 to a pointer is
 * exactly what such a check turns away -- so the board would never have
 * compared, and would have reprogrammed the region every time it was asked.
 * The two cases below that get UBERTOOTH_BL_UPDATE_SAME and
 * UBERTOOTH_BL_UPDATE_OK over the same array are what hold that.
 */
static int run(struct bl_update_report *report)
{
	return bl_update_run(&fake_ops, NULL, payload, PAYLOAD_LENGTH,
	                     (uintptr_t)installed, buffer, report);
}

static int check_payload(size_t length, struct bl_update_report *report)
{
	return bl_update_check(payload, length, (uintptr_t)installed, report);
}

/* ------------------------------------------------------------------ */
/* What the bytes alone decide                                        */
/* ------------------------------------------------------------------ */

MU_TEST(a_whole_region_is_accepted)
{
	struct bl_update_report report;

	build_good_payload();
	reset_part();
	mu_assert_int_eq(UBERTOOTH_BL_UPDATE_OK,
	                 check_payload(PAYLOAD_LENGTH, &report));
	mu_assert_int_eq(0x0102, report.bootloader_version);
}

MU_TEST(a_payload_that_is_not_the_whole_region_is_refused)
{
	struct bl_update_report report;

	build_good_payload();
	mu_assert_int_eq(UBERTOOTH_BL_UPDATE_SHORT,
	                 check_payload(PAYLOAD_LENGTH - 1, &report));
	mu_assert_int_eq(UBERTOOTH_BL_UPDATE_SHORT,
	                 check_payload(PAYLOAD_LENGTH + 1, &report));
	mu_assert_int_eq(UBERTOOTH_BL_UPDATE_SHORT,
	                 bl_update_check(NULL, PAYLOAD_LENGTH,
	                                 (uintptr_t)installed, &report));
}

/*
 * Each of the four values the boot ROM reads as a request for protection.
 * This is the refusal that matters most: every one of them shuts a way into
 * the part, and CRP3 and NO_ISP shut the last one.
 */
MU_TEST(every_code_read_protection_value_is_refused)
{
	static const uint32_t values[] = {
		UBERTOOTH_LPC_CRP1, UBERTOOTH_LPC_CRP2,
		UBERTOOTH_LPC_CRP3, UBERTOOTH_LPC_NO_ISP,
	};
	struct bl_update_report report;
	unsigned i;

	for (i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
		build_good_payload();
		write_le32(payload + UBERTOOTH_LPC_CRP_OFFSET, values[i]);
		mu_assert_int_eq(UBERTOOTH_BL_UPDATE_CRP,
		                 check_payload(PAYLOAD_LENGTH, &report));
		mu_assert(ubertooth_lpc_crp_name(values[i]) != NULL,
		          "a refused protection value has no name to report");
	}
}

/* The protection word is answered before anything else about the payload, so
 * a payload that is wrong in some other way as well is still reported as the
 * protection it asks for: that is the reason a user must be given. */
MU_TEST(protection_is_reported_before_any_other_fault)
{
	struct bl_update_report report;

	build_good_payload();
	write_le32(payload + UBERTOOTH_LPC_CRP_OFFSET, UBERTOOTH_LPC_CRP3);
	memset(payload + UBERTOOTH_BL_VERSION_OFFSET, 0,
	       UBERTOOTH_BL_VERSION_MAGIC_LEN);
	write_le32(payload, 0);
	write_le32(payload + 4, 0);
	mu_assert_int_eq(UBERTOOTH_BL_UPDATE_CRP,
	                 check_payload(PAYLOAD_LENGTH, &report));
}

/* Any other word asks for nothing, including the two a payload is most likely
 * to carry: erased flash and a zero. */
MU_TEST(an_ordinary_word_at_the_protection_offset_is_accepted)
{
	static const uint32_t values[] = {
		0xffffffffu, 0x00000000u, 0x12345679u, 0x87654320u,
	};
	struct bl_update_report report;
	unsigned i;

	for (i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
		build_good_payload();
		write_le32(payload + UBERTOOTH_LPC_CRP_OFFSET, values[i]);
		mu_assert_int_eq(UBERTOOTH_BL_UPDATE_OK,
		                 check_payload(PAYLOAD_LENGTH, &report));
	}
}

MU_TEST(a_payload_with_no_version_block_is_refused)
{
	struct bl_update_report report;

	build_good_payload();
	payload[UBERTOOTH_BL_VERSION_OFFSET] ^= 0xff;
	mu_assert_int_eq(UBERTOOTH_BL_UPDATE_NO_VERSION_BLOCK,
	                 check_payload(PAYLOAD_LENGTH, &report));
	mu_assert_int_eq(0, report.bootloader_version);
}

MU_TEST(a_payload_with_no_startable_vector_table_is_refused)
{
	struct bl_update_report report;

	/* Erased flash. */
	build_good_payload();
	write_le32(payload, 0xffffffffu);
	write_le32(payload + 4, 0xffffffffu);
	mu_assert_int_eq(UBERTOOTH_BL_UPDATE_VECTORS,
	                 check_payload(PAYLOAD_LENGTH, &report));

	/* Zeros. */
	build_good_payload();
	write_le32(payload, 0);
	write_le32(payload + 4, 0);
	mu_assert_int_eq(UBERTOOTH_BL_UPDATE_VECTORS,
	                 check_payload(PAYLOAD_LENGTH, &report));

	/* A stack pointer outside RAM. */
	build_good_payload();
	write_le32(payload, 0x00001000u);
	mu_assert_int_eq(UBERTOOTH_BL_UPDATE_VECTORS,
	                 check_payload(PAYLOAD_LENGTH, &report));

	/* A stack pointer that is not word-aligned. */
	build_good_payload();
	write_le32(payload, 0x10003fe2u);
	mu_assert_int_eq(UBERTOOTH_BL_UPDATE_VECTORS,
	                 check_payload(PAYLOAD_LENGTH, &report));

	/* An ARM rather than a Thumb reset vector. */
	build_good_payload();
	write_le32(payload + 4, 0x00000300u);
	mu_assert_int_eq(UBERTOOTH_BL_UPDATE_VECTORS,
	                 check_payload(PAYLOAD_LENGTH, &report));

	/* An entry point above the bootloader's own region, which is the
	 * application's code: a bootloader does not start there. */
	build_good_payload();
	write_le32(payload + 4, 0x00004001u);
	mu_assert_int_eq(UBERTOOTH_BL_UPDATE_VECTORS,
	                 check_payload(PAYLOAD_LENGTH, &report));
}

/*
 * A region that already holds these bytes is not written again. The
 * comparison has to allow for the checksum word this code fills in, or a
 * board would be reprogrammed every time it was asked, which is the one
 * operation worth doing as rarely as possible.
 */
MU_TEST(a_region_that_already_holds_the_payload_is_left_alone)
{
	struct bl_update_report report;

	build_good_payload();
	reset_part();
	memcpy(installed, payload, PAYLOAD_LENGTH);
	write_le32(installed + UBERTOOTH_BL_UPDATE_CHECKSUM_INDEX * 4,
	           ubertooth_bl_update_vector_checksum(payload));
	mu_assert_int_eq(UBERTOOTH_BL_UPDATE_SAME, run(&report));
	mu_assert_int_eq(0, report.sectors_written);
	mu_assert(part.log[0] == '\0',
	          "a region that already holds the payload still reached the part");
}

MU_TEST(a_region_holding_the_payload_with_no_checksum_is_written)
{
	struct bl_update_report report;

	/* The same bytes as the file, checksum word still zero: a board whose
	 * bootloader was installed without one. It is not the same bootloader
	 * the updater would leave, so it is written. */
	build_good_payload();
	reset_part();
	memcpy(installed, payload, PAYLOAD_LENGTH);
	mu_assert_int_eq(UBERTOOTH_BL_UPDATE_OK, run(&report));
	mu_assert_int_eq(4, report.sectors_written);
}

/* ------------------------------------------------------------------ */
/* What the part decides                                              */
/* ------------------------------------------------------------------ */

MU_TEST(a_part_that_will_not_name_itself_is_refused)
{
	struct bl_update_report report;

	build_good_payload();
	reset_part();
	part.part_readable = 0;
	mu_assert_int_eq(UBERTOOTH_BL_UPDATE_PART_UNREADABLE, run(&report));
	mu_assert_int_eq(0, report.sectors_written);
	mu_assert(strstr(part.log, "erase") == NULL,
	          "a part that would not name itself was erased anyway");

	/* A part identification number of zero is no part: no part in the
	 * family has it, so it is a ROM that answered without answering. */
	build_good_payload();
	reset_part();
	part.part_id = 0;
	mu_assert_int_eq(UBERTOOTH_BL_UPDATE_PART_UNREADABLE, run(&report));
}

MU_TEST(a_part_without_the_region_is_refused)
{
	struct bl_update_report report;

	build_good_payload();
	reset_part();
	part.top_sector_present = 0;
	mu_assert_int_eq(UBERTOOTH_BL_UPDATE_PART_TOO_SMALL, run(&report));
	mu_assert_int_eq(0, report.sectors_written);
	mu_assert(strstr(part.log, "erase") == NULL,
	          "a part without the region was erased anyway");

	/* A part that would not say is not a part that is too small. */
	build_good_payload();
	reset_part();
	part.top_sector_present = -1;
	mu_assert_int_eq(UBERTOOTH_BL_UPDATE_PART_UNREADABLE, run(&report));
}

MU_TEST(the_part_is_reported_when_it_was_read)
{
	struct bl_update_report report;

	build_good_payload();
	reset_part();
	mu_assert_int_eq(UBERTOOTH_BL_UPDATE_OK, run(&report));
	mu_assert_int_eq(0x25011722, (int)report.part_id);
}

/* ------------------------------------------------------------------ */
/* The write order                                                    */
/* ------------------------------------------------------------------ */

/*
 * The whole sequence, read back as text.
 *
 * The region is prepared and erased in one command before any of it is
 * programmed, so a run that stops part way leaves erased flash where the
 * vector table belongs and the boot ROM starts its serial programmer. Then
 * each sector is prepared immediately before its own program, because the ROM
 * clears the preparation as it programs. The read-back comes after all four,
 * so a later write that disturbed an earlier sector is still caught.
 */
MU_TEST(the_sectors_are_erased_before_any_is_programmed)
{
	static const char expected[] =
		"read_part\n"
		"sector_present 3\n"
		"prepare 0-3\n"
		"erase 0-3\n"
		"prepare 0-0\n"
		"program 0x0000 4096\n"
		"prepare 1-1\n"
		"program 0x1000 4096\n"
		"prepare 2-2\n"
		"program 0x2000 4096\n"
		"prepare 3-3\n"
		"program 0x3000 4096\n"
		"verify 0x0000 4096\n"
		"verify 0x1000 4096\n"
		"verify 0x2000 4096\n"
		"verify 0x3000 4096\n";
	struct bl_update_report report;

	build_good_payload();
	reset_part();
	mu_assert_int_eq(UBERTOOTH_BL_UPDATE_OK, run(&report));
	if (strcmp(expected, part.log) != 0)
		printf("  wanted:\n%s  got:\n%s", expected, part.log);
	mu_assert(strcmp(expected, part.log) == 0,
	          "the bootloader region was not written in the order "
	          "bl_update.h describes");
	mu_assert_int_eq(4, report.sectors_written);
	mu_assert_int_eq(4, report.sectors_verified);
	mu_assert_int_eq(0, part.programmed_without_prepare);
}

/* Every byte of the region reaches flash, and the vector checksum is the one
 * byte range that differs from the file. */
MU_TEST(the_region_in_flash_is_the_payload_with_a_filled_in_checksum)
{
	struct bl_update_report report;
	uint32_t checksum;
	uint32_t sum = 0;
	unsigned i;

	build_good_payload();
	reset_part();
	mu_assert_int_eq(UBERTOOTH_BL_UPDATE_OK, run(&report));

	checksum = read_le32(part.flash + UBERTOOTH_BL_UPDATE_CHECKSUM_INDEX * 4);
	mu_assert(checksum != 0, "the vector checksum was left unfilled");
	for (i = 0; i < UBERTOOTH_BL_UPDATE_CHECKED_WORDS; i++)
		sum += read_le32(part.flash + i * 4);
	mu_assert_int_eq(0, (int)sum);

	/* Everything outside the checksum word is the payload byte for byte. */
	mu_assert(memcmp(part.flash, payload, UBERTOOTH_BL_UPDATE_CHECKSUM_INDEX * 4) == 0,
	          "the words before the checksum are not the payload's");
	mu_assert(memcmp(part.flash + UBERTOOTH_BL_UPDATE_CHECKED_WORDS * 4,
	                 payload + UBERTOOTH_BL_UPDATE_CHECKED_WORDS * 4,
	                 PAYLOAD_LENGTH - UBERTOOTH_BL_UPDATE_CHECKED_WORDS * 4) == 0,
	          "the rest of the region is not the payload's");
}

/* The checksum covers the seven words in front of it and nothing else, so a
 * payload that already carries a right one is unchanged by this. */
MU_TEST(a_payload_that_already_adds_up_is_left_as_it_is)
{
	uint32_t checksum;

	build_good_payload();
	checksum = ubertooth_bl_update_vector_checksum(payload);
	write_le32(payload + UBERTOOTH_BL_UPDATE_CHECKSUM_INDEX * 4, checksum);
	mu_assert_int_eq((int)checksum, (int)ubertooth_bl_update_vector_checksum(payload));
}

MU_TEST(a_sector_the_part_will_not_prepare_stops_the_run)
{
	struct bl_update_report report;

	/* The first prepare, over the whole region, before any erase. */
	build_good_payload();
	reset_part();
	part.fail_prepare_at = 1;
	mu_assert_int_eq(UBERTOOTH_BL_UPDATE_PREPARE_FAILED, run(&report));
	mu_assert(strstr(part.log, "erase") == NULL,
	          "the region was erased after the prepare was refused");

	/* The prepare in front of the third sector's program. */
	build_good_payload();
	reset_part();
	part.fail_prepare_at = 4;
	mu_assert_int_eq(UBERTOOTH_BL_UPDATE_PREPARE_FAILED, run(&report));
	mu_assert_int_eq(2, report.sectors_written);
	mu_assert(strstr(part.log, "verify") == NULL,
	          "the region was read back after the run had stopped");
}

MU_TEST(an_erase_the_part_refuses_stops_the_run)
{
	struct bl_update_report report;

	build_good_payload();
	reset_part();
	part.fail_erase = 1;
	mu_assert_int_eq(UBERTOOTH_BL_UPDATE_ERASE_FAILED, run(&report));
	mu_assert_int_eq(0, report.sectors_written);
	mu_assert(strstr(part.log, "program") == NULL,
	          "a sector was programmed after the erase was refused");
}

MU_TEST(a_program_the_part_refuses_stops_the_run)
{
	struct bl_update_report report;

	build_good_payload();
	reset_part();
	part.fail_program_sector = 2;
	mu_assert_int_eq(UBERTOOTH_BL_UPDATE_WRITE_FAILED, run(&report));
	mu_assert_int_eq(2, report.sectors_written);
	mu_assert_int_eq(0, report.sectors_verified);
	mu_assert(strstr(part.log, "verify") == NULL,
	          "the region was read back after a program was refused");
}

/* The read-back is the point of the last step: a region that does not hold
 * what was sent is a failure, not a success. */
MU_TEST(a_region_that_reads_back_wrong_is_a_failure)
{
	struct bl_update_report report;

	/* A part that says so itself. */
	build_good_payload();
	reset_part();
	part.fail_verify_sector = 1;
	mu_assert_int_eq(UBERTOOTH_BL_UPDATE_READ_BACK_FAILED, run(&report));
	mu_assert_int_eq(4, report.sectors_written);
	mu_assert_int_eq(1, report.sectors_verified);

	/* Flash that took the bytes and then held different ones. The
	 * corruption is in the first sector, which was programmed first, so
	 * only a read-back after every program catches it. */
	build_good_payload();
	reset_part();
	part.corrupt_offset = 0x40;
	part.corrupt_length = 4;
	mu_assert_int_eq(UBERTOOTH_BL_UPDATE_READ_BACK_FAILED, run(&report));
	mu_assert_int_eq(0, report.sectors_verified);
}

MU_TEST(a_run_with_no_buffer_writes_nothing)
{
	struct bl_update_report report;

	build_good_payload();
	reset_part();
	mu_assert(bl_update_run(&fake_ops, NULL, payload, PAYLOAD_LENGTH,
	                        (uintptr_t)installed, NULL, &report) < 0,
	          "a run with no buffer to program from was not refused");
	mu_assert(part.log[0] == '\0', "a run with no buffer reached the part");
}

/* Every result has words of its own, so a report cannot name two outcomes the
 * same way or leave one unnamed. */
MU_TEST(every_result_has_its_own_phrase)
{
	int results[] = {
		UBERTOOTH_BL_UPDATE_OK, UBERTOOTH_BL_UPDATE_SAME,
		UBERTOOTH_BL_UPDATE_SHORT,
		UBERTOOTH_BL_UPDATE_NO_VERSION_BLOCK,
		UBERTOOTH_BL_UPDATE_CRP, UBERTOOTH_BL_UPDATE_VECTORS,
		UBERTOOTH_BL_UPDATE_PART_UNREADABLE,
		UBERTOOTH_BL_UPDATE_PART_TOO_SMALL,
		UBERTOOTH_BL_UPDATE_PREPARE_FAILED,
		UBERTOOTH_BL_UPDATE_ERASE_FAILED,
		UBERTOOTH_BL_UPDATE_WRITE_FAILED,
		UBERTOOTH_BL_UPDATE_READ_BACK_FAILED,
		UBERTOOTH_BL_UPDATE_NOT_UPDATER,
		UBERTOOTH_BL_UPDATE_FILE_SHORT,
	};
	const int count = (int)(sizeof(results) / sizeof(results[0]));
	int i, j;

	for (i = 0; i < count; i++) {
		const char *phrase = ubertooth_bl_update_result_string(results[i]);

		mu_assert(phrase != NULL && phrase[0] != '\0',
		          "a result has no phrase to report");
		for (j = 0; j < i; j++)
			mu_assert(strcmp(phrase,
			                 ubertooth_bl_update_result_string(results[j])) != 0,
			          "two results share one phrase");
	}
}

/* ------------------------------------------------------------------ */
/* The numbers three files have to agree on                           */
/* ------------------------------------------------------------------ */

/* The file's whole text with comments stripped, or NULL when it will not
 * open. The caller frees it. Comments name the offsets in prose, so a scan
 * that read them would find whatever it was looking for. */
static char *read_source(const char *path)
{
	char line[1024];
	char *text = NULL;
	size_t used = 0, size = 0;
	FILE *file = fopen(path, "r");
	int in_block = 0;

	if (file == NULL)
		return NULL;
	while (fgets(line, sizeof(line), file) != NULL) {
		size_t length;

		strip_comments(line, &in_block);
		length = strlen(line);
		if (used + length + 1 > size) {
			char *grown = realloc(text, size + length + 4096);

			if (grown == NULL) {
				free(text);
				fclose(file);
				return NULL;
			}
			text = grown;
			size += length + 4096;
		}
		memcpy(text + used, line, length);
		used += length;
		text[used] = '\0';
	}
	fclose(file);
	return text;
}

/*
 * A makefile's text. A makefile's `#` comments are not C comments, so
 * strip_comments() cannot be used on one; skip_comments drops whole comment
 * lines instead, which is what a check that something is ABSENT needs -- the
 * prose beside it says why it is absent, and a search over that prose would
 * find the very word it is looking for. A check that something is present
 * passes either way and reads the file whole.
 */
static char *read_makefile(const char *path, int skip_comments)
{
	char line[1024];
	char *text = NULL;
	size_t used = 0, size = 0;
	FILE *file = fopen(path, "r");

	if (file == NULL)
		return NULL;
	while (fgets(line, sizeof(line), file) != NULL) {
		size_t length;
		const char *at = line;

		while (*at == ' ' || *at == '\t')
			at++;
		if (skip_comments && *at == '#')
			continue;
		length = strlen(line);

		if (used + length + 1 > size) {
			char *grown = realloc(text, size + length + 4096);

			if (grown == NULL) {
				free(text);
				fclose(file);
				return NULL;
			}
			text = grown;
			size += length + 4096;
		}
		memcpy(text + used, line, length);
		used += length;
		text[used] = '\0';
	}
	fclose(file);
	return text;
}

/* The two readings of a makefile the cases below ask for. */
static char *read_plain(const char *path)
{
	return read_makefile(path, 0);
}

static char *read_makefile_recipes(const char *path)
{
	return read_makefile(path, 1);
}

/*
 * The linker script puts the payload at the offset, and the header is where
 * the host reads it from. Nothing at run time would notice them drifting
 * apart: the host would simply read the wrong 16 KiB out of a firmware file
 * and call it a bootloader.
 */
MU_TEST(the_linker_script_places_the_payload_where_the_header_says)
{
	char *text = read_source(FW_COMMON_DIR "/sections.ld");

	mu_assert_fatal(text != NULL, "firmware/common/sections.ld would not open");
	mu_assert(strstr(text, ".bl_payload") != NULL,
	          "sections.ld does not place the bootloader payload section");
	mu_assert(strstr(text, "_interrupt_vector_table + 0x200") != NULL,
	          "sections.ld does not place the payload at offset 0x200, which "
	          "is UBERTOOTH_BL_UPDATE_PAYLOAD_OFFSET");
	mu_assert(strstr(text, "_bl_payload_end - _bl_payload_start == 0x4000") != NULL,
	          "sections.ld does not hold the payload to the whole 16 KiB "
	          "region, which is UBERTOOTH_BL_UPDATE_PAYLOAD_LENGTH");
	free(text);
	mu_assert_int_eq(0x200, UBERTOOTH_BL_UPDATE_PAYLOAD_OFFSET);
	mu_assert_int_eq(0x4000, (int)UBERTOOTH_BL_UPDATE_PAYLOAD_LENGTH);
}

/* The image's makefile checks the finished image at the same offset, in
 * decimal, where the linker script and the header use hex. */
MU_TEST(the_image_makefile_checks_the_same_offset)
{
	char *text = read_plain(FW_BL_UPDATE_DIR "/Makefile");

	mu_assert_fatal(text != NULL, "firmware/bl_update/Makefile would not open");
	mu_assert(strstr(text, "skip=512") != NULL,
	          "the image's makefile does not read the payload at offset 512, "
	          "which is UBERTOOTH_BL_UPDATE_PAYLOAD_OFFSET in decimal");
	mu_assert(strstr(text, "../bootloader/bootloader.bin") != NULL,
	          "the image's makefile does not compare the payload with the "
	          "bootloader it was built from");
	free(text);
}

/* The payload section is the assembler file's, and the padding is the 0xff of
 * erased flash: a region padded with anything else would hash differently
 * from the same bootloader installed over the serial programming interface. */
MU_TEST(the_payload_is_padded_with_erased_flash)
{
	char *text = read_plain(FW_BL_UPDATE_DIR "/bl_payload.S");

	mu_assert_fatal(text != NULL, "firmware/bl_update/bl_payload.S would not open");
	mu_assert(strstr(text, ".incbin \"../bootloader/bootloader.bin\"") != NULL,
	          "bl_payload.S does not include the bootloader binary");
	mu_assert(strstr(text, "0x4000 - (. - bl_payload), 0xff") != NULL,
	          "bl_payload.S does not pad the payload to 16 KiB with 0xff");
	free(text);
}

/*
 * The built image, where there is one: the payload has to be at the offset in
 * the bytes that would be flashed. The image's own makefile checks this as it
 * builds, so this case is for a tree where it has been built and reads as a
 * second pair of eyes; the unit tests run before the build in CI, so it has
 * nothing to read there.
 */
MU_TEST(the_built_image_carries_the_payload_at_the_offset)
{
	const char *image_path = FW_BL_UPDATE_DIR "/bl_update.bin";
	const char *bootloader_path = FW_BOOTLOADER_DIR "/bootloader.bin";
	FILE *image = fopen(image_path, "rb");
	FILE *bootloader;
	uint8_t from_image[256];
	uint8_t from_bootloader[256];

	if (image == NULL) {
		printf("  (%s is not built here)\n", image_path);
		return;
	}
	bootloader = fopen(bootloader_path, "rb");
	if (bootloader == NULL) {
		fclose(image);
		printf("  (%s is not built here)\n", bootloader_path);
		return;
	}
	mu_assert(fseek(image, UBERTOOTH_BL_UPDATE_PAYLOAD_OFFSET, SEEK_SET) == 0,
	          "the built image is too short to hold a payload at the offset");
	mu_assert_int_eq((int)sizeof(from_image),
	                 (int)fread(from_image, 1, sizeof(from_image), image));
	mu_assert_int_eq((int)sizeof(from_bootloader),
	                 (int)fread(from_bootloader, 1, sizeof(from_bootloader),
	                            bootloader));
	fclose(image);
	fclose(bootloader);
	mu_assert(memcmp(from_image, from_bootloader, sizeof(from_image)) == 0,
	          "the built image does not carry the bootloader at "
	          "UBERTOOTH_BL_UPDATE_PAYLOAD_OFFSET");
}

/* The image this tool looks for by name. A host that looked for another name
 * would refuse the image the firmware builds. */
MU_TEST(the_image_is_named_what_the_host_looks_for)
{
	char *text = read_plain(FW_BL_UPDATE_DIR "/Makefile");

	mu_assert_fatal(text != NULL, "firmware/bl_update/Makefile would not open");
	mu_assert(strstr(text, "TARGET = " UBERTOOTH_BL_UPDATE_IMAGE_NAME) != NULL,
	          "the image's target name is not UBERTOOTH_BL_UPDATE_IMAGE_NAME, "
	          "which is the name ubertooth-dfu identifies an updater by");
	free(text);
}

/*
 * The image must not arm the watchdog. Programming four sectors takes longer
 * than the three seconds the application's watchdog allows, and a reset in the
 * middle of the write is the one failure that leaves a board needing the
 * serial programming interface.
 *
 * What stops it is the source list: watchdog.c arms the watchdog, and nothing
 * else in the tree does. So this reads the list rather than a define -- a
 * define that says so and a build that links the file anyway would be the
 * worst of both.
 */
MU_TEST(the_image_does_not_arm_the_watchdog)
{
	char *text = read_makefile_recipes(FW_BL_UPDATE_DIR "/Makefile");

	mu_assert_fatal(text != NULL, "firmware/bl_update/Makefile would not open");
	mu_assert(strstr(text, "watchdog.c") == NULL,
	          "the updater image links watchdog.c, so the watchdog could "
	          "reset the board while the bootloader region is being written");
	free(text);
}

/*
 * The buffer the ROM programs from is aligned well past what it asks for.
 * A source that is not on a word boundary is refused, and the refusal would
 * come after the region had been erased.
 */
MU_TEST(the_programming_buffer_is_aligned)
{
	char *text = read_source(FW_BL_UPDATE_DIR "/bl_update_main.c");

	mu_assert_fatal(text != NULL,
	                "firmware/bl_update/bl_update_main.c would not open");
	mu_assert(strstr(text, "__attribute__((aligned(256)))") != NULL,
	          "the buffer the boot ROM programs from is not aligned, so its "
	          "address rests on where the linker put it");
	free(text);
	/* And the addresses it programs to are sector starts, so each is on the
	 * 256-byte boundary the ROM asks of a destination. */
	mu_assert_int_eq(0, (int)(BL_UPDATE_SECTOR_LENGTH % 256u));
}

/* Interrupts are off for every ROM call: flash is not readable while it is
 * being erased or programmed, so a handler fetched from it would not be
 * there. */
MU_TEST(the_rom_is_called_with_interrupts_off)
{
	char *text = read_source(FW_BL_UPDATE_DIR "/bl_update_main.c");

	mu_assert_fatal(text != NULL,
	                "firmware/bl_update/bl_update_main.c would not open");
	mu_assert(calls_identifier(text, "irq_save_disable"),
	          "the updater does not turn interrupts off around a ROM call");
	mu_assert(calls_identifier(text, "irq_restore"),
	          "the updater does not turn interrupts back on after a ROM call");
	/* One wrapper reaches the ROM, so there is one place the masking can be
	 * read: a second call site could be the one that forgot it. */
	mu_assert(strstr(text, "iap_entry(") != NULL &&
	          strstr(strstr(text, "iap_entry(") + 1, "iap_entry(") == NULL,
	          "the updater reaches the boot ROM from more than one place");
	free(text);
}

/* The board is returned to DFU however the run ended: this image sits in the
 * application region, so a board left running it is a board that cannot be
 * flashed. */
MU_TEST(the_board_is_returned_to_dfu)
{
	char *text = read_source(FW_BL_UPDATE_DIR "/bl_update_main.c");

	mu_assert_fatal(text != NULL,
	                "firmware/bl_update/bl_update_main.c would not open");
	mu_assert(is_assignment_of(text, "bootloader_ctrl", "DFU_MODE"),
	          "the updater does not ask the bootloader for DFU");
	mu_assert(calls_identifier(text, "reset"),
	          "the updater does not reset into the bootloader");
	free(text);
}

int main(void)
{
	MU_RUN(a_whole_region_is_accepted);
	MU_RUN(a_payload_that_is_not_the_whole_region_is_refused);
	MU_RUN(every_code_read_protection_value_is_refused);
	MU_RUN(protection_is_reported_before_any_other_fault);
	MU_RUN(an_ordinary_word_at_the_protection_offset_is_accepted);
	MU_RUN(a_payload_with_no_version_block_is_refused);
	MU_RUN(a_payload_with_no_startable_vector_table_is_refused);
	MU_RUN(a_region_that_already_holds_the_payload_is_left_alone);
	MU_RUN(a_region_holding_the_payload_with_no_checksum_is_written);
	MU_RUN(a_part_that_will_not_name_itself_is_refused);
	MU_RUN(a_part_without_the_region_is_refused);
	MU_RUN(the_part_is_reported_when_it_was_read);
	MU_RUN(the_sectors_are_erased_before_any_is_programmed);
	MU_RUN(the_region_in_flash_is_the_payload_with_a_filled_in_checksum);
	MU_RUN(a_payload_that_already_adds_up_is_left_as_it_is);
	MU_RUN(a_sector_the_part_will_not_prepare_stops_the_run);
	MU_RUN(an_erase_the_part_refuses_stops_the_run);
	MU_RUN(a_program_the_part_refuses_stops_the_run);
	MU_RUN(a_region_that_reads_back_wrong_is_a_failure);
	MU_RUN(a_run_with_no_buffer_writes_nothing);
	MU_RUN(every_result_has_its_own_phrase);
	MU_RUN(the_linker_script_places_the_payload_where_the_header_says);
	MU_RUN(the_image_makefile_checks_the_same_offset);
	MU_RUN(the_payload_is_padded_with_erased_flash);
	MU_RUN(the_built_image_carries_the_payload_at_the_offset);
	MU_RUN(the_image_is_named_what_the_host_looks_for);
	MU_RUN(the_image_does_not_arm_the_watchdog);
	MU_RUN(the_programming_buffer_is_aligned);
	MU_RUN(the_rom_is_called_with_interrupts_off);
	MU_RUN(the_board_is_returned_to_dfu);
	return mu_report();
}
