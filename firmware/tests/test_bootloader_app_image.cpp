/*
 * What the bootloader will and will not start.
 *
 * An interrupted firmware download used to leave a board that started an
 * application missing most of itself: the vector table went in first, so it
 * was in place for the whole of the transfer. The board then faulted on every
 * power-up, and the only way back was to bridge the bootloader entry pin at
 * the bench. The host now writes the vector table last and leaves erased
 * flash in its place until then (host/ubertooth-tools/src/ubertooth-dfu.c,
 * download()), and the bootloader refuses to start what that leaves behind.
 *
 * app_image.h compiles here as it is -- it reads no hardware, which is the
 * point of keeping the check in a header of its own -- so the predicate below
 * is the real one. The jump it guards is two instructions of assembly that
 * cannot run on a host, so the last tests read bootloader.cpp as text and
 * show the guard is in front of them, the way test_bootloader_flash_bounds
 * scans dfu.cpp.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "minunit.h"
#include "source_scan.h"

/* The real check. */
#include "app_image.h"

/* The flash bound the bootloader passes in: the largest part in the family,
 * LPC_FLASH_SIZE_FALLBACK in flash.h. Spelled out rather than included,
 * because flash.h pulls in the In-Application Programming code and this test
 * is about the predicate alone. The scan at the end shows the bootloader
 * passes that constant, and reads flash.h to show it still has this value. */
#define FLASH_TOP 0x80000u

/* The sector the application region starts in, LPC_SMALL_SECTOR_SIZE in
 * flash.h, which the host's SECTOR_SIZE has to match: ubertooth-dfu writes
 * that many bytes as the last step of a download, and the erase the
 * bootloader does covers exactly one sector. Read out of flash.h below too.
 */
#define FIRST_SECTOR_SIZE 0x1000u

/* The first two words of a real image, read out of the bluetooth_rxtx.bin
 * this tree builds. The stack pointer is _StackTop, the top of the 16 KiB the
 * images are linked for less the 32 bytes sections.ld keeps back; the reset
 * vector is Reset_Handler with its Thumb bit set, as every entry in a
 * Cortex-M vector table has.
 *
 * The word at offset 0x1c of that image, where the LPC vector checksum goes,
 * reads 0x00000000: the image file carries no checksum and ubertooth-dfu
 * fills the word in as it writes the block. That is why a zero word is
 * accepted as no checksum rather than read as a wrong one. */
#define REAL_STACK_POINTER 0x10003fe0u
#define REAL_RESET_VECTOR  0x00009e99u

/* ---- no image at all ---------------------------------------------------- */

/* Erased flash, which is what the host now leaves at the start of the region
 * for all but the last few blocks of a download. Nothing may be started from
 * it. */
MU_TEST(erased_flash_is_not_startable)
{
	mu_assert(!app_image_startable(0xffffffffu, 0xffffffffu, FLASH_TOP),
	          "erased flash was taken for an application");
}

/* The same read the other way round: the host sends a whole block of 0xFF, so
 * take the two words out of such a block rather than writing them out. */
MU_TEST(the_blank_block_the_host_writes_first_is_not_startable)
{
	uint8_t block[256];
	uint32_t words[2];

	memset(block, 0xff, sizeof(block));
	memcpy(words, block, sizeof(words));
	mu_assert(!app_image_startable(words[0], words[1], FLASH_TOP),
	          "the blank first block was taken for an application");
}

/* Flash erases to 0xFF rather than to zero, so a region of zeros is not an
 * erased one: it is what a region reads as once something has written zeros
 * over it. Nothing may be started from that either. */
MU_TEST(a_region_of_zeros_is_not_startable)
{
	mu_assert(!app_image_startable(0, 0, FLASH_TOP),
	          "a region of zeros was taken for an application");
}

/* Half an image: the code is there, the vector table is not. That is the
 * board this whole change is about. */
MU_TEST(code_with_no_vector_table_is_not_startable)
{
	mu_assert(!app_image_startable(0xffffffffu, REAL_RESET_VECTOR, FLASH_TOP),
	          "an image with no stack pointer was taken for an application");
	mu_assert(!app_image_startable(REAL_STACK_POINTER, 0xffffffffu, FLASH_TOP),
	          "an image with no reset vector was taken for an application");
}

/* ---- a real image ------------------------------------------------------- */

MU_TEST(a_real_image_is_startable)
{
	mu_assert(app_image_startable(REAL_STACK_POINTER, REAL_RESET_VECTOR,
	                              FLASH_TOP),
	          "an image this tree builds was refused");
}

/* Two words read back off a board's flash with ubertooth-dfu -u after a
 * flash, so what the check accepts is not only what an image file says but
 * what a board holds. The reset vector is not the one above and is not meant
 * to be: its address moves with the build, and both have to be accepted. That
 * board was flashed before the checksum was written, so the word at 0x1c read
 * zero there as well. */
MU_TEST(an_image_read_back_off_a_board_is_startable)
{
	mu_assert(app_image_startable(0x10003fe0u, 0x00009f11u, FLASH_TOP),
	          "an image read back off a board was refused");
}

/* The check must not depend on the part: the same image has to start on the
 * 128 KiB part the Ubertooth One schematic specifies and on the larger parts
 * other boards carry. */
MU_TEST(a_real_image_is_startable_whatever_the_part)
{
	mu_assert(app_image_startable(REAL_STACK_POINTER, REAL_RESET_VECTOR,
	                              0x20000u),
	          "an image was refused on a 128 KiB part");
	mu_assert(app_image_startable(REAL_STACK_POINTER, REAL_RESET_VECTOR,
	                              0x80000u),
	          "an image was refused on a 512 KiB part");
}

/* An application right at the top of the region it is allowed, and one right
 * at the bottom, are both startable: the bound is one past the last address
 * with flash behind it. */
MU_TEST(the_ends_of_the_application_region_are_startable)
{
	mu_assert(app_image_startable(REAL_STACK_POINTER, APP_IMAGE_BASE | 1u,
	                              FLASH_TOP),
	          "a reset vector at the base of the region was refused");
	mu_assert(app_image_startable(REAL_STACK_POINTER, (FLASH_TOP - 2) | 1u,
	                              FLASH_TOP),
	          "a reset vector at the top of flash was refused");
}

/* ---- the stack pointer -------------------------------------------------- */

/* A stack in the AHB SRAM is accepted too. No image in this tree links that
 * way, but one outside it could, and refusing it would leave a board waiting
 * in DFU for an image it already has. */
MU_TEST(a_stack_pointer_in_the_ahb_ram_is_startable)
{
	mu_assert(app_image_startable(APP_AHB_RAM_BASE + 0x4000u,
	                              REAL_RESET_VECTOR, FLASH_TOP),
	          "a stack pointer in the AHB SRAM was refused");
	mu_assert(app_image_startable(APP_AHB_RAM_LIMIT, REAL_RESET_VECTOR,
	                              FLASH_TOP),
	          "a stack pointer at the top of the AHB SRAM was refused");
}

MU_TEST(a_stack_pointer_outside_ram_is_not_startable)
{
	/* Below RAM: flash, and the one word of it the processor would push to
	 * would fault. */
	mu_assert(!app_image_startable(0x8000u, REAL_RESET_VECTOR, FLASH_TOP),
	          "a stack pointer in flash was accepted");
	/* At the very base of RAM, so a stack with nothing in it. */
	mu_assert(!app_image_startable(APP_RAM_BASE, REAL_RESET_VECTOR, FLASH_TOP),
	          "a stack pointer at the base of RAM was accepted");
	/* Past the most RAM any part in the family has. */
	mu_assert(!app_image_startable(APP_RAM_LIMIT + 4, REAL_RESET_VECTOR,
	                               FLASH_TOP),
	          "a stack pointer past the end of RAM was accepted");
	/* The peripheral region, which is neither. */
	mu_assert(!app_image_startable(0x40000000u, REAL_RESET_VECTOR, FLASH_TOP),
	          "a stack pointer among the peripherals was accepted");
}

MU_TEST(a_stack_pointer_at_the_top_of_ram_is_startable)
{
	mu_assert(app_image_startable(APP_RAM_LIMIT, REAL_RESET_VECTOR, FLASH_TOP),
	          "a stack pointer at the top of RAM was refused");
}

MU_TEST(an_unaligned_stack_pointer_is_not_startable)
{
	unsigned offset;

	for (offset = 1; offset < 4; offset++)
		mu_assert(!app_image_startable(REAL_STACK_POINTER + offset,
		                               REAL_RESET_VECTOR, FLASH_TOP),
		          "an unaligned stack pointer was accepted");
}

/* ---- the reset vector --------------------------------------------------- */

/* Every entry in a Cortex-M vector table is a Thumb address, so the low bit
 * is set. A word without it is not something the processor could have been
 * given to jump to. */
MU_TEST(a_reset_vector_without_the_thumb_bit_is_not_startable)
{
	mu_assert(!app_image_startable(REAL_STACK_POINTER,
	                               REAL_RESET_VECTOR & ~1u, FLASH_TOP),
	          "a reset vector without the Thumb bit was accepted");
}

MU_TEST(a_reset_vector_outside_the_application_region_is_not_startable)
{
	/* Inside the bootloader's own flash: an application cannot start there,
	 * and the bootloader is what would be jumped into. */
	mu_assert(!app_image_startable(REAL_STACK_POINTER, 0x1001u, FLASH_TOP),
	          "a reset vector inside the bootloader was accepted");
	/* The last word below the region. */
	mu_assert(!app_image_startable(REAL_STACK_POINTER,
	                               (APP_IMAGE_BASE - 2) | 1u, FLASH_TOP),
	          "a reset vector below the region was accepted");
	/* Past the flash the part has. */
	mu_assert(!app_image_startable(REAL_STACK_POINTER, FLASH_TOP | 1u,
	                               FLASH_TOP),
	          "a reset vector past the end of flash was accepted");
	/* In RAM, which holds nothing at power-up. */
	mu_assert(!app_image_startable(REAL_STACK_POINTER, 0x10000001u, FLASH_TOP),
	          "a reset vector in RAM was accepted");
}

/* A caller that knows the part's own flash gets a check bounded by it. */
MU_TEST(a_reset_vector_past_the_given_bound_is_not_startable)
{
	mu_assert(!app_image_startable(REAL_STACK_POINTER, 0x30001u, 0x20000u),
	          "a reset vector past a 128 KiB bound was accepted");
}

/* The bootloader is not that caller: it passes the largest part in the
 * family, so on a 128 KiB part a reset vector at 0x30000 is accepted and
 * jumped into, which faults. That is deliberate, and app_image.h says why --
 * reading the part's own bound means ROM calls on every power-up in code this
 * tree cannot exercise on hardware. Pinned here so the next reader finds it
 * stated rather than has to work it out. */
MU_TEST(the_bootloaders_own_bound_is_the_familys_not_the_parts)
{
	mu_assert(app_image_startable(REAL_STACK_POINTER, 0x30001u, FLASH_TOP),
	          "the family-wide bound no longer accepts this");
}

/* ---- the vector checksum ------------------------------------------------ */

/* The eight words at the start of a real image: the stack pointer, the reset
 * vector, five handlers, and the checksum word. The handler addresses are
 * arbitrary here -- the check does not read them on their own -- but they
 * have to be in the sum, which is the point of filling them in. */
static void a_vector_table(uint32_t *vectors)
{
	unsigned i;

	vectors[0] = REAL_STACK_POINTER;
	vectors[1] = REAL_RESET_VECTOR;
	for (i = 2; i < APP_IMAGE_CHECKED_WORDS; i++)
		vectors[i] = APP_IMAGE_BASE + 0x100u * i + 1u;
	vectors[APP_IMAGE_CHECKSUM_INDEX] = 0;
}

/* What ubertooth-dfu writes into the checksum word: the two's complement of
 * the seven words in front of it. Worked out here rather than copied from the
 * tool, so the two have to agree about the rule and not only about a
 * number. */
static void checksum_the_table(uint32_t *vectors)
{
	uint32_t sum = 0;
	unsigned i;

	for (i = 0; i < APP_IMAGE_CHECKSUM_INDEX; i++)
		sum += vectors[i];
	vectors[APP_IMAGE_CHECKSUM_INDEX] = 0u - sum;
}

MU_TEST(a_checksummed_vector_table_is_startable)
{
	uint32_t vectors[APP_IMAGE_CHECKED_WORDS];

	a_vector_table(vectors);
	checksum_the_table(vectors);
	mu_assert(app_image_table_startable(vectors, FLASH_TOP, false),
	          "an image carrying a correct checksum was refused");
}

/* Every image this tree builds leaves the word zero, and so does a board
 * flashed by an earlier host tool. Those have to start: refusing them would
 * leave a board waiting in DFU for the image it already has. */
MU_TEST(a_vector_table_with_no_checksum_is_startable)
{
	uint32_t vectors[APP_IMAGE_CHECKED_WORDS];

	a_vector_table(vectors);
	mu_assert_int_eq(0, vectors[APP_IMAGE_CHECKSUM_INDEX]);
	mu_assert(app_image_table_startable(vectors, FLASH_TOP, false),
	          "an image carrying no checksum was refused");
}

/* The sum has to come to zero, whichever word is out: the checksum word
 * itself, or a handler address that changed after the checksum was worked
 * out. */
MU_TEST(a_wrong_checksum_is_not_startable)
{
	uint32_t vectors[APP_IMAGE_CHECKED_WORDS];

	a_vector_table(vectors);
	checksum_the_table(vectors);
	vectors[APP_IMAGE_CHECKSUM_INDEX] += 1;
	mu_assert(!app_image_table_startable(vectors, FLASH_TOP, false),
	          "an image whose checksum was wrong was accepted");

	a_vector_table(vectors);
	checksum_the_table(vectors);
	vectors[3] = APP_IMAGE_BASE + 0x2001u;
	mu_assert(!app_image_table_startable(vectors, FLASH_TOP, false),
	          "a vector table that changed under its checksum was accepted");
}

/* Erased flash in the checksum word, with the two words in front of it
 * written: a first block the part did not finish programming, which is what a
 * board that loses power during that one program can be left with. The two
 * words on their own read plausibly, so this is the case the checksum is here
 * for. */
MU_TEST(a_first_block_programmed_part_way_is_not_startable)
{
	uint32_t vectors[APP_IMAGE_CHECKED_WORDS];
	unsigned i;

	a_vector_table(vectors);
	checksum_the_table(vectors);
	for (i = 4; i < APP_IMAGE_CHECKED_WORDS; i++)
		vectors[i] = 0xffffffffu;
	mu_assert(app_image_startable(vectors[0], vectors[1], FLASH_TOP),
	          "the two words were meant to read plausibly");
	mu_assert(!app_image_table_startable(vectors, FLASH_TOP, false),
	          "a first block that was not programmed whole was accepted");
}

/* A whole block of erased flash: no checksum word to speak of, and refused on
 * the two words, as it was before the checksum was asked for. */
MU_TEST(erased_flash_is_not_startable_whatever_its_checksum)
{
	uint32_t vectors[APP_IMAGE_CHECKED_WORDS];
	unsigned i;

	for (i = 0; i < APP_IMAGE_CHECKED_WORDS; i++)
		vectors[i] = 0xffffffffu;
	mu_assert(!app_image_table_startable(vectors, FLASH_TOP, false),
	          "erased flash was taken for an application");
	/* ...and one whose eight erased words happen to add up is still refused,
	 * since the two words are read as well. */
	checksum_the_table(vectors);
	mu_assert(!app_image_table_startable(vectors, FLASH_TOP, false),
	          "erased flash with a correct checksum was accepted");
}

/* The checksum does not rescue a table the two words refuse: both have to
 * pass. */
MU_TEST(a_checksummed_table_with_bad_words_is_not_startable)
{
	uint32_t vectors[APP_IMAGE_CHECKED_WORDS];

	a_vector_table(vectors);
	vectors[0] = 0x8000u;              /* a stack pointer in flash */
	checksum_the_table(vectors);
	mu_assert(!app_image_table_startable(vectors, FLASH_TOP, false),
	          "a checksum covered for a stack pointer outside RAM");

	a_vector_table(vectors);
	vectors[1] = REAL_RESET_VECTOR & ~1u;  /* no Thumb bit */
	checksum_the_table(vectors);
	mu_assert(!app_image_table_startable(vectors, FLASH_TOP, false),
	          "a checksum covered for a reset vector without the Thumb bit");

	a_vector_table(vectors);
	vectors[1] = 0x1001u;              /* inside the bootloader */
	checksum_the_table(vectors);
	mu_assert(!app_image_table_startable(vectors, FLASH_TOP, false),
	          "a checksum covered for a reset vector outside the region");
}

/* The checksum word is at offset 0x1c, which is where NXP puts it and where
 * the host writes it. */
MU_TEST(the_checksum_word_is_the_one_the_part_defines)
{
	mu_assert_int_eq(0x1cu, APP_IMAGE_CHECKSUM_INDEX * 4);
	mu_assert_int_eq(0x20u, APP_IMAGE_CHECKED_WORDS * 4);
}

/* ---- the image check block ---------------------------------------------- */

/* A region four blocks long: enough to hold the vector table, the image check
 * block, and blocks above them for a checksum to have something to say. */
#define A_REGION_LENGTH 0x400u
#define A_REGION_WORDS (A_REGION_LENGTH / 4u)

/* Declared as words so the block inside it is word-aligned, as it is in
 * flash: the region starts at a sector boundary and the offset is a multiple
 * of 32. */
static uint32_t a_region_buffer[A_REGION_WORDS];

static struct ubertooth_image_check_block *region_check(uint32_t *region)
{
	return (struct ubertooth_image_check_block *)
	       ((uint8_t *)region + UBERTOOTH_IMAGE_CHECK_OFFSET);
}

/* A region as an image file holds it: a vector table with no checksum, an
 * image check block with its magic and nothing filled in, and arbitrary
 * content everywhere else. This is what a board flashed by an earlier host
 * tool carries. */
static void a_region(uint32_t *region)
{
	struct ubertooth_image_check_block *check;
	unsigned i;

	for (i = 0; i < A_REGION_WORDS; i++)
		region[i] = 0x01010101u * (i % 23u);
	a_vector_table(region);
	check = region_check(region);
	memset(check, 0, sizeof(*check));
	memcpy(check->magic, UBERTOOTH_IMAGE_CHECK_MAGIC,
	       UBERTOOTH_IMAGE_CHECK_MAGIC_LEN);
	check->format_version = UBERTOOTH_IMAGE_CHECK_FORMAT;
}

/* ...and the same region after ubertooth-dfu has written it: the vector
 * checksum, the flags, the length twice over, and the two's complement of the
 * sum of every word, so the whole of it adds up to zero. Worked out here
 * rather than copied from the tool, so the two have to agree about the rule
 * and not only about a number. */
static void a_written_region(uint32_t *region, uint16_t flags)
{
	struct ubertooth_image_check_block *check = region_check(region);
	uint32_t sum = 0;
	unsigned i;

	checksum_the_table(region);
	check->flags = flags;
	check->length = A_REGION_LENGTH;
	check->length_complement = ~A_REGION_LENGTH;
	check->checksum = 0;
	for (i = 0; i < A_REGION_WORDS; i++)
		sum += region[i];
	check->checksum = 0u - sum;
}

#define BOTH_CHECKS (UBERTOOTH_IMAGE_CHECK_VECTOR | UBERTOOTH_IMAGE_CHECK_REGION)

MU_TEST(an_image_the_host_wrote_whole_is_startable)
{
	uint32_t *region = a_region_buffer;

	a_region(region);
	a_written_region(region, BOTH_CHECKS);
	mu_assert(app_image_region_startable(region, FLASH_TOP),
	          "an image written whole was refused");
}

/* The block has to reach the flash with the vector table it describes, so it
 * lives inside the image's first 256 bytes: the block the host writes whole
 * or not at all. */
MU_TEST(the_block_is_inside_the_image_first_block)
{
	mu_assert(UBERTOOTH_IMAGE_CHECK_OFFSET + UBERTOOTH_IMAGE_CHECK_LENGTH <=
	          0x100u,
	          "the image check block is not inside the image's first block");
	mu_assert(UBERTOOTH_IMAGE_CHECK_OFFSET >= APP_IMAGE_CHECKED_WORDS * 4,
	          "the image check block overlaps the words the checksum covers");
}

/* The gap this closes. The host writes the region's first block before the
 * rest of the sector it starts, because writing that block is what erases the
 * sector, so an interruption in between leaves a vector table that reads
 * perfectly in front of erased flash. The table and its checksum both pass;
 * the region checksum is what refuses it. */
MU_TEST(a_sector_left_part_written_is_not_startable)
{
	uint32_t *region = a_region_buffer;
	unsigned i;

	a_region(region);
	a_written_region(region, BOTH_CHECKS);
	/* Erased flash from the end of the check block to the end of the first
	 * 4 KiB sector, which here is the rest of the buffer. */
	for (i = (UBERTOOTH_IMAGE_CHECK_OFFSET + UBERTOOTH_IMAGE_CHECK_LENGTH) / 4u;
	     i < A_REGION_WORDS; i++)
		region[i] = 0xffffffffu;
	mu_assert(app_image_table_startable(region, FLASH_TOP, true),
	          "the vector table was meant to still read perfectly");
	mu_assert(!app_image_region_startable(region, FLASH_TOP),
	          "an image whose sector was left part written was accepted");
}

/* One word out anywhere in the region, which is what a block that did not
 * program as it should leaves. */
MU_TEST(a_single_word_out_is_not_startable)
{
	uint32_t *region = a_region_buffer;

	a_region(region);
	a_written_region(region, BOTH_CHECKS);
	region[A_REGION_WORDS - 1] += 1;
	mu_assert(!app_image_region_startable(region, FLASH_TOP),
	          "an image with a word out at the top was accepted");

	a_region(region);
	a_written_region(region, BOTH_CHECKS);
	region[(UBERTOOTH_IMAGE_CHECK_OFFSET / 4u) - 1] += 1;
	mu_assert(!app_image_region_startable(region, FLASH_TOP),
	          "an image with a word out below the block was accepted");
}

/* An image from a tree that has no image check block: the 32 bytes hold the
 * image's own code or data, the magic does not match, and the checks are the
 * ones that were made before the block existed. Such an image has to start:
 * refusing it would leave a board waiting in DFU for the image it already
 * has. */
MU_TEST(an_image_with_no_check_block_is_startable)
{
	uint32_t *region = a_region_buffer;

	a_region(region);
	memset(region_check(region), 0x5a, UBERTOOTH_IMAGE_CHECK_LENGTH);
	mu_assert(app_image_region_startable(region, FLASH_TOP),
	          "an image carrying no image check block was refused");
}

/* An image built from this tree but written by an earlier host tool: the
 * block is there with its flags at zero, which asks for nothing. The vector
 * checksum word is zero too, and that still means "no checksum". */
MU_TEST(an_unwritten_check_block_asks_for_nothing)
{
	uint32_t *region = a_region_buffer;

	a_region(region);
	mu_assert_int_eq(0, region[APP_IMAGE_CHECKSUM_INDEX]);
	mu_assert_int_eq(0, region_check(region)->flags);
	mu_assert(app_image_region_startable(region, FLASH_TOP),
	          "an image no host had written was refused");
}

/* A block in a format this bootloader does not know is a block it cannot
 * read, so it asks for nothing rather than guessing at the fields. */
MU_TEST(a_check_block_in_an_unknown_format_asks_for_nothing)
{
	uint32_t *region = a_region_buffer;

	a_region(region);
	a_written_region(region, BOTH_CHECKS);
	region_check(region)->format_version = UBERTOOTH_IMAGE_CHECK_FORMAT + 1;
	region[A_REGION_WORDS - 1] += 1;
	mu_assert(app_image_region_startable(region, FLASH_TOP),
	          "a block in an unknown format was read anyway");
}

/* An image that says for itself that its checksum word is filled in does not
 * get the zero-word escape: a zero word there is eight words that do not add
 * up, not an image that predates the checksum. */
MU_TEST(a_flagged_image_with_a_zero_checksum_word_is_not_startable)
{
	uint32_t *region = a_region_buffer;

	a_region(region);
	a_written_region(region, UBERTOOTH_IMAGE_CHECK_VECTOR);
	region[APP_IMAGE_CHECKSUM_INDEX] = 0;
	mu_assert(!app_image_region_startable(region, FLASH_TOP),
	          "a flagged image whose checksum word was zero was accepted");
	/* The same words with the flag clear are accepted, which is what keeps
	 * every image written before this startable. */
	region_check(region)->flags = 0;
	mu_assert(app_image_region_startable(region, FLASH_TOP),
	          "an unflagged image whose checksum word was zero was refused");
}

/* The length decides how much flash is read, so it is read carefully: the two
 * copies have to agree. A length of its own is one word, and a word a program
 * left part way through could read as anything. */
MU_TEST(a_length_that_does_not_agree_with_its_complement_is_not_startable)
{
	uint32_t *region = a_region_buffer;

	a_region(region);
	a_written_region(region, BOTH_CHECKS);
	region_check(region)->length_complement += 1;
	mu_assert(!app_image_region_startable(region, FLASH_TOP),
	          "a length whose two copies disagreed was used");

	a_region(region);
	a_written_region(region, BOTH_CHECKS);
	region_check(region)->length_complement = 0xffffffffu;
	region_check(region)->length = 0;
	mu_assert(!app_image_region_startable(region, FLASH_TOP),
	          "a length of zero was used");
}

/* ...and a length that agrees with itself still has to be one an image could
 * have: long enough to hold the block it came out of, and a whole number of
 * words. A record that fails either was not programmed whole. */
MU_TEST(a_length_no_image_could_have_is_not_startable)
{
	uint32_t *region = a_region_buffer;
	struct ubertooth_image_check_block *check;
	const uint32_t no_image_could_have[] = {
		4u,                              /* shorter than the block itself */
		APP_IMAGE_MIN_LENGTH - 4u,
		A_REGION_LENGTH + 2u,            /* not a whole number of words */
		A_REGION_LENGTH + 1u,
	};
	unsigned i;

	for (i = 0; i < sizeof(no_image_could_have) / sizeof(*no_image_could_have);
	     i++) {
		a_region(region);
		a_written_region(region, BOTH_CHECKS);
		check = region_check(region);
		check->length = no_image_could_have[i];
		check->length_complement = ~no_image_could_have[i];
		mu_assert(!app_image_region_startable(region, FLASH_TOP),
		          "a length no image could have was used");
	}
}

/* A length past the flash the smallest of these parts has is not read: a
 * board that faulted reading flash it has not got would need someone at the
 * bench, which is worse than anything this check is for. Such an image starts
 * on its vector table alone instead of being refused, so a board never
 * refuses a good image on this account. */
MU_TEST(a_length_past_the_read_limit_is_not_read)
{
	uint32_t *region = a_region_buffer;
	struct ubertooth_image_check_block *check;
	const uint32_t past_the_limit[] = {
		APP_IMAGE_REGION_READ_LIMIT + 4u,
		FLASH_TOP - APP_IMAGE_BASE,
		0xfffffffcu,
	};
	unsigned i;

	mu_assert(APP_IMAGE_REGION_READ_LIMIT < FLASH_TOP - APP_IMAGE_BASE,
	          "the read limit is no lower than the family's largest part, so "
	          "it bounds nothing");
	for (i = 0; i < sizeof(past_the_limit) / sizeof(*past_the_limit); i++) {
		a_region(region);
		a_written_region(region, BOTH_CHECKS);
		/* ...and a word out, so a bootloader that did read that far would
		 * refuse this image and the test would say so. */
		region[A_REGION_WORDS - 1] += 1;
		check = region_check(region);
		check->length = past_the_limit[i];
		check->length_complement = ~past_the_limit[i];
		mu_assert(app_image_region_startable(region, FLASH_TOP),
		          "an image whose length is past the read limit was refused "
		          "rather than started unchecked");
	}
}

/* The limit is the application region of a 128 KiB part, which is the floor
 * for these boards, and it has to leave room for the images this tree builds:
 * bluetooth_rxtx is about 36 KiB. */
MU_TEST(the_read_limit_is_a_part_the_boards_have)
{
	mu_assert_int_eq(0x20000u, APP_IMAGE_SMALLEST_FLASH);
	mu_assert_int_eq(0x20000u - APP_IMAGE_BASE, APP_IMAGE_REGION_READ_LIMIT);
	mu_assert(APP_IMAGE_REGION_READ_LIMIT > 64u * 1024u,
	          "the read limit leaves no room for the images this tree builds");
}

/* The region check does not rescue a vector table the two words refuse, and
 * it is not reached by a region that holds no image at all: erased flash
 * costs two words and no more. */
MU_TEST(erased_flash_is_not_startable_whatever_its_check_block)
{
	uint32_t *region = a_region_buffer;
	unsigned i;

	for (i = 0; i < A_REGION_WORDS; i++)
		region[i] = 0xffffffffu;
	mu_assert(!app_image_region_startable(region, FLASH_TOP),
	          "erased flash was taken for an application");

	a_region(region);
	a_written_region(region, BOTH_CHECKS);
	region[0] = 0x8000u;  /* a stack pointer in flash */
	mu_assert(!app_image_region_startable(region, FLASH_TOP),
	          "a region checksum covered for a stack pointer outside RAM");
}

/* The host adds the words up a byte at a time out of a file and the
 * bootloader a word at a time out of flash. They have to come to the same
 * number: a board whose sum differs from the host's refuses the image it was
 * just given. */
MU_TEST(the_host_and_the_bootloader_add_up_the_same_words)
{
	uint32_t *region = a_region_buffer;

	a_region(region);
	a_written_region(region, BOTH_CHECKS);
	mu_assert_int_eq(app_image_region_sum(region, A_REGION_WORDS),
	                 ubertooth_image_check_sum((const uint8_t *)region,
	                                           A_REGION_LENGTH));
	mu_assert_int_eq(0, app_image_region_sum(region, A_REGION_WORDS));
}

/* ---- the guard in front of the jump ------------------------------------- */

/* bootloader.cpp with its comments and string literals stripped, so a scan
 * cannot match text that is not code. */
static void read_bootloader_source_keeping(char *out, size_t size,
                                           int keep_strings)
{
	FILE *file = fopen(FW_BOOTLOADER_DIR "/bootloader.cpp", "r");
	char line[SCAN_LINE_MAX];
	int in_block = 0;
	size_t used = 0;

	out[0] = '\0';
	if (file == NULL) {
		/* The scans below then find nothing and fail with their own
		 * messages. Reading on from here would be a read through NULL. */
		mu_assert(0, "cannot read bootloader.cpp");
		return;
	}
	while (fgets(line, sizeof(line), file) != NULL) {
		size_t length;

		strip_comments(line, &in_block);
		if (!keep_strings)
			strip_string_literals(line);
		length = strlen(line);
		if (used + length + 1 >= size)
			break;
		memcpy(out + used, line, length);
		used += length;
		out[used] = '\0';
	}
	fclose(file);
	mu_assert(used > 0, "bootloader.cpp read as nothing");
}

static void read_bootloader_source(char *out, size_t size)
{
	read_bootloader_source_keeping(out, size, 0);
}

/* The whole point of the predicate is that it runs before the jump. The jump
 * is the asm() block in run_application(), so the check has to appear between
 * the start of that function and that block. */
MU_TEST(run_application_checks_before_it_jumps)
{
	static char source[200000];
	const char *function, *check, *jump;

	read_bootloader_source(source, sizeof(source));
	/* Fatal, not plain: every line below reads what the one before it
	 * found, so a miss here has to end the test rather than be recorded and
	 * walked past. */
	function = strstr(source, "run_application() {");
	mu_assert_fatal(function != NULL,
	                "run_application()'s body is not in bootloader.cpp");
	check = strstr(function, "application_startable");
	jump = strstr(function, "asm");
	mu_assert_fatal(jump != NULL, "run_application() no longer jumps");
	mu_assert_fatal(check != NULL,
	                "run_application() does not check the image");
	mu_assert(check < jump, "run_application() jumps before it checks");
}

/* The jump loads its address as a decimal number of its own, so the check has
 * to be about the same address. The instruction looked for here is built from
 * APP_IMAGE_BASE rather than written out, so the two cannot drift apart. The
 * jump is assembly inside a string literal, so this is the one scan here that
 * keeps literals. */
MU_TEST(the_check_and_the_jump_use_the_same_address)
{
	static char source[200000];
	char collapsed[200000];
	char instruction[48];

	snprintf(instruction, sizeof(instruction), "movw r3, %c%u", '#',
	         APP_IMAGE_BASE);
	read_bootloader_source_keeping(source, sizeof(source), 1);
	collapse_spaces(source, collapsed, sizeof(collapsed));
	mu_assert(strstr(collapsed, instruction) != NULL,
	          "the jump no longer loads the address the check reads");
}

/* The numbers this test and the host both copy out of flash.h, read back out
 * of flash.h: a change there has to reach them rather than leave this suite
 * passing while it describes something else. flash.h is not included here
 * because it pulls in the In-Application Programming code. */
static unsigned long flash_h_define(const char *name)
{
	FILE *file = fopen(FW_BOOTLOADER_DIR "/flash.h", "r");
	char line[SCAN_LINE_MAX];
	unsigned long value = 0;
	int in_block = 0;

	if (file == NULL) {
		mu_assert(0, "cannot read flash.h");
		return 0;
	}
	while (fgets(line, sizeof(line), file) != NULL) {
		char *at;

		strip_comments(line, &in_block);
		if (strncmp(line, "#define ", 8) != 0)
			continue;
		at = line + 8;
		while (*at == ' ')
			at++;
		if (strncmp(at, name, strlen(name)) != 0 ||
		    is_ident_char(at[strlen(name)]))
			continue;
		value = strtoul(at + strlen(name), NULL, 0);
		break;
	}
	fclose(file);
	return value;
}

MU_TEST(the_bound_and_the_sector_size_match_flash_h)
{
	mu_assert_int_eq(FLASH_TOP, flash_h_define("LPC_FLASH_SIZE_FALLBACK"));
	mu_assert_int_eq(FIRST_SECTOR_SIZE,
	                 flash_h_define("LPC_SMALL_SECTOR_SIZE"));
}

/* ...and the host writes that same sector size as the last step of a
 * download. The two are separate constants in separate trees, and the order
 * only works while they agree, so the host's is read here as text. */
MU_TEST(the_hosts_sector_size_matches_the_firmwares)
{
	FILE *file = fopen(HOST_DFU_DIR "/dfu.h", "r");
	char line[SCAN_LINE_MAX];
	int found = 0;
	int in_block = 0;

	mu_assert_fatal(file != NULL, "cannot read the host's dfu.h");
	while (fgets(line, sizeof(line), file) != NULL) {
		char collapsed[SCAN_LINE_MAX];

		strip_comments(line, &in_block);
		collapse_spaces(line, collapsed, sizeof(collapsed));
		/* 1<<12 is 0x1000. Matched as text because the host spells it as a
		 * shift: a reader who changes it has to come here. */
		if (strcmp(collapsed, "#define SECTOR_SIZE (1<<12)") == 0)
			found = 1;
	}
	fclose(file);
	mu_assert(found, "the host's SECTOR_SIZE is no longer (1<<12), which is "
	          "the sector the bootloader erases");
	mu_assert_int_eq(0x1000u, FIRST_SECTOR_SIZE);
}

/* And a board with nothing to start has to end up somewhere useful: main()
 * runs the bootloader for it, so the next image can be written over USB. */
/* ...and the check in front of it is the whole one. The cases above are about
 * app_image_table_startable(), so they say nothing on their own about which
 * predicate the bootloader calls: a bootloader that went back to reading the
 * two words alone would still pass every one of them. */
MU_TEST(the_bootloader_asks_the_whole_check)
{
	static char source[200000];
	char collapsed[200000];

	const char *function;

	read_bootloader_source(source, sizeof(source));
	collapse_spaces(source, collapsed, sizeof(collapsed));
	function = strstr(collapsed, "application_startable() {");
	mu_assert_fatal(function != NULL,
	                "application_startable()'s body is not in bootloader.cpp");
	mu_assert(strstr(function, "app_image_region_startable(") != NULL,
	          "application_startable() no longer puts the region to "
	          "app_image_region_startable(), so the checks the image asks for "
	          "are not made");
}

MU_TEST(main_runs_the_bootloader_when_there_is_nothing_to_start)
{
	static char source[200000];
	char collapsed[200000];

	read_bootloader_source(source, sizeof(source));
	collapse_spaces(source, collapsed, sizeof(collapsed));
	mu_assert(strstr(collapsed, "} else if (!application_startable()) {") !=
	          NULL,
	          "main() does not run the bootloader for a board with nothing "
	          "to start");
	/* The bound the check is given is the family's, not a figure of its
	 * own. */
	mu_assert(strstr(collapsed, "LPC_FLASH_SIZE_FALLBACK") != NULL,
	          "the check is no longer bounded by the family's flash size");
}

int main(void)
{
	MU_RUN(erased_flash_is_not_startable);
	MU_RUN(the_blank_block_the_host_writes_first_is_not_startable);
	MU_RUN(a_region_of_zeros_is_not_startable);
	MU_RUN(code_with_no_vector_table_is_not_startable);
	MU_RUN(a_real_image_is_startable);
	MU_RUN(an_image_read_back_off_a_board_is_startable);
	MU_RUN(a_real_image_is_startable_whatever_the_part);
	MU_RUN(the_ends_of_the_application_region_are_startable);
	MU_RUN(a_stack_pointer_outside_ram_is_not_startable);
	MU_RUN(a_stack_pointer_at_the_top_of_ram_is_startable);
	MU_RUN(a_stack_pointer_in_the_ahb_ram_is_startable);
	MU_RUN(an_unaligned_stack_pointer_is_not_startable);
	MU_RUN(a_reset_vector_without_the_thumb_bit_is_not_startable);
	MU_RUN(a_reset_vector_outside_the_application_region_is_not_startable);
	MU_RUN(a_reset_vector_past_the_given_bound_is_not_startable);
	MU_RUN(the_bootloaders_own_bound_is_the_familys_not_the_parts);
	MU_RUN(a_checksummed_vector_table_is_startable);
	MU_RUN(a_vector_table_with_no_checksum_is_startable);
	MU_RUN(a_wrong_checksum_is_not_startable);
	MU_RUN(a_first_block_programmed_part_way_is_not_startable);
	MU_RUN(erased_flash_is_not_startable_whatever_its_checksum);
	MU_RUN(a_checksummed_table_with_bad_words_is_not_startable);
	MU_RUN(the_checksum_word_is_the_one_the_part_defines);
	MU_RUN(an_image_the_host_wrote_whole_is_startable);
	MU_RUN(the_block_is_inside_the_image_first_block);
	MU_RUN(a_sector_left_part_written_is_not_startable);
	MU_RUN(a_single_word_out_is_not_startable);
	MU_RUN(an_image_with_no_check_block_is_startable);
	MU_RUN(an_unwritten_check_block_asks_for_nothing);
	MU_RUN(a_check_block_in_an_unknown_format_asks_for_nothing);
	MU_RUN(a_flagged_image_with_a_zero_checksum_word_is_not_startable);
	MU_RUN(a_length_that_does_not_agree_with_its_complement_is_not_startable);
	MU_RUN(a_length_no_image_could_have_is_not_startable);
	MU_RUN(a_length_past_the_read_limit_is_not_read);
	MU_RUN(the_read_limit_is_a_part_the_boards_have);
	MU_RUN(erased_flash_is_not_startable_whatever_its_check_block);
	MU_RUN(the_host_and_the_bootloader_add_up_the_same_words);
	MU_RUN(run_application_checks_before_it_jumps);
	MU_RUN(the_check_and_the_jump_use_the_same_address);
	MU_RUN(the_bound_and_the_sector_size_match_flash_h);
	MU_RUN(the_hosts_sector_size_matches_the_firmwares);
	MU_RUN(the_bootloader_asks_the_whole_check);
	MU_RUN(main_runs_the_bootloader_when_there_is_nothing_to_start);
	return mu_report();
}
