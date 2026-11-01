/*
 * Hardware-independent tests of ubertooth-dfu's main() against a faked DFU
 * transport: one device already in the bootloader, and scripted control
 * transfers.
 *
 * ubertooth-dfu.c is compiled into this test with main() renamed, and the
 * libusb and libubertooth calls it makes are replaced by the fakes below, so
 * the real command runs end to end without a board. The cases here are the
 * ones a board cannot be asked for on demand: a bootloader that stops
 * answering, and a read-back that fails part-way.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <libusb.h>

#include "../../../firmware/tests/minunit.h"

/* Stands in for libc's, the one call every wait in the tool comes down to.
 * Those waits are what the tool does while a real device changes identity on
 * the bus; the faked device changes the moment it is told to, so each one is
 * dead time here and the cases that poll a device out would spend seconds of
 * it. Defined with libc's own prototype, so it is this definition the test
 * binary links rather than a renaming the tool's own headers could trip over.
 * Nothing in these tests depends on wall-clock time passing. */
int nanosleep(const struct timespec *requested, struct timespec *remaining)
{
	(void)requested;
	(void)remaining;
	return 0;
}

#define main ubertooth_dfu_main
#include "ubertooth-dfu.c"
#undef main

/* A read-back prints one dot per 256-byte block, so a 256 KiB part is 960 of
 * them, plus the stage trace around them: more standard error than the shared
 * harness keeps room for by default. */
#define RUN_RESULT_ERR_SIZE 16384
#include "run_main.h"

/* The flash region -u reads back. Its length follows the part: these runs
 * reach the bootloader without an application to ask, so they get the size
 * the host assumes. */
#define DUMP_ADDRESS (BOOTLOADER_OFFSET + BOOTLOADER_SIZE)
#define DUMP_LENGTH_FOR(flash_size) ((flash_size) - DUMP_ADDRESS)
#define DUMP_LENGTH DUMP_LENGTH_FOR(DFU_ASSUMED_FLASH_SIZE)
#define FIRST_BLOCK (DUMP_ADDRESS / BLOCK_SIZE)
/* The part IDs of two parts the fleet holds, and one no table has. */
#define PART_ID_128K 0x25011722u  /* LPC1754 */
#define PART_ID_256K 0x25011723u  /* LPC1756 */
#define PART_ID_UNKNOWN 0x12345678u

/* What the test images say about themselves in their firmware info block. The
 * API version is the host's own, as a real image from this tree carries: an
 * image that reports another one is warned about, which one case below asks
 * for on purpose. */
#define IMAGE_REVISION "git-2671234"
#define IMAGE_NAME "bluetooth_rxtx"
#define IMAGE_API UBERTOOTH_API_VERSION

/* What the faked flash holds at an address, so a dump can be checked byte for
 * byte against the region it claims to be. */
static uint8_t flash_byte(uint32_t address)
{
	return (uint8_t)((address * 7u + 0x5au) & 0xff);
}

/* The -V answer, which libubertooth gives in every tool: faked here, so this
 * test stays about the tool's own command-line handling. The answer's own
 * cases are in test_version_report.c. */
int ubertooth_print_versions(FILE *out, const char *selector)
{
	(void)selector;
	fprintf(out, "version (test)\n");
	return 0;
}

/* ---- the faked device --------------------------------------------------- */

/* How many block writes the order of are kept. Enough for the images the
 * tests below write, which is 451 writes at most; a download of the whole
 * application region of the largest part in the family would send 1985, and
 * the recorder and assert_write_order() both stop at this bound rather than
 * run past the record. */
#define DNLOAD_RECORD_MAX 512

/* The eight words of the vector table at the start of the application region:
 * the stack pointer, the first handlers, and the checksum word over them. */
#define VECTOR_TABLE_WORDS 8
#define VECTOR_TABLE_BYTES (VECTOR_TABLE_WORDS * 4)

static struct {
	int present;            /* the DFU device is on the bus */
	uint16_t vid, pid;
	uint32_t flash_size;    /* the flash the part really has */
	int wedged;             /* it was read past: it answers nothing now */
	int app_present;        /* an application device -U can open */
	uint32_t part_id;       /* ...and the part ID it reports */
	int part_id_rc;         /* < 0: the part ID read fails with this */
	int part_id_calls;
	int state;              /* the state GETSTATE reports */
	int get_state_rc;       /* < 0: GETSTATE fails with this instead */
	int get_state_calls;
	int upload_calls;
	int upload_fail_block;  /* this block fails to read, or -1 */
	int upload_fail_rc;
	int upload_fail_once;   /* ...and then reads normally */
	int dnload_calls;
	int dnload_rc;          /* < 0: block writes fail with this */
	int dnload_fail_after;  /* ...from this many blocks written on */
	/* Every block write in the order it was sent, so the order an image is
	 * written in can be read back: the block number, and whether what was
	 * sent was a whole block of 0xFF, which is how the blank vector table a
	 * download starts with is told from the real one it ends with. Every
	 * write is recorded, whether or not this fake then refuses it, so a test
	 * can say what the tool sent rather than what reached flash. Writes past
	 * the end of the record are counted but not kept. */
	int dnload_blocks[DNLOAD_RECORD_MAX];
	int dnload_blank[DNLOAD_RECORD_MAX];
	/* The vector table each write started with, so a test can read the
	 * checksum the tool put over it. */
	uint8_t dnload_head[DNLOAD_RECORD_MAX][VECTOR_TABLE_BYTES];
	/* The region as the writes leave it: every block this fake accepted, at
	 * the offset it was written to. A test then reads the image the board was
	 * given and puts it to the checks a bootloader makes of it. Blocks past
	 * the end of this are accepted but not kept. */
	uint8_t region[DNLOAD_RECORD_MAX * BLOCK_SIZE];
	int region_blocks;      /* the highest block written, plus one */
	int getstatus_rc;       /* < 0: the status read fails with this */
	int getstatus_fail_after; /* ...from this many blocks written on */
	int detach_calls;
	int detach_rc;
	int detach_resets;      /* the bootloader acts on REQ_DETACH and resets */
	int detach_drops_off_bus; /* ...and the board then leaves USB entirely */
	int claim_rc_after_detach; /* what claiming the returned board returns */
	int open_rc_after_detach;  /* ...and what opening it returns */
	int detach_returns_to_dfu; /* ...and the bootloader runs again */
	int absent_lists;       /* this many listings find nothing on the bus */
	int list_rc;            /* < 0: a device listing fails with this */
	int list_rc_after_detach; /* ...from the reset on */
	int list_rc_lists;      /* ...for this many listings, or 0 for all */
	char revision[64];      /* what the returned application reports, if any */
	int revision_calls;
	char image[32];         /* ...and which image its info block names */
	int info_calls;
	int abort_calls, clrstatus_calls, getstatus_calls;
	int open_rc;            /* < 0: opening the device fails with this */
	int opens, closes, claims, releases, claim_rc;
	int claim_reports;            /* ubertooth_report_claim_failed() calls */
	int claim_report_err;         /* the code it was given */
	const char *claim_report_board;
	int lists;              /* outstanding device lists */
	int flash_calls;        /* cmd_flash(): the application-to-DFU request */
	int libusb_errors;      /* show_libusb_error() calls */
} fake;

static int fake_device_body;
static int fake_handle_body;
#define FAKE_DEVICE ((libusb_device *)&fake_device_body)
#define FAKE_HANDLE ((libusb_device_handle *)&fake_handle_body)

/* A board sitting in the bootloader, answering everything, on a part with the
 * 128 KiB of flash the application is linked for. There is no application to
 * ask for the part ID, as on a board entered through the expansion header. */
static void board_in_dfu(void)
{
	memset(&fake, 0, sizeof(fake));
	fake.present = 1;
	fake.vid = U1_DFU_VENDORID;
	fake.pid = U1_DFU_PRODUCTID;
	fake.flash_size = DFU_ASSUMED_FLASH_SIZE;
	fake.state = STATE_DFU_IDLE;
	fake.upload_fail_block = -1;
	fake.detach_resets = 1;
	/* The image the returned application names, which is the one the test
	 * images carry: a case that wants the other one says so. */
	snprintf(fake.image, sizeof(fake.image), "%s", IMAGE_NAME);
}

/* A board running its application, which -U opens and switches into DFU: the
 * part ID is readable there, so the read-back can follow the part. */
static void board_in_application(uint32_t part_id, uint32_t flash_size)
{
	board_in_dfu();
	fake.app_present = 1;
	fake.part_id = part_id;
	fake.flash_size = flash_size;
}

/* ---- libusb fakes ------------------------------------------------------- */
/* Defined here, so they take precedence over the libusb library symbols. */

int LIBUSB_CALL libusb_init(libusb_context **ctx)
{
	if (ctx != NULL)
		*ctx = (libusb_context *)&fake;
	return 0;
}

void LIBUSB_CALL libusb_exit(libusb_context *ctx)
{
	(void)ctx;
}

ssize_t LIBUSB_CALL libusb_get_device_list(libusb_context *ctx,
                                           libusb_device ***list)
{
	*list = calloc(2, sizeof(libusb_device *));
	if (*list == NULL)
		return LIBUSB_ERROR_NO_MEM;
	/* A listing that cannot be read at all, which is not the same as one
	 * that found nothing. It returns no list, so the caller frees nothing
	 * and this one is not counted. */
	if (fake.list_rc < 0 && fake.absent_lists == 0) {
		int rc = fake.list_rc;

		/* A case that asks for a given number of failures gets them and
		 * then a bus that reads normally again. */
		if (fake.list_rc_lists > 0 && --fake.list_rc_lists == 0)
			fake.list_rc = 0;
		free(*list);
		*list = NULL;
		return rc;
	}
	fake.lists++;
	/* A board that is off the bus while it comes up again: the listings in
	 * that gap find nothing, and the one after it finds whatever the case
	 * left on fake.vid/fake.pid. */
	if (fake.absent_lists > 0) {
		fake.absent_lists--;
		return 0;
	}
	if (!fake.present)
		return 0;
	(*list)[0] = FAKE_DEVICE;
	return 1;
}

void LIBUSB_CALL libusb_free_device_list(libusb_device **list, int unref)
{
	(void)unref;
	fake.lists--;
	free(list);
}

int LIBUSB_CALL libusb_get_device_descriptor(libusb_device *dev,
                                             struct libusb_device_descriptor *desc)
{
	mu_assert(dev == FAKE_DEVICE, "descriptor read for an unknown device");
	memset(desc, 0, sizeof(*desc));
	desc->idVendor = fake.vid;
	desc->idProduct = fake.pid;
	return 0;
}

uint8_t LIBUSB_CALL libusb_get_bus_number(libusb_device *dev)
{
	(void)dev;
	return 3;
}

int LIBUSB_CALL libusb_get_port_numbers(libusb_device *dev, uint8_t *port_numbers,
                                        int port_numbers_len)
{
	(void)dev;
	mu_assert(port_numbers_len >= 1, "no room for the port path");
	port_numbers[0] = 5;
	return 1;
}

libusb_device *LIBUSB_CALL libusb_get_device(libusb_device_handle *devh)
{
	mu_assert(devh == FAKE_HANDLE, "device asked for an unknown handle");
	return FAKE_DEVICE;
}

int LIBUSB_CALL libusb_open(libusb_device *dev, libusb_device_handle **devh)
{
	mu_assert(dev == FAKE_DEVICE, "opened an unknown device");
	/* A device that was listed and then would not open, counted before the
	 * handle exists: NO_DEVICE is the one a case asks for most, and it means
	 * the device left the bus between the listing and the open. */
	if (fake.open_rc < 0)
		return fake.open_rc;
	fake.opens++;
	*devh = FAKE_HANDLE;
	return 0;
}

void LIBUSB_CALL libusb_close(libusb_device_handle *devh)
{
	mu_assert(devh == FAKE_HANDLE, "closed an unknown handle");
	fake.closes++;
}

int LIBUSB_CALL libusb_claim_interface(libusb_device_handle *devh,
                                       int interface_number)
{
	mu_assert(devh == FAKE_HANDLE, "claimed an unknown handle");
	(void)interface_number;
	fake.claims++;
	return fake.claim_rc;
}

int LIBUSB_CALL libusb_release_interface(libusb_device_handle *devh,
                                         int interface_number)
{
	(void)devh;
	(void)interface_number;
	fake.releases++;
	return 0;
}

int LIBUSB_CALL libusb_control_transfer(libusb_device_handle *devh,
                                        uint8_t request_type, uint8_t request,
                                        uint16_t value, uint16_t index,
                                        unsigned char *data, uint16_t length,
                                        unsigned int timeout)
{
	uint32_t address, i;

	mu_assert(devh == FAKE_HANDLE, "control transfer on an unknown handle");
	mu_assert(index == 0, "control transfer named another interface");
	mu_assert(timeout > 0, "control transfer without a timeout");
	/* Once the bootloader has faulted it answers nothing at all, state reads
	 * included, until the board is power-cycled. */
	if (fake.wedged)
		return LIBUSB_ERROR_TIMEOUT;
	switch (request) {
	case REQ_GETSTATE:
		mu_assert(length == 1, "GETSTATE asked for the wrong length");
		fake.get_state_calls++;
		if (fake.get_state_rc < 0)
			return fake.get_state_rc;
		data[0] = (unsigned char)fake.state;
		return 1;
	case REQ_UPLOAD:
		mu_assert(length == BLOCK_SIZE, "UPLOAD asked for the wrong length");
		mu_assert(value >= FIRST_BLOCK,
		          "UPLOAD asked for a block inside the bootloader");
		fake.upload_calls++;
		address = (uint32_t)value * BLOCK_SIZE;
		/* A bootloader without the per-part bound -- which is what a board
		 * flashed before it carries -- checks the address against 512 KiB
		 * whatever part it runs on, so a read past the end of this part's
		 * flash is a bus fault it never returns from: it stops answering for
		 * good instead of refusing the request. That is the board the host
		 * has to avoid asking, so it is the one faked here. */
		if (address + BLOCK_SIZE > fake.flash_size) {
			fake.wedged = 1;
			return LIBUSB_ERROR_TIMEOUT;
		}
		if (fake.upload_fail_block >= 0 && value == fake.upload_fail_block) {
			if (fake.upload_fail_once)
				fake.upload_fail_block = -1;
			return fake.upload_fail_rc;
		}
		for (i = 0; i < length; i++)
			data[i] = flash_byte(address + i);
		fake.state = STATE_DFU_IDLE;
		return length;
	case REQ_DNLOAD:
		mu_assert(length == BLOCK_SIZE, "DNLOAD sent the wrong length");
		mu_assert(value >= FIRST_BLOCK,
		          "DNLOAD wrote a block inside the bootloader");
		if (fake.dnload_calls < DNLOAD_RECORD_MAX) {
			fake.dnload_blocks[fake.dnload_calls] = value;
			fake.dnload_blank[fake.dnload_calls] = 1;
			for (i = 0; i < length; i++)
				if (data[i] != 0xff)
					fake.dnload_blank[fake.dnload_calls] = 0;
			memcpy(fake.dnload_head[fake.dnload_calls], data,
			       VECTOR_TABLE_BYTES);
		}
		fake.dnload_calls++;
		/* The failure starts once that many blocks have gone through, so a
		 * test can let a row of dots build up before it. */
		if (fake.dnload_rc < 0 && fake.dnload_calls > fake.dnload_fail_after)
			return fake.dnload_rc;
		/* Written, so it is in the region now. A block written twice leaves
		 * what the second write put there, as flash does. */
		if (value - FIRST_BLOCK < DNLOAD_RECORD_MAX) {
			memcpy(fake.region + (size_t)(value - FIRST_BLOCK) * BLOCK_SIZE,
			       data, BLOCK_SIZE);
			if (value - FIRST_BLOCK >= fake.region_blocks)
				fake.region_blocks = value - FIRST_BLOCK + 1;
		}
		return length;
	case REQ_GETSTATUS:
		fake.getstatus_calls++;
		if (fake.getstatus_rc < 0 &&
		    fake.getstatus_calls > fake.getstatus_fail_after)
			return fake.getstatus_rc;
		memset(data, 0, length);
		return length;
	case REQ_ABORT:
		fake.abort_calls++;
		fake.state = STATE_DFU_IDLE;
		return 0;
	case REQ_CLRSTATUS:
		fake.clrstatus_calls++;
		fake.state = STATE_DFU_IDLE;
		return 0;
	case REQ_DETACH:
		fake.detach_calls++;
		/* A bootloader that acts on the request resets into the
		 * application: the same device comes back on the same path under
		 * the application's USB ID, so the DFU device is gone. The reset
		 * happens whatever the request itself returns, because it is
		 * answered by a device that is already going away. */
		if (fake.detach_resets) {
			fake.vid = U1_VENDORID;
			fake.pid = U1_PRODUCTID;
			/* A board that resets back into the bootloader: it is off the
			 * bus while it comes up, and then the DFU device is there again
			 * on the same path. */
			if (fake.detach_returns_to_dfu) {
				fake.vid = U1_DFU_VENDORID;
				fake.pid = U1_DFU_PRODUCTID;
				fake.absent_lists = 1;
			}
			if (fake.list_rc_after_detach != 0) {
				fake.absent_lists = 1;
				fake.list_rc = fake.list_rc_after_detach;
			}
			/* A board that does not re-enumerate is simply not on the bus
			 * any more: the DFU device has gone, which is what the detach
			 * check asks, and no application arrives in its place. */
			if (fake.detach_drops_off_bus)
				fake.present = 0;
			/* A board that comes back and will not be claimed: the reset is
			 * when that starts, since the claim before it had to succeed for
			 * the download to happen at all. */
			if (fake.claim_rc_after_detach != 0)
				fake.claim_rc = fake.claim_rc_after_detach;
			/* The same for an open that fails only once the board is back:
			 * the opens before the reset had to work for the download to
			 * happen at all. */
			if (fake.open_rc_after_detach != 0)
				fake.open_rc = fake.open_rc_after_detach;
		}
		return fake.detach_rc;
	default:
		return LIBUSB_ERROR_PIPE;
	}
}

/* ---- libubertooth fakes ------------------------------------------------- */

void show_libusb_error(int error_code)
{
	fake.libusb_errors++;
	fprintf(stderr, "error: fake (libusb %d)\n", error_code);
}

/* Stands in for the library's reporter, which the other tools share, so no
 * case here depends on libubertooth being linked. It records the call rather
 * than repeating the wording: what this file can check is that a claim
 * failure goes through the shared reporter at all, with the code it got. The
 * wording itself is pinned where the real function is compiled in, in
 * test_device_select.c. */
void ubertooth_report_claim_failed(const char* board, int err)
{
	fake.claim_reports++;
	fake.claim_report_err = err;
	fake.claim_report_board = board;
	fprintf(stderr, "<the shared claim-failure report, libusb %d>\n", err);
}

/* The application device -U opens, when the case has one. The handle is the
 * same faked one the bootloader answers on, which is what the real board does
 * from the host's point of view: one device that changes identity. */
static ubertooth_t fake_ut;

ubertooth_t *ubertooth_start_select(const char *u_arg)
{
	(void)u_arg;
	if (!fake.app_present)
		return NULL;
	memset(&fake_ut, 0, sizeof(fake_ut));
	fake_ut.devh = FAKE_HANDLE;
	return &fake_ut;
}

void ubertooth_close(ubertooth_t *ut) { (void)ut; }

void ubertooth_stop(ubertooth_t *ut) { (void)ut; }

void ubertooth_free(ubertooth_t *ut) { (void)ut; }

/* The real one reads UBERTOOTH_TEST_NO_DEVICES; these tests drive a faked bus
 * instead, so devices are never hidden here. */
int ubertooth_devices_hidden(void) { return 0; }

int cmd_flash(struct libusb_device_handle *devh)
{
	(void)devh;
	fake.flash_calls++;
	return 0;
}

/* The part ID, read from the application before it leaves for DFU. */
int cmd_get_part_id(struct libusb_device_handle *devh, u32 *part_id)
{
	(void)devh;
	fake.part_id_calls++;
	if (fake.part_id_rc < 0)
		return fake.part_id_rc;
	*part_id = fake.part_id;
	return 0;
}

/* What the application reports after the reset, for the -d ... -r
 * verification. A case that sets no revision has nothing to report, as a
 * board whose firmware will not answer. */
int cmd_get_firmware_revision(struct libusb_device_handle *devh, char *revision,
                              size_t revision_size)
{
	(void)devh;
	fake.revision_calls++;
	if (fake.revision[0] == '\0')
		return -1;
	snprintf(revision, revision_size, "%s", fake.revision);
	return (int)strlen(revision);
}

/* The info block the returned application reports, which is where the
 * verification reads the image name. A case that clears the image name has a
 * board that stalls the request, as firmware built before the block does. */
int cmd_get_firmware_info(struct libusb_device_handle *devh,
                          struct ubertooth_fw_info *info)
{
	(void)devh;
	fake.info_calls++;
	memset(info, 0, sizeof(*info));
	if (fake.image[0] == '\0')
		return LIBUSB_ERROR_PIPE;
	info->format_version = UBERTOOTH_FW_INFO_FORMAT;
	info->api_version = IMAGE_API;
	snprintf(info->image, sizeof(info->image), "%s", fake.image);
	snprintf(info->revision, sizeof(info->revision), "%s", fake.revision);
	return 0;
}

/* ---- running main() and capturing what it prints ------------------------ */

static char dump_path[512];
static char image_path[512];
/* A second image, written per case, for the sizes the flash bound is about. */
static char sized_image_path[512];

/* The capture a case puts around a call it makes itself, rather than around a
 * run of the tool. One case is running at a time, so one of these is enough.
 */
static struct run_capture capture;

static void begin_capture(void)
{
	/* mu_assert() counts a failure and carries on, which here would mean
	 * making the call with its output going to whoever is running the
	 * tests and nothing reading it back: stop instead. */
	if (run_capture_begin(&capture) < 0)
		exit(1);
}

static void end_capture(struct run_result *result)
{
	run_capture_end(&capture, result);
}

static struct run_result run_dfu(const char *const *args)
{
	struct run_result result = run_tool_main(ubertooth_dfu_main, args);

	/* Every handle opened must have been closed, and every list freed. */
	mu_assert_int_eq(fake.opens, fake.closes);
	mu_assert_int_eq(0, fake.lists);
	return result;
}

#define RUN(...) run_dfu((const char *const[]){ "ubertooth-dfu", __VA_ARGS__, NULL })

/* How many lines of a capture start with "error: ". Every failure this tool
 * reports itself is one of those, so a run that failed has at least one and a
 * run that worked has none. */
static int error_lines(const char *text)
{
	const char *line = text;
	int count = 0;

	while (line != NULL && *line != '\0') {
		if (strncmp(line, "error: ", 7) == 0)
			count++;
		line = strchr(line, '\n');
		if (line != NULL)
			line++;
	}
	return count;
}

/* The whole dump, compared with the region it came from. Returns its length,
 * or -1 when the file is not there. */
static long dump_length_and_content(int *content_ok)
{
	FILE *file = fopen(dump_path, "rb");
	unsigned char buffer[BLOCK_SIZE];
	long length = 0;
	size_t got;

	*content_ok = 0;
	if (file == NULL)
		return -1;
	*content_ok = 1;
	while ((got = fread(buffer, 1, sizeof(buffer), file)) > 0) {
		size_t i;
		for (i = 0; i < got; i++) {
			if (buffer[i] != flash_byte((uint32_t)(DUMP_ADDRESS + length + (long)i)))
				*content_ok = 0;
		}
		length += (long)got;
	}
	fclose(file);
	return length;
}

static void remove_dump(void)
{
	unlink(dump_path);
}

/* ---- an image for -d to write ------------------------------------------ */

/* Three blocks of payload, so a download sends several blocks and the suffix
 * falls into a fourth. */
#define IMAGE_PAYLOAD (3 * BLOCK_SIZE)
#define IMAGE_BLOCKS 4

/* How many block writes a download of an image of this many blocks sends: one
 * more than the image has, because the region's first block is written twice.
 * It goes out blank at the start of the transfer, so that nothing is startable
 * while the image is going in, and carries the real vector table at the end of
 * it. download() in ubertooth-dfu.c says why. */
#define DNLOADS_FOR_BLOCKS(blocks) ((blocks) + 1)

/* A signed DFU file for this device, laid out like a real image: the build
 * string at the start, and the firmware info block at
 * UBERTOOTH_FW_INFO_OFFSET, which is where -d reads the image name and the
 * revision from. payload is the firmware length before the 16-byte suffix.
 * sign() is the tool's own, so these are images the tool really accepts.
 *
 * block_format is the block's own format version, or 0 for a file with no
 * block at all: either way, one the tool must refuse before it goes near the
 * board. */
static void write_image_with(const char *path, const char *image_name,
                             const char *revision, long payload,
                             unsigned block_format, unsigned api_version)
{
	FILE *payload_file = tmpfile();
	FILE *image = fopen(path, "w+b");
	uint8_t block[UBERTOOTH_FW_INFO_LENGTH];
	uint8_t check[UBERTOOTH_IMAGE_CHECK_LENGTH];
	char marker[128];
	long i;
	int length;

	if (payload_file == NULL || image == NULL) {
		fprintf(stderr, "cannot create the test image\n");
		exit(1);
	}
	if (payload < UBERTOOTH_FW_INFO_OFFSET + UBERTOOTH_FW_INFO_LENGTH) {
		fprintf(stderr, "the test image is too short for an info block\n");
		exit(1);
	}
	length = snprintf(marker, sizeof(marker), "ubertooth %s (tests) now",
	                  revision);
	fwrite(marker, 1, (size_t)length + 1, payload_file);
	for (i = length + 1; i < payload; i++)
		fputc(0xff, payload_file);
	/* The image check block, as a build leaves it: the magic and the format
	 * version, with the flags, the length and the checksum at zero for -d to
	 * fill in. */
	memset(check, 0, sizeof(check));
	memcpy(check + UBERTOOTH_IMAGE_CHECK_OFF_MAGIC, UBERTOOTH_IMAGE_CHECK_MAGIC,
	       UBERTOOTH_IMAGE_CHECK_MAGIC_LEN);
	write_le16(check + UBERTOOTH_IMAGE_CHECK_OFF_FORMAT,
	           UBERTOOTH_IMAGE_CHECK_FORMAT);
	if (fseek(payload_file, UBERTOOTH_IMAGE_CHECK_OFFSET, SEEK_SET) != 0 ||
	    fwrite(check, 1, sizeof(check), payload_file) != sizeof(check)) {
		fprintf(stderr, "cannot write the test image check block\n");
		exit(1);
	}
	if (block_format != 0) {
		memset(block, 0, sizeof(block));
		memcpy(block + UBERTOOTH_FW_INFO_OFF_MAGIC,
		       UBERTOOTH_FW_INFO_MAGIC, UBERTOOTH_FW_INFO_MAGIC_LEN);
		block[UBERTOOTH_FW_INFO_OFF_FORMAT] = (uint8_t)(block_format & 0xff);
		block[UBERTOOTH_FW_INFO_OFF_FORMAT + 1] = (uint8_t)(block_format >> 8);
		block[UBERTOOTH_FW_INFO_OFF_API] = (uint8_t)(api_version & 0xff);
		block[UBERTOOTH_FW_INFO_OFF_API + 1] = (uint8_t)(api_version >> 8);
		memcpy(block + UBERTOOTH_FW_INFO_OFF_IMAGE, image_name,
		       strlen(image_name));
		memcpy(block + UBERTOOTH_FW_INFO_OFF_REVISION, revision,
		       strlen(revision));
		if (fseek(payload_file, UBERTOOTH_FW_INFO_OFFSET, SEEK_SET) != 0 ||
		    fwrite(block, 1, sizeof(block), payload_file) != sizeof(block)) {
			fprintf(stderr, "cannot write the test info block\n");
			exit(1);
		}
	}
	if (sign("the test payload", payload_file, path, image,
	         U1_DFU_VENDORID, U1_DFU_PRODUCTID) != 0) {
		fprintf(stderr, "cannot sign the test image\n");
		exit(1);
	}
	fclose(payload_file);
	fclose(image);
}

static void write_image_of(const char *path, const char *revision, long payload)
{
	write_image_with(path, IMAGE_NAME, revision, payload,
	                 UBERTOOTH_FW_INFO_FORMAT, IMAGE_API);
}

static void write_image(const char *revision)
{
	write_image_of(image_path, revision, IMAGE_PAYLOAD);
}

/* ---- a read-back that works, as the baseline --------------------------- */

/* A bootloader something else is already talking to opens and then refuses the
 * claim, which this reported as "usb_claim_interface error -3" followed by
 * advice about permissions -- the one thing it cannot be, since the board
 * opened. It now hands the failure to the reporter every tool shares, with the
 * code it got, so the user gets the same wording here as anywhere else.
 *
 * And nothing is sent to that board on the way out: -r asks for a reset, but
 * detaching a board this run never claimed is a reset aimed at whatever does
 * hold it, which could be another flash in progress. */
MU_TEST(a_dfu_board_in_use_goes_through_the_shared_report_and_is_left_alone)
{
	static const int refusals[] = {
		LIBUSB_ERROR_ACCESS, LIBUSB_ERROR_BUSY, LIBUSB_ERROR_NO_DEVICE
	};
	struct run_result r;
	size_t i;

	for (i = 0; i < sizeof(refusals) / sizeof(refusals[0]); i++) {
		board_in_dfu();
		remove_dump();
		fake.claim_rc = refusals[i];
		r = RUN("-u", dump_path, "-r");
		mu_assert_int_eq(1, r.rc);
		mu_assert_int_eq(1, fake.claim_reports);
		mu_assert_int_eq(refusals[i], fake.claim_report_err);
		mu_assert(strstr(r.err, "permissions") == NULL, r.err);
		mu_assert_int_eq(0, fake.detach_calls);
		/* These runs give no -U, so there is no board to name. With one, the
		 * board is named by that string; reaching the claim with a selector
		 * needs the application device this case does not put on the bus. */
		mu_assert(fake.claim_report_board == NULL, "unnamed");
	}
	remove_dump();
}

MU_TEST(a_read_back_with_reset_writes_the_dump_and_detaches)
{
	struct run_result r;
	int content_ok;

	board_in_dfu();
	remove_dump();
	r = RUN("-u", dump_path, "-r");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(DUMP_LENGTH, dump_length_and_content(&content_ok));
	mu_assert(content_ok, "the dump is not the region it read");
	mu_assert_int_eq(1, fake.detach_calls);
	mu_assert(strstr(r.err, "Detached\n") != NULL, "the detach was not reported");
	remove_dump();
}

/* ---- a bootloader that stops answering --------------------------------- */

/* The state read failing used to loop forever, one error line per attempt. */
MU_TEST(a_bootloader_that_never_reports_its_state_ends_the_read_back)
{
	struct run_result r;
	int ignored;

	board_in_dfu();
	remove_dump();
	fake.get_state_rc = LIBUSB_ERROR_TIMEOUT;
	r = RUN("-u", dump_path, "-r");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "did not report its state") != NULL,
	          "the failure does not say the state could not be read");
	mu_assert(strstr(r.err, "error: firmware upload failed after 2 attempt(s)") != NULL,
	          "the read-back failure was not reported");
	/* Bounded: the two read-back attempts and the two detach attempts ask
	 * once each, instead of until the user kills the command. */
	mu_assert_int_eq(3, fake.get_state_calls);
	mu_assert_int_eq(0, fake.upload_calls);
	mu_assert_int_eq(-1, dump_length_and_content(&ignored));
}

/* -r on its own: the same state read, reached through detach(). */
MU_TEST(a_bootloader_that_never_reports_its_state_ends_a_bare_reset)
{
	struct run_result r;

	board_in_dfu();
	fake.get_state_rc = LIBUSB_ERROR_TIMEOUT;
	r = RUN("-r");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "error: detach failed: could not enter DFU mode") != NULL,
	          "the detach failure was not reported");
	mu_assert_int_eq(0, fake.detach_calls);
	/* The detach, and cleanup's one further try with the same handle. */
	mu_assert_int_eq(2, fake.get_state_calls);
}

/* A state outside the DFU 1.1 set was polled forever too. */
MU_TEST(a_state_this_host_cannot_drive_ends_the_command)
{
	struct run_result r;

	board_in_dfu();
	fake.state = 0x7f;
	r = RUN("-r");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "not in a state this host can drive") != NULL,
	          "the unexpected state was not reported");
	mu_assert_int_eq(2, fake.get_state_calls);
	mu_assert_int_eq(0, fake.detach_calls);
}

/* A half-finished transfer is still aborted, and then the command runs. */
MU_TEST(a_half_finished_transfer_is_aborted_before_the_read_back)
{
	struct run_result r;
	int content_ok;

	board_in_dfu();
	remove_dump();
	fake.state = STATE_DFU_DNLOAD_IDLE;
	r = RUN("-u", dump_path, "-r");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(1, fake.abort_calls);
	mu_assert_int_eq(DUMP_LENGTH, dump_length_and_content(&content_ok));
	mu_assert(content_ok, "the dump is not the region it read");
	remove_dump();
}

/* ---- a read-back that fails part-way ----------------------------------- */

MU_TEST(a_read_back_failing_part_way_still_detaches_with_reset)
{
	struct run_result r;
	int ignored;

	board_in_dfu();
	remove_dump();
	fake.upload_fail_block = FIRST_BLOCK + 100;
	fake.upload_fail_rc = LIBUSB_ERROR_OTHER;  /* as the reported failure did */
	r = RUN("-u", dump_path, "-r");
	/* The libusb status is reported, but the exit status is 1, not the
	 * truncated negative code the original failure returned. */
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "error: firmware upload failed after 2 attempt(s)") != NULL,
	          "the read-back failure was not reported");
	mu_assert(strstr(r.err, "error: upload failed: block 164 was not read") != NULL,
	          "the failing block was not named");
	/* -r was given, so the board is still returned to its application. */
	mu_assert_int_eq(1, fake.detach_calls);
	/* And no partial dump is left behind. */
	mu_assert_int_eq(-1, dump_length_and_content(&ignored));
}

MU_TEST(a_read_back_failing_without_reset_leaves_the_board_in_dfu)
{
	struct run_result r;
	int ignored;

	board_in_dfu();
	remove_dump();
	fake.upload_fail_block = FIRST_BLOCK + 100;
	fake.upload_fail_rc = LIBUSB_ERROR_OTHER;
	r = RUN("-u", dump_path);
	mu_assert_int_eq(1, r.rc);
	/* Without -r nothing was asked to reset, so nothing is detached. */
	mu_assert_int_eq(0, fake.detach_calls);
	mu_assert_int_eq(-1, dump_length_and_content(&ignored));
}

/* A transient failure: one block fails, and the retry reads the whole region
 * again from its first block. */
MU_TEST(a_transient_read_back_failure_is_retried_once_and_succeeds)
{
	struct run_result r;
	int content_ok;

	board_in_dfu();
	remove_dump();
	fake.upload_fail_block = FIRST_BLOCK + 100;
	fake.upload_fail_rc = LIBUSB_ERROR_OTHER;
	fake.upload_fail_once = 1;
	r = RUN("-u", dump_path, "-r");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(DUMP_LENGTH, dump_length_and_content(&content_ok));
	mu_assert(content_ok, "the retried dump is not the region it read");
	/* 101 blocks of the first attempt, then the whole region again. */
	mu_assert_int_eq(101 + (DUMP_LENGTH / BLOCK_SIZE), fake.upload_calls);
	mu_assert(strstr(r.err, "firmware read-back attempt 2 started") != NULL,
	          "the retry was not logged");
	mu_assert_int_eq(1, fake.detach_calls);
	remove_dump();
}

/* A reset that the bootloader refuses must not be reported as success. */
MU_TEST(a_refused_detach_fails_the_command)
{
	struct run_result r;
	int content_ok;

	board_in_dfu();
	remove_dump();
	fake.detach_rc = LIBUSB_ERROR_TIMEOUT;
	/* The request never reached a bootloader that would act on it, so the
	 * board stays where it is. */
	fake.detach_resets = 0;
	r = RUN("-u", dump_path, "-r");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "error: detach failed: the board is still in DFU") != NULL,
	          "the refused detach was not reported");
	/* The dump itself arrived, so it is kept. */
	mu_assert_int_eq(DUMP_LENGTH, dump_length_and_content(&content_ok));
	mu_assert(content_ok, "the dump is not the region it read");
	/* Cleanup tries the detach once more with the handle it still has. */
	mu_assert_int_eq(2, fake.detach_calls);
	remove_dump();
}

/* ---- a detach whose request fails because the board reset -------------- */

/* Flash the image and reset, with the detach request failing the way it does
 * on a board that resets while it is in flight. The board comes back on the
 * new firmware, as it does on every macOS flash. */
static struct run_result flash_with_detach_error(int detach_rc)
{
	board_in_dfu();
	fake.detach_rc = detach_rc;
	snprintf(fake.revision, sizeof(fake.revision), "%s", IMAGE_REVISION);
	return RUN("-d", image_path, "-r");
}

/* The "Image: ..." line a test image produces, built from the host's API
 * version rather than pinned to a number that goes stale on every bump. */
static const char *expected_image_line(void)
{
	static char line[128];

	snprintf(line, sizeof(line), "Image: %s %s (API:%x.%02x)\n", IMAGE_NAME,
	         IMAGE_REVISION, (IMAGE_API >> 8) & 0xFF, IMAGE_API & 0xFF);
	return line;
}

static void assert_flash_passed(struct run_result *r)
{
	mu_assert_int_eq(0, r->rc);
	mu_assert_int_eq(DNLOADS_FOR_BLOCKS(IMAGE_BLOCKS), fake.dnload_calls);
	mu_assert(strstr(r->err, "Detached\n") != NULL,
	          "the detach was not reported");
	mu_assert(strstr(r->out, "Verified firmware revision: " IMAGE_REVISION)
	          != NULL, "the flashed revision was not verified");
	/* Read out of the image's firmware info block, not searched for, and a
	 * note about the run, so on standard error. */
	mu_assert(strstr(r->err, expected_image_line()) != NULL,
	          "the image and revision the file carries were not named");
	/* The image carries the host's own API version, so that is not warned
	 * about. */
	mu_assert(strstr(r->err, "API version") == NULL,
	          "a matching image was warned about");
	mu_assert(strstr(r->out, "Image: ") == NULL,
	          "the image line took standard output");
	mu_assert(strstr(r->err, "still in DFU") == NULL,
	          "a board that left DFU was reported as still in it");
	/* The board is out of the bootloader, so cleanup does not detach it a
	 * second time: that second attempt is the -99 of the issue. */
	mu_assert_int_eq(1, fake.detach_calls);
}

/* The macOS case of the issue: LIBUSB_ERROR_PIPE on every flash that worked. */
MU_TEST(a_detach_that_fails_with_pipe_as_the_board_resets_passes)
{
	struct run_result r = flash_with_detach_error(LIBUSB_ERROR_PIPE);

	assert_flash_passed(&r);
}

/* The same race seen as the device disappearing. */
MU_TEST(a_detach_that_fails_with_no_device_as_the_board_resets_passes)
{
	struct run_result r = flash_with_detach_error(LIBUSB_ERROR_NO_DEVICE);

	assert_flash_passed(&r);
}

/* And as a transfer that was cut short. */
MU_TEST(a_detach_that_fails_with_io_as_the_board_resets_passes)
{
	struct run_result r = flash_with_detach_error(LIBUSB_ERROR_IO);

	assert_flash_passed(&r);
}

/* No status is read as a verdict, so a code the host has no name for is no
 * different: the macOS backend returns LIBUSB_ERROR_OTHER for the IOKit
 * results it does not map, and a board that had already gone has been seen
 * reported that way. */
MU_TEST(a_detach_that_fails_with_an_unnamed_error_as_the_board_resets_passes)
{
	struct run_result r = flash_with_detach_error(LIBUSB_ERROR_OTHER);

	assert_flash_passed(&r);
}

/* The same failed request with the DFU device still on its path afterwards is
 * a real failure: the board never left the bootloader, so nothing is verified
 * and the run fails. This is the case the message belongs to. */
MU_TEST(a_failed_detach_with_the_dfu_device_still_there_fails_the_command)
{
	struct run_result r;

	board_in_dfu();
	fake.detach_rc = LIBUSB_ERROR_PIPE;
	fake.detach_resets = 0;
	snprintf(fake.revision, sizeof(fake.revision), "%s", IMAGE_REVISION);
	r = RUN("-d", image_path, "-r");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "error: detach failed: the board is still in DFU") != NULL,
	          "a board left in DFU was not reported");
	mu_assert(strstr(r.out, "Verified firmware revision") == NULL,
	          "a board still in DFU was reported as verified");
	mu_assert_int_eq(0, fake.revision_calls);
	/* Cleanup still tries once more to get it out of the bootloader. */
	mu_assert_int_eq(2, fake.detach_calls);
	/* The device never went, so the request's own status is evidence after
	 * all and both attempts name it. This is the half of that rule the
	 * cases above do not cover. */
	mu_assert_int_eq(2, fake.libusb_errors);
}

/* With no -d there is no verification to fall back on, so the DFU device
 * being gone is the whole answer. */
MU_TEST(a_bare_reset_whose_detach_races_the_reset_passes)
{
	struct run_result r;

	board_in_dfu();
	fake.detach_rc = LIBUSB_ERROR_PIPE;
	r = RUN("-r");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(1, fake.detach_calls);
	mu_assert(strstr(r.err, "Detached\n") != NULL,
	          "the detach was not reported");
}

/* A detach with no path to look at cannot answer the question either way, so
 * it says that rather than calling the board detached. Driven directly: both
 * callers pass the path the command bound itself to, so no run gets here. */
MU_TEST(a_detach_with_no_usb_path_says_it_could_not_tell)
{
	struct dfu_runtime_context runtime;
	struct dfu_usb_identity nowhere;
	struct run_result r;

	board_in_dfu();
	fake.detach_rc = LIBUSB_ERROR_PIPE;
	memset(&runtime, 0, sizeof(runtime));
	memset(&nowhere, 0, sizeof(nowhere));
	memset(&r, 0, sizeof(r));
	begin_capture();
	r.rc = detach(&runtime, FAKE_HANDLE, &nowhere);
	end_capture(&r);
	mu_assert_int_eq(LIBUSB_ERROR_PIPE, r.rc);
	mu_assert(strstr(r.err, "could not tell whether the board left DFU")
	          != NULL, "the unanswerable detach was not reported");
	mu_assert(strstr(r.err, "Detached\n") == NULL,
	          "a board that was not looked for was called detached");
	mu_assert_int_eq(1, fake.detach_calls);
}

/* ---- a detach the bootloader accepts and ignores ----------------------- */

/* The request completing says nothing either: the board is detached only if
 * its DFU device has gone. A bare reset has no verification behind it, so
 * this used to be the case that exited 0 with the board still in the
 * bootloader. */
MU_TEST(a_detach_that_is_accepted_and_ignored_fails_a_bare_reset)
{
	struct run_result r;

	board_in_dfu();
	fake.detach_resets = 0;
	r = RUN("-r");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "error: detach failed: the board is still in DFU") != NULL,
	          "a board left in DFU was not reported");
	mu_assert(strstr(r.err, "Detached\n") == NULL,
	          "a board still in DFU was reported as detached");
	/* Nothing failed, so nothing names a libusb status. */
	mu_assert_int_eq(0, fake.libusb_errors);
	/* The detach, and cleanup's one further try. */
	mu_assert_int_eq(2, fake.detach_calls);
}

/* The same under -u: the dump arrived, so it is kept, but the command still
 * fails rather than reporting a board it left in the bootloader as reset. */
MU_TEST(a_detach_that_is_accepted_and_ignored_fails_a_read_back)
{
	struct run_result r;
	int content_ok;

	board_in_dfu();
	remove_dump();
	fake.detach_resets = 0;
	r = RUN("-u", dump_path, "-r");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "error: detach failed: the board is still in DFU") != NULL,
	          "a board left in DFU was not reported");
	mu_assert_int_eq(DUMP_LENGTH, dump_length_and_content(&content_ok));
	mu_assert(content_ok, "the dump is not the region it read");
	mu_assert_int_eq(2, fake.detach_calls);
	remove_dump();
}

/* With -d the verification would have caught it, but the detach now does, so
 * the failure names what is wrong instead of the revision that never came. */
MU_TEST(a_detach_that_is_accepted_and_ignored_fails_before_verification)
{
	struct run_result r;

	board_in_dfu();
	fake.detach_resets = 0;
	snprintf(fake.revision, sizeof(fake.revision), "%s", IMAGE_REVISION);
	r = RUN("-d", image_path, "-r");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(DNLOADS_FOR_BLOCKS(IMAGE_BLOCKS), fake.dnload_calls);
	mu_assert(strstr(r.err, "error: detach failed: the board is still in DFU") != NULL,
	          "a board left in DFU was not reported");
	mu_assert(strstr(r.out, "Verified firmware revision") == NULL,
	          "a board still in DFU was reported as verified");
	mu_assert_int_eq(0, fake.revision_calls);
}

/* An accepted request with no path to look at cannot be settled either, so it
 * says so rather than calling the board detached. Driven directly, as the
 * failed-request case above is. The failure it returns is the one the
 * accepted path has to invent, since the request itself carried no status. */
MU_TEST(an_accepted_detach_with_no_usb_path_says_it_could_not_tell)
{
	struct dfu_runtime_context runtime;
	struct dfu_usb_identity nowhere;
	struct run_result r;

	board_in_dfu();
	fake.detach_resets = 0;
	memset(&runtime, 0, sizeof(runtime));
	memset(&nowhere, 0, sizeof(nowhere));
	memset(&r, 0, sizeof(r));
	begin_capture();
	r.rc = detach(&runtime, FAKE_HANDLE, &nowhere);
	end_capture(&r);
	/* The probe's own reason, since the request carried no status. */
	mu_assert_int_eq(LIBUSB_ERROR_INVALID_PARAM, r.rc);
	mu_assert(strstr(r.err, "could not tell whether the board left DFU")
	          != NULL, "the unanswerable detach was not reported");
	mu_assert(strstr(r.err, "Detached\n") == NULL,
	          "a board that was not looked for was called detached");
	mu_assert_int_eq(1, fake.detach_calls);
}

/* The verification still decides: a board that comes back on the firmware it
 * already had fails the run, failed detach request or not. */
MU_TEST(a_detach_race_followed_by_the_old_firmware_fails_the_command)
{
	struct run_result r;

	board_in_dfu();
	fake.detach_rc = LIBUSB_ERROR_PIPE;
	snprintf(fake.revision, sizeof(fake.revision), "git-0000000");
	r = RUN("-d", image_path, "-r");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "error: the board came back running git-0000000, "
	                 "not " IMAGE_REVISION ": the firmware was not written, "
	                 "so flash it again\n") != NULL,
	          "the unchanged revision was not reported");
	mu_assert_int_eq(1, fake.revision_calls);
}

/* ---- a board that resets straight back into the bootloader -------------- */

/* The reset happened, and the board is in the bootloader again. One listing
 * cannot tell this from a board that left, because the bootloader is absent
 * while it comes up either way; the confirming listing can, and a bare reset
 * has nothing else behind it. */
MU_TEST(a_board_that_comes_back_in_the_bootloader_fails_a_bare_reset)
{
	struct run_result r;

	board_in_dfu();
	fake.detach_returns_to_dfu = 1;
	r = RUN("-r");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "error: detach failed: the board reset and came "
	                 "back in the bootloader\n") != NULL,
	          "the board back in the bootloader was not reported");
	/* The causes any board can be in, before the one that needs the header:
	 * a user who never fitted a bridge has to be told something true. */
	mu_assert(strstr(r.err, "note: the bootloader runs instead of the "
	                 "application when something asks the board into DFU "
	                 "again, or when its bootloader entry pin reads grounded "
	                 "as the board resets\n") != NULL,
	          "the general causes were not named");
	mu_assert(strstr(r.err, "note: if the expansion-header bridge is still "
	                 "fitted, remove it (pins 1 and 3 of P4 on an Ubertooth "
	                 "One), then unplug the board and plug it back in\n")
	          != NULL, "the bridge was not suggested");
	/* Nothing was written, so nothing claims it was. */
	mu_assert(strstr(r.err, "the firmware was written") == NULL,
	          "a bare reset claimed to have written firmware");
	mu_assert(strstr(r.err, "Detached\n") == NULL,
	          "a board back in the bootloader was reported as detached");
	mu_assert(strstr(r.err, "still in DFU") == NULL,
	          "a board that did reset was reported as never having left");
	/* The general causes come before the bridge. */
	mu_assert(strstr(r.err, "something asks the board into DFU") <
	          strstr(r.err, "expansion-header bridge"),
	          "the bridge was suggested before the general causes");
	mu_assert_int_eq(0, (int)strlen(r.out));
	/* The board reset once and came back, so cleanup does not reset it
	 * again: that would land in the same place and say so in codes rather
	 * than words. One error line, one diagnosis. */
	mu_assert_int_eq(1, error_lines(r.err));
	mu_assert_int_eq(1, fake.detach_calls);
}

/* The same under -u: the dump is kept, and the run still fails. */
MU_TEST(a_board_that_comes_back_in_the_bootloader_fails_a_read_back)
{
	struct run_result r;
	int content_ok;

	board_in_dfu();
	remove_dump();
	fake.detach_returns_to_dfu = 1;
	r = RUN("-u", dump_path, "-r");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "came back in the bootloader") != NULL,
	          "the board back in the bootloader was not reported");
	mu_assert_int_eq(DUMP_LENGTH, dump_length_and_content(&content_ok));
	mu_assert(content_ok, "the dump is not the region it read");
	remove_dump();
}

/* A flash whose board comes back in the bootloader fails at the detach, so
 * the message says where the board is rather than waiting out the
 * verification to report an application that was never coming. */
MU_TEST(a_board_that_comes_back_in_the_bootloader_fails_before_verification)
{
	struct run_result r;

	board_in_dfu();
	fake.detach_returns_to_dfu = 1;
	snprintf(fake.revision, sizeof(fake.revision), "%s", IMAGE_REVISION);
	r = RUN("-d", image_path, "-r");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(DNLOADS_FOR_BLOCKS(IMAGE_BLOCKS), fake.dnload_calls);
	mu_assert(strstr(r.err, "came back in the bootloader") != NULL,
	          "the board back in the bootloader was not reported");
	/* The image is in the flash, so the line that would otherwise send the
	 * reader back to write it again says so first. */
	mu_assert(strstr(r.err, "note: the firmware was written, so the board "
	                 "holds it; it is in the bootloader rather than running "
	                 "it\n") != NULL,
	          "the completed write was not reported");
	mu_assert(strstr(r.err, "the firmware was written") <
	          strstr(r.err, "the bootloader runs instead"),
	          "the causes came before what is known about the flash");
	mu_assert(strstr(r.out, "Verified firmware revision") == NULL,
	          "a board in the bootloader was reported as verified");
	mu_assert_int_eq(0, fake.revision_calls);
}

/* The confirming listing is what settles the detach, so a bus that cannot be
 * listed after the device went leaves it unsettled. It is retried a few times
 * first, and then said rather than guessed either way. */
MU_TEST(a_confirming_listing_that_cannot_be_read_says_it_could_not_tell)
{
	struct run_result r;

	board_in_dfu();
	fake.list_rc_after_detach = LIBUSB_ERROR_NO_MEM;
	fake.list_rc_lists = 0; /* every listing after the reset */
	r = RUN("-r");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "error: detach failed: could not tell whether "
	                 "the board left DFU (usb_error=-11)\n") != NULL,
	          "the unanswerable confirming listing was not reported");
	mu_assert(strstr(r.err, "Detached\n") == NULL,
	          "an unsettled detach was reported as detached");
	mu_assert(strstr(r.err, "error: detach failed: the board reset and came "
	                 "back") == NULL,
	          "an unsettled detach was reported as a board in the bootloader");
	/* The device had gone before the confirming listings failed, so the
	 * handle is stale: cleanup does not reset the board again through it,
	 * and the failure is reported once. */
	mu_assert_int_eq(1, fake.detach_calls);
	mu_assert_int_eq(1, error_lines(r.err));
	/* The gap was waited, so the trace says why the detach took that long,
	 * as a whole line: it is built into a buffer, and a line cut short mid
	 * word is how that buffer being too small shows up. */
	mu_assert(strstr(r.err, "the DFU device has gone: looking again in 1500 "
	                 "ms, in case the board came back in the bootloader\n")
	          != NULL, "the confirming wait is not in the stage trace");
}

/* A confirming listing that could not be read leaves the detach unsettled,
 * and the board had still gone, so a request that failed as it reset says
 * nothing about that either: no libusb status in front of this failure. */
MU_TEST(a_board_that_went_names_no_libusb_status_when_the_bus_will_not_read)
{
	struct run_result r;

	board_in_dfu();
	fake.detach_rc = LIBUSB_ERROR_PIPE;
	fake.list_rc_after_detach = LIBUSB_ERROR_NO_MEM;
	r = RUN("-r");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "could not tell whether the board left DFU")
	          != NULL, "the unsettled detach was not reported");
	mu_assert_int_eq(0, fake.libusb_errors);
	/* The device had gone, so the handle is stale and cleanup does not
	 * reset the board again through it. */
	mu_assert_int_eq(1, fake.detach_calls);
	mu_assert_int_eq(1, error_lines(r.err));
}

/* One confirming listing that cannot be read is not the answer either: it is
 * tried again, and the next one settles it. */
MU_TEST(one_unreadable_confirming_listing_is_tried_again)
{
	struct run_result r;

	board_in_dfu();
	fake.detach_returns_to_dfu = 1;
	fake.list_rc_after_detach = LIBUSB_ERROR_NO_MEM;
	fake.list_rc_lists = 1;
	r = RUN("-r");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "error: detach failed: the board reset and came "
	                 "back in the bootloader\n") != NULL,
	          "the retried confirming listing did not settle the detach");
	mu_assert(strstr(r.err, "could not tell") == NULL,
	          "one unreadable listing decided the detach");
}

/* The detach request failing while the board resets is what a reset that
 * worked looks like on macOS, so a board that came back in the bootloader
 * must not have a libusb status put in front of its failure. */
MU_TEST(a_board_back_in_the_bootloader_names_no_libusb_status)
{
	struct run_result r;

	board_in_dfu();
	fake.detach_returns_to_dfu = 1;
	fake.detach_rc = LIBUSB_ERROR_PIPE;
	r = RUN("-r");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "error: detach failed: the board reset and came "
	                 "back in the bootloader\n") != NULL,
	          "the board back in the bootloader was not reported");
	mu_assert_int_eq(0, fake.libusb_errors);
	mu_assert_int_eq(1, error_lines(r.err));
}

/* ---- what the image says about itself ---------------------------------- */

/* An image built for another API version leaves a board every other tool
 * refuses, so it is warned about. It is still written: this command is the
 * way back from such a board, so it has to be able to write any valid image.
 */
MU_TEST(an_image_for_another_api_version_is_warned_about_and_written)
{
	struct run_result r;

	board_in_dfu();
	write_image_with(sized_image_path, IMAGE_NAME, IMAGE_REVISION,
	                 IMAGE_PAYLOAD, UBERTOOTH_FW_INFO_FORMAT, 0x0107);
	snprintf(fake.revision, sizeof(fake.revision), "%s", IMAGE_REVISION);
	r = RUN("-d", sized_image_path, "-r");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strstr(r.err, "warning: ") != NULL,
	          "the API version mismatch was not warned about");
	mu_assert(strstr(r.err, "this image reports API version 1.07 in its "
	                 "firmware info block, and these tools report ") != NULL,
	          "the warning does not name both API versions");
	mu_assert(strstr(r.err, "have to report the same one") != NULL,
	          "the warning does not say the two have to match");
	/* What the file says, not what the other tools will make of the board:
	 * they read the USB bcdDevice, which is a different field. */
	mu_assert(strstr(r.err, "will refuse") == NULL,
	          "the warning promises what the other tools will do");
	/* A warning, not a refusal: the image is written and verified. */
	mu_assert_int_eq(DNLOADS_FOR_BLOCKS(IMAGE_BLOCKS), fake.dnload_calls);
	mu_assert(strstr(r.out, "Verified firmware revision: " IMAGE_REVISION)
	          != NULL, "the image was not written after the warning");
	mu_assert(strstr(r.err, "error: ") == NULL,
	          "the warning was also reported as an error");
}

/* ---- which of the post-reset outcomes it was ---------------------------- */

/* Both images of this tree carry the same revision, so a write of one over
 * the other that did not take passes a revision check. The image name is
 * what catches it. */
MU_TEST(a_board_running_the_other_image_fails_the_verification)
{
	struct run_result r;

	board_in_dfu();
	snprintf(fake.revision, sizeof(fake.revision), "%s", IMAGE_REVISION);
	snprintf(fake.image, sizeof(fake.image), "btbr");
	r = RUN("-d", image_path, "-r");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "error: the board came back running the btbr "
	                 "firmware, not the " IMAGE_NAME " image that was "
	                 "written: the firmware was not written, so flash it "
	                 "again\n") != NULL,
	          "the other image was not reported as a failed write");
	mu_assert(strstr(r.out, "Verified firmware revision") == NULL,
	          "a board running the other image was reported as verified");
	mu_assert_int_eq(1, fake.info_calls);
	mu_assert_int_eq(1, error_lines(r.err));
	mu_assert_int_eq(0, (int)strlen(r.out));
}

/* A board that answers its revision but not its info block leaves the image
 * unknown. That is an unchecked flash, not a failed one. */
MU_TEST(a_board_that_will_not_name_its_image_fails_the_verification)
{
	struct run_result r;

	board_in_dfu();
	snprintf(fake.revision, sizeof(fake.revision), "%s", IMAGE_REVISION);
	fake.image[0] = '\0';
	r = RUN("-d", image_path, "-r");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "error: the board came back running "
	                 IMAGE_REVISION ", the revision that was written, but "
	                 "would not say which firmware image it is, so which of "
	                 "this tree's images it runs is unchecked\n") != NULL,
	          "the unreadable image was not reported as unchecked");
	mu_assert(strstr(r.err, "not written") == NULL,
	          "the line claims the write did not take");
	/* The faked board stalls the request, which is the one failure the
	 * library keeps quiet about, so this line is the only error. Its other
	 * failures report themselves first. */
	mu_assert_int_eq(1, error_lines(r.err));
}

/* The board took the whole image and then left the bus, so its flash holds
 * the new firmware and nothing came back to say so. The line has to tell that
 * apart from a write that did not take: flashing again is what this board
 * does not need. */
MU_TEST(a_board_that_does_not_come_back_says_the_firmware_was_written)
{
	struct run_result r;

	board_in_dfu();
	fake.detach_drops_off_bus = 1;
	r = RUN("-d", image_path, "-r");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "Detached\n") != NULL,
	          "the board was not reported as having left the bootloader");
	mu_assert(strstr(r.err, "error: the firmware was written, but the board "
	                 "did not come back on USB: power-cycle it and read its "
	                 "revision with ubertooth-util -V before flashing it "
	                 "again\n") != NULL,
	          "the board that stayed away was reported as a failed write");
	/* The raw codes are still there for anyone debugging the tool, in the
	 * stage trace rather than in the line the user reads. */
	mu_assert(strstr(r.err, "post-reset application verification failed: "
	                 "result=-2 polls=20 expected_image=" IMAGE_NAME
	                 " expected_revision=" IMAGE_REVISION
	                 " actual_image= actual_revision= usb_error=") != NULL,
	          "the stage trace lost the verification's own result");
	mu_assert_int_eq(1, error_lines(r.err));
	mu_assert_int_eq(0, (int)strlen(r.out));
	/* Nothing answered, so the revision was never asked for. */
	mu_assert_int_eq(0, fake.revision_calls);
}

/* The search for the returned application tells its three outcomes apart,
 * because the line the user reads differs for each: a board that is not
 * there, a bus that could not be read at all, and a device that is there and
 * would not open. The first two used to come back as the third. */
MU_TEST(the_search_for_the_returned_application_tells_its_outcomes_apart)
{
	struct dfu_usb_identity identity;
	libusb_device_handle *devh = FAKE_HANDLE;
	int bus_error = -1;

	board_in_dfu();
	/* The application device on the path the flashed board was on. */
	fake.vid = U1_VENDORID;
	fake.pid = U1_PRODUCTID;
	mu_assert_int_eq(0, get_usb_identity(FAKE_DEVICE, &identity));

	mu_assert_int_eq(1, find_ubertooth_application_device(
		(libusb_context *)&fake, &identity, &devh, &bus_error));
	mu_assert(devh == FAKE_HANDLE, "the opened handle was not returned");
	mu_assert_int_eq(0, bus_error);

	/* Nothing on the bus: absent, and the caller polls again. */
	fake.present = 0;
	mu_assert_int_eq(0, find_ubertooth_application_device(
		(libusb_context *)&fake, &identity, &devh, &bus_error));
	mu_assert(devh == NULL, "a search that found nothing returned a handle");
	mu_assert_int_eq(0, bus_error);

	/* The bus itself could not be read, so no device was looked at: its own
	 * outcome, not a device that refused to open. The libusb code comes back
	 * separately, for the stage trace. */
	fake.present = 1;
	fake.list_rc = LIBUSB_ERROR_NO_MEM;
	mu_assert_int_eq(DFU_OPEN_BUS_UNREADABLE,
	                 find_ubertooth_application_device(
		(libusb_context *)&fake, &identity, &devh, &bus_error));
	mu_assert(devh == NULL, "an unreadable bus returned a handle");
	mu_assert_int_eq(LIBUSB_ERROR_NO_MEM, bus_error);

	/* Listed, then gone by the time it was opened: an absent board, which
	 * is what the caller polls for, and not one that would not open. */
	fake.list_rc = 0;
	fake.open_rc = LIBUSB_ERROR_NO_DEVICE;
	mu_assert_int_eq(0, find_ubertooth_application_device(
		(libusb_context *)&fake, &identity, &devh, &bus_error));
	mu_assert_int_eq(0, bus_error);

	/* A device that is there and will not open is reported as itself. */
	fake.open_rc = LIBUSB_ERROR_ACCESS;
	mu_assert_int_eq(LIBUSB_ERROR_ACCESS,
	                 find_ubertooth_application_device(
		(libusb_context *)&fake, &identity, &devh, &bus_error));
	mu_assert_int_eq(0, bus_error);
	mu_assert_int_eq(0, fake.lists);
}

/* The same three outcomes, end to end: a board that keeps leaving the bus
 * between the listing and the open is a board that did not come back, so the
 * line must not blame something holding its interface. */
MU_TEST(a_board_that_goes_between_the_listing_and_the_open_is_reported_as_absent)
{
	struct run_result r;

	board_in_dfu();
	fake.open_rc_after_detach = LIBUSB_ERROR_NO_DEVICE;
	r = RUN("-d", image_path, "-r");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "error: the firmware was written, but the board "
	                 "did not come back on USB: power-cycle it and read its "
	                 "revision with ubertooth-util -V before flashing it "
	                 "again\n") != NULL,
	          "a board that was gone by the open was not reported as absent");
	mu_assert(strstr(r.err, "would not open") == NULL,
	          "a board that had gone was reported as one that would not open");
	mu_assert_int_eq(1, error_lines(r.err));
	mu_assert_int_eq(0, (int)strlen(r.out));
}

/* The board came back, and the interface would not be claimed: something else
 * is using it, or this user may not have it. The write still completed, but
 * the board is there, so neither "it did not come back" nor a power cycle
 * would be true of it. */
MU_TEST(a_board_that_will_not_be_claimed_is_not_reported_as_missing)
{
	struct run_result r;

	board_in_dfu();
	fake.claim_rc_after_detach = LIBUSB_ERROR_BUSY;
	r = RUN("-d", image_path, "-r");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "error: the firmware was written, but the board "
	                 "came back and would not open, so the flash was not "
	                 "checked: read its revision with ubertooth-util -V once "
	                 "nothing else is using it\n")
	          != NULL,
	          "the board that refused to open was not reported as such");
	mu_assert(strstr(r.err, "did not come back on USB") == NULL,
	          "a board that is on the bus was reported as missing");
	mu_assert(strstr(r.err, "post-reset application verification failed: "
	                 "result=-2 polls=20 expected_image=" IMAGE_NAME
	                 " expected_revision=" IMAGE_REVISION
	                 " actual_image= actual_revision= usb_error=-6") != NULL,
	          "the stage trace lost the error that stopped the open");
	mu_assert_int_eq(1, error_lines(r.err));
	mu_assert_int_eq(0, (int)strlen(r.out));
	mu_assert_int_eq(0, fake.revision_calls);
}

/* The board came back and would not say what it runs, which is not the same
 * as a board running the wrong firmware: the flash is unchecked, not known to
 * have failed. */
MU_TEST(a_board_that_will_not_report_its_revision_says_the_flash_is_unchecked)
{
	struct run_result r;

	/* Leaves the revision empty, which is a board whose firmware does not
	 * answer the request for it. */
	board_in_dfu();
	r = RUN("-d", image_path, "-r");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "error: the board came back but would not report "
	                 "its firmware revision, so the flash was not checked: "
	                 "read it with ubertooth-util -V\n") != NULL,
	          "the board that would not answer was not reported as such");
	mu_assert(strstr(r.err, "post-reset application verification failed: "
	                 "result=-4 polls=1") != NULL,
	          "the stage trace lost the verification's own result");
	mu_assert_int_eq(1, error_lines(r.err));
	mu_assert_int_eq(0, (int)strlen(r.out));
	mu_assert_int_eq(1, fake.revision_calls);
}

/* ---- reading only the flash the part has ------------------------------- */

/* The failure itself, with the length the host used to ask for: 240 KiB from
 * a 128 KiB part faults the bootloader, and nothing answers afterwards. No
 * run of the command can produce this any more, so it is driven directly. */
MU_TEST(asking_past_the_end_of_flash_takes_the_bootloader_down)
{
	struct run_result r;
	FILE *dump = tmpfile();

	board_in_dfu();
	if (dump == NULL) {
		fprintf(stderr, "tmpfile failed\n");
		exit(1);
	}
	memset(&r, 0, sizeof(r));
	begin_capture();
	r.rc = upload(FAKE_HANDLE, dump, (256 * 1024) - DUMP_ADDRESS);
	end_capture(&r);
	mu_assert_int_eq(LIBUSB_ERROR_TIMEOUT, r.rc);
	mu_assert(fake.wedged, "the bootloader was read past without faulting");
	mu_assert(strstr(r.err, "block 512 was not read") != NULL,
	          "the block at 0x20000 was not named");
	/* As in the original failure: the state read stops answering too. */
	begin_capture();
	mu_assert_int_eq(LIBUSB_ERROR_TIMEOUT, dfu_get_state(FAKE_HANDLE));
	end_capture(&r);
	fclose(dump);
}

/* A board already in DFU cannot be asked for its part, so the read-back
 * covers only the flash the application is linked for, and says so. */
MU_TEST(a_board_already_in_dfu_reads_the_assumed_flash_only)
{
	struct run_result r;
	int content_ok;

	board_in_dfu();
	remove_dump();
	r = RUN("-u", dump_path, "-r");
	mu_assert_int_eq(0, r.rc);
	mu_assert(!fake.wedged, "the bootloader was read past the end of flash");
	mu_assert_int_eq(DUMP_LENGTH, dump_length_and_content(&content_ok));
	mu_assert(content_ok, "the dump is not the region it read");
	mu_assert_int_eq(DUMP_LENGTH / BLOCK_SIZE, fake.upload_calls);
	mu_assert(strstr(r.err, "Reading back 114688 bytes, 0x4000-0x1ffff: ")
	          != NULL, "the read-back did not say what it reads");
	mu_assert(strstr(r.err, "the part could not be read") != NULL,
	          "the read-back did not say why it assumed a size");
	mu_assert_int_eq(0, fake.part_id_calls);
	remove_dump();
}

/* With -U the application is opened first, so the part ID settles the size.
 * On a 128 KiB part that is the same region as the old fixed length asked
 * for, without the fault. */
MU_TEST(a_128_kib_part_is_read_to_the_end_of_its_flash)
{
	struct run_result r;
	int content_ok;

	board_in_application(PART_ID_128K, 128 * 1024);
	remove_dump();
	r = RUN("-U", "0", "-u", dump_path, "-r");
	mu_assert_int_eq(0, r.rc);
	mu_assert(!fake.wedged, "the bootloader was read past the end of flash");
	mu_assert_int_eq(1, fake.part_id_calls);
	mu_assert_int_eq(DUMP_LENGTH_FOR(128 * 1024),
	                 dump_length_and_content(&content_ok));
	mu_assert(content_ok, "the dump is not the region it read");
	mu_assert(strstr(r.err,
	                 "Reading back 114688 bytes, 0x4000-0x1ffff: part "
	                 "0x25011722 (LPC1754) has 128 KiB of flash") != NULL,
	          "the part and its flash were not reported");
	remove_dump();
}

/* And a part with more flash gives the longer dump the tool used to assume
 * for every board. */
MU_TEST(a_256_kib_part_is_read_in_full)
{
	struct run_result r;
	int content_ok;

	board_in_application(PART_ID_256K, 256 * 1024);
	remove_dump();
	r = RUN("-U", "0", "-u", dump_path, "-r");
	mu_assert_int_eq(0, r.rc);
	mu_assert(!fake.wedged, "the bootloader was read past the end of flash");
	mu_assert_int_eq(DUMP_LENGTH_FOR(256 * 1024),
	                 dump_length_and_content(&content_ok));
	mu_assert(content_ok, "the dump is not the region it read");
	mu_assert_int_eq(DUMP_LENGTH_FOR(256 * 1024) / BLOCK_SIZE,
	                 fake.upload_calls);
	mu_assert(strstr(r.err,
	                 "Reading back 245760 bytes, 0x4000-0x3ffff: part "
	                 "0x25011723 (LPC1756) has 256 KiB of flash") != NULL,
	          "the part and its flash were not reported");
	remove_dump();
}

/* A part ID this host has no entry for falls back to the assumed size, even
 * on a part with more flash: a figure that is not known is not guessed. */
MU_TEST(an_unknown_part_id_falls_back_to_the_assumed_flash)
{
	struct run_result r;
	int content_ok;

	board_in_application(PART_ID_UNKNOWN, 256 * 1024);
	remove_dump();
	r = RUN("-U", "0", "-u", dump_path, "-r");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(DUMP_LENGTH, dump_length_and_content(&content_ok));
	mu_assert(content_ok, "the dump is not the region it read");
	mu_assert(strstr(r.err, "part 0x12345678 is not known here") != NULL,
	          "the unknown part ID was not reported");
	remove_dump();
}

/* A part ID that will not read does not fail the command either. */
MU_TEST(a_part_id_that_cannot_be_read_falls_back_to_the_assumed_flash)
{
	struct run_result r;
	int content_ok;

	board_in_application(PART_ID_256K, 256 * 1024);
	fake.part_id_rc = LIBUSB_ERROR_TIMEOUT;
	remove_dump();
	r = RUN("-U", "0", "-u", dump_path, "-r");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(DUMP_LENGTH, dump_length_and_content(&content_ok));
	mu_assert(content_ok, "the dump is not the region it read");
	mu_assert(strstr(r.err, "the part could not be read") != NULL,
	          "the unreadable part ID was not reported");
	remove_dump();
}

/* ---- the order an image is written in ---------------------------------- */

/* The region's first block holds the vector table the board starts from, so
 * it goes out blank first and real last: in between a board has nothing its
 * bootloader will start, and a download that stops part way through leaves it
 * that way. download() in ubertooth-dfu.c says why the whole first sector
 * rather than that one block has to be what is written last. */

#define SECTOR_BLOCKS (SECTOR_SIZE / BLOCK_SIZE)

/* An image with five blocks above the region's first sector, so the order has
 * all three of its parts to get right. */
#define ORDER_IMAGE_BLOCKS (SECTOR_BLOCKS + 5)
#define ORDER_IMAGE_PAYLOAD \
	(ORDER_IMAGE_BLOCKS * BLOCK_SIZE - DFU_SUFFIX_LENGTH)

/* The blocks that were written, against the blocks that should have been, as
 * offsets from the start of the application region. */
static void assert_write_order(const int *expected, int count)
{
	int i;

	mu_assert_int_eq(count, fake.dnload_calls);
	mu_assert(count <= DNLOAD_RECORD_MAX,
	          "more writes were expected than the record holds");
	for (i = 0; i < count && i < fake.dnload_calls &&
	     i < DNLOAD_RECORD_MAX; i++) {
		char message[96];

		snprintf(message, sizeof(message),
		         "write %d went to block %d, not to block %d", i,
		         fake.dnload_blocks[i] - FIRST_BLOCK, expected[i]);
		mu_assert(fake.dnload_blocks[i] == FIRST_BLOCK + expected[i],
		          message);
	}
}

MU_TEST(a_download_writes_the_first_sector_last)
{
	int expected[DNLOADS_FOR_BLOCKS(ORDER_IMAGE_BLOCKS)];
	struct run_result r;
	int i, count = 0;

	/* The blank vector table, then everything above the first sector, then
	 * the first sector. */
	expected[count++] = 0;
	for (i = SECTOR_BLOCKS; i < ORDER_IMAGE_BLOCKS; i++)
		expected[count++] = i;
	for (i = 0; i < SECTOR_BLOCKS; i++)
		expected[count++] = i;

	board_in_application(PART_ID_128K, 128 * 1024);
	write_image_of(sized_image_path, IMAGE_REVISION, ORDER_IMAGE_PAYLOAD);
	snprintf(fake.revision, sizeof(fake.revision), "%s", IMAGE_REVISION);
	r = RUN("-U", "0", "-d", sized_image_path, "-r");
	mu_assert_int_eq(0, r.rc);
	assert_write_order(expected, count);
	/* The first write leaves erased flash where the vector table goes, and
	 * the one that starts the last sector puts the real one there. */
	mu_assert(fake.dnload_blank[0], "the vector table was not blanked first");
	mu_assert(!fake.dnload_blank[count - SECTOR_BLOCKS],
	          "the vector table written last was blank");
	mu_assert(strstr(r.out, "Verified firmware revision: " IMAGE_REVISION)
	          != NULL, "the flashed revision was not verified");
}

/* An image that does not reach past the first sector has nothing for the
 * middle step, and is written in the same two others. */
MU_TEST(an_image_inside_the_first_sector_is_written_in_the_same_order)
{
	int expected[DNLOADS_FOR_BLOCKS(IMAGE_BLOCKS)];
	struct run_result r;
	int i, count = 0;

	expected[count++] = 0;
	for (i = 0; i < IMAGE_BLOCKS; i++)
		expected[count++] = i;

	board_in_application(PART_ID_128K, 128 * 1024);
	snprintf(fake.revision, sizeof(fake.revision), "%s", IMAGE_REVISION);
	r = RUN("-U", "0", "-d", image_path, "-r");
	mu_assert_int_eq(0, r.rc);
	assert_write_order(expected, count);
	mu_assert(fake.dnload_blank[0], "the vector table was not blanked first");
	mu_assert(!fake.dnload_blank[1], "the vector table written last was blank");
}

/* One word of a recorded write, little-endian as the board reads it. */
static uint32_t recorded_word(int write, int word)
{
	const uint8_t *at = fake.dnload_head[write] + word * 4;

	return (uint32_t)at[0] | ((uint32_t)at[1] << 8) |
	       ((uint32_t)at[2] << 16) | ((uint32_t)at[3] << 24);
}

/* The same eight words, as the image file has them. */
static void read_image_vectors(const char *path, uint32_t *words)
{
	FILE *file = fopen(path, "rb");
	uint8_t bytes[VECTOR_TABLE_BYTES];
	int word;

	mu_assert_fatal(file != NULL, "cannot read the test image back");
	mu_assert_fatal(fread(bytes, 1, sizeof(bytes), file) == sizeof(bytes),
	                "the test image has no vector table");
	fclose(file);
	for (word = 0; word < VECTOR_TABLE_WORDS; word++)
		words[word] = (uint32_t)bytes[word * 4] |
		              ((uint32_t)bytes[word * 4 + 1] << 8) |
		              ((uint32_t)bytes[word * 4 + 2] << 16) |
		              ((uint32_t)bytes[word * 4 + 3] << 24);
}

/* Put a wrong checksum into an image that has already been written: what an
 * image built outside this tree could carry, and what must not reach a board,
 * since a board would then refuse to start the image just flashed to it. The
 * file is signed again, because the suffix CRC covers the payload. */
#define STALE_CHECKSUM 0xdeadbeefu

static void stale_checksum_image(const char *path, long payload)
{
	FILE *file = fopen(path, "rb");
	FILE *payload_file = tmpfile();
	FILE *image;
	uint8_t *bytes = malloc((size_t)payload);

	mu_assert_fatal(file != NULL && payload_file != NULL && bytes != NULL,
	                "cannot rewrite the test image");
	mu_assert_fatal(fread(bytes, 1, (size_t)payload, file) == (size_t)payload,
	                "the test image is shorter than its payload");
	fclose(file);
	bytes[28] = (uint8_t)(STALE_CHECKSUM & 0xff);
	bytes[29] = (uint8_t)((STALE_CHECKSUM >> 8) & 0xff);
	bytes[30] = (uint8_t)((STALE_CHECKSUM >> 16) & 0xff);
	bytes[31] = (uint8_t)(STALE_CHECKSUM >> 24);
	mu_assert_fatal(fwrite(bytes, 1, (size_t)payload, payload_file) ==
	                (size_t)payload, "cannot write the test payload");
	free(bytes);
	image = fopen(path, "w+b");
	mu_assert_fatal(image != NULL, "cannot reopen the test image");
	mu_assert_fatal(sign("the test payload", payload_file, path, image,
	                     U1_DFU_VENDORID, U1_DFU_PRODUCTID) == 0,
	                "cannot sign the test image again");
	fclose(payload_file);
	fclose(image);
}

/* Fill length bytes of an image's payload with one byte, and sign it again
 * because the suffix CRC covers the payload. The tests below use it to make
 * an image that carries no image check block, and one that carries a filled-in
 * block from somewhere else. */
static void patch_image_payload(const char *path, long payload, int offset,
                                uint8_t byte, int length)
{
	FILE *file = fopen(path, "rb");
	FILE *payload_file = tmpfile();
	FILE *image;
	uint8_t *bytes = malloc((size_t)payload);

	mu_assert_fatal(file != NULL && payload_file != NULL && bytes != NULL,
	                "cannot rewrite the test image");
	mu_assert_fatal(fread(bytes, 1, (size_t)payload, file) == (size_t)payload,
	                "the test image is shorter than its payload");
	fclose(file);
	mu_assert_fatal(offset + length <= payload,
	                "the patch runs past the test image's payload");
	memset(bytes + offset, byte, (size_t)length);
	mu_assert_fatal(fwrite(bytes, 1, (size_t)payload, payload_file) ==
	                (size_t)payload, "cannot write the test payload");
	free(bytes);
	image = fopen(path, "w+b");
	mu_assert_fatal(image != NULL, "cannot reopen the test image");
	mu_assert_fatal(sign("the test payload", payload_file, path, image,
	                     U1_DFU_VENDORID, U1_DFU_PRODUCTID) == 0,
	                "cannot sign the test image again");
	fclose(payload_file);
	fclose(image);
}

/* An image that carries a checksum of its own is written with the right one
 * rather than the one it came with, so what the board holds always adds up. */
MU_TEST(a_stale_checksum_in_the_image_is_written_over)
{
	uint32_t from_image[VECTOR_TABLE_WORDS];
	struct run_result r;
	uint32_t sum = 0;
	int last = -1;
	int i, word;

	board_in_application(PART_ID_128K, 128 * 1024);
	write_image_of(sized_image_path, IMAGE_REVISION, ORDER_IMAGE_PAYLOAD);
	stale_checksum_image(sized_image_path, ORDER_IMAGE_PAYLOAD);
	read_image_vectors(sized_image_path, from_image);
	mu_assert_int_eq(STALE_CHECKSUM, from_image[VECTOR_TABLE_WORDS - 1]);

	snprintf(fake.revision, sizeof(fake.revision), "%s", IMAGE_REVISION);
	r = RUN("-U", "0", "-d", sized_image_path, "-r");
	mu_assert_int_eq(0, r.rc);

	for (i = 0; i < fake.dnload_calls && i < DNLOAD_RECORD_MAX; i++)
		if (fake.dnload_blocks[i] == FIRST_BLOCK)
			last = i;
	mu_assert_fatal(last > 0, "no vector table was written");
	for (word = 0; word < VECTOR_TABLE_WORDS; word++)
		sum += recorded_word(last, word);
	mu_assert_int_eq(0, sum);
	mu_assert(recorded_word(last, VECTOR_TABLE_WORDS - 1) != STALE_CHECKSUM,
	          "the image's own wrong checksum was sent to the board");
}

/* The vector table the tool writes last carries the checksum the board checks:
 * the eight words add up to zero (the LPC criterion for valid user code). A
 * first block that only partly reached the flash does not add up, so the
 * bootloader leaves it alone and waits in DFU instead of starting it. */
MU_TEST(the_vector_table_written_last_carries_its_checksum)
{
	uint32_t from_image[VECTOR_TABLE_WORDS];
	struct run_result r;
	uint32_t sum = 0;
	int last = -1;
	int i, word;

	board_in_application(PART_ID_128K, 128 * 1024);
	write_image_of(sized_image_path, IMAGE_REVISION, ORDER_IMAGE_PAYLOAD);
	snprintf(fake.revision, sizeof(fake.revision), "%s", IMAGE_REVISION);
	r = RUN("-U", "0", "-d", sized_image_path, "-r");
	mu_assert_int_eq(0, r.rc);

	for (i = 0; i < fake.dnload_calls && i < DNLOAD_RECORD_MAX; i++)
		if (fake.dnload_blocks[i] == FIRST_BLOCK)
			last = i;
	mu_assert_fatal(last > 0, "no vector table was written");

	for (word = 0; word < VECTOR_TABLE_WORDS; word++)
		sum += recorded_word(last, word);
	mu_assert_int_eq(0, sum);

	/* Only the checksum word is the tool's: the rest of the vector table is
	 * the image's, byte for byte. */
	read_image_vectors(sized_image_path, from_image);
	for (word = 0; word < VECTOR_TABLE_WORDS - 1; word++)
		mu_assert_int_eq(from_image[word], recorded_word(last, word));
	/* ...and it had to be filled in, so the image did not carry it. */
	mu_assert(from_image[VECTOR_TABLE_WORDS - 1] !=
	          recorded_word(last, VECTOR_TABLE_WORDS - 1),
	          "the test image already carried this checksum, so the test "
	          "shows nothing");
}

/* What the order is for: a download that stops part way through has sent no
 * vector table at all, so the board's bootloader finds nothing to start and
 * waits in DFU for another image, instead of starting one that is missing
 * most of itself. */
MU_TEST(a_download_that_stops_part_way_sends_no_vector_table)
{
	struct run_result r;
	int i;

	board_in_application(PART_ID_128K, 128 * 1024);
	write_image_of(sized_image_path, IMAGE_REVISION, ORDER_IMAGE_PAYLOAD);
	fake.dnload_rc = LIBUSB_ERROR_IO;
	/* Four blocks through: past the blanking and into the blocks above the
	 * first sector. */
	fake.dnload_fail_after = 4;
	snprintf(fake.revision, sizeof(fake.revision), "%s", IMAGE_REVISION);
	r = RUN("-U", "0", "-d", sized_image_path, "-r");
	mu_assert_int_eq(1, r.rc);
	mu_assert(fake.dnload_calls > 4, "the download stopped before it started");
	for (i = 0; i < fake.dnload_calls && i < DNLOAD_RECORD_MAX; i++)
		mu_assert(fake.dnload_blocks[i] != FIRST_BLOCK ||
		          fake.dnload_blank[i],
		          "a download that stopped part way sent a vector table");
}

/* ---- the image check block ---------------------------------------------- */

/* A field of the image check block, as the region holds it after the
 * download. */
static const uint8_t *written_check_block(void)
{
	return fake.region + UBERTOOTH_IMAGE_CHECK_OFFSET;
}

static uint32_t written_check_le32(int offset)
{
	return read_le32(written_check_block() + offset);
}

static uint16_t written_check_le16(int offset)
{
	return read_le16(written_check_block() + offset);
}

/* What a board does with the region it was given: add every word of the
 * length the block claims up and see whether it comes to zero
 * (firmware/bootloader/app_image.h, app_image_region_checksum_ok). Worked out
 * here rather than copied from the tool, so the two have to agree about the
 * rule and not only about a number. */
static uint32_t written_region_sum(uint32_t length)
{
	uint32_t sum = 0;
	uint32_t at;

	for (at = 0; at + 4 <= length; at += 4)
		sum += read_le32(fake.region + at);
	return sum;
}

/* A download leaves a board a record of what it wrote: how many bytes, twice
 * over, and a checksum over them that comes to zero. This is the check that
 * covers the fifteen blocks the vector table's own checksum says nothing
 * about. */
MU_TEST(a_download_records_what_it_wrote)
{
	struct run_result r;
	uint32_t length;

	board_in_application(PART_ID_128K, 128 * 1024);
	write_image_of(sized_image_path, IMAGE_REVISION, ORDER_IMAGE_PAYLOAD);
	snprintf(fake.revision, sizeof(fake.revision), "%s", IMAGE_REVISION);
	r = RUN("-U", "0", "-d", sized_image_path, "-r");
	mu_assert_int_eq(0, r.rc);

	mu_assert_int_eq(UBERTOOTH_IMAGE_CHECK_VECTOR |
	                 UBERTOOTH_IMAGE_CHECK_REGION,
	                 written_check_le16(UBERTOOTH_IMAGE_CHECK_OFF_FLAGS));
	length = written_check_le32(UBERTOOTH_IMAGE_CHECK_OFF_LENGTH);
	mu_assert_int_eq((uint32_t)fake.region_blocks * BLOCK_SIZE, length);
	mu_assert_int_eq(~length,
	                 written_check_le32(
	                     UBERTOOTH_IMAGE_CHECK_OFF_LENGTH_COMPLEMENT));
	mu_assert_int_eq(0, written_region_sum(length));
	/* ...and the checksum is a figure of its own, not a word left at zero. */
	mu_assert(written_check_le32(UBERTOOTH_IMAGE_CHECK_OFF_CHECKSUM) != 0,
	          "the checksum word was left at zero");
}

/* The record has to reach the flash with the vector table it describes, or an
 * interruption could leave a board a startable table and no record of what
 * should be behind it. The region's first block is written whole or not at
 * all, so that means the record is in that block: the write that carries the
 * real vector table is the write that carries the record. */
MU_TEST(the_record_goes_in_with_the_vector_table)
{
	struct run_result r;
	int last = -1;
	int i;

	board_in_application(PART_ID_128K, 128 * 1024);
	write_image_of(sized_image_path, IMAGE_REVISION, ORDER_IMAGE_PAYLOAD);
	snprintf(fake.revision, sizeof(fake.revision), "%s", IMAGE_REVISION);
	r = RUN("-U", "0", "-d", sized_image_path, "-r");
	mu_assert_int_eq(0, r.rc);

	for (i = 0; i < fake.dnload_calls && i < DNLOAD_RECORD_MAX; i++)
		if (fake.dnload_blocks[i] == FIRST_BLOCK)
			last = i;
	mu_assert_fatal(last > 0, "no vector table was written");
	/* The write that carried the vector table was the last write to the
	 * region's first block, and the record is inside that block. */
	mu_assert(!fake.dnload_blank[last],
	          "the last write to the first block was blank");
	mu_assert(UBERTOOTH_IMAGE_CHECK_OFFSET + UBERTOOTH_IMAGE_CHECK_LENGTH <=
	          BLOCK_SIZE,
	          "the record is not inside the block the vector table is in");
}

/* One block of the image that never reached the flash, which is the board
 * this closes: the vector table and its own checksum are perfect, and the sum
 * over the region is what says the image is not all there. */
MU_TEST(a_region_the_board_did_not_get_whole_does_not_add_up)
{
	struct run_result r;
	uint32_t length, at;

	board_in_application(PART_ID_128K, 128 * 1024);
	write_image_of(sized_image_path, IMAGE_REVISION, ORDER_IMAGE_PAYLOAD);
	snprintf(fake.revision, sizeof(fake.revision), "%s", IMAGE_REVISION);
	r = RUN("-U", "0", "-d", sized_image_path, "-r");
	mu_assert_int_eq(0, r.rc);

	length = written_check_le32(UBERTOOTH_IMAGE_CHECK_OFF_LENGTH);
	mu_assert_int_eq(0, written_region_sum(length));
	/* Erased flash where the region's second block should be: the block
	 * after the vector table, and the first of the fifteen an interruption
	 * in the last step leaves out. It holds the firmware info block, so
	 * there is something there to lose -- most of this test image is 0xFF
	 * already, which is what erased flash reads as. */
	for (at = BLOCK_SIZE; at < 2 * BLOCK_SIZE; at++)
		fake.region[at] = 0xff;
	mu_assert(written_region_sum(length) != 0,
	          "a region missing a block still added up");
}

/* What the tool writes over, and nothing else. ubertooth-dfu(1) tells a
 * reader that a read-back differs from the image file in the checksum word at
 * 0x1c and in the fourteen bytes of the record from 0xea to 0xf8: the magic
 * and the format version in front of them are the image's own, and the
 * reserved bytes after them are left alone. */
MU_TEST(the_tool_writes_over_the_checksum_word_and_the_record_only)
{
	struct run_result r;
	uint8_t from_image[BLOCK_SIZE];
	FILE *file;
	int at;

	board_in_application(PART_ID_128K, 128 * 1024);
	write_image_of(sized_image_path, IMAGE_REVISION, ORDER_IMAGE_PAYLOAD);
	snprintf(fake.revision, sizeof(fake.revision), "%s", IMAGE_REVISION);
	r = RUN("-U", "0", "-d", sized_image_path, "-r");
	mu_assert_int_eq(0, r.rc);

	file = fopen(sized_image_path, "rb");
	mu_assert_fatal(file != NULL, "cannot read the test image back");
	mu_assert_fatal(fread(from_image, 1, sizeof(from_image), file) ==
	                sizeof(from_image), "the test image has no first block");
	fclose(file);
	for (at = 0; at < BLOCK_SIZE; at++) {
		char message[96];

		if ((at >= 0x1c && at < 0x20) || (at >= 0xea && at < 0xf8))
			continue;
		snprintf(message, sizeof(message),
		         "the tool wrote over byte 0x%02x of the first block", at);
		mu_assert(from_image[at] == fake.region[at], message);
	}
	/* ...and it did write inside both spans, so the test is not passing on
	 * a tool that writes nothing at all. A byte inside a span may well be
	 * unchanged, because the image has zeros there and some of what goes in
	 * is zero too. */
	mu_assert(memcmp(from_image + 0x1c, fake.region + 0x1c, 4) != 0,
	          "the checksum word was not filled in");
	mu_assert(memcmp(from_image + 0xea, fake.region + 0xea, 0xf8 - 0xea) != 0,
	          "the record was not filled in");
}

/* An image from a tree that has no image check block: those 32 bytes are the
 * image's own code or data, and the tool leaves them exactly as the file has
 * them. Writing over them would corrupt the image it is flashing. */
MU_TEST(an_image_with_no_check_block_is_written_unchanged)
{
	struct run_result r;
	uint8_t from_image[UBERTOOTH_IMAGE_CHECK_LENGTH];
	FILE *file;

	board_in_application(PART_ID_128K, 128 * 1024);
	write_image_of(sized_image_path, IMAGE_REVISION, ORDER_IMAGE_PAYLOAD);
	patch_image_payload(sized_image_path, ORDER_IMAGE_PAYLOAD,
	                    UBERTOOTH_IMAGE_CHECK_OFFSET, 0x5a,
	                    UBERTOOTH_IMAGE_CHECK_LENGTH);
	snprintf(fake.revision, sizeof(fake.revision), "%s", IMAGE_REVISION);
	r = RUN("-U", "0", "-d", sized_image_path, "-r");
	mu_assert_int_eq(0, r.rc);

	file = fopen(sized_image_path, "rb");
	mu_assert_fatal(file != NULL, "cannot read the test image back");
	mu_assert_fatal(fseek(file, UBERTOOTH_IMAGE_CHECK_OFFSET, SEEK_SET) == 0 &&
	                fread(from_image, 1, sizeof(from_image), file) ==
	                sizeof(from_image),
	                "the test image is shorter than the block's offset");
	fclose(file);
	mu_assert(memcmp(from_image, written_check_block(),
	                 sizeof(from_image)) == 0,
	          "the tool wrote over 32 bytes of an image that carries no "
	          "image check block");
}

/* An image that already carries a filled-in block -- one written by another
 * tool, or read back off a board and flashed again -- is written with this
 * download's record rather than the one it came with, so what the board holds
 * always describes what was just written to it. */
MU_TEST(a_stale_record_in_the_image_is_written_over)
{
	struct run_result r;
	uint32_t length;

	board_in_application(PART_ID_128K, 128 * 1024);
	write_image_of(sized_image_path, IMAGE_REVISION, ORDER_IMAGE_PAYLOAD);
	/* A length and checksum from somewhere else, and both flags already
	 * set. */
	patch_image_payload(sized_image_path, ORDER_IMAGE_PAYLOAD,
	                    UBERTOOTH_IMAGE_CHECK_OFFSET +
	                    UBERTOOTH_IMAGE_CHECK_OFF_FLAGS, 0xa5,
	                    UBERTOOTH_IMAGE_CHECK_LENGTH -
	                    UBERTOOTH_IMAGE_CHECK_OFF_FLAGS);
	snprintf(fake.revision, sizeof(fake.revision), "%s", IMAGE_REVISION);
	r = RUN("-U", "0", "-d", sized_image_path, "-r");
	mu_assert_int_eq(0, r.rc);

	mu_assert_int_eq(UBERTOOTH_IMAGE_CHECK_VECTOR |
	                 UBERTOOTH_IMAGE_CHECK_REGION,
	                 written_check_le16(UBERTOOTH_IMAGE_CHECK_OFF_FLAGS));
	length = written_check_le32(UBERTOOTH_IMAGE_CHECK_OFF_LENGTH);
	mu_assert_int_eq((uint32_t)fake.region_blocks * BLOCK_SIZE, length);
	mu_assert_int_eq(0, written_region_sum(length));
}

/* ---- writing only into the flash the part has -------------------------- */

/* The application region of a 128 KiB part: all the flash a -d write has to
 * go into there, and a whole number of blocks. */
#define REGION_128K DUMP_LENGTH_FOR(128 * 1024)

/* The whole file is written, suffix included, so an image of exactly the
 * region fits it exactly. */
MU_TEST(an_image_that_fills_the_part_is_written)
{
	struct run_result r;

	board_in_application(PART_ID_128K, 128 * 1024);
	write_image_of(sized_image_path, IMAGE_REVISION,
	               REGION_128K - DFU_SUFFIX_LENGTH);
	snprintf(fake.revision, sizeof(fake.revision), "%s", IMAGE_REVISION);
	r = RUN("-U", "0", "-d", sized_image_path, "-r");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(DNLOADS_FOR_BLOCKS(REGION_128K / BLOCK_SIZE),
	                 fake.dnload_calls);
	mu_assert_int_eq(1, fake.part_id_calls);
	mu_assert(strstr(r.err,
	                 "The image needs 114688 bytes, 0x4000-0x1ffff: part "
	                 "0x25011722 (LPC1754) has 128 KiB of flash") != NULL,
	          "the download did not say what flash it covers");
	mu_assert(strstr(r.out, "Verified firmware revision: " IMAGE_REVISION)
	          != NULL, "the flashed revision was not verified");
}

/* One byte more needs a further block, which the part has no flash for: the
 * bootloader would send it to IAP, fail, and leave the application missing
 * its tail. So the image is refused while the board is still running that
 * application. */
MU_TEST(an_image_one_byte_past_the_part_is_refused)
{
	struct run_result r;

	board_in_application(PART_ID_128K, 128 * 1024);
	write_image_of(sized_image_path, IMAGE_REVISION,
	               REGION_128K - DFU_SUFFIX_LENGTH + 1);
	snprintf(fake.revision, sizeof(fake.revision), "%s", IMAGE_REVISION);
	r = RUN("-U", "0", "-d", sized_image_path, "-r");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "needs 114944 bytes of flash; part 0x25011722 "
	                 "(LPC1754) has 114688 above the bootloader") != NULL,
	          "the refusal did not name the image and the part");
	mu_assert(strstr(r.err, "firmware will not be updated") != NULL,
	          "the refusal did not say the firmware was left alone");
	/* The part is read, and then nothing else happens: the board is never
	 * asked for DFU, so nothing is erased and nothing needs detaching. */
	mu_assert_int_eq(1, fake.part_id_calls);
	mu_assert_int_eq(0, fake.flash_calls);
	mu_assert_int_eq(0, fake.dnload_calls);
	mu_assert_int_eq(0, fake.detach_calls);
	/* And the entry failure the refusal is carried out through is not
	 * reported on top of it. */
	mu_assert(strstr(r.err, "unable to open selected application device")
	          == NULL, "the refusal was reported as a device that would not open");
}

/* A file with a valid DFU suffix but no firmware info block is not an image
 * from this tree: another device's image, or something that happens to carry
 * a suffix. It is refused while the board is still running its application,
 * so nothing is erased, and the message names the file. */
MU_TEST(an_image_with_no_info_block_is_refused)
{
	struct run_result r;

	board_in_application(PART_ID_128K, 128 * 1024);
	write_image_with(sized_image_path, IMAGE_NAME, IMAGE_REVISION,
	                 IMAGE_PAYLOAD, 0, IMAGE_API);
	r = RUN("-U", "0", "-d", sized_image_path, "-r");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "there is no firmware info block here at offset "
	                 "0x100; firmware will not be updated") != NULL,
	          "the refusal did not say the block is missing and the board was "
	          "left alone");
	/* One line, not two, and it is the only error the run reports. */
	mu_assert_int_eq(1, error_lines(r.err));
	/* Nothing was opened, so nothing is erased and nothing needs detaching. */
	mu_assert_int_eq(0, fake.part_id_calls);
	mu_assert_int_eq(0, fake.flash_calls);
	mu_assert_int_eq(0, fake.dnload_calls);
	mu_assert_int_eq(0, fake.detach_calls);
}

/* A block in a format this host does not know: its fields may have moved, so
 * nothing is read out of them and the file is refused the same way. */
MU_TEST(an_image_with_an_unknown_block_format_is_refused)
{
	struct run_result r;

	board_in_application(PART_ID_128K, 128 * 1024);
	write_image_with(sized_image_path, IMAGE_NAME, IMAGE_REVISION,
	                 IMAGE_PAYLOAD, UBERTOOTH_FW_INFO_FORMAT + 1, IMAGE_API);
	r = RUN("-U", "0", "-d", sized_image_path, "-r");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "format this host does not know") != NULL,
	          "the refusal did not name the format as the problem");
	mu_assert_int_eq(0, fake.dnload_calls);
}

/* A part this host has no entry for has no flash size to check against, so
 * the image is written as it always was, with a line saying it was not
 * checked: the assumed size is not a floor, and refusing a valid image for a
 * part nobody knows would be worse. */
MU_TEST(an_unknown_part_warns_and_writes_the_image)
{
	struct run_result r;

	board_in_application(PART_ID_UNKNOWN, 128 * 1024);
	write_image_of(sized_image_path, IMAGE_REVISION,
	               REGION_128K - DFU_SUFFIX_LENGTH + 1);
	snprintf(fake.revision, sizeof(fake.revision), "%s", IMAGE_REVISION);
	r = RUN("-U", "0", "-d", sized_image_path, "-r");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strstr(r.err, "warning: part 0x12345678 is not known here, so ")
	          != NULL, "the unknown part was not reported");
	mu_assert(strstr(r.err, "is not checked against this board's flash")
	          != NULL, "the unchecked image was not reported");
	mu_assert_int_eq(DNLOADS_FOR_BLOCKS((REGION_128K / BLOCK_SIZE) + 1),
	                 fake.dnload_calls);
}

/* The bound follows the part, not a figure: the image the 128 KiB part has no
 * room for is written to a 256 KiB one. */
MU_TEST(an_image_too_large_for_one_part_fits_a_bigger_one)
{
	struct run_result r;

	board_in_application(PART_ID_256K, 256 * 1024);
	write_image_of(sized_image_path, IMAGE_REVISION,
	               REGION_128K - DFU_SUFFIX_LENGTH + 1);
	snprintf(fake.revision, sizeof(fake.revision), "%s", IMAGE_REVISION);
	r = RUN("-U", "0", "-d", sized_image_path, "-r");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(DNLOADS_FOR_BLOCKS((REGION_128K / BLOCK_SIZE) + 1),
	                 fake.dnload_calls);
	mu_assert(strstr(r.err,
	                 "The image needs 114944 bytes, 0x4000-0x200ff: part "
	                 "0x25011723 (LPC1756) has 256 KiB of flash") != NULL,
	          "the download did not say what flash it covers");
	mu_assert(strstr(r.err, "too large") == NULL,
	          "an image that fits this part was refused");
}

/* And one that is too large for a 256 KiB part as well. */
MU_TEST(an_image_past_a_256_kib_part_is_refused_too)
{
	struct run_result r;

	board_in_application(PART_ID_256K, 256 * 1024);
	write_image_of(sized_image_path, IMAGE_REVISION,
	               DUMP_LENGTH_FOR(256 * 1024) - DFU_SUFFIX_LENGTH + 1);
	snprintf(fake.revision, sizeof(fake.revision), "%s", IMAGE_REVISION);
	r = RUN("-U", "0", "-d", sized_image_path, "-r");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "needs 246016 bytes of flash; part 0x25011723 "
	                 "(LPC1756) has 245760 above the bootloader") != NULL,
	          "the refusal did not name the image and the part");
	mu_assert_int_eq(0, fake.flash_calls);
	mu_assert_int_eq(0, fake.dnload_calls);
}

/* And a part that will not report itself at all reads the same way. */
MU_TEST(a_part_that_cannot_be_read_warns_and_writes_the_image)
{
	struct run_result r;

	board_in_application(PART_ID_128K, 128 * 1024);
	fake.part_id_rc = LIBUSB_ERROR_TIMEOUT;
	write_image_of(sized_image_path, IMAGE_REVISION,
	               REGION_128K - DFU_SUFFIX_LENGTH + 1);
	snprintf(fake.revision, sizeof(fake.revision), "%s", IMAGE_REVISION);
	r = RUN("-U", "0", "-d", sized_image_path, "-r");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strstr(r.err, "warning: the part could not be read, so ")
	          != NULL, "the unreadable part was not reported");
	mu_assert_int_eq(DNLOADS_FOR_BLOCKS((REGION_128K / BLOCK_SIZE) + 1),
	                 fake.dnload_calls);
}

/* A board adopted in the bootloader has no application to ask, so it takes
 * the same unchecked path. */
MU_TEST(a_board_already_in_dfu_warns_that_the_image_is_not_checked)
{
	struct run_result r;

	board_in_dfu();
	snprintf(fake.revision, sizeof(fake.revision), "%s", IMAGE_REVISION);
	r = RUN("-d", image_path, "-r");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(0, fake.part_id_calls);
	mu_assert(strstr(r.err, "warning: the part could not be read, so ")
	          != NULL, "the unchecked image was not reported");
	mu_assert_int_eq(DNLOADS_FOR_BLOCKS(IMAGE_BLOCKS), fake.dnload_calls);
}

/* The part ID is wanted for the transfers, which are both bounded by the
 * part's flash, and reading it has its own error output and timeout, so a
 * command that would ignore the answer does not ask. A bare reset through the
 * application is that case. */
MU_TEST(a_bare_reset_does_not_ask_for_the_part_id)
{
	struct run_result r;

	board_in_application(PART_ID_256K, 256 * 1024);
	r = RUN("-U", "0", "-r");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(1, fake.flash_calls);
	mu_assert_int_eq(1, fake.detach_calls);
	mu_assert_int_eq(0, fake.part_id_calls);
	mu_assert(strstr(r.err, "part ID") == NULL,
	          "a reset reported something about the part ID");
}

/* ---- which stream each line is on ------------------------------------- */

/* Standard output carries only what the user asked for. For a flash with a
 * reset that is one line: the revision the board reports once it is running
 * the new firmware, which is the only thing this command reads back. The
 * programming trace -- the entry, what flash the image covers, a dot per
 * block, the detach -- is progress, so it is on standard error. */
MU_TEST(a_flash_with_reset_reports_the_revision_on_standard_output)
{
	struct run_result r;

	board_in_application(PART_ID_128K, 128 * 1024);
	snprintf(fake.revision, sizeof(fake.revision), "%s", IMAGE_REVISION);
	r = RUN("-U", "0", "-d", image_path, "-r");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, "Verified firmware revision: " IMAGE_REVISION "\n")
	          == 0, r.out);
	mu_assert(strstr(r.err, "Switching selected device to DFU mode...\n")
	          != NULL, "the DFU entry was not reported on standard error");
	mu_assert(strstr(r.err, "The image needs 1024 bytes, 0x4000-0x43ff: part "
	                 "0x25011722 (LPC1754) has 128 KiB of flash") != NULL,
	          "the flash the image covers was not reported on standard error");
	mu_assert(strstr(r.err, "....\n") != NULL,
	          "the programming dots were not on standard error");
	mu_assert(strstr(r.err, "Detached\n") != NULL,
	          "the detach was not reported on standard error");
	mu_assert_int_eq(0, error_lines(r.err));
}

/* A read-back writes its dump to the file it was given, so nothing at all
 * goes to standard output: not the line saying what it is about to read, and
 * not the dots. */
MU_TEST(a_read_back_says_nothing_on_standard_output)
{
	struct run_result r;
	int content_ok;

	board_in_application(PART_ID_128K, 128 * 1024);
	remove_dump();
	r = RUN("-U", "0", "-u", dump_path, "-r");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(DUMP_LENGTH_FOR(128 * 1024),
	                 dump_length_and_content(&content_ok));
	mu_assert(content_ok, "the dump is not the region it read");
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strstr(r.err, "Reading back 114688 bytes") != NULL,
	          "the read-back plan was not on standard error");
	mu_assert(strstr(r.err, "....\n") != NULL,
	          "the read-back dots were not on standard error");
	remove_dump();
	mu_assert_int_eq(0, error_lines(r.err));
}

/* A reset only sets something, so it says nothing on standard output either. */
MU_TEST(a_bare_reset_says_nothing_on_standard_output)
{
	struct run_result r;

	board_in_application(PART_ID_256K, 256 * 1024);
	r = RUN("-U", "0", "-r");
	mu_assert_int_eq(0, r.rc);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strstr(r.err, "Detached\n") != NULL,
	          "the detach was not reported on standard error");
}

#ifndef TEST_REAL_API_REPORTS
/* Built against an installed libubertooth, so there is no library source to
 * compile in: the report for a request the bootloader refused is a stub here,
 * and the expectations below then look for nothing from it. */
void ubertooth_report_unsupported_request(const char* what, int code)
{
	(void)what;
	(void)code;
}
#endif

/* The report about a refused request asks whether the board has left the bus
 * before it blames the board's firmware. Nothing here unplugs a board, so it
 * is always still there. */
int ubertooth_device_gone(const void* board)
{
	(void)board;
	return 0;
}

/* What the library's shared report says about a request the bootloader
 * stalled: the request named, and no firmware API version, because this tool
 * talks to the bootloader and never reads one. */
#ifdef TEST_REAL_API_REPORTS
#define STALLED_BLOCK \
	"error: could not write block 64: the board's firmware does not " \
	"support it (stalled, libusb -9)\n"
#define STALLED_STATE \
	"error: could not read the DFU state: the board's firmware does not " \
	"support it (stalled, libusb -9)\n"
#else
#define STALLED_BLOCK ""
#define STALLED_STATE ""
#endif

/* How many lines that report puts on standard error for one stalled request:
 * one when the library's own report is compiled in, none when it is the stub
 * above. The counting cases below add it up rather than naming a total, so
 * they hold in both configurations. */
#ifdef TEST_REAL_API_REPORTS
#define STALLED_REQUEST_LINES 1
#else
#define STALLED_REQUEST_LINES 0
#endif

/* A programming transfer that fails: both attempts, the control-request
 * failure under them and the line that gives up are all on standard error,
 * each starting "error: ", and nothing is left on standard output. */
MU_TEST(a_failed_flash_reports_errors_only_on_standard_error)
{
	struct run_result r;

	board_in_application(PART_ID_128K, 128 * 1024);
	fake.dnload_rc = LIBUSB_ERROR_PIPE;
	snprintf(fake.revision, sizeof(fake.revision), "%s", IMAGE_REVISION);
	r = RUN("-U", "0", "-d", image_path, "-r");
	mu_assert_int_eq(1, r.rc);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strstr(r.err, STALLED_BLOCK) != NULL,
	          "the refused block write was not reported as an error");
	mu_assert(strstr(r.err, "error: firmware update failed after 2 "
	                 "attempt(s)") != NULL,
	          "giving up was not reported as an error");
	/* -r still returns the board to its application, and that is progress. */
	mu_assert(strstr(r.err, "Detached\n") != NULL,
	          "the board was not returned to its application");
}

/* A block write that fails with anything other than a stall takes the other
 * branch, where the status is named by libubertooth rather than by this tool.
 * That line is still on standard error, standard output is still empty, and
 * the line this tool prints to give up still begins "error: ". The
 * libubertooth line itself does not carry that prefix: it comes from the
 * shared reporter every tool goes through, which is converted separately, so
 * this test pins the stream and leaves the wording alone. */
MU_TEST(a_flash_failing_without_a_stall_still_keeps_standard_output_empty)
{
	struct run_result r;

	board_in_application(PART_ID_128K, 128 * 1024);
	fake.dnload_rc = LIBUSB_ERROR_IO;
	snprintf(fake.revision, sizeof(fake.revision), "%s", IMAGE_REVISION);
	r = RUN("-U", "0", "-d", image_path, "-r");
	mu_assert_int_eq(1, r.rc);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strstr(r.err, "does not support it (stalled") == NULL,
	          "a plain transfer failure was reported as a stall");
	mu_assert(strstr(r.err, "error: firmware update failed after 2 "
	                 "attempt(s)") != NULL,
	          "giving up was not reported as an error");
	mu_assert(strstr(r.err, "Detached\n") != NULL,
	          "the board was not returned to its application");
}

/* The bootloader stalling a state read, the other request this tool reports
 * as unsupported, ends the command the same way. */
MU_TEST(a_bootloader_that_stalls_the_state_read_reports_one_error_per_line)
{
	struct run_result r;

	board_in_dfu();
	fake.get_state_rc = LIBUSB_ERROR_PIPE;
	r = RUN("-r");
	mu_assert_int_eq(1, r.rc);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strstr(r.err, STALLED_STATE) != NULL,
	          "the stalled state read was not reported as an error");
	mu_assert(strstr(r.err, "error: DFU gave up: the bootloader did not "
	                 "report its state") != NULL,
	          "giving up on the state read was not reported as an error");
	mu_assert(strstr(r.err, "error: detach failed: could not enter DFU mode")
	          != NULL, "the failed detach was not reported as an error");
}

/* The dots of a flash share standard error with the error lines, so a
 * failure part way through has to finish the row of dots first. Otherwise
 * the error starts in the middle of it ("....error: ...") and nothing
 * reading standard error can match it against the start of a line. */
MU_TEST(a_flash_failing_part_way_finishes_its_row_of_dots_first)
{
	struct run_result r;

	board_in_application(PART_ID_128K, 128 * 1024);
	fake.dnload_rc = LIBUSB_ERROR_PIPE;
	fake.dnload_fail_after = 2;
	r = RUN("-U", "0", "-d", image_path, "-r");
	mu_assert_int_eq(1, r.rc);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strstr(r.err, "..\n") != NULL,
	          "the row of dots was not finished");
	mu_assert(strstr(r.err, ".error:") == NULL,
	          "an error line started in the middle of the row of dots");
	/* One stalled block write per attempt, and the line that gives up. */
	mu_assert_int_eq(1 + 2 * STALLED_REQUEST_LINES, error_lines(r.err));
}

/* The status read after a block is the other failure that can land mid-row. */
MU_TEST(a_status_read_failing_part_way_finishes_its_row_of_dots_first)
{
	struct run_result r;

	board_in_application(PART_ID_128K, 128 * 1024);
	fake.getstatus_rc = LIBUSB_ERROR_PIPE;
	fake.getstatus_fail_after = 2;
	r = RUN("-U", "0", "-d", image_path, "-r");
	mu_assert_int_eq(1, r.rc);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strstr(r.err, "..\n") != NULL,
	          "the row of dots was not finished");
	mu_assert(strstr(r.err, ".error:") == NULL,
	          "an error line started in the middle of the row of dots");
	mu_assert_int_eq(1 + 2 * STALLED_REQUEST_LINES, error_lines(r.err));
}

/* A read-back failing part way has the same rows of dots to finish. */
MU_TEST(a_read_back_failing_part_way_finishes_its_row_of_dots_first)
{
	struct run_result r;

	board_in_application(PART_ID_128K, 128 * 1024);
	remove_dump();
	fake.upload_fail_block = FIRST_BLOCK + 2;
	fake.upload_fail_rc = LIBUSB_ERROR_PIPE;
	r = RUN("-U", "0", "-u", dump_path, "-r");
	mu_assert_int_eq(1, r.rc);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strstr(r.err, "..\n") != NULL,
	          "the row of dots was not finished");
	mu_assert(strstr(r.err, ".error:") == NULL,
	          "an error line started in the middle of the row of dots");
	remove_dump();
}

int main(void)
{
	char directory[] = "/tmp/ubertooth-dfu-test.XXXXXX";

	printf("DFU main tests\n");
	if (mkdtemp(directory) == NULL) {
		fprintf(stderr, "cannot create a working directory\n");
		return 1;
	}
	snprintf(dump_path, sizeof(dump_path), "%s/readback.bin", directory);
	snprintf(image_path, sizeof(image_path), "%s/firmware.dfu", directory);
	snprintf(sized_image_path, sizeof(sized_image_path), "%s/sized.dfu",
	         directory);
	write_image(IMAGE_REVISION);
	/* A detach that fails waits for the DFU device to go away, and the cases
	 * here that keep it there wait out that whole budget, twice each. What
	 * they check is where the wait ends up, not how long the tool is willing
	 * to wait, so they do it in a few milliseconds. */
	detach_gone_polls = 3;
	detach_gone_interval_ms = 1;
	/* The confirming wait keeps its real length, because the stage line it
	 * logs is built from that number and a shorter one would not fill the
	 * buffer the way the shipped interval does. Sleeping costs nothing
	 * here: nanosleep() above returns at once. */
	detach_confirm_interval_ms = DETACH_CONFIRM_INTERVAL_MS;

	MU_RUN(a_dfu_board_in_use_goes_through_the_shared_report_and_is_left_alone);
	MU_RUN(a_read_back_with_reset_writes_the_dump_and_detaches);
	MU_RUN(a_bootloader_that_never_reports_its_state_ends_the_read_back);
	MU_RUN(a_bootloader_that_never_reports_its_state_ends_a_bare_reset);
	MU_RUN(a_state_this_host_cannot_drive_ends_the_command);
	MU_RUN(a_half_finished_transfer_is_aborted_before_the_read_back);
	MU_RUN(a_read_back_failing_part_way_still_detaches_with_reset);
	MU_RUN(a_read_back_failing_without_reset_leaves_the_board_in_dfu);
	MU_RUN(a_transient_read_back_failure_is_retried_once_and_succeeds);
	MU_RUN(a_refused_detach_fails_the_command);
	MU_RUN(a_detach_that_fails_with_pipe_as_the_board_resets_passes);
	MU_RUN(a_detach_that_fails_with_no_device_as_the_board_resets_passes);
	MU_RUN(a_detach_that_fails_with_io_as_the_board_resets_passes);
	MU_RUN(a_detach_that_fails_with_an_unnamed_error_as_the_board_resets_passes);
	MU_RUN(a_failed_detach_with_the_dfu_device_still_there_fails_the_command);
	MU_RUN(a_bare_reset_whose_detach_races_the_reset_passes);
	MU_RUN(a_detach_with_no_usb_path_says_it_could_not_tell);
	MU_RUN(a_detach_that_is_accepted_and_ignored_fails_a_bare_reset);
	MU_RUN(a_detach_that_is_accepted_and_ignored_fails_a_read_back);
	MU_RUN(a_detach_that_is_accepted_and_ignored_fails_before_verification);
	MU_RUN(an_accepted_detach_with_no_usb_path_says_it_could_not_tell);
	MU_RUN(a_detach_race_followed_by_the_old_firmware_fails_the_command);
	MU_RUN(a_board_that_comes_back_in_the_bootloader_fails_a_bare_reset);
	MU_RUN(a_board_that_comes_back_in_the_bootloader_fails_a_read_back);
	MU_RUN(a_board_that_comes_back_in_the_bootloader_fails_before_verification);
	MU_RUN(a_confirming_listing_that_cannot_be_read_says_it_could_not_tell);
	MU_RUN(one_unreadable_confirming_listing_is_tried_again);
	MU_RUN(a_board_that_went_names_no_libusb_status_when_the_bus_will_not_read);
	MU_RUN(a_board_back_in_the_bootloader_names_no_libusb_status);
	MU_RUN(an_image_for_another_api_version_is_warned_about_and_written);
	MU_RUN(a_board_running_the_other_image_fails_the_verification);
	MU_RUN(a_board_that_will_not_name_its_image_fails_the_verification);
	MU_RUN(a_board_that_does_not_come_back_says_the_firmware_was_written);
	MU_RUN(the_search_for_the_returned_application_tells_its_outcomes_apart);
	MU_RUN(a_board_that_goes_between_the_listing_and_the_open_is_reported_as_absent);
	MU_RUN(a_board_that_will_not_be_claimed_is_not_reported_as_missing);
	MU_RUN(a_board_that_will_not_report_its_revision_says_the_flash_is_unchecked);
	MU_RUN(asking_past_the_end_of_flash_takes_the_bootloader_down);
	MU_RUN(a_board_already_in_dfu_reads_the_assumed_flash_only);
	MU_RUN(a_128_kib_part_is_read_to_the_end_of_its_flash);
	MU_RUN(a_256_kib_part_is_read_in_full);
	MU_RUN(an_unknown_part_id_falls_back_to_the_assumed_flash);
	MU_RUN(a_part_id_that_cannot_be_read_falls_back_to_the_assumed_flash);
	MU_RUN(a_download_writes_the_first_sector_last);
	MU_RUN(an_image_inside_the_first_sector_is_written_in_the_same_order);
	MU_RUN(the_vector_table_written_last_carries_its_checksum);
	MU_RUN(a_stale_checksum_in_the_image_is_written_over);
	MU_RUN(a_download_that_stops_part_way_sends_no_vector_table);
	MU_RUN(a_download_records_what_it_wrote);
	MU_RUN(the_record_goes_in_with_the_vector_table);
	MU_RUN(a_region_the_board_did_not_get_whole_does_not_add_up);
	MU_RUN(the_tool_writes_over_the_checksum_word_and_the_record_only);
	MU_RUN(an_image_with_no_check_block_is_written_unchanged);
	MU_RUN(a_stale_record_in_the_image_is_written_over);
	MU_RUN(an_image_that_fills_the_part_is_written);
	MU_RUN(an_image_one_byte_past_the_part_is_refused);
	MU_RUN(an_image_with_no_info_block_is_refused);
	MU_RUN(an_image_with_an_unknown_block_format_is_refused);
	MU_RUN(an_unknown_part_warns_and_writes_the_image);
	MU_RUN(an_image_too_large_for_one_part_fits_a_bigger_one);
	MU_RUN(an_image_past_a_256_kib_part_is_refused_too);
	MU_RUN(a_part_that_cannot_be_read_warns_and_writes_the_image);
	MU_RUN(a_board_already_in_dfu_warns_that_the_image_is_not_checked);
	MU_RUN(a_bare_reset_does_not_ask_for_the_part_id);
	MU_RUN(a_flash_with_reset_reports_the_revision_on_standard_output);
	MU_RUN(a_read_back_says_nothing_on_standard_output);
	MU_RUN(a_bare_reset_says_nothing_on_standard_output);
	MU_RUN(a_failed_flash_reports_errors_only_on_standard_error);
	MU_RUN(a_flash_failing_without_a_stall_still_keeps_standard_output_empty);
	MU_RUN(a_bootloader_that_stalls_the_state_read_reports_one_error_per_line);
	MU_RUN(a_flash_failing_part_way_finishes_its_row_of_dots_first);
	MU_RUN(a_status_read_failing_part_way_finishes_its_row_of_dots_first);
	MU_RUN(a_read_back_failing_part_way_finishes_its_row_of_dots_first);

	remove_dump();
	unlink(image_path);
	unlink(sized_image_path);
	rmdir(directory);
	return mu_report();
}
