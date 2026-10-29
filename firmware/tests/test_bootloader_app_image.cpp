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
#include <string.h>

#include "minunit.h"
#include "source_scan.h"

/* The real check. */
#include "app_image.h"

/* The flash bound the bootloader passes in: the largest part in the family,
 * LPC_FLASH_SIZE_FALLBACK in flash.h. Spelled out rather than included,
 * because flash.h pulls in the In-Application Programming code and this test
 * is about the predicate alone; the scan at the end shows the bootloader
 * passes that constant. */
#define FLASH_TOP 0x80000u

/* The first two words of a real image, read out of the bluetooth_rxtx.bin
 * this tree builds. The stack pointer is _StackTop, the top of the 16 KiB the
 * images are linked for less the 32 bytes sections.ld keeps back; the reset
 * vector is Reset_Handler with its Thumb bit set, as every entry in a
 * Cortex-M vector table has.
 *
 * The word at offset 0x1c of that image, where the LPC vector checksum would
 * go, reads 0x00000000 -- which is why the check does not ask for it. */
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

/* A region that was erased and never written reads as zeros on some parts and
 * is what a cleared sector looks like to a reader that sees it as code. */
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

/* The same two words read back off a board's flash with ubertooth-dfu -u
 * after a flash, so what the check accepts is not only what an image file
 * says but what the board holds. The reset vector differs from the one above
 * because it came from a different build; the word at 0x1c read zero there
 * too. */
MU_TEST(an_image_read_back_off_a_board_is_startable)
{
	mu_assert(app_image_startable(0x10003fe0u, 0x00009eedu, FLASH_TOP),
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

/* An image for a larger part than the one it is on: the host refuses to write
 * one, but a board that was flashed by an older host can be carrying it. The
 * reset vector may be inside the flash this part has not got, and jumping to
 * it is the bus fault the bound exists to avoid. */
MU_TEST(a_reset_vector_past_this_parts_flash_is_not_startable)
{
	mu_assert(!app_image_startable(REAL_STACK_POINTER, 0x30001u, 0x20000u),
	          "a reset vector past a 128 KiB part's flash was accepted");
}

/* ---- the guard in front of the jump ------------------------------------- */

/* bootloader.cpp with its comments and string literals stripped, so a scan
 * cannot match text that is not code. */
static void read_bootloader_source(char *out, size_t size)
{
	FILE *file = fopen(FW_BOOTLOADER_DIR "/bootloader.cpp", "r");
	char line[SCAN_LINE_MAX];
	int in_block = 0;
	size_t used = 0;

	mu_assert(file != NULL, "cannot read bootloader.cpp");
	out[0] = '\0';
	while (fgets(line, sizeof(line), file) != NULL) {
		size_t length;

		strip_comments(line, &in_block);
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

/* The whole point of the predicate is that it runs before the jump. The jump
 * is the asm() block in run_application(), so the check has to appear between
 * the start of that function and that block. */
MU_TEST(run_application_checks_before_it_jumps)
{
	static char source[200000];
	const char *function, *check, *jump;

	read_bootloader_source(source, sizeof(source));
	function = strstr(source, "run_application()");
	mu_assert(function != NULL, "run_application() is not in bootloader.cpp");
	/* The definition, not the forward uses: the one followed by a body. */
	function = strstr(function, "run_application() {");
	mu_assert(function != NULL, "run_application()'s body was not found");
	check = strstr(function, "application_startable");
	jump = strstr(function, "asm");
	mu_assert(jump != NULL, "run_application() no longer jumps");
	mu_assert(check != NULL, "run_application() does not check the image");
	mu_assert(check < jump, "run_application() jumps before it checks");
}

/* And a board with nothing to start has to end up somewhere useful: main()
 * runs the bootloader for it, so the next image can be written over USB. */
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
	MU_RUN(an_unaligned_stack_pointer_is_not_startable);
	MU_RUN(a_reset_vector_without_the_thumb_bit_is_not_startable);
	MU_RUN(a_reset_vector_outside_the_application_region_is_not_startable);
	MU_RUN(a_reset_vector_past_this_parts_flash_is_not_startable);
	MU_RUN(run_application_checks_before_it_jumps);
	MU_RUN(main_runs_the_bootloader_when_there_is_nothing_to_start);
	return mu_report();
}
