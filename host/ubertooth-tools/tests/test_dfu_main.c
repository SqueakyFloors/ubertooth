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
#include <unistd.h>

#include <libusb.h>

#include "../../../firmware/tests/minunit.h"

#define main ubertooth_dfu_main
#include "ubertooth-dfu.c"
#undef main

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

/* What the faked flash holds at an address, so a dump can be checked byte for
 * byte against the region it claims to be. */
static uint8_t flash_byte(uint32_t address)
{
	return (uint8_t)((address * 7u + 0x5au) & 0xff);
}

/* ---- the faked device --------------------------------------------------- */

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
	int detach_calls;
	int detach_rc;
	int detach_resets;      /* the bootloader acts on REQ_DETACH and resets */
	char revision[64];      /* what the returned application reports, if any */
	int revision_calls;
	int abort_calls, clrstatus_calls, getstatus_calls;
	int opens, closes, claims, releases, claim_rc;
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
	fake.lists++;
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
		/* The bootloader checks the address against 512 KiB whatever part it
		 * runs on, so a read past the end of this part's flash is a bus
		 * fault that it never returns from: it stops answering for good
		 * instead of refusing the request. */
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
		fake.dnload_calls++;
		return length;
	case REQ_GETSTATUS:
		fake.getstatus_calls++;
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
		 * happens whatever the request itself returns, which is the whole
		 * point of #267: it is answered by a device that is going away. */
		if (fake.detach_resets) {
			fake.vid = U1_VENDORID;
			fake.pid = U1_PRODUCTID;
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
	fprintf(stderr, "libUSB Error: fake: (%d)\n", error_code);
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

/* ---- running main() and capturing what it prints ------------------------ */

struct run_result {
	int rc;
	char out[8192];
	char err[4096];
};

static char dump_path[512];
static char image_path[512];

static void read_back(FILE *file, char *buffer, size_t size)
{
	size_t length;

	fflush(file);
	rewind(file);
	length = fread(buffer, 1, size - 1, file);
	buffer[length] = '\0';
	/* A capture that did not fit must fail loudly instead of comparing a
	 * prefix. The dots of a read-back are most of it. */
	mu_assert(fgetc(file) == EOF, "captured output was truncated");
}

/* The two streams of one run, held aside while it prints into temporary
 * files. A read-back prints a dot per block, so nothing here is left on the
 * test's own output. */
static struct {
	FILE *out, *err;
	int saved_out, saved_err;
} capture;

static void begin_capture(void)
{
	capture.out = tmpfile();
	capture.err = tmpfile();
	/* mu_assert() counts a failure and carries on, which here would mean
	 * dup2(fileno(NULL), ...): stop instead. */
	if (capture.out == NULL || capture.err == NULL) {
		fprintf(stderr, "tmpfile failed\n");
		exit(1);
	}
	fflush(stdout);
	fflush(stderr);
	capture.saved_out = dup(STDOUT_FILENO);
	capture.saved_err = dup(STDERR_FILENO);
	dup2(fileno(capture.out), STDOUT_FILENO);
	dup2(fileno(capture.err), STDERR_FILENO);
}

static void end_capture(struct run_result *result)
{
	fflush(stdout);
	fflush(stderr);
	dup2(capture.saved_out, STDOUT_FILENO);
	dup2(capture.saved_err, STDERR_FILENO);
	close(capture.saved_out);
	close(capture.saved_err);
	read_back(capture.out, result->out, sizeof(result->out));
	read_back(capture.err, result->err, sizeof(result->err));
	fclose(capture.out);
	fclose(capture.err);
}

static struct run_result run_dfu(const char *const *args)
{
	struct run_result result;
	char *argv[16];
	int argc = 0;

	memset(&result, 0, sizeof(result));
	result.rc = -1; /* a failed capture must not look like a successful run */
	while (args[argc] != NULL && argc < 15) {
		argv[argc] = (char *)args[argc];
		argc++;
	}
	argv[argc] = NULL;

	begin_capture();
	optind = 0; /* restart option parsing for each run */
	result.rc = ubertooth_dfu_main(argc, argv);
	end_capture(&result);
	/* Every handle opened must have been closed, and every list freed. */
	mu_assert_int_eq(fake.opens, fake.closes);
	mu_assert_int_eq(0, fake.lists);
	return result;
}

#define RUN(...) run_dfu((const char *const[]){ "ubertooth-dfu", __VA_ARGS__, NULL })

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

#define IMAGE_REVISION "git-2671234"
/* Three blocks of payload, so a download sends several blocks and the suffix
 * falls into a fourth. */
#define IMAGE_PAYLOAD (3 * BLOCK_SIZE)
#define IMAGE_BLOCKS 4

/* A signed DFU file for this device whose compile-information marker names
 * revision. -d checks the whole image and -r reads the revision it expects
 * out of it, so these runs need a real one; sign() is the tool's own. */
static void write_image(const char *revision)
{
	FILE *payload = tmpfile();
	FILE *image = fopen(image_path, "w+b");
	char marker[128];
	int length, i;

	if (payload == NULL || image == NULL) {
		fprintf(stderr, "cannot create the test image\n");
		exit(1);
	}
	length = snprintf(marker, sizeof(marker), "ubertooth %s (tests) now",
	                  revision);
	fwrite(marker, 1, (size_t)length + 1, payload);
	for (i = length + 1; i < IMAGE_PAYLOAD; i++)
		fputc(0xff, payload);
	if (sign(payload, image, U1_DFU_VENDORID, U1_DFU_PRODUCTID) != 0) {
		fprintf(stderr, "cannot sign the test image\n");
		exit(1);
	}
	fclose(payload);
	fclose(image);
}

/* ---- a read-back that works, as the baseline --------------------------- */

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
	mu_assert(strstr(r.out, "Detached\n") != NULL, "the detach was not reported");
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
	mu_assert(strstr(r.err, "Firmware upload failed after 2 attempt(s)") != NULL,
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
	mu_assert(strstr(r.err, "Detach failed: could not enter DFU mode") != NULL,
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
	mu_assert(strstr(r.err, "Firmware upload failed after 2 attempt(s)") != NULL,
	          "the read-back failure was not reported");
	mu_assert(strstr(r.err, "Upload failed: block 164 was not read") != NULL,
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
	mu_assert(strstr(r.err, "Detach failed: the board is still in DFU") != NULL,
	          "the refused detach was not reported");
	/* The dump itself arrived, so it is kept. */
	mu_assert_int_eq(DUMP_LENGTH, dump_length_and_content(&content_ok));
	mu_assert(content_ok, "the dump is not the region it read");
	/* Cleanup tries the detach once more with the handle it still has. */
	mu_assert_int_eq(2, fake.detach_calls);
	remove_dump();
}

/* ---- #267: a detach whose request fails because the board reset -------- */

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

static void assert_flash_passed(struct run_result *r)
{
	mu_assert_int_eq(0, r->rc);
	mu_assert_int_eq(IMAGE_BLOCKS, fake.dnload_calls);
	mu_assert(strstr(r->out, "Detached\n") != NULL,
	          "the detach was not reported");
	mu_assert(strstr(r->err, "Verified firmware revision: " IMAGE_REVISION)
	          != NULL, "the flashed revision was not verified");
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
	mu_assert(strstr(r.err, "Detach failed: the board is still in DFU") != NULL,
	          "a board left in DFU was not reported");
	mu_assert(strstr(r.err, "Verified firmware revision") == NULL,
	          "a board still in DFU was reported as verified");
	mu_assert_int_eq(0, fake.revision_calls);
	/* Cleanup still tries once more to get it out of the bootloader. */
	mu_assert_int_eq(2, fake.detach_calls);
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
	mu_assert(strstr(r.out, "Detached\n") != NULL,
	          "the detach was not reported");
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
	mu_assert(strstr(r.err, "Post-reset verification failed") != NULL,
	          "the unchanged revision was not reported");
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
	mu_assert(strstr(r.out, "Reading back 114688 bytes, 0x4000-0x1ffff: ")
	          != NULL, "the read-back did not say what it reads");
	mu_assert(strstr(r.out, "the part could not be read") != NULL,
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
	mu_assert(strstr(r.out,
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
	mu_assert(strstr(r.out,
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
	mu_assert(strstr(r.out, "part 0x12345678 is not known here") != NULL,
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
	mu_assert(strstr(r.out, "the part could not be read") != NULL,
	          "the unreadable part ID was not reported");
	remove_dump();
}

/* The part ID is only wanted for a read-back, and reading it has its own
 * error output and timeout, so a command that would ignore the answer does
 * not ask. A bare reset through the application is that case; a download
 * takes the same gate. */
MU_TEST(a_command_that_is_not_a_read_back_does_not_ask_for_the_part_id)
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
	write_image(IMAGE_REVISION);

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
	MU_RUN(a_failed_detach_with_the_dfu_device_still_there_fails_the_command);
	MU_RUN(a_bare_reset_whose_detach_races_the_reset_passes);
	MU_RUN(a_detach_race_followed_by_the_old_firmware_fails_the_command);
	MU_RUN(asking_past_the_end_of_flash_takes_the_bootloader_down);
	MU_RUN(a_board_already_in_dfu_reads_the_assumed_flash_only);
	MU_RUN(a_128_kib_part_is_read_to_the_end_of_its_flash);
	MU_RUN(a_256_kib_part_is_read_in_full);
	MU_RUN(an_unknown_part_id_falls_back_to_the_assumed_flash);
	MU_RUN(a_part_id_that_cannot_be_read_falls_back_to_the_assumed_flash);
	MU_RUN(a_command_that_is_not_a_read_back_does_not_ask_for_the_part_id);

	remove_dump();
	unlink(image_path);
	rmdir(directory);
	return mu_report();
}
