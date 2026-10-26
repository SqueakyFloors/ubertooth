/*
 * The bound on what the bootloader will read back (REQ_UPLOAD).
 *
 * The bound used to be a fixed 512 KiB whatever part the bootloader ran on.
 * On a part with less flash than that -- the Ubertooth One schematic
 * specifies an LPC1754 with 128 KiB -- the read ran off the end of the memory
 * the part has, which is a precise bus fault on LPC17xx, and the fault does
 * not return to the bootloader's polled USB loop: the board answered nothing
 * more, a state request included, until it was power-cycled. The first block
 * past the end of a 128 KiB part is block 512, at 0x20000, and that is the
 * case these tests are built around.
 *
 * This is the tree's only C++, and the two pieces that decide the bound --
 * the part table and Flash::valid_address -- compile here as they are, so
 * they are the real thing below, with the In-Application Programming read
 * behind them faked. request_upload() cannot be compiled here (dfu.cpp casts
 * a pointer to uint32_t, which does not fit on a 64-bit host), so its shape
 * is faked, with a stand-in for the read that records a fault instead of
 * taking one, and the last tests scan dfu.cpp to show the firmware has that
 * same shape -- the way test_tx_generic_bounds.c does.
 *
 * The last test puts the host's copy of the part table against the
 * bootloader's: the two have to agree or the host will ask for flash a board
 * has not got.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "minunit.h"
#include "source_scan.h"

/* The real table, the real bound and the real Flash::valid_address. */
#include "flash.h"

/* The size of a DFU transfer, DFU::transfer_size in dfu.h. */
#define TRANSFER_SIZE 0x100

/* The two DFU statuses the read-back answers with; DFU::Status is private. */
#define DFU_STATUS_OK 0x00
#define DFU_STATUS_ERRADDRESS 0x08

/* A part identification number no LPC175x/176x part has. */
#define PART_UNKNOWN 0x12345678

#define PART_LPC1754 0x25011722  /* 128 KiB, the Ubertooth One schematic's */
#define PART_LPC1768 0x26013F37  /* 512 KiB */

/*
 * The In-Application Programming read behind Flash::valid_address. Defined
 * here rather than linked from iap.cpp, which calls the part's ROM.
 */
static uint32_t fake_part_id;
static IAP::ReturnCode fake_part_result;
static unsigned part_reads;

IAP::ReturnCode IAP::read_part_identification_number(uint32_t& part_id) const
{
	part_reads++;
	if (fake_part_result == CMD_SUCCESS)
		part_id = fake_part_id;
	return fake_part_result;
}

/*
 * The part's flash as the read would see it: bytes up to the flash the part
 * has, and no memory at all behind them. Reading past the end is the bus
 * fault, so this records it rather than reading something.
 */
static uint32_t present_flash;
static unsigned reads;
static int faulted;

static void read_flash(uint8_t *out, uint32_t address, uint32_t length)
{
	reads++;
	if (address + length > present_flash) {
		faulted = 1;
		return;
	}
	memset(out, 0x5a, length);
}

/* A part with `flash_size` bytes of flash, answering the part read. */
static void attach_part(uint32_t part_id, uint32_t flash_size)
{
	fake_part_id = part_id;
	fake_part_result = IAP::CMD_SUCCESS;
	present_flash = flash_size;
	part_reads = 0;
	reads = 0;
	faulted = 0;
}

/*
 * request_upload()'s shape: the first and last address of the block are put
 * to the bound, and only a block that passes both is read. Returns the DFU
 * status the request leaves behind.
 *
 * Takes the address and length rather than a block number so that a block
 * straddling the end of flash can be asked for. The firmware's own blocks are
 * wValue * 0x100, and every part in the table has a whole number of those, so
 * a straddling block cannot come from a request -- the last-address check is
 * what makes that true rather than an assumption.
 */
static uint8_t upload(Flash& flash, uint32_t address, uint32_t length,
                      uint8_t *out)
{
	if (flash.valid_address(address) && flash.valid_address(address + length - 1)) {
		read_flash(out, address, length);
		return DFU_STATUS_OK;
	}
	return DFU_STATUS_ERRADDRESS;
}

/* The same, for the block number a host puts in wValue. */
static uint8_t upload_block(Flash& flash, uint16_t block, uint8_t *out)
{
	return upload(flash, (uint32_t)block * TRANSFER_SIZE, TRANSFER_SIZE, out);
}

/* The table says what the manual says. */
MU_TEST(test_the_table_knows_the_parts_in_the_family)
{
	mu_assert_int_eq(128 * 1024, (int)lpc_part_flash_size(PART_LPC1754));
	mu_assert_int_eq(512 * 1024, (int)lpc_part_flash_size(PART_LPC1768));
	mu_assert_int_eq(32 * 1024, (int)lpc_part_flash_size(0x25001110));
	mu_assert_int_eq(0, (int)lpc_part_flash_size(PART_UNKNOWN));
	mu_assert_int_eq(0, (int)lpc_part_flash_size(0));
}

/* The bound is the flash the part has, and the old fixed one for the rest. */
MU_TEST(test_the_bound_follows_the_part)
{
	mu_assert_int_eq(128 * 1024, (int)lpc_part_flash_bound(PART_LPC1754));
	mu_assert_int_eq(512 * 1024, (int)lpc_part_flash_bound(PART_LPC1768));
	mu_assert_int_eq(LPC_FLASH_SIZE_FALLBACK,
	                 (int)lpc_part_flash_bound(PART_UNKNOWN));
}

/* Flash::valid_address on a 128 KiB part: the last byte of flash and no more. */
MU_TEST(test_the_last_byte_of_flash_is_the_last_valid_address)
{
	Flash flash;

	attach_part(PART_LPC1754, 128 * 1024);
	mu_assert(flash.valid_address(0), "address 0 is not flash on a 128 KiB part");
	mu_assert(flash.valid_address(0x1FFFF),
	          "the last byte of a 128 KiB part is not flash");
	mu_assert(!flash.valid_address(0x20000),
	          "the first byte past a 128 KiB part is flash");
	mu_assert(!flash.valid_address(0x7FFFF),
	          "an address inside the old fixed bound is flash on a 128 KiB part");
}

/* A block wholly inside the part's flash is read back. */
MU_TEST(test_a_block_inside_the_part_is_read)
{
	uint8_t out[TRANSFER_SIZE];
	Flash flash;

	attach_part(PART_LPC1754, 128 * 1024);
	memset(out, 0, sizeof(out));
	/* The first block of the application region, and the last block there is. */
	mu_assert_int_eq(DFU_STATUS_OK, upload_block(flash, 0x40, out));
	mu_assert_int_eq(DFU_STATUS_OK, upload_block(flash, 0x1FF, out));
	mu_assert_int_eq(2, (int)reads);
	mu_assert(!faulted, "a block inside the part's flash was read past the end");
	mu_assert_int_eq(0x5a, out[0]);
	mu_assert_int_eq(0x5a, out[TRANSFER_SIZE - 1]);
}

/*
 * The case from the field: the first block past the end of a 128 KiB part.
 * Refused, and nothing is read -- the read is the fault.
 */
MU_TEST(test_the_first_block_past_a_128k_part_is_refused)
{
	uint8_t out[TRANSFER_SIZE];
	Flash flash;

	attach_part(PART_LPC1754, 128 * 1024);
	memset(out, 0, sizeof(out));
	mu_assert_int_eq(DFU_STATUS_ERRADDRESS, upload_block(flash, 512, out));
	mu_assert_int_eq(0, (int)reads);
	mu_assert(!faulted, "the refused block was read anyway");
	mu_assert_int_eq(0, out[0]);
}

/* So is every block above it, up to the end of what a host can ask for. */
MU_TEST(test_every_block_past_a_128k_part_is_refused)
{
	uint8_t out[TRANSFER_SIZE];
	Flash flash;
	uint32_t block, accepted = 0;

	attach_part(PART_LPC1754, 128 * 1024);
	for (block = 512; block <= 0xFFFF; block++)
		if (upload_block(flash, (uint16_t)block, out) != DFU_STATUS_ERRADDRESS)
			accepted++;
	mu_assert_int_eq(0, (int)accepted);
	mu_assert_int_eq(0, (int)reads);
	mu_assert(!faulted, "a block past the end of the part was read");
}

/* A block that starts inside flash and ends past it is refused too. */
MU_TEST(test_a_block_that_ends_past_the_part_is_refused)
{
	uint8_t out[TRANSFER_SIZE];
	Flash flash;

	attach_part(PART_LPC1754, 128 * 1024);
	/* Starts 0x80 bytes before the end of flash, so the last 0x80 bytes have
	 * no memory behind them. */
	mu_assert_int_eq(DFU_STATUS_ERRADDRESS,
	                 upload(flash, 0x1FF80, TRANSFER_SIZE, out));
	mu_assert_int_eq(0, (int)reads);
	mu_assert(!faulted, "a block ending past the part's flash was read");
}

/* A bigger part reads back all of its flash. */
MU_TEST(test_a_512k_part_reads_back_all_of_it)
{
	uint8_t out[TRANSFER_SIZE];
	Flash flash;

	attach_part(PART_LPC1768, 512 * 1024);
	mu_assert_int_eq(DFU_STATUS_OK, upload_block(flash, 512, out));
	mu_assert_int_eq(DFU_STATUS_OK, upload_block(flash, 0x7FF, out));
	mu_assert_int_eq(DFU_STATUS_ERRADDRESS, upload_block(flash, 0x800, out));
	mu_assert(!faulted, "a 512 KiB part was read past its flash");
}

/*
 * A part with no entry in the table keeps the bound the bootloader enforced
 * on every part before the table existed. Such a board either reads back
 * today, because it has at least that much flash, or already wedges on a read
 * past its end; neither gets worse, where bounding it at the smallest part in
 * the family would refuse a read-back that works.
 */
MU_TEST(test_an_unknown_part_keeps_the_old_bound)
{
	uint8_t out[TRANSFER_SIZE];
	Flash flash;

	attach_part(PART_UNKNOWN, 512 * 1024);
	mu_assert(flash.valid_address(0x7FFFF),
	          "an unknown part refuses an address the old bound allowed");
	mu_assert(!flash.valid_address(LPC_FLASH_SIZE_FALLBACK),
	          "an unknown part is not bounded at all");
	mu_assert_int_eq(DFU_STATUS_OK, upload_block(flash, 0x7FF, out));
	mu_assert_int_eq(DFU_STATUS_ERRADDRESS, upload_block(flash, 0x800, out));
}

/* A part read that fails is the same case: no figure, so the old bound. */
MU_TEST(test_a_failed_part_read_keeps_the_old_bound)
{
	Flash flash;

	attach_part(PART_LPC1754, 128 * 1024);
	fake_part_result = IAP::BUSY;

	mu_assert(flash.valid_address(0x7FFFF),
	          "a failed part read refuses an address the old bound allowed");
	mu_assert(!flash.valid_address(LPC_FLASH_SIZE_FALLBACK),
	          "a failed part read leaves the read-back unbounded");
	/* Nothing was remembered, so the next request asks again: a part that
	 * answers late still ends up bounded by its own flash. */
	mu_assert(part_reads > 1, "a failed part read is not retried");

	fake_part_result = IAP::CMD_SUCCESS;
	mu_assert(!flash.valid_address(0x20000),
	          "the part was not read again after a failure");
}

/* A part that answered is not asked again. */
MU_TEST(test_the_part_is_read_once)
{
	uint8_t out[TRANSFER_SIZE];
	Flash flash;
	uint32_t block;

	attach_part(PART_LPC1754, 128 * 1024);
	for (block = 0x40; block < 0x240; block++)
		upload_block(flash, (uint16_t)block, out);
	mu_assert_int_eq(1, (int)part_reads);
}

/*
 * What makes the tests above mean something: the fixed 512 KiB bound accepted
 * the block at 0x20000 on a 128 KiB part, and reading there runs past the
 * memory the part has.
 */
MU_TEST(test_the_old_fixed_bound_read_past_the_end)
{
	uint8_t out[TRANSFER_SIZE];

	attach_part(PART_LPC1754, 128 * 1024);
	mu_assert(0x20000 < LPC_FLASH_SIZE_FALLBACK,
	          "the old fixed bound would have refused the block from the field, "
	          "so the tests above are checking nothing");
	read_flash(out, 0x20000, TRANSFER_SIZE);
	mu_assert(faulted, "a read past a 128 KiB part did not run past its flash");
}

/*
 * The fakes are worth something only if request_upload() has the same shape,
 * and it cannot be compiled here. So: find it, and check that both ends of
 * the block are put to Flash::valid_address, that the one read out of flash
 * comes after both, and that a block that fails them answers ERRADDRESS.
 */
struct scan {
	int function;        /* request_upload()'s definition */
	int start_check;     /* the block's first address */
	int end_check;       /* ...and its last */
	int copy;            /* the read out of flash */
	int copies;          /* how many reads out of flash the function has */
	int erraddress;      /* the refusal */
};

static int scan_dfu(const char *path, struct scan *s)
{
	char line[512], flat[512];
	int lineno = 0, in_block = 0, in_function = 0;
	FILE *f = fopen(path, "r");

	if (f == NULL)
		return 0;

	memset(s, 0, sizeof(*s));

	while (fgets(line, sizeof(line), f) != NULL) {
		lineno++;
		strip_comments(line, &in_block);
		collapse_spaces(line, flat, sizeof(flat));

		if (strstr(flat, "bool DFU::request_upload(") != NULL) {
			s->function = lineno;
			in_function = 1;
			continue;
		}
		if (!in_function)
			continue;
		/* A function's own closing brace is the only one in the first
		 * column; the braces inside it are indented. Tested on the line as
		 * it is, since collapse_spaces() drops the indentation. */
		if (line[0] == '}') {
			in_function = 0;
			continue;
		}

		if (strstr(flat, "flash.valid_address(flash_address_start)") != NULL)
			s->start_check = lineno;
		if (strstr(flat, "flash.valid_address(flash_address_end - 1)") != NULL)
			s->end_check = lineno;
		if (strstr(flat, "memcpy(") != NULL) {
			s->copies++;
			if (s->copy == 0)
				s->copy = lineno;
		}
		if (strstr(flat, "error(ERRADDRESS)") != NULL)
			s->erraddress = lineno;
	}

	fclose(f);
	return 1;
}

static struct scan s;
static int scanned;
static const char *dfu_path = FW_BOOTLOADER_DIR "/dfu.cpp";

MU_TEST(test_dfu_source_was_scanned)
{
	char msg[512];

	snprintf(msg, sizeof(msg), "cannot open %s to scan", dfu_path);
	mu_assert(scanned, msg);

	snprintf(msg, sizeof(msg),
	         "%s: the scan did not find request_upload(), so the checks below "
	         "would pass without reading anything", dfu_path);
	mu_assert(s.function > 0, msg);
}

MU_TEST(test_the_read_back_refuses_before_it_reads)
{
	char msg[640];

	snprintf(msg, sizeof(msg),
	         "%s:%d: request_upload() does not put the block's first address to "
	         "Flash::valid_address", dfu_path, s.function);
	mu_assert(s.start_check > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: request_upload() does not put the block's last address to "
	         "Flash::valid_address, so a block ending past the end of flash "
	         "would be read", dfu_path, s.function);
	mu_assert(s.end_check > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s: request_upload() must read out of flash exactly once, after "
	         "both checks; the scan found %d reads, the first at line %d",
	         dfu_path, s.copies, s.copy);
	mu_assert(s.copies == 1 && s.copy > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: the read out of flash comes before the checks at lines "
	         "%d and %d, so a block is read before it is refused",
	         dfu_path, s.copy, s.start_check, s.end_check);
	mu_assert(s.start_check < s.copy && s.end_check < s.copy, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: request_upload() does not answer ERRADDRESS for a block "
	         "outside flash", dfu_path, s.function);
	mu_assert(s.erraddress > s.copy, msg);
}

/*
 * The host keeps its own copy of the part table, with the names it prints, so
 * that it does not ask for flash a board has not got. An entry that disagrees
 * with the bootloader's is how a board gets wedged again, so the two are read
 * out of their sources and compared.
 */
#define PART_TABLE_MAX 64

struct part_table {
	unsigned count;
	uint32_t part_id[PART_TABLE_MAX];
	uint32_t flash_size[PART_TABLE_MAX];
};

/* "128 * 1024" anywhere in the line, as bytes. */
static int parse_kib(const char *line, uint32_t *flash_size)
{
	const char *at = strstr(line, "* 1024");
	const char *digits;
	unsigned long kib;

	if (at == NULL)
		return 0;
	digits = at;
	while (digits > line && (digits[-1] == ' ' || digits[-1] == '\t'))
		digits--;
	while (digits > line && isdigit((unsigned char)digits[-1]))
		digits--;
	if (sscanf(digits, "%lu", &kib) != 1)
		return 0;
	*flash_size = (uint32_t)(kib * 1024);
	return 1;
}

/*
 * Read the table between `opener` and the first line holding `};` after it.
 * Each entry is a line with a hexadecimal part identification number and a
 * size in KiB, whatever else the line carries.
 */
static int parse_table(const char *path, const char *opener,
                       struct part_table *table)
{
	char line[512];
	int in_table = 0;
	FILE *f = fopen(path, "r");

	if (f == NULL)
		return 0;

	memset(table, 0, sizeof(*table));

	while (fgets(line, sizeof(line), f) != NULL) {
		unsigned long part_id;
		uint32_t flash_size;

		if (!in_table) {
			in_table = strstr(line, opener) != NULL;
			continue;
		}
		if (strstr(line, "};") != NULL)
			break;
		if (sscanf(line, " { 0x%lx", &part_id) != 1)
			continue;
		if (!parse_kib(line, &flash_size))
			continue;
		if (table->count >= PART_TABLE_MAX)
			break;
		table->part_id[table->count] = (uint32_t)part_id;
		table->flash_size[table->count] = flash_size;
		table->count++;
	}

	fclose(f);
	return 1;
}

MU_TEST(test_the_host_and_bootloader_part_tables_agree)
{
	const char *host_path = HOST_DFU_DIR "/dfu_recovery.c";
	const char *flash_path = FW_BOOTLOADER_DIR "/flash.h";
	struct part_table host, boot;
	char msg[640];
	unsigned i;

	snprintf(msg, sizeof(msg), "cannot open %s to scan", flash_path);
	mu_assert_fatal(parse_table(flash_path, "static const entry table[] = {",
	                            &boot), msg);
	snprintf(msg, sizeof(msg), "cannot open %s to scan", host_path);
	mu_assert_fatal(parse_table(host_path,
	                            "static const struct dfu_part DFU_PARTS[] = {",
	                            &host), msg);

	snprintf(msg, sizeof(msg),
	         "%s: the scan read no part table, so this test checks nothing",
	         flash_path);
	mu_assert_fatal(boot.count > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s has %u parts, %s has %u: the host would ask about a part the "
	         "bootloader cannot bound, or the other way round",
	         flash_path, boot.count, host_path, host.count);
	mu_assert_fatal(boot.count == host.count, msg);

	for (i = 0; i < boot.count; i++) {
		snprintf(msg, sizeof(msg),
		         "entry %u: %s has part 0x%08x with %u bytes of flash, %s has "
		         "part 0x%08x with %u", i, flash_path, boot.part_id[i],
		         boot.flash_size[i], host_path, host.part_id[i],
		         host.flash_size[i]);
		mu_assert(boot.part_id[i] == host.part_id[i] &&
		          boot.flash_size[i] == host.flash_size[i], msg);
	}
}

int main(void)
{
	printf("bootloader flash read-back bound tests\n");
	MU_RUN(test_the_table_knows_the_parts_in_the_family);
	MU_RUN(test_the_bound_follows_the_part);
	MU_RUN(test_the_last_byte_of_flash_is_the_last_valid_address);
	MU_RUN(test_a_block_inside_the_part_is_read);
	MU_RUN(test_the_first_block_past_a_128k_part_is_refused);
	MU_RUN(test_every_block_past_a_128k_part_is_refused);
	MU_RUN(test_a_block_that_ends_past_the_part_is_refused);
	MU_RUN(test_a_512k_part_reads_back_all_of_it);
	MU_RUN(test_an_unknown_part_keeps_the_old_bound);
	MU_RUN(test_a_failed_part_read_keeps_the_old_bound);
	MU_RUN(test_the_part_is_read_once);
	MU_RUN(test_the_old_fixed_bound_read_past_the_end);

	/* Scanned once here rather than from the first of the tests below,
	 * which would leave them depending on the order they run in. */
	scanned = scan_dfu(dfu_path, &s);
	MU_RUN(test_dfu_source_was_scanned);
	MU_RUN(test_the_read_back_refuses_before_it_reads);
	MU_RUN(test_the_host_and_bootloader_part_tables_agree);
	return mu_report();
}
