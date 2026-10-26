/*
 * The bounds on the flash the bootloader will read back (REQ_UPLOAD) and
 * write (REQ_DNLOAD).
 *
 * Both used to be wrong in the same way: the read-back stopped at a fixed
 * 512 KiB whatever part the bootloader ran on, and the download stopped
 * nowhere at all. On a part with less flash than the host asked about -- the
 * Ubertooth One schematic specifies an LPC1754 with 128 KiB -- a read ran off
 * the end of the memory the part has, which is a precise bus fault on
 * LPC17xx, and the fault does not return to the bootloader's polled USB loop:
 * the board answered nothing more, a state request included, until it was
 * power-cycled. A write off the end was milder: In-Application Programming
 * refused it, but only after the blocks below it had been programmed, so the
 * board was left running an application missing its tail. The first block
 * past the end of a 128 KiB part is block 512, at 0x20000, and that is the
 * case these tests are built around.
 *
 * This is the tree's only C++, and the pieces that decide the bound -- the
 * part table, the sector map, the sector probe, Flash::valid_address and
 * Flash::write -- compile here as they are, so they are the real thing below,
 * with the In-Application Programming commands behind them faked. The DFU
 * request handlers cannot be compiled here (dfu.cpp casts a pointer to
 * uint32_t, which does not fit on a 64-bit host), so their shape is faked and
 * the last tests scan dfu.cpp to show the firmware has that same shape -- the
 * way test_tx_generic_bounds.c does.
 *
 * The fake part has a guard region above its flash, and the fakes record any
 * command aimed above it. Nothing the bootloader does should reach either, so
 * a bound that let a block through shows up as a command aimed past the end
 * of flash, or as changed guard bytes where the write actually landed, rather
 * than as a fake that happened to refuse the block too.
 *
 * One test puts the host's copy of the part table against the bootloader's:
 * the two have to agree or the host will ask for flash a board has not got.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "minunit.h"
#include "source_scan.h"

/* The real tables, the real bound, the real Flash. */
#include "flash.h"

/* The size of a DFU transfer, DFU::transfer_size in dfu.h. */
#define TRANSFER_SIZE 0x100

/* The DFU statuses the two paths answer with; DFU::Status is private. */
#define DFU_STATUS_OK 0x00
#define DFU_STATUS_ERRWRITE 0x03
#define DFU_STATUS_ERRPROG 0x06
#define DFU_STATUS_ERRADDRESS 0x08

/* The first address the bootloader will write: below it is its own code,
 * which memory_policy::write_permitted in dfu.cpp protects. */
#define BOOTLOADER_LIMIT 0x4000

/* A part identification number no LPC175x/176x part has. */
#define PART_UNKNOWN 0x12345678

#define PART_LPC1754 0x25011722  /* 128 KiB, the Ubertooth One schematic's */
#define PART_LPC1768 0x26013F37  /* 512 KiB */

/*
 * The part's flash as the bootloader's reads and writes would see it: bytes
 * up to the flash the part has, then a guard region with nothing behind it.
 *
 * `present_flash` is where the part's flash ends. The array runs past that so
 * the guard has somewhere to live, and the fakes below write into it when
 * they are told to, which is how a bound that let a block through is caught.
 */
#define FLASH_MAX (512 * 1024)
#define GUARD_LENGTH 0x1000
#define GUARD_BYTE 0xc5
#define ERASED_BYTE 0xff
#define PROGRAMMED_BYTE 0x5a

static uint8_t fake_flash[FLASH_MAX + GUARD_LENGTH];
static uint32_t present_flash;

/* What the fakes recorded since the part was attached. */
static uint32_t fake_part_id;
static IAP::ReturnCode fake_part_result;
static IAP::ReturnCode fake_blank_check_result;  /* for a sector the part has */
static IAP::ReturnCode fake_program_result;      /* for a write inside flash */
static unsigned part_reads;
static unsigned blank_checks;
static unsigned reads;        /* reads out of flash */
static unsigned programs;     /* copy_ram_to_flash calls */
static unsigned erases;
static int faulted;           /* a read ran past the end of flash */
static int touched_absent;    /* a write was aimed past the end of flash */

/*
 * The source of a write. Flash::write hands In-Application Programming a
 * 32-bit source address, which cannot carry a pointer on a 64-bit host --
 * the same reason dfu.cpp will not compile here -- so the fake programmer
 * reads from here instead.
 */
static const uint8_t *write_source;

/* Does the part have this sector? Sectors run from 0 up to the end of the
 * part's flash, so a sector starting at or above it is one the part has not
 * got, and the ROM answers INVALID_SECTOR for it. */
static int sector_present(uint32_t sector)
{
	return lpc_sector_start(sector) < present_flash;
}

static int sectors_present(uint32_t start, uint32_t end)
{
	uint32_t sector;

	for (sector = start; sector <= end; sector++)
		if (!sector_present(sector))
			return 0;
	return 1;
}

/*
 * The In-Application Programming commands behind Flash. Defined here rather
 * than linked from iap.cpp, which calls the part's ROM.
 */
IAP::ReturnCode IAP::read_part_identification_number(uint32_t& part_id) const
{
	part_reads++;
	if (fake_part_result == CMD_SUCCESS)
		part_id = fake_part_id;
	return fake_part_result;
}

IAP::ReturnCode IAP::blank_check_sectors(const uint32_t start_sector_number,
                                         const uint32_t end_sector_number) const
{
	blank_checks++;
	if (!sectors_present(start_sector_number, end_sector_number))
		return INVALID_SECTOR;
	return fake_blank_check_result;
}

IAP::ReturnCode IAP::prepare_sectors_for_write_operation(const uint32_t start_sector_number,
                                                         const uint32_t end_sector_number)
{
	if (!sectors_present(start_sector_number, end_sector_number)) {
		touched_absent = 1;
		return INVALID_SECTOR;
	}
	return CMD_SUCCESS;
}

IAP::ReturnCode IAP::erase_sectors(const uint32_t start_sector_number,
                                   const uint32_t end_sector_number,
                                   const uint32_t cpu_clock_frequency_khz)
{
	uint32_t sector;

	(void)cpu_clock_frequency_khz;
	erases++;
	if (!sectors_present(start_sector_number, end_sector_number)) {
		touched_absent = 1;
		return INVALID_SECTOR;
	}
	for (sector = start_sector_number; sector <= end_sector_number; sector++) {
		uint32_t start = lpc_sector_start(sector);

		memset(fake_flash + start, ERASED_BYTE, lpc_sector_size(start));
	}
	return CMD_SUCCESS;
}

/*
 * The programmer. A real part refuses an address it has no flash for, but
 * this writes it anyway, into the guard region: the point of the guard is to
 * catch a block the bound should have stopped, and a fake that refused it as
 * well would hide exactly that.
 */
IAP::ReturnCode IAP::copy_ram_to_flash(const uint32_t destination_flash_address,
                                       const uint32_t source_ram_address,
                                       const uint32_t number_of_bytes_to_write,
                                       const uint32_t cpu_clock_frequency_khz)
{
	const uint32_t end = destination_flash_address + number_of_bytes_to_write;

	(void)source_ram_address;
	(void)cpu_clock_frequency_khz;
	programs++;
	if (end > present_flash)
		touched_absent = 1;
	if (end <= sizeof(fake_flash))
		memcpy(fake_flash + destination_flash_address, write_source,
		       number_of_bytes_to_write);
	return (end > present_flash) ? DST_ADDR_ERROR : fake_program_result;
}

/*
 * The part's flash as a read would see it: reading past the end is the bus
 * fault, so this records it rather than reading something.
 */
static void read_flash(uint8_t *out, uint32_t address, uint32_t length)
{
	reads++;
	if (address + length > present_flash) {
		faulted = 1;
		return;
	}
	memcpy(out, fake_flash + address, length);
}

/* A part with `flash_size` bytes of flash, answering the part read. */
static void attach_part(uint32_t part_id, uint32_t flash_size)
{
	fake_part_id = part_id;
	fake_part_result = IAP::CMD_SUCCESS;
	fake_blank_check_result = IAP::SECTOR_NOT_BLANK;
	fake_program_result = IAP::CMD_SUCCESS;
	present_flash = flash_size;
	part_reads = 0;
	blank_checks = 0;
	reads = 0;
	programs = 0;
	erases = 0;
	faulted = 0;
	touched_absent = 0;
	memset(fake_flash, PROGRAMMED_BYTE, flash_size);
	memset(fake_flash + flash_size, GUARD_BYTE, sizeof(fake_flash) - flash_size);
}

/* Is every byte above the part's flash still as the guard was laid down? */
static int guard_intact(void)
{
	uint32_t i;

	for (i = present_flash; i < sizeof(fake_flash); i++)
		if (fake_flash[i] != GUARD_BYTE)
			return 0;
	return 1;
}

/*
 * request_upload()'s shape: the first and last address of the block are both
 * checked against the bound, and only a block that passes both is read.
 * Returns the DFU status the request leaves behind.
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

/*
 * request_dnload()'s shape: the bootloader's own flash is refused first, then
 * both ends of the block go through the bound, and only a block that passes
 * all three is written. Returns the DFU status the request leaves behind.
 */
static uint8_t dnload(Flash& flash, uint32_t address, uint32_t length,
                      const uint8_t *in)
{
	if (address < BOOTLOADER_LIMIT)
		return DFU_STATUS_ERRWRITE;
	if (!flash.valid_address(address) || !flash.valid_address(address + length - 1))
		return DFU_STATUS_ERRADDRESS;
	write_source = in;
	if (!flash.write(address, 0, length))
		return DFU_STATUS_ERRPROG;
	return DFU_STATUS_OK;
}

static uint8_t dnload_block(Flash& flash, uint16_t block, const uint8_t *in)
{
	return dnload(flash, (uint32_t)block * TRANSFER_SIZE, TRANSFER_SIZE, in);
}

/* A block of image, filled so a write of it is recognisable in the flash. */
static void fill_block(uint8_t *block, uint8_t value)
{
	memset(block, value, TRANSFER_SIZE);
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

/*
 * The sector map, which the probe walks and Flash::write erases by. A sector
 * number and a sector start have to be inverses of each other, or the probe
 * would report a bound in the wrong place.
 */
MU_TEST(test_the_sector_map_agrees_with_itself)
{
	/* The flash sizes the family comes in; the table has no other. */
	static const uint32_t family_sizes[] = {
		32 * 1024, 64 * 1024, 128 * 1024, 256 * 1024, 512 * 1024
	};
	uint32_t sector;
	size_t i;

	for (sector = 0; sector <= LPC_TOP_SECTOR; sector++) {
		uint32_t start = lpc_sector_start(sector);

		mu_assert_int_eq((int)sector, (int)lpc_sector_number(start));
		/* The last byte of the sector is still in it, and the next byte is
		 * the start of the next sector. */
		mu_assert_int_eq((int)sector,
		                 (int)lpc_sector_number(start + lpc_sector_size(start) - 1));
		mu_assert_int_eq((int)start + (int)lpc_sector_size(start),
		                 (int)lpc_sector_start(sector + 1));
	}

	/* The family's own figures: sixteen 4 KiB sectors, then 32 KiB ones. */
	mu_assert_int_eq(0x1000, (int)lpc_sector_size(0));
	mu_assert_int_eq(0x1000, (int)lpc_sector_size(0xFFFF));
	mu_assert_int_eq(0x8000, (int)lpc_sector_size(0x10000));
	mu_assert_int_eq(16, (int)lpc_sector_number(0x10000));
	mu_assert_int_eq(29, (int)lpc_sector_number(0x7FFFF));

	/* One past the top sector is the top of the largest part in the family,
	 * which is also the fallback bound. */
	mu_assert_int_eq(LPC_FLASH_SIZE_FALLBACK,
	                 (int)lpc_sector_start(LPC_TOP_SECTOR + 1));

	/* Every flash size in the family ends on a sector boundary. That is what
	 * lets the probe find the end of flash by asking about sectors: a part
	 * whose flash stopped mid-sector could not be placed this way. */
	for (i = 0; i < sizeof(family_sizes) / sizeof(family_sizes[0]); i++)
		mu_assert_int_eq((int)family_sizes[i],
		                 (int)lpc_sector_start(lpc_sector_number(family_sizes[i])));
}

/* The bound is the flash the part has. */
MU_TEST(test_the_bound_follows_the_part)
{
	/* A fresh Flash for each part: one remembers the bound it worked out, so
	 * it cannot be reused to ask about a different part. */
	Flash small, large;

	attach_part(PART_LPC1754, 128 * 1024);
	mu_assert_int_eq(128 * 1024, (int)small.flash_bound());

	attach_part(PART_LPC1768, 512 * 1024);
	mu_assert_int_eq(512 * 1024, (int)large.flash_bound());
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
	mu_assert_int_eq(PROGRAMMED_BYTE, out[0]);
	mu_assert_int_eq(PROGRAMMED_BYTE, out[TRANSFER_SIZE - 1]);
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

/* A block inside the part's flash is written, and only where it was aimed. */
MU_TEST(test_a_block_inside_the_part_is_written)
{
	uint8_t in[TRANSFER_SIZE];
	Flash flash;

	attach_part(PART_LPC1754, 128 * 1024);
	fill_block(in, 0xa5);
	mu_assert_int_eq(DFU_STATUS_OK, dnload(flash, BOOTLOADER_LIMIT,
	                                       TRANSFER_SIZE, in));
	mu_assert_int_eq(1, (int)programs);
	mu_assert_int_eq(0xa5, fake_flash[BOOTLOADER_LIMIT]);
	mu_assert_int_eq(0xa5, fake_flash[BOOTLOADER_LIMIT + TRANSFER_SIZE - 1]);
	/* The rest of the sector was erased for it, and nothing below the
	 * application region was touched. */
	mu_assert_int_eq(ERASED_BYTE, fake_flash[BOOTLOADER_LIMIT + TRANSFER_SIZE]);
	mu_assert_int_eq(PROGRAMMED_BYTE, fake_flash[BOOTLOADER_LIMIT - 1]);
	mu_assert(!touched_absent, "a block inside the part's flash was aimed past it");
	mu_assert(guard_intact(), "a block inside the part's flash reached the guard");
}

/* The last block of a 128 KiB part is written; the first block past it is not. */
MU_TEST(test_the_first_block_past_a_128k_part_is_not_written)
{
	uint8_t in[TRANSFER_SIZE];
	Flash flash;

	attach_part(PART_LPC1754, 128 * 1024);
	fill_block(in, 0xa5);
	mu_assert_int_eq(DFU_STATUS_OK, dnload_block(flash, 0x1FF, in));
	mu_assert_int_eq(1, (int)programs);

	mu_assert_int_eq(DFU_STATUS_ERRADDRESS, dnload_block(flash, 512, in));
	/* Refused before In-Application Programming was asked for anything: no
	 * further write, and no erase of a sector the part has not got. */
	mu_assert_int_eq(1, (int)programs);
	mu_assert(!touched_absent,
	          "the refused block was still aimed past the end of flash");
	mu_assert(guard_intact(), "the refused block was written into the guard");
}

/* So is every block above it, up to the end of what a host can send. */
MU_TEST(test_every_block_past_a_128k_part_is_not_written)
{
	uint8_t in[TRANSFER_SIZE];
	Flash flash;
	uint32_t block, accepted = 0;

	attach_part(PART_LPC1754, 128 * 1024);
	fill_block(in, 0xa5);
	for (block = 512; block <= 0xFFFF; block++)
		if (dnload_block(flash, (uint16_t)block, in) != DFU_STATUS_ERRADDRESS)
			accepted++;
	mu_assert_int_eq(0, (int)accepted);
	mu_assert_int_eq(0, (int)programs);
	mu_assert_int_eq(0, (int)erases);
	mu_assert(!touched_absent, "a block past the end of the part was written");
	mu_assert(guard_intact(), "a block past the end of the part reached the guard");
}

/* A block that starts inside flash and ends past it is refused too. */
MU_TEST(test_a_block_that_ends_past_the_part_is_not_written)
{
	uint8_t in[TRANSFER_SIZE];
	Flash flash;

	attach_part(PART_LPC1754, 128 * 1024);
	fill_block(in, 0xa5);
	mu_assert_int_eq(DFU_STATUS_ERRADDRESS,
	                 dnload(flash, 0x1FF80, TRANSFER_SIZE, in));
	mu_assert_int_eq(0, (int)programs);
	mu_assert(guard_intact(), "a block ending past the part's flash was written");
}

/* The bootloader's own flash is still refused, and with the status it always
 * answered: the floor has not moved. */
MU_TEST(test_the_bootloaders_own_flash_is_still_refused)
{
	uint8_t in[TRANSFER_SIZE];
	Flash flash;

	attach_part(PART_LPC1754, 128 * 1024);
	fill_block(in, 0xa5);
	mu_assert_int_eq(DFU_STATUS_ERRWRITE, dnload(flash, 0, TRANSFER_SIZE, in));
	mu_assert_int_eq(DFU_STATUS_ERRWRITE,
	                 dnload(flash, BOOTLOADER_LIMIT - TRANSFER_SIZE,
	                        TRANSFER_SIZE, in));
	mu_assert_int_eq(0, (int)programs);
	mu_assert_int_eq(PROGRAMMED_BYTE, fake_flash[0]);
}

/*
 * The case from the issue, end to end: an image too big for the part. The
 * blocks that fit are written, the rest are refused, and nothing lands above
 * the part's flash.
 */
MU_TEST(test_an_oversize_image_stops_at_the_end_of_flash)
{
	uint8_t in[TRANSFER_SIZE];
	Flash flash;
	uint32_t address, written = 0, refused = 0;

	attach_part(PART_LPC1754, 128 * 1024);
	fill_block(in, 0xa5);
	/* 192 KiB of image into a 128 KiB part, from the first application
	 * address upwards, the way the host sends it. */
	for (address = BOOTLOADER_LIMIT; address < BOOTLOADER_LIMIT + 192 * 1024;
	     address += TRANSFER_SIZE) {
		switch (dnload(flash, address, TRANSFER_SIZE, in)) {
		case DFU_STATUS_OK:
			written++;
			break;
		case DFU_STATUS_ERRADDRESS:
			refused++;
			break;
		default:
			mu_assert(0, "a block was neither written nor refused for its address");
		}
	}

	/* Everything from the application region to the top of the part's flash,
	 * and nothing above it. */
	mu_assert_int_eq((int)((128 * 1024 - BOOTLOADER_LIMIT) / TRANSFER_SIZE),
	                 (int)written);
	mu_assert_int_eq((int)((192 * 1024 - (128 * 1024 - BOOTLOADER_LIMIT)) /
	                       TRANSFER_SIZE), (int)refused);
	mu_assert_int_eq((int)written, (int)programs);
	mu_assert_int_eq(0xa5, fake_flash[128 * 1024 - 1]);
	mu_assert(!touched_absent, "an oversize image was written past the end of flash");
	mu_assert(guard_intact(), "an oversize image reached the guard");
}

/*
 * What makes the tests above mean something. The old bounds let both of these
 * through on a 128 KiB part: a read at 0x20000 runs past the memory the part
 * has, and a write there lands in the guard.
 */
MU_TEST(test_the_old_bounds_ran_past_the_end)
{
	uint8_t buffer[TRANSFER_SIZE];

	attach_part(PART_LPC1754, 128 * 1024);
	mu_assert(0x20000 < LPC_FLASH_SIZE_FALLBACK,
	          "the old fixed bound would have refused the block from the field, "
	          "so the read-back tests above are checking nothing");

	read_flash(buffer, 0x20000, TRANSFER_SIZE);
	mu_assert(faulted, "a read past a 128 KiB part did not run past its flash");

	fill_block(buffer, 0xa5);
	write_source = buffer;
	{
		IAP iap;

		iap.copy_ram_to_flash(0x20000, 0, TRANSFER_SIZE, 100000);
	}
	mu_assert(touched_absent,
	          "a write past a 128 KiB part was not aimed past its flash");
	mu_assert(!guard_intact(),
	          "a write past the end of flash left the guard alone, so the "
	          "download tests above would pass whatever the bound did");
}

/*
 * A block the part accepted the sector for but failed to program is reported
 * as a failure, not answered as a block written. The bound cannot cause this
 * one -- it stands for a part that fails the program itself.
 */
MU_TEST(test_a_failed_program_is_reported)
{
	uint8_t in[TRANSFER_SIZE];
	Flash flash;

	attach_part(PART_LPC1754, 128 * 1024);
	fill_block(in, 0xa5);
	fake_program_result = IAP::BUSY;
	mu_assert_int_eq(DFU_STATUS_ERRPROG, dnload(flash, BOOTLOADER_LIMIT,
	                                            TRANSFER_SIZE, in));
	mu_assert_int_eq(1, (int)programs);
}

/*
 * A part the table has no entry for is bounded by the sectors it has, found
 * with the blank check. Nothing here depends on the part being listed.
 */
MU_TEST(test_an_unknown_part_is_bounded_by_the_sectors_it_has)
{
	uint8_t buffer[TRANSFER_SIZE];
	Flash flash;

	attach_part(PART_UNKNOWN, 128 * 1024);
	mu_assert_int_eq(128 * 1024, (int)flash.flash_bound());
	mu_assert(blank_checks > 0, "an unlisted part was not probed");
	/* The probe asks about sectors; it never reads flash, which is what
	 * would have faulted. */
	mu_assert_int_eq(0, (int)reads);
	mu_assert(!faulted, "the probe read past the end of flash");

	mu_assert_int_eq(DFU_STATUS_ERRADDRESS, upload_block(flash, 512, buffer));
	fill_block(buffer, 0xa5);
	mu_assert_int_eq(DFU_STATUS_ERRADDRESS, dnload_block(flash, 512, buffer));
	mu_assert(guard_intact(), "an unlisted part was written past its flash");
}

/* The same for the largest part in the family, where the probe runs out of
 * sectors rather than finding one the part has not got. */
MU_TEST(test_an_unknown_512k_part_is_bounded_at_the_top_of_the_map)
{
	Flash flash;

	attach_part(PART_UNKNOWN, 512 * 1024);
	mu_assert_int_eq(512 * 1024, (int)flash.flash_bound());
	mu_assert(flash.valid_address(0x7FFFF),
	          "an unlisted 512 KiB part refuses the last byte of its flash");
	mu_assert(!flash.valid_address(0x80000),
	          "an unlisted 512 KiB part is not bounded at all");
}

/* A part with every sector erased answers the blank check differently, and
 * the probe has to read that as a sector the part has. */
MU_TEST(test_a_blank_part_is_bounded_the_same_way)
{
	Flash flash;

	attach_part(PART_UNKNOWN, 128 * 1024);
	fake_blank_check_result = IAP::CMD_SUCCESS;
	mu_assert_int_eq(128 * 1024, (int)flash.flash_bound());
}

/* A listed part is not probed: the table is the fast answer. */
MU_TEST(test_a_listed_part_is_not_probed)
{
	Flash flash;

	attach_part(PART_LPC1754, 128 * 1024);
	mu_assert_int_eq(128 * 1024, (int)flash.flash_bound());
	mu_assert_int_eq(0, (int)blank_checks);
}

/* The part and its sectors are asked about once, however many blocks go by. */
MU_TEST(test_the_part_is_asked_about_once)
{
	uint8_t out[TRANSFER_SIZE];
	Flash listed, unlisted;
	uint32_t block;

	attach_part(PART_LPC1754, 128 * 1024);
	for (block = 0x40; block < 0x240; block++)
		upload_block(listed, (uint16_t)block, out);
	mu_assert_int_eq(1, (int)part_reads);

	attach_part(PART_UNKNOWN, 128 * 1024);
	for (block = 0x40; block < 0x240; block++)
		upload_block(unlisted, (uint16_t)block, out);
	mu_assert_int_eq(1, (int)part_reads);
	/* One walk up the sector map, not one per address checked: a check for
	 * every sector the part has, and one for the first sector it has not,
	 * which is the sector the top of its flash falls in. */
	mu_assert_int_eq((int)lpc_sector_number(128 * 1024) + 1,
	                 (int)blank_checks);
}

/*
 * A part that answers neither question leaves no figure to bound by, so it
 * keeps the 512 KiB the bootloader enforced on every part before the table
 * existed. Such a board is no worse off: it either has at least 512 KiB and
 * behaves as it did, or has less and wedges on a read past its end as it did.
 * Bounding it at the smallest part in the family would refuse a read-back,
 * and an image, that work.
 */
MU_TEST(test_a_part_that_answers_nothing_keeps_the_old_bound)
{
	Flash flash;

	attach_part(PART_LPC1754, 128 * 1024);
	fake_part_result = IAP::BUSY;
	fake_blank_check_result = IAP::BUSY;

	mu_assert_int_eq(LPC_FLASH_SIZE_FALLBACK, (int)flash.flash_bound());
	mu_assert(flash.valid_address(0x7FFFF),
	          "a part that answered nothing refuses an address the old bound allowed");
	mu_assert(!flash.valid_address(LPC_FLASH_SIZE_FALLBACK),
	          "a part that answered nothing is not bounded at all");

	/* Nothing was remembered, so the next request asks again: a part that
	 * answers late still ends up bounded by its own flash. */
	mu_assert(part_reads > 1, "a part that answered nothing is not asked again");
	fake_part_result = IAP::CMD_SUCCESS;
	mu_assert_int_eq(128 * 1024, (int)flash.flash_bound());
}

/* A part read that fails on its own falls through to the probe, which is the
 * part answering the same question directly. */
MU_TEST(test_a_failed_part_read_falls_through_to_the_probe)
{
	Flash flash;

	attach_part(PART_LPC1754, 128 * 1024);
	fake_part_result = IAP::BUSY;

	mu_assert_int_eq(128 * 1024, (int)flash.flash_bound());
	mu_assert(blank_checks > 0, "a failed part read did not reach the probe");
	mu_assert(!flash.valid_address(0x20000),
	          "a failed part read left the old fixed bound in place");
}

/*
 * The fakes are worth something only if the request handlers have the same
 * shape, and they cannot be compiled here. So: find them, and check that both
 * ends of the block go through Flash::valid_address, that the read or write
 * comes after both, and that a block that fails them answers ERRADDRESS.
 */
struct scan {
	int floor;                 /* the bootloader's own flash, in memory_policy */

	int upload;                /* request_upload()'s definition */
	int upload_start_check;    /* the block's first address */
	int upload_end_check;      /* ...and its last */
	int upload_read;           /* the read out of flash */
	int upload_reads;          /* how many reads the function has */
	int upload_erraddress;     /* the refusal */

	int dnload;                /* request_dnload()'s definition */
	int dnload_permitted;      /* the bootloader-flash check */
	int dnload_errwrite;       /* ...and its refusal */
	int dnload_start_check;
	int dnload_end_check;
	int dnload_write;          /* the write to flash */
	int dnload_writes;
	int dnload_erraddress;
};

static int scan_dfu(const char *path, struct scan *s)
{
	char line[512], flat[512];
	int lineno = 0, in_block = 0, in_upload = 0, in_dnload = 0;
	FILE *f = fopen(path, "r");

	if (f == NULL)
		return 0;

	memset(s, 0, sizeof(*s));

	while (fgets(line, sizeof(line), f) != NULL) {
		lineno++;
		strip_comments(line, &in_block);
		collapse_spaces(line, flat, sizeof(flat));

		if (strstr(flat, "flash_address >= 0x4000") != NULL)
			s->floor = lineno;

		if (strstr(flat, "bool DFU::request_upload(") != NULL) {
			s->upload = lineno;
			in_upload = 1;
			continue;
		}
		if (strstr(flat, "bool DFU::request_dnload(") != NULL) {
			s->dnload = lineno;
			in_dnload = 1;
			continue;
		}
		/* A function's own closing brace is the only one in the first
		 * column; the braces inside it are indented. Tested on the line as
		 * it is, since collapse_spaces() drops the indentation. */
		if (line[0] == '}') {
			in_upload = 0;
			in_dnload = 0;
			continue;
		}

		if (in_upload) {
			if (strstr(flat, "flash.valid_address(flash_address_start)") != NULL)
				s->upload_start_check = lineno;
			if (strstr(flat, "flash.valid_address(flash_address_end - 1)") != NULL)
				s->upload_end_check = lineno;
			if (strstr(flat, "memcpy(") != NULL) {
				s->upload_reads++;
				if (s->upload_read == 0)
					s->upload_read = lineno;
			}
			if (strstr(flat, "error(ERRADDRESS)") != NULL)
				s->upload_erraddress = lineno;
		}

		if (in_dnload) {
			if (strstr(flat, "memory_policy::write_permitted(") != NULL)
				s->dnload_permitted = lineno;
			if (strstr(flat, "error(ERRWRITE)") != NULL)
				s->dnload_errwrite = lineno;
			if (strstr(flat, "flash.valid_address(flash_address_start)") != NULL)
				s->dnload_start_check = lineno;
			if (strstr(flat, "flash.valid_address(flash_address_end - 1)") != NULL)
				s->dnload_end_check = lineno;
			if (strstr(flat, "flash.write(") != NULL) {
				s->dnload_writes++;
				if (s->dnload_write == 0)
					s->dnload_write = lineno;
			}
			if (strstr(flat, "error(ERRADDRESS)") != NULL)
				s->dnload_erraddress = lineno;
		}
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
	mu_assert(s.upload > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s: the scan did not find request_dnload(), so the checks below "
	         "would pass without reading anything", dfu_path);
	mu_assert(s.dnload > 0, msg);
}

MU_TEST(test_the_read_back_refuses_before_it_reads)
{
	char msg[640];

	snprintf(msg, sizeof(msg),
	         "%s:%d: request_upload() does not put the block's first address to "
	         "Flash::valid_address", dfu_path, s.upload);
	mu_assert(s.upload_start_check > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: request_upload() does not put the block's last address to "
	         "Flash::valid_address, so a block ending past the end of flash "
	         "would be read", dfu_path, s.upload);
	mu_assert(s.upload_end_check > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s: request_upload() must read out of flash exactly once, after "
	         "both checks; the scan found %d reads, the first at line %d",
	         dfu_path, s.upload_reads, s.upload_read);
	mu_assert(s.upload_reads == 1 && s.upload_read > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: the read out of flash comes before the checks at lines "
	         "%d and %d, so a block is read before it is refused",
	         dfu_path, s.upload_read, s.upload_start_check, s.upload_end_check);
	mu_assert(s.upload_start_check < s.upload_read &&
	          s.upload_end_check < s.upload_read, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: request_upload() does not answer ERRADDRESS for a block "
	         "outside flash", dfu_path, s.upload);
	mu_assert(s.upload_erraddress > s.upload_read, msg);
}

MU_TEST(test_the_download_refuses_before_it_writes)
{
	char msg[640];

	snprintf(msg, sizeof(msg),
	         "%s: the scan did not find the bootloader's own flash limit, so "
	         "nothing here says a download cannot overwrite the bootloader",
	         dfu_path);
	mu_assert(s.floor > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: request_dnload() does not check the block against the "
	         "bootloader's own flash", dfu_path, s.dnload);
	mu_assert(s.dnload_permitted > 0 && s.dnload_errwrite > s.dnload_permitted,
	          msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: request_dnload() does not put the block's first address to "
	         "Flash::valid_address, so a block with no flash behind it would "
	         "be written", dfu_path, s.dnload);
	mu_assert(s.dnload_start_check > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: request_dnload() does not put the block's last address to "
	         "Flash::valid_address, so a block ending past the end of flash "
	         "would be written", dfu_path, s.dnload);
	mu_assert(s.dnload_end_check > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s: request_dnload() must write flash exactly once, after both "
	         "checks; the scan found %d writes, the first at line %d",
	         dfu_path, s.dnload_writes, s.dnload_write);
	mu_assert(s.dnload_writes == 1 && s.dnload_write > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: the write to flash comes before the checks at lines %d "
	         "and %d, so a block is written before it is refused",
	         dfu_path, s.dnload_write, s.dnload_start_check, s.dnload_end_check);
	mu_assert(s.dnload_start_check < s.dnload_write &&
	          s.dnload_end_check < s.dnload_write, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: request_dnload() answers ERRADDRESS at line %d, which is "
	         "not between the checks and the write, so the refusal does not "
	         "come first", dfu_path, s.dnload, s.dnload_erraddress);
	mu_assert(s.dnload_erraddress > s.dnload_end_check &&
	          s.dnload_erraddress < s.dnload_write, msg);
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

/* A size written as a multiple of 1024, as bytes, whatever the spacing. */
static int parse_kib(const char *line, uint32_t *flash_size)
{
	const char *at = strchr(line, '*');
	const char *digits;
	unsigned long kib, unit;

	if (at == NULL)
		return 0;
	if (sscanf(at + 1, " %lu", &unit) != 1 || unit != 1024)
		return 0;
	digits = at;
	while (digits > line && (digits[-1] == ' ' || digits[-1] == '\t'))
		digits--;
	while (digits > line && isdigit((unsigned char)digits[-1]))
		digits--;
	if (sscanf(digits, "%lu", &kib) != 1)
		return 0;
	*flash_size = (uint32_t)(kib * unit);
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
	printf("bootloader flash bound tests\n");
	MU_RUN(test_the_table_knows_the_parts_in_the_family);
	MU_RUN(test_the_sector_map_agrees_with_itself);
	MU_RUN(test_the_bound_follows_the_part);
	MU_RUN(test_the_last_byte_of_flash_is_the_last_valid_address);

	MU_RUN(test_a_block_inside_the_part_is_read);
	MU_RUN(test_the_first_block_past_a_128k_part_is_refused);
	MU_RUN(test_every_block_past_a_128k_part_is_refused);
	MU_RUN(test_a_block_that_ends_past_the_part_is_refused);
	MU_RUN(test_a_512k_part_reads_back_all_of_it);

	MU_RUN(test_a_block_inside_the_part_is_written);
	MU_RUN(test_the_first_block_past_a_128k_part_is_not_written);
	MU_RUN(test_every_block_past_a_128k_part_is_not_written);
	MU_RUN(test_a_block_that_ends_past_the_part_is_not_written);
	MU_RUN(test_the_bootloaders_own_flash_is_still_refused);
	MU_RUN(test_an_oversize_image_stops_at_the_end_of_flash);
	MU_RUN(test_a_failed_program_is_reported);
	MU_RUN(test_the_old_bounds_ran_past_the_end);

	MU_RUN(test_an_unknown_part_is_bounded_by_the_sectors_it_has);
	MU_RUN(test_an_unknown_512k_part_is_bounded_at_the_top_of_the_map);
	MU_RUN(test_a_blank_part_is_bounded_the_same_way);
	MU_RUN(test_a_listed_part_is_not_probed);
	MU_RUN(test_the_part_is_asked_about_once);
	MU_RUN(test_a_part_that_answers_nothing_keeps_the_old_bound);
	MU_RUN(test_a_failed_part_read_falls_through_to_the_probe);

	/* Scanned once here rather than from the first of the tests below,
	 * which would leave them depending on the order they run in. */
	scanned = scan_dfu(dfu_path, &s);
	MU_RUN(test_dfu_source_was_scanned);
	MU_RUN(test_the_read_back_refuses_before_it_reads);
	MU_RUN(test_the_download_refuses_before_it_writes);
	MU_RUN(test_the_host_and_bootloader_part_tables_agree);
	return mu_report();
}
