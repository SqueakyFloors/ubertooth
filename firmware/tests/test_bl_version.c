/*
 * The bootloader version block, the report an application makes of it, and
 * the places that have to agree about where it is.
 *
 * A board keeps the bootloader it came with, because installing one needs ISP
 * or JTAG, so a running application reading the bootloader's own region is
 * the only way to say which bootloader a board has. That works while the
 * bootloader, the linker script and the host agree on where the block is, how
 * big it is, and what is in it. Nothing at build time checks all of them, so:
 *
 * - ../bootloader/bl_version.c is compiled here with the host compiler,
 *   against the stub version.h in stubs-fwinfo/, and the block it produces is
 *   put into a region ../common/bl_report.c then reports, and the report is
 *   handed to the host's own parser. That is the end-to-end check: the
 *   bootloader builds the block, the application reports it, the host reads
 *   it.
 * - ../common/sections.ld is read as text and the numbers in it are put
 *   against the header's, the way test_fw_info does for the firmware info
 *   block. A linker script cannot include a C header, so the offset is
 *   written out in both places.
 * - the bootloader's Makefile and each image's request handler are read as
 *   text, to check that the block is built into the image and that the
 *   request returns the shared report rather than something assembled by
 *   hand.
 *
 * The ASSERTs in sections.ld cover what this cannot: whether the link really
 * put the block at the offset, whether the vector table has grown into it,
 * and whether it reaches the code read protection word.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#include "minunit.h"
#include "source_scan.h"
#include "bl_report.h"
#include "lpc17.h"
#include "ubertooth_interface.h"
#include "version.h"

/* The two request handlers, and what to call them in a message. */
static const char *const handler_paths[] = {
	FW_RXTX_DIR "/bluetooth_rxtx.c",
	FW_BTBR_DIR "/src/btctl/vendor_request_handler.c",
};
#define HANDLER_COUNT (int)(sizeof(handler_paths) / sizeof(handler_paths[0]))

/* The built image, where the magic is read back out of the bytes that would
 * be flashed. Only built where the ARM toolchain is. */
#define BOOTLOADER_BIN FW_BOOTLOADER_DIR "/bootloader.bin"

/*
 * A stand-in for the bootloader's 16 KiB of flash. The report reads the
 * region through a pointer, so a test can hand it one of these instead of the
 * board's address 0, which no host can read.
 */
static uint8_t region[UBERTOOTH_BOOTLOADER_REGION_LENGTH];

/* Fills the region with something that is not a block anywhere in it, so a
 * case that puts one in is the only reason the report finds one. The pattern
 * is arbitrary and not constant, so a hash of it is worth comparing. */
static void fill_region_with_code(void)
{
	size_t i;

	for (i = 0; i < sizeof(region); i++)
		region[i] = (uint8_t)(i * 7 + 3);
}

/* The file's whole text with comments stripped, or NULL when it will not
 * open. The caller frees it. Comments are stripped because they name the
 * offset and the length in prose, so a scan that read them would find
 * whatever it was looking for. */
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
			char *grown;

			size = (used + length + 1) * 2;
			grown = realloc(text, size);
			if (grown == NULL) {
				free(text);
				fclose(file);
				return NULL;
			}
			text = grown;
		}
		memcpy(text + used, line, length);
		used += length;
		text[used] = '\0';
	}
	fclose(file);
	return text;
}

/* ---- the block's layout ------------------------------------------------ */

/* A reader working on raw bytes uses the offsets, so they must be where the
 * struct puts the fields: a compiler that padded the struct would move every
 * field after the padding. */
MU_TEST(the_field_offsets_match_the_struct)
{
	/* The size is a compile-time assertion in the header
	 * (ubertooth_bl_version_block_is_48_bytes); what needs checking here is
	 * where each field landed inside it. */
	mu_assert_int_eq(UBERTOOTH_BL_VERSION_OFF_MAGIC,
	                 (int)offsetof(struct ubertooth_bl_version_block, magic));
	mu_assert_int_eq(UBERTOOTH_BL_VERSION_OFF_FORMAT,
	                 (int)offsetof(struct ubertooth_bl_version_block,
	                               format_version));
	mu_assert_int_eq(UBERTOOTH_BL_VERSION_OFF_VERSION,
	                 (int)offsetof(struct ubertooth_bl_version_block, version));
	mu_assert_int_eq(UBERTOOTH_BL_VERSION_OFF_RESERVED,
	                 (int)offsetof(struct ubertooth_bl_version_block, reserved));
	mu_assert_int_eq(UBERTOOTH_BL_VERSION_OFF_BUILD,
	                 (int)offsetof(struct ubertooth_bl_version_block, build));
}

/* The magic the bootloader spells byte by byte is the macro the host compares
 * against: the field holds no terminator, so the two cannot be written the
 * same way. It also has to differ from the firmware info block's, because the
 * two share an offset and the magic is what tells them apart. */
MU_TEST(the_block_starts_with_the_magic_the_host_looks_for)
{
	mu_assert(memcmp(bl_version_block.magic, UBERTOOTH_BL_VERSION_MAGIC,
	                 UBERTOOTH_BL_VERSION_MAGIC_LEN) == 0,
	          "the block's magic is not UBERTOOTH_BL_VERSION_MAGIC");
	mu_assert_int_eq(UBERTOOTH_BL_VERSION_MAGIC_LEN,
	                 (int)strlen(UBERTOOTH_BL_VERSION_MAGIC));
	mu_assert_int_eq(UBERTOOTH_FW_INFO_MAGIC_LEN,
	                 UBERTOOTH_BL_VERSION_MAGIC_LEN);
	mu_assert(memcmp(UBERTOOTH_BL_VERSION_MAGIC, UBERTOOTH_FW_INFO_MAGIC,
	                 UBERTOOTH_BL_VERSION_MAGIC_LEN) != 0,
	          "the two blocks share an offset and a magic, so neither could "
	          "be told from the other");
	/* The image check block is the third one in the first 256 bytes of an
	 * image. It is at its own offset, but a reader that mistook one magic
	 * for another would still read the wrong bytes. */
	mu_assert(memcmp(UBERTOOTH_BL_VERSION_MAGIC, UBERTOOTH_IMAGE_CHECK_MAGIC,
	                 UBERTOOTH_BL_VERSION_MAGIC_LEN) != 0,
	          "the version block and the image check block share a magic");
}

/* The block the bootloader builds is the block the host reads. The values
 * come from stubs-fwinfo/version.h. */
MU_TEST(the_host_reads_the_block_the_bootloader_builds)
{
	struct ubertooth_bl_version version;
	int result;

	result = ubertooth_bl_version_parse((const uint8_t *)&bl_version_block,
	                                    sizeof(bl_version_block), &version);
	mu_assert_int_eq(UBERTOOTH_BL_VERSION_OK, result);
	mu_assert_int_eq(UBERTOOTH_BL_VERSION_FORMAT, version.format_version);
	mu_assert_int_eq(UBERTOOTH_BOOTLOADER_VERSION, version.version);
	mu_assert(strcmp(version.build, GIT_REVISION) == 0,
	          "the block does not carry the revision version.h gives");
}

/*
 * The two version numbers a bootloader has, which must not be confused. The
 * block's version says which generation of bootloader this is and moves with
 * a release; the DFU-mode bcdDevice is the DFU protocol version and stays
 * where it is, because ubertooth-dfu has to work against any board.
 */
MU_TEST(the_blocks_version_is_not_the_dfu_protocol_version)
{
	mu_assert(UBERTOOTH_BOOTLOADER_API_VERSION == 0x0100,
	          "the bootloader's DFU-mode bcdDevice has moved, and nothing "
	          "may move it");
	mu_assert(UBERTOOTH_BOOTLOADER_VERSION != UBERTOOTH_BOOTLOADER_API_VERSION,
	          "the block's version and the DFU protocol version are the same "
	          "number, so one could be read for the other");
	/* 2.00 and up. A bootloader that carries a block is from the first fork
	 * release or later, so a board with no block is the 1.x generation, and
	 * that is what -B tells the user. */
	mu_assert(UBERTOOTH_BOOTLOADER_VERSION >= 0x0200,
	          "the block's version is below 2.00, which is the generation a "
	          "board with no block is reported as");
	/* Binary-coded decimal, as the API version is, so each nibble of the
	 * low byte is a decimal digit. */
	mu_assert((UBERTOOTH_BOOTLOADER_VERSION & 0x0f) <= 9 &&
	          ((UBERTOOTH_BOOTLOADER_VERSION >> 4) & 0x0f) <= 9,
	          "the bootloader version is not binary-coded decimal");
}

/* ---- where the block sits ---------------------------------------------- */

/*
 * The offset in the linker script is the offset in the header. The script
 * cannot include the header, so this is the only thing keeping the two in
 * step; getting it wrong would move the block out from under the host without
 * breaking the build.
 */
MU_TEST(the_linker_script_puts_the_block_where_the_host_looks)
{
	char *script = read_source(FW_COMMON_DIR "/sections.ld");
	char expected[256];

	mu_assert_fatal(script != NULL,
	                FW_COMMON_DIR "/sections.ld would not open");

	/* The same move the firmware info block makes: the two share the
	 * offset, so there is one of these rather than two. */
	snprintf(expected, sizeof(expected),
	         ". = MAX(., _interrupt_vector_table + 0x%x);",
	         UBERTOOTH_BL_VERSION_OFFSET);
	mu_assert(strstr(script, expected) != NULL,
	          "sections.ld does not move to UBERTOOTH_BL_VERSION_OFFSET "
	          "before the blocks, so the host would read the wrong bytes");

	mu_assert(strstr(script, "KEEP(*(.bl_version))") != NULL,
	          "sections.ld does not KEEP the .bl_version section, so "
	          "--gc-sections could drop the block");

	/* The link-time checks, each of which has to pass for an image that
	 * carries no such block. */
	snprintf(expected, sizeof(expected),
	         "_bl_version_start - _interrupt_vector_table == 0x%x",
	         UBERTOOTH_BL_VERSION_OFFSET);
	mu_assert(strstr(script, expected) != NULL,
	          "sections.ld does not check that the block landed at "
	          "UBERTOOTH_BL_VERSION_OFFSET");

	snprintf(expected, sizeof(expected),
	         "_bl_version_end - _bl_version_start == %d",
	         UBERTOOTH_BL_VERSION_LENGTH);
	mu_assert(strstr(script, expected) != NULL,
	          "sections.ld does not check the block's length against "
	          "UBERTOOTH_BL_VERSION_LENGTH");

	snprintf(expected, sizeof(expected),
	         "_bl_version_end - _interrupt_vector_table <= 0x%x",
	         UBERTOOTH_LPC_CRP_OFFSET);
	mu_assert(strstr(script, expected) != NULL,
	          "sections.ld does not check that the block stops short of the "
	          "code read protection word");

	/* The two blocks share the offset, so an image carrying both would push
	 * one of them somewhere no host looks. */
	mu_assert(strstr(script,
	                 "_fw_info_end - _fw_info_start == 0 || "
	                 "_bl_version_end - _bl_version_start == 0") != NULL,
	          "sections.ld does not refuse an image carrying both blocks");

	free(script);
}

/*
 * The block clears the two words in the region that are not ours to write:
 * the vector table's checksum at 0x1c, which the LPC boot ROM reads over the
 * table, and the code read protection word at 0x2fc, whose value can lock the
 * part's debug and programming interfaces.
 */
MU_TEST(the_offset_clears_the_checksum_and_the_protection_word)
{
	mu_assert(UBERTOOTH_BL_VERSION_OFFSET > 0x1c,
	          "the block would overlap the vector table checksum at 0x1c");
	/* 51 words of vector table today (common/LPC17xx_Interrupts.c). The
	 * link fails if it ever reaches the block; this says there is room to
	 * grow rather than that it fits exactly. */
	mu_assert(UBERTOOTH_BL_VERSION_OFFSET >= 51 * 4,
	          "the block starts inside the vector table");
	/* And it starts past the image check block, the other block the linker
	 * script places in the first 256 bytes. No image carries both of those
	 * either, but the offsets have to leave room for the case where one
	 * does. */
	mu_assert(UBERTOOTH_BL_VERSION_OFFSET >=
	          UBERTOOTH_IMAGE_CHECK_OFFSET + UBERTOOTH_IMAGE_CHECK_LENGTH,
	          "the version block starts inside the image check block");
	mu_assert(UBERTOOTH_BL_VERSION_OFFSET + UBERTOOTH_BL_VERSION_LENGTH
	          <= UBERTOOTH_LPC_CRP_OFFSET,
	          "the block reaches the code read protection word at 0x2fc");
	/* And the whole block is inside the region an application reads. */
	mu_assert(UBERTOOTH_BL_VERSION_OFFSET + UBERTOOTH_BL_VERSION_LENGTH
	          <= (int)UBERTOOTH_BOOTLOADER_REGION_LENGTH,
	          "the block reaches past the bootloader region");
	mu_assert(UBERTOOTH_LPC_CRP_OFFSET + UBERTOOTH_LPC_CRP_LENGTH
	          <= (int)UBERTOOTH_BOOTLOADER_REGION_LENGTH,
	          "the protection word is outside the bootloader region");
}

/*
 * The block is built into the bootloader and read back out of the finished
 * image. The ASSERTs in sections.ld have to allow a zero-length block,
 * because the application images share the script and carry none, so they
 * cannot catch a bootloader that stopped linking bl_version.o; the Makefile
 * reads the magic out of the .bin instead. Its numbers are plain text, so
 * check them against the header's.
 */
MU_TEST(the_bootloader_build_reads_the_block_back_out_of_the_image)
{
	char *makefile = read_source(FW_BOOTLOADER_DIR "/Makefile");
	char expected[128];

	mu_assert_fatal(makefile != NULL,
	                FW_BOOTLOADER_DIR "/Makefile would not open");
	mu_assert(strstr(makefile, "SRC = bl_version.c") != NULL,
	          "the bootloader's Makefile does not build bl_version.c, so the "
	          "image would carry no version block");
	mu_assert(strstr(makefile, "#define GIT_REVISION \"$(VERSION_NOW)\"")
	          != NULL,
	          "the bootloader's Makefile does not generate the revision the "
	          "block is built from");
	snprintf(expected, sizeof(expected), "skip=%u count=%u",
	         UBERTOOTH_BL_VERSION_OFFSET, UBERTOOTH_BL_VERSION_MAGIC_LEN);
	mu_assert(strstr(makefile, expected) != NULL,
	          "the bootloader's Makefile does not read the magic at "
	          "UBERTOOTH_BL_VERSION_OFFSET back out of the built image");
	snprintf(expected, sizeof(expected), "\"$$magic\" != \"%s\"",
	         UBERTOOTH_BL_VERSION_MAGIC);
	mu_assert(strstr(makefile, expected) != NULL,
	          "the bootloader's Makefile does not compare what it read "
	          "against UBERTOOTH_BL_VERSION_MAGIC");
	free(makefile);
}

/*
 * And the magic really is there, read out of the built image. This needs the
 * ARM toolchain, which a host test run does not have, so a tree with no
 * bootloader.bin has nothing to say: the Makefile check above is what fails a
 * build that lost the block. Where the image is there -- CI builds it, and so
 * does anyone flashing one -- the bytes are read from the file.
 */
MU_TEST(the_built_bootloader_carries_the_magic_at_the_offset)
{
	uint8_t found[UBERTOOTH_BL_VERSION_MAGIC_LEN];
	FILE *image = fopen(BOOTLOADER_BIN, "rb");

	if (image == NULL) {
		printf("    (no " BOOTLOADER_BIN " built; the Makefile check covers "
		       "this)\n");
		return;
	}
	mu_assert(fseek(image, UBERTOOTH_BL_VERSION_OFFSET, SEEK_SET) == 0,
	          BOOTLOADER_BIN " is shorter than the block's offset");
	mu_assert(fread(found, 1, sizeof(found), image) == sizeof(found),
	          BOOTLOADER_BIN " has no block at the offset");
	fclose(image);
	mu_assert(memcmp(found, UBERTOOTH_BL_VERSION_MAGIC, sizeof(found)) == 0,
	          "the built bootloader does not carry "
	          "UBERTOOTH_BL_VERSION_MAGIC at UBERTOOTH_BL_VERSION_OFFSET");
}

/* ---- the report an application makes of it ----------------------------- */

/*
 * The report asks for the user flash at the bottom of the address map before
 * it reads there. The part maps either the flash or the boot ROM over the
 * first 512 bytes, which is where the block sits, and a Cortex-M3 finds its
 * vector table through VTOR instead, so nothing else in this firmware ever
 * had a reason to set the register. With the boot ROM mapped, the report
 * would hash 512 bytes of ROM and find no block on a board that has one.
 *
 * The store reaches a register, so the host build of bl_report.c compiles it
 * to nothing and this reads the file as text, as the tests of the other
 * hardware paths do.
 */
MU_TEST(the_report_maps_the_user_flash_before_reading_it)
{
	char *source = read_source(FW_COMMON_DIR "/bl_report.c");

	mu_assert_fatal(source != NULL,
	                FW_COMMON_DIR "/bl_report.c would not open");
	mu_assert(strstr(source, "MEMMAP = MEMMAP_USER_FLASH;") != NULL,
	          "bl_report.c does not put the user flash at the bottom of the "
	          "address map, so the read could see the boot ROM instead");
	{
		const char *map = strstr(source, "bl_report_map_user_flash();");
		const char *hash = strstr(source, "bl_report_crc32(region,");

		mu_assert(map != NULL,
		          "bl_report_init() does not set the mapping at all");
		mu_assert(hash != NULL,
		          "bl_report_init() does not hash the region");
		mu_assert(map < hash,
		          "bl_report_init() hashes the region before it asks for "
		          "the user flash, so the first 512 bytes could be ROM");
	}
	free(source);

	/* The two values the register takes, against the part's own: user flash
	 * is 1 and the boot ROM 0 (NXP LPC176x/5x user manual UM10360). */
	mu_assert_int_eq(0, MEMMAP_BOOT_ROM);
	mu_assert_int_eq(1, MEMMAP_USER_FLASH);
}

/* The hash is the usual CRC-32, so a value can be checked against any other
 * implementation of it. These four are the published ones. */
MU_TEST(the_hash_is_the_usual_crc32)
{
	static const struct { const char *text; uint32_t crc; } vectors[] = {
		{ "", 0x00000000 },
		{ "a", 0xe8b7be43 },
		{ "123456789", 0xcbf43926 },
		{ "The quick brown fox jumps over the lazy dog", 0x414fa339 },
	};
	size_t i;

	for (i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
		char message[128];

		snprintf(message, sizeof(message),
		         "the CRC-32 of \"%s\" is not 0x%08x", vectors[i].text,
		         vectors[i].crc);
		mu_assert(bl_report_crc32((uintptr_t)vectors[i].text,
		                          strlen(vectors[i].text)) == vectors[i].crc,
		          message);
	}
}

/*
 * A region carrying this tree's bootloader: the report hands back the block
 * itself, and the host's parser reads the version and the revision out of it.
 * The block is the one bl_version.c built, so this is the whole way from the
 * bootloader's source to the host's struct.
 */
MU_TEST(a_region_with_a_block_is_reported_whole)
{
	uint8_t report[UBERTOOTH_BL_REPORT_LENGTH];
	struct ubertooth_bootloader_info info;

	fill_region_with_code();
	memcpy(region + UBERTOOTH_BL_VERSION_OFFSET, &bl_version_block,
	       sizeof(bl_version_block));
	bl_report_init((uintptr_t)region);
	bl_report_build(report);

	mu_assert_int_eq(0, ubertooth_bootloader_info_parse(report, sizeof(report),
	                                                    &info));
	mu_assert_int_eq(UBERTOOTH_BL_VERSION_OK, info.version_result);
	mu_assert_int_eq(UBERTOOTH_BOOTLOADER_VERSION, info.version.version);
	mu_assert(strcmp(info.version.build, GIT_REVISION) == 0,
	          "the report did not carry the bootloader's revision");
	/* The hash is of the whole region, not of the block: it is what names a
	 * bootloader that has no block at all. */
	mu_assert(info.crc32 == bl_report_crc32((uintptr_t)region, sizeof(region)),
	          "the report's hash is not the hash of the whole region");
}

/*
 * A factory bootloader, which is every board as shipped: its own code is at
 * the offset, so there is no block to report. The hash still is, and the host
 * reads the absence rather than 48 bytes of someone's bootloader.
 */
MU_TEST(a_region_with_no_block_still_reports_its_hash)
{
	uint8_t report[UBERTOOTH_BL_REPORT_LENGTH];
	struct ubertooth_bootloader_info info;
	size_t i;

	fill_region_with_code();
	bl_report_init((uintptr_t)region);
	bl_report_build(report);

	mu_assert_int_eq(0, ubertooth_bootloader_info_parse(report, sizeof(report),
	                                                    &info));
	mu_assert_int_eq(UBERTOOTH_BL_VERSION_NO_MAGIC, info.version_result);
	mu_assert(info.crc32 == bl_report_crc32((uintptr_t)region, sizeof(region)),
	          "the report's hash is not the hash of the whole region");
	for (i = 0; i < UBERTOOTH_BL_VERSION_LENGTH; i++) {
		mu_assert(report[UBERTOOTH_BL_REPORT_OFF_BLOCK + i] == 0,
		          "the report sent the bootloader's own code in place of a "
		          "block");
	}
}

/*
 * The hash covers the region to its last byte. A bootloader whose code
 * differs only near the end of its 16 KiB must not read as the same one, or
 * the hash could not tell two bootloaders apart.
 */
MU_TEST(the_hash_covers_the_whole_region)
{
	uint8_t report[UBERTOOTH_BL_REPORT_LENGTH];
	struct ubertooth_bootloader_info first, second;

	fill_region_with_code();
	bl_report_init((uintptr_t)region);
	bl_report_build(report);
	mu_assert_int_eq(0, ubertooth_bootloader_info_parse(report, sizeof(report),
	                                                    &first));

	region[UBERTOOTH_BOOTLOADER_REGION_LENGTH - 1] ^= 0x01;
	bl_report_init((uintptr_t)region);
	bl_report_build(report);
	mu_assert_int_eq(0, ubertooth_bootloader_info_parse(report, sizeof(report),
	                                                    &second));
	mu_assert(first.crc32 != second.crc32,
	          "one changed byte at the end of the region left the hash alone");
}

/*
 * The hash is worked out at startup, not in the request handler. It takes
 * milliseconds, and several capture modes answer requests from the USB
 * interrupt, so a handler that hashed 16 KiB would be long enough inside one
 * to cost a capture transfer. Each image therefore calls bl_report_init()
 * where device_id_init() already is, before any mode can run; the handler
 * copies what it left.
 *
 * Both are startup and interrupt code, so this reads the two images as text,
 * as the tests of the other hardware paths do.
 */
MU_TEST(each_image_works_the_report_out_at_startup)
{
	static const char *const startup_paths[] = {
		FW_RXTX_DIR "/bluetooth_rxtx.c",
		FW_BTBR_DIR "/src/btctl/main.c",
	};
	char message[1024];
	size_t i;

	for (i = 0; i < sizeof(startup_paths) / sizeof(startup_paths[0]); i++) {
		char *source = read_source(startup_paths[i]);

		snprintf(message, sizeof(message), "%s would not open",
		         startup_paths[i]);
		mu_assert_fatal(source != NULL, message);
		const char *init, *usb;

		snprintf(message, sizeof(message),
		         "%s: does not call bl_report_init("
		         "UBERTOOTH_BOOTLOADER_REGION_START) at startup, so the "
		         "hash would be worked out in the request handler",
		         startup_paths[i]);
		init = strstr(source,
		              "bl_report_init(UBERTOOTH_BOOTLOADER_REGION_START);");
		mu_assert(init != NULL, message);

		/* Before the USB stack is brought up, which is what makes "at
		 * startup" mean anything: once the stack is up a request can
		 * arrive, and one answered before this ran would copy an empty
		 * cache and report a hash of zero. */
		usb = strstr(source, "ubertooth_usb_init(");
		snprintf(message, sizeof(message),
		         "%s: no ubertooth_usb_init() call to order against",
		         startup_paths[i]);
		mu_assert(usb != NULL, message);
		snprintf(message, sizeof(message),
		         "%s: bl_report_init() runs after the USB stack is up, so a "
		         "request arriving first would report a hash of zero",
		         startup_paths[i]);
		mu_assert(init < usb, message);
		free(source);
	}

	/* And the handler's own file does not work it out: that is what the
	 * scan below reads each case for. */
	{
		char *source = read_source(FW_COMMON_DIR "/bl_report.c");

		mu_assert_fatal(source != NULL,
		                FW_COMMON_DIR "/bl_report.c would not open");
		mu_assert(strstr(source, "bl_report_crc32(region,") != NULL,
		          "bl_report_init() does not hash the region it was given");
		free(source);
	}
}

/*
 * Every image answers the request from the shared report. Writing the reply
 * out again in a handler is how the two images would come to describe the
 * same board differently.
 */
MU_TEST(both_handlers_return_the_shared_report)
{
	char line[1024], message[1024];
	int i;

	for (i = 0; i < HANDLER_COUNT; i++) {
		FILE *file = fopen(handler_paths[i], "r");
		int in_block = 0, in_case = 0, builds = 0, lengths = 0;

		snprintf(message, sizeof(message), "%s would not open",
		         handler_paths[i]);
		mu_assert_fatal(file != NULL, message);
		while (fgets(line, sizeof(line), file) != NULL) {
			strip_comments(line, &in_block);
			if (contains_identifier(line, "UBERTOOTH_GET_BOOTLOADER_INFO")) {
				in_case = 1;
				continue;
			}
			if (!in_case)
				continue;
			if (calls_identifier(line, "bl_report_build"))
				builds = 1;
			if (contains_identifier(line, "UBERTOOTH_BL_REPORT_LENGTH"))
				lengths = 1;
			/* The case ends at its break. */
			if (contains_identifier(line, "break"))
				break;
		}
		fclose(file);
		snprintf(message, sizeof(message),
		         "%s: there is no UBERTOOTH_GET_BOOTLOADER_INFO case",
		         handler_paths[i]);
		mu_assert(in_case, message);
		snprintf(message, sizeof(message),
		         "%s: the UBERTOOTH_GET_BOOTLOADER_INFO case does not call "
		         "bl_report_build(), so the two images could describe the "
		         "same board differently", handler_paths[i]);
		mu_assert(builds, message);
		snprintf(message, sizeof(message),
		         "%s: the UBERTOOTH_GET_BOOTLOADER_INFO case does not use "
		         "UBERTOOTH_BL_REPORT_LENGTH for the reply length",
		         handler_paths[i]);
		mu_assert(lengths, message);
	}
}

int main(void)
{
	printf("bootloader version block tests\n");
	MU_RUN(the_field_offsets_match_the_struct);
	MU_RUN(the_block_starts_with_the_magic_the_host_looks_for);
	MU_RUN(the_host_reads_the_block_the_bootloader_builds);
	MU_RUN(the_blocks_version_is_not_the_dfu_protocol_version);
	MU_RUN(the_linker_script_puts_the_block_where_the_host_looks);
	MU_RUN(the_offset_clears_the_checksum_and_the_protection_word);
	MU_RUN(the_bootloader_build_reads_the_block_back_out_of_the_image);
	MU_RUN(the_built_bootloader_carries_the_magic_at_the_offset);
	MU_RUN(the_report_maps_the_user_flash_before_reading_it);
	MU_RUN(the_hash_is_the_usual_crc32);
	MU_RUN(a_region_with_a_block_is_reported_whole);
	MU_RUN(a_region_with_no_block_still_reports_its_hash);
	MU_RUN(the_hash_covers_the_whole_region);
	MU_RUN(each_image_works_the_report_out_at_startup);
	MU_RUN(both_handlers_return_the_shared_report);
	return mu_report();
}
