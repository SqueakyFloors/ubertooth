/*
 * The two blocks every image carries at a fixed offset, and the three places
 * that have to agree about each of them.
 *
 * ubertooth-dfu reads 64 bytes of firmware info out of a file to name the
 * image and the revision it holds, and refuses a file that has no readable
 * block there before it erases anything. It writes 32 bytes of image check
 * block as it flashes, so a board can tell a whole image from a half-written
 * one. Both only work while the firmware, the linker script and the host
 * agree on where each block is, how big it is, and what is in it. Nothing at
 * build time checks all three, so:
 *
 * - ../common/fw_info.c and ../common/image_check.c are compiled here with
 *   the host compiler, against the stub version.h in stubs-fwinfo/, and the
 *   bytes they produce are read the way the host reads them. That is the
 *   end-to-end check: the firmware builds the blocks, the host reads them.
 * - ../common/sections.ld is read as text and the numbers in it are put
 *   against the header's, the way test_bootloader_flash_bounds puts the
 *   bootloader's part table against the host's copy. A linker script cannot
 *   include a C header, so the offset is written out in both places.
 * - each image's request handler is read as text, to check that the request
 *   returns the block rather than something built again by hand.
 *
 * The ASSERTs in sections.ld cover what this cannot: whether the link really
 * put the block at the offset, and whether the vector table has grown into
 * it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#include "minunit.h"
#include "source_scan.h"
#include "ubertooth_interface.h"
#include "version.h"

/* The two request handlers, and what to call them in a message. */
static const char *const handler_paths[] = {
	FW_RXTX_DIR "/bluetooth_rxtx.c",
	FW_BTBR_DIR "/src/btctl/vendor_request_handler.c",
};
#define HANDLER_COUNT (int)(sizeof(handler_paths) / sizeof(handler_paths[0]))

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

/*
 * The block the firmware builds is the block the host reads: the one check
 * that covers both sides at once. The values come from stubs-fwinfo/version.h.
 */
MU_TEST(the_host_reads_the_block_the_firmware_builds)
{
	struct ubertooth_fw_info info;
	int result;

	mu_assert_int_eq(UBERTOOTH_FW_INFO_LENGTH,
	                 (int)sizeof(struct ubertooth_fw_info_block));
	result = ubertooth_fw_info_parse((const uint8_t *)&fw_info_block,
	                                 sizeof(fw_info_block), &info);
	mu_assert_int_eq(UBERTOOTH_FW_INFO_OK, result);
	mu_assert(strcmp(info.image, FW_IMAGE_NAME) == 0,
	          "the block does not name the image version.h gives");
	mu_assert(strcmp(info.revision, GIT_REVISION) == 0,
	          "the block does not carry the revision version.h gives");
	mu_assert_int_eq(UBERTOOTH_FW_INFO_FORMAT, info.format_version);
	mu_assert_int_eq(UBERTOOTH_API_VERSION, info.api_version);
}

/* The magic the firmware spells byte by byte is the macro the host compares
 * against: the field holds no terminator, so the two cannot be written the
 * same way. */
MU_TEST(the_block_starts_with_the_magic_the_host_looks_for)
{
	mu_assert(memcmp(fw_info_block.magic, UBERTOOTH_FW_INFO_MAGIC,
	                 UBERTOOTH_FW_INFO_MAGIC_LEN) == 0,
	          "the block's magic is not UBERTOOTH_FW_INFO_MAGIC");
	mu_assert_int_eq(UBERTOOTH_FW_INFO_MAGIC_LEN,
	                 (int)strlen(UBERTOOTH_FW_INFO_MAGIC));
}

/* A reader working on raw bytes uses the offsets, so they must be where the
 * struct puts the fields: a compiler that padded the struct would move every
 * field after the padding. */
MU_TEST(the_field_offsets_match_the_struct)
{
	mu_assert_int_eq(UBERTOOTH_FW_INFO_OFF_MAGIC,
	                 (int)offsetof(struct ubertooth_fw_info_block, magic));
	mu_assert_int_eq(UBERTOOTH_FW_INFO_OFF_FORMAT,
	                 (int)offsetof(struct ubertooth_fw_info_block,
	                               format_version));
	mu_assert_int_eq(UBERTOOTH_FW_INFO_OFF_API,
	                 (int)offsetof(struct ubertooth_fw_info_block,
	                               api_version));
	mu_assert_int_eq(UBERTOOTH_FW_INFO_OFF_RESERVED,
	                 (int)offsetof(struct ubertooth_fw_info_block, reserved));
	mu_assert_int_eq(UBERTOOTH_FW_INFO_OFF_IMAGE,
	                 (int)offsetof(struct ubertooth_fw_info_block, image));
	mu_assert_int_eq(UBERTOOTH_FW_INFO_OFF_REVISION,
	                 (int)offsetof(struct ubertooth_fw_info_block, revision));
}

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

	snprintf(expected, sizeof(expected),
	         ". = MAX(., _interrupt_vector_table + 0x%x);",
	         UBERTOOTH_FW_INFO_OFFSET);
	mu_assert(strstr(script, expected) != NULL,
	          "sections.ld does not move to UBERTOOTH_FW_INFO_OFFSET before "
	          "the block, so the host would read the wrong bytes");

	mu_assert(strstr(script, "KEEP(*(.fw_info))") != NULL,
	          "sections.ld does not KEEP the .fw_info section, so "
	          "--gc-sections could drop the block");

	/* The link-time checks: the block really landed at the offset, and the
	 * vector table has not grown into it. */
	snprintf(expected, sizeof(expected),
	         "ASSERT(_fw_info_start - _interrupt_vector_table == 0x%x,",
	         UBERTOOTH_FW_INFO_OFFSET);
	mu_assert(strstr(script, expected) != NULL,
	          "sections.ld does not check that the block landed at "
	          "UBERTOOTH_FW_INFO_OFFSET");

	/* The image check block sits below this one, so it is the block the
	 * vector table has to stop short of. */
	snprintf(expected, sizeof(expected),
	         "ASSERT(_interrupt_vector_table_end - _interrupt_vector_table "
	         "<= 0x%x,", UBERTOOTH_IMAGE_CHECK_OFFSET);
	mu_assert(strstr(script, expected) != NULL,
	          "sections.ld does not check that the vector table stops short "
	          "of the blocks");

	snprintf(expected, sizeof(expected),
	         "_fw_info_end - _fw_info_start == %d",
	         UBERTOOTH_FW_INFO_LENGTH);
	mu_assert(strstr(script, expected) != NULL,
	          "sections.ld does not check the block's length against "
	          "UBERTOOTH_FW_INFO_LENGTH");

	free(script);
}

/* ---- the image check block ---------------------------------------------- */

/* The magic the firmware spells byte by byte is the macro the host compares
 * against, as for the block above. */
MU_TEST(the_image_check_block_starts_with_the_magic_the_host_looks_for)
{
	mu_assert(memcmp(image_check_block.magic, UBERTOOTH_IMAGE_CHECK_MAGIC,
	                 UBERTOOTH_IMAGE_CHECK_MAGIC_LEN) == 0,
	          "the block's magic is not UBERTOOTH_IMAGE_CHECK_MAGIC");
	mu_assert_int_eq(UBERTOOTH_IMAGE_CHECK_MAGIC_LEN,
	                 (int)strlen(UBERTOOTH_IMAGE_CHECK_MAGIC));
	mu_assert_int_eq(UBERTOOTH_IMAGE_CHECK_FORMAT,
	                 image_check_block.format_version);
}

/* An image asks for no check until a host has written it: the flags, the
 * length and the checksum are all zero in the image, and a board that reads
 * flags of zero makes no check. That is what keeps an image written by an
 * earlier host tool startable. */
MU_TEST(the_image_carries_the_block_empty)
{
	unsigned i;

	mu_assert_int_eq(0, image_check_block.flags);
	mu_assert_int_eq(0, (int)image_check_block.length);
	mu_assert_int_eq(0, (int)image_check_block.length_complement);
	mu_assert_int_eq(0, (int)image_check_block.checksum);
	for (i = 0; i < sizeof(image_check_block.reserved); i++)
		mu_assert_int_eq(0, image_check_block.reserved[i]);
}

MU_TEST(the_image_check_field_offsets_match_the_struct)
{
	mu_assert_int_eq(UBERTOOTH_IMAGE_CHECK_LENGTH,
	                 (int)sizeof(struct ubertooth_image_check_block));
	mu_assert_int_eq(UBERTOOTH_IMAGE_CHECK_OFF_MAGIC,
	                 (int)offsetof(struct ubertooth_image_check_block, magic));
	mu_assert_int_eq(UBERTOOTH_IMAGE_CHECK_OFF_FORMAT,
	                 (int)offsetof(struct ubertooth_image_check_block,
	                               format_version));
	mu_assert_int_eq(UBERTOOTH_IMAGE_CHECK_OFF_FLAGS,
	                 (int)offsetof(struct ubertooth_image_check_block, flags));
	mu_assert_int_eq(UBERTOOTH_IMAGE_CHECK_OFF_LENGTH,
	                 (int)offsetof(struct ubertooth_image_check_block, length));
	mu_assert_int_eq(UBERTOOTH_IMAGE_CHECK_OFF_LENGTH_COMPLEMENT,
	                 (int)offsetof(struct ubertooth_image_check_block,
	                               length_complement));
	mu_assert_int_eq(UBERTOOTH_IMAGE_CHECK_OFF_CHECKSUM,
	                 (int)offsetof(struct ubertooth_image_check_block,
	                               checksum));
	mu_assert_int_eq(UBERTOOTH_IMAGE_CHECK_OFF_RESERVED,
	                 (int)offsetof(struct ubertooth_image_check_block,
	                               reserved));
}

/*
 * The image check block's offset in the linker script is its offset in the
 * header, for the same reason as the block above. It also has to sit inside
 * the image's first 256 bytes and clear the vector table, which is what
 * leaves it exactly one place to be.
 */
MU_TEST(the_linker_script_puts_the_image_check_block_where_the_host_looks)
{
	char *script = read_source(FW_COMMON_DIR "/sections.ld");
	char expected[256];

	mu_assert_fatal(script != NULL,
	                FW_COMMON_DIR "/sections.ld would not open");

	snprintf(expected, sizeof(expected),
	         ". = MAX(., _interrupt_vector_table + 0x%x);",
	         UBERTOOTH_IMAGE_CHECK_OFFSET);
	mu_assert(strstr(script, expected) != NULL,
	          "sections.ld does not move to UBERTOOTH_IMAGE_CHECK_OFFSET "
	          "before the block, so the host would write the wrong bytes");

	mu_assert(strstr(script, "KEEP(*(.image_check))") != NULL,
	          "sections.ld does not KEEP the .image_check section, so "
	          "--gc-sections could drop the block");

	snprintf(expected, sizeof(expected),
	         "ASSERT(_image_check_start - _interrupt_vector_table == 0x%x,",
	         UBERTOOTH_IMAGE_CHECK_OFFSET);
	mu_assert(strstr(script, expected) != NULL,
	          "sections.ld does not check that the block landed at "
	          "UBERTOOTH_IMAGE_CHECK_OFFSET");

	snprintf(expected, sizeof(expected),
	         "_image_check_end - _image_check_start == %d",
	         UBERTOOTH_IMAGE_CHECK_LENGTH);
	mu_assert(strstr(script, expected) != NULL,
	          "sections.ld does not check the block's length against "
	          "UBERTOOTH_IMAGE_CHECK_LENGTH");

	free(script);
}

/*
 * Where the block has to be. Below the firmware info block, inside the
 * image's first 256 bytes -- the block ubertooth-dfu writes whole or not at
 * all, so that the record of a download reaches the flash with the vector
 * table it describes -- and past the vector table and its checksum word.
 */
MU_TEST(the_image_check_offset_is_in_the_image_first_block)
{
	mu_assert(UBERTOOTH_IMAGE_CHECK_OFFSET + UBERTOOTH_IMAGE_CHECK_LENGTH <=
	          0x100,
	          "the block is not inside the image's first 256 bytes");
	mu_assert(UBERTOOTH_IMAGE_CHECK_OFFSET + UBERTOOTH_IMAGE_CHECK_LENGTH <=
	          UBERTOOTH_FW_INFO_OFFSET,
	          "the block overlaps the firmware info block");
	/* 51 words of vector table today (common/LPC17xx_Interrupts.c), the
	 * checksum word at 0x1c among them. */
	mu_assert(UBERTOOTH_IMAGE_CHECK_OFFSET >= 51 * 4,
	          "the block starts inside the vector table");
}

/*
 * The block clears the vector table's checksum word. The LPC boot ROM reads
 * the eighth word of the table, at 0x1c, as a checksum over the seven before
 * it, so a block that reached back that far would be read as part of the
 * table.
 */
MU_TEST(the_offset_clears_the_vector_table_checksum)
{
	mu_assert(UBERTOOTH_FW_INFO_OFFSET > 0x1c,
	          "the block would overlap the vector table checksum at 0x1c");
	/* 51 words of vector table today (common/LPC17xx_Interrupts.c). The
	 * link fails if it ever reaches the block; this says there is room to
	 * grow rather than that it fits exactly. */
	mu_assert(UBERTOOTH_FW_INFO_OFFSET >= 51 * 4,
	          "the block starts inside the vector table");
}

/*
 * Every image answers the request with the block itself. Writing the fields
 * out again in a handler is how the board and the file would come to disagree,
 * which is the thing this block exists to stop.
 */
MU_TEST(both_handlers_return_the_block_itself)
{
	char line[1024], message[1024];
	int i;

	for (i = 0; i < HANDLER_COUNT; i++) {
		FILE *file = fopen(handler_paths[i], "r");
		int in_block = 0, in_case = 0, copies = 0, lengths = 0;

		snprintf(message, sizeof(message), "%s would not open",
		         handler_paths[i]);
		mu_assert_fatal(file != NULL, message);
		while (fgets(line, sizeof(line), file) != NULL) {
			strip_comments(line, &in_block);
			if (contains_identifier(line, "UBERTOOTH_GET_FW_INFO")) {
				in_case = 1;
				continue;
			}
			if (!in_case)
				continue;
			if (contains_identifier(line, "fw_info_block"))
				copies = 1;
			if (contains_identifier(line, "UBERTOOTH_FW_INFO_LENGTH"))
				lengths++;
			/* The case ends at its break. */
			if (contains_identifier(line, "break"))
				break;
		}
		fclose(file);
		snprintf(message, sizeof(message),
		         "%s: there is no UBERTOOTH_GET_FW_INFO case", handler_paths[i]);
		mu_assert(in_case, message);
		snprintf(message, sizeof(message),
		         "%s: the UBERTOOTH_GET_FW_INFO case does not return "
		         "fw_info_block, so the board could describe itself "
		         "differently from the file it was flashed from",
		         handler_paths[i]);
		mu_assert(copies, message);
		/* Once for the copy and once for the reply length. */
		snprintf(message, sizeof(message),
		         "%s: the UBERTOOTH_GET_FW_INFO case does not use "
		         "UBERTOOTH_FW_INFO_LENGTH for both the copy and the reply "
		         "length", handler_paths[i]);
		mu_assert(lengths >= 2, message);
	}
}

/*
 * The other place an image reports its API version: the USB bcdDevice, which
 * is the number ubertooth_check_api() compares a board against. The block and
 * the descriptor are read by different code on the host, so an image that
 * writes one of them as a bare number can report two different versions --
 * which is what btbr did for twenty-nine minor versions, with its descriptor
 * fixed at 1.06 while its block followed the tree.
 *
 * Each descriptor is read as text, because none of this is reachable from a
 * host build. The field is found by its `// bcdDevice` label, so comments are
 * stripped from the line only after it is found: the label is the field's
 * name, not prose about it. Rename the label and the test says it could not
 * find the field.
 */
MU_TEST(no_image_writes_its_api_version_as_a_bare_number)
{
	static const struct { const char *path; const char *constant; } images[] = {
		{ FW_RXTX_DIR "/ubertooth_usb.c", "UBERTOOTH_API_VERSION" },
		{ FW_BTBR_DIR "/src/drivers/ubertooth_usb.c", "UBERTOOTH_API_VERSION" },
		/* The bootloader's is fixed on purpose and nothing checks it,
		 * but it is still a named constant rather than a literal, so
		 * that it reads as a decision. */
		{ FW_BOOTLOADER_DIR "/bootloader.cpp",
		  "UBERTOOTH_BOOTLOADER_API_VERSION" },
	};
	char line[1024], message[1024];
	size_t i;

	for (i = 0; i < sizeof(images) / sizeof(images[0]); i++) {
		FILE *file = fopen(images[i].path, "r");
		int found = 0, named = 0, in_block = 0;

		snprintf(message, sizeof(message), "%s would not open",
		         images[i].path);
		mu_assert_fatal(file != NULL, message);
		while (fgets(line, sizeof(line), file) != NULL) {
			char code[1024];

			/* Two readings of the same line. The label is a
			 * comment, so it is looked for in the line as written;
			 * the value has to be read with the comments gone, or
			 * prose about the field would count as the field. The
			 * block-comment state carries across lines, so a
			 * comment spanning several of them cannot hide code. */
			snprintf(code, sizeof(code), "%s", line);
			strip_comments(code, &in_block);
			if (strstr(line, "bcdDevice") == NULL ||
			    !calls_identifier(code, "LE_WORD"))
				continue;
			found++;
			if (contains_identifier(code, images[i].constant))
				named++;
		}
		fclose(file);

		snprintf(message, sizeof(message),
		         "%s: expected one bcdDevice field, found %d",
		         images[i].path, found);
		mu_assert(found == 1, message);
		snprintf(message, sizeof(message),
		         "%s: bcdDevice is not %s, so this image can report one "
		         "API version in its USB descriptor and another in its "
		         "firmware info block", images[i].path,
		         images[i].constant);
		mu_assert(named == 1, message);
	}
}

/*
 * The block is added to every image by common.mk, not by each image's own
 * Makefile: that is what stops an image leaving it out. Its object also has to
 * be per-image, because it carries the image's name, so it goes through the
 * same $(per_image) every other object does (test_firmware_object_paths covers
 * what that function produces).
 */
MU_TEST(every_image_gets_the_block_from_the_shared_makefile)
{
	char *makefile = read_source(FW_DIR "/common.mk");

	mu_assert_fatal(makefile != NULL, FW_DIR "/common.mk would not open");
	mu_assert(strstr(makefile, "FW_INFO_SRC = $(LIBS_PATH)/fw_info.c") != NULL,
	          "common.mk does not build ../common/fw_info.c, so images would "
	          "carry no firmware info block");
	mu_assert(strstr(makefile,
	                 "FW_INFO_OBJ = $(call per_image,$(FW_INFO_SRC:%.c=%.o))")
	          != NULL,
	          "common.mk does not build fw_info.o per image, so one image's "
	          "block could be linked into another");
	mu_assert(strstr(makefile, "OBJ += $(FW_INFO_OBJ)") != NULL,
	          "common.mk does not link the firmware info block into the image");
	mu_assert(strstr(makefile, "#define FW_IMAGE_NAME \"$(IMAGE_NAME)\"") != NULL,
	          "common.mk does not give version.h the image name the block is "
	          "built from");
	mu_assert(strstr(makefile, "IMAGE_NAME ?= $(TARGET)") != NULL,
	          "common.mk does not default the image name to the target, so "
	          "an image that sets none would be left without one");
	free(makefile);
}

/* ...and the same for the image check block, for the same reason: an image
 * that left it out would be written with no check over it at all. */
MU_TEST(every_image_gets_the_image_check_block_from_the_shared_makefile)
{
	char *makefile = read_source(FW_DIR "/common.mk");

	mu_assert_fatal(makefile != NULL, FW_DIR "/common.mk would not open");
	mu_assert(strstr(makefile,
	                 "IMAGE_CHECK_SRC = $(LIBS_PATH)/image_check.c") != NULL,
	          "common.mk does not build ../common/image_check.c, so images "
	          "would carry no image check block");
	mu_assert(strstr(makefile,
	                 "OBJ += $(IMAGE_CHECK_OBJ)") != NULL,
	          "common.mk does not link the image check block into the image");
	free(makefile);
}

/*
 * The last line of defence: common.mk reads the finished .bin back and fails
 * the build if the magic is not at the offset. The ASSERTs in sections.ld have
 * to allow a zero-length block, because the bootloader shares the script and
 * carries none, so they cannot catch an image that stopped linking fw_info.o.
 * The numbers in the Makefile are plain text, so check them against the
 * header's.
 */
MU_TEST(the_build_reads_the_block_back_out_of_the_image)
{
	char *makefile = read_source(FW_DIR "/common.mk");
	char expected[128];

	mu_assert_fatal(makefile != NULL, FW_DIR "/common.mk would not open");
	snprintf(expected, sizeof(expected), "skip=%u count=%u",
	         UBERTOOTH_FW_INFO_OFFSET, UBERTOOTH_FW_INFO_MAGIC_LEN);
	mu_assert(strstr(makefile, expected) != NULL,
	          "common.mk does not read the magic at UBERTOOTH_FW_INFO_OFFSET "
	          "back out of the built image");
	snprintf(expected, sizeof(expected), "\"$$magic\" != \"%s\"",
	         UBERTOOTH_FW_INFO_MAGIC);
	mu_assert(strstr(makefile, expected) != NULL,
	          "common.mk does not compare what it read against "
	          "UBERTOOTH_FW_INFO_MAGIC");
	snprintf(expected, sizeof(expected), "skip=%u count=%u",
	         UBERTOOTH_IMAGE_CHECK_OFFSET, UBERTOOTH_IMAGE_CHECK_MAGIC_LEN);
	mu_assert(strstr(makefile, expected) != NULL,
	          "common.mk does not read the magic at "
	          "UBERTOOTH_IMAGE_CHECK_OFFSET back out of the built image");
	snprintf(expected, sizeof(expected), "\"$$magic\" != \"%s\"",
	         UBERTOOTH_IMAGE_CHECK_MAGIC);
	mu_assert(strstr(makefile, expected) != NULL,
	          "common.mk does not compare what it read against "
	          "UBERTOOTH_IMAGE_CHECK_MAGIC");
	free(makefile);
}

int main(void)
{
	printf("fixed-offset image block tests\n");
	MU_RUN(the_host_reads_the_block_the_firmware_builds);
	MU_RUN(the_block_starts_with_the_magic_the_host_looks_for);
	MU_RUN(the_field_offsets_match_the_struct);
	MU_RUN(the_linker_script_puts_the_block_where_the_host_looks);
	MU_RUN(the_offset_clears_the_vector_table_checksum);
	MU_RUN(the_image_check_block_starts_with_the_magic_the_host_looks_for);
	MU_RUN(the_image_carries_the_block_empty);
	MU_RUN(the_image_check_field_offsets_match_the_struct);
	MU_RUN(the_linker_script_puts_the_image_check_block_where_the_host_looks);
	MU_RUN(the_image_check_offset_is_in_the_image_first_block);
	MU_RUN(both_handlers_return_the_block_itself);
	MU_RUN(no_image_writes_its_api_version_as_a_bare_number);
	MU_RUN(every_image_gets_the_block_from_the_shared_makefile);
	MU_RUN(every_image_gets_the_image_check_block_from_the_shared_makefile);
	MU_RUN(the_build_reads_the_block_back_out_of_the_image);
	return mu_report();
}
