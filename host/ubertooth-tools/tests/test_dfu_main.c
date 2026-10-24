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

/* The flash region -u reads back, and its length in the dump. */
#define DUMP_ADDRESS (BOOTLOADER_OFFSET + BOOTLOADER_SIZE)
#define DUMP_LENGTH ((256 * 1024) - DUMP_ADDRESS)
#define FIRST_BLOCK (DUMP_ADDRESS / BLOCK_SIZE)
#define LAST_BLOCK (FIRST_BLOCK + (DUMP_LENGTH / BLOCK_SIZE) - 1)

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
	int state;              /* the state GETSTATE reports */
	int get_state_rc;       /* < 0: GETSTATE fails with this instead */
	int get_state_calls;
	int upload_calls;
	int upload_fail_block;  /* this block fails to read, or -1 */
	int upload_fail_rc;
	int upload_fail_once;   /* ...and then reads normally */
	int detach_calls;
	int detach_rc;
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

/* A board sitting in the bootloader, answering everything. */
static void board_in_dfu(void)
{
	memset(&fake, 0, sizeof(fake));
	fake.present = 1;
	fake.vid = U1_DFU_VENDORID;
	fake.pid = U1_DFU_PRODUCTID;
	fake.state = STATE_DFU_IDLE;
	fake.upload_fail_block = -1;
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
		mu_assert(value >= FIRST_BLOCK && value <= LAST_BLOCK,
		          "UPLOAD asked for a block outside the application region");
		fake.upload_calls++;
		if (fake.upload_fail_block >= 0 && value == fake.upload_fail_block) {
			if (fake.upload_fail_once)
				fake.upload_fail_block = -1;
			return fake.upload_fail_rc;
		}
		address = (uint32_t)value * BLOCK_SIZE;
		for (i = 0; i < length; i++)
			data[i] = flash_byte(address + i);
		fake.state = STATE_DFU_IDLE;
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

/* No test here uses -U, so no application device is ever opened: these exist
 * for the link, and fail the way a missing board does. */
ubertooth_t *ubertooth_start_select(const char *u_arg)
{
	(void)u_arg;
	return NULL;
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

int cmd_get_firmware_revision(struct libusb_device_handle *devh, char *revision,
                              size_t revision_size)
{
	(void)devh;
	(void)revision;
	(void)revision_size;
	return -1;
}

/* ---- running main() and capturing what it prints ------------------------ */

struct run_result {
	int rc;
	char out[8192];
	char err[4096];
};

static char dump_path[512];

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

static struct run_result run_dfu(const char *const *args)
{
	struct run_result result;
	char *argv[16];
	int argc = 0;
	int saved_out, saved_err;
	FILE *out = tmpfile();
	FILE *err = tmpfile();

	memset(&result, 0, sizeof(result));
	result.rc = -1; /* a failed capture must not look like a successful run */
	mu_assert(out != NULL && err != NULL, "tmpfile failed");
	if (out == NULL || err == NULL)
		return result;
	while (args[argc] != NULL && argc < 15) {
		argv[argc] = (char *)args[argc];
		argc++;
	}
	argv[argc] = NULL;

	fflush(stdout);
	fflush(stderr);
	saved_out = dup(STDOUT_FILENO);
	saved_err = dup(STDERR_FILENO);
	dup2(fileno(out), STDOUT_FILENO);
	dup2(fileno(err), STDERR_FILENO);
	optind = 0; /* restart option parsing for each run */
	result.rc = ubertooth_dfu_main(argc, argv);
	fflush(stdout);
	fflush(stderr);
	dup2(saved_out, STDOUT_FILENO);
	dup2(saved_err, STDERR_FILENO);
	close(saved_out);
	close(saved_err);

	read_back(out, result.out, sizeof(result.out));
	read_back(err, result.err, sizeof(result.err));
	fclose(out);
	fclose(err);
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

/* ---- #216: a bootloader that stops answering --------------------------- */

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

/* ---- #208: a read-back that fails part-way ----------------------------- */

MU_TEST(a_read_back_failing_part_way_still_detaches_with_reset)
{
	struct run_result r;
	int ignored;

	board_in_dfu();
	remove_dump();
	fake.upload_fail_block = FIRST_BLOCK + 100;
	fake.upload_fail_rc = LIBUSB_ERROR_OTHER;  /* as seen in #208 */
	r = RUN("-u", dump_path, "-r");
	/* The libusb status is reported, but the exit status is 1, not the
	 * truncated negative code the issue saw. */
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

/* The transient failure of #208: one block fails, and the retry reads the
 * whole region again from its first block. */
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

int main(void)
{
	char directory[] = "/tmp/ubertooth-dfu-test.XXXXXX";

	printf("DFU main tests\n");
	if (mkdtemp(directory) == NULL) {
		fprintf(stderr, "cannot create a working directory\n");
		return 1;
	}
	snprintf(dump_path, sizeof(dump_path), "%s/readback.bin", directory);

	MU_RUN(a_read_back_with_reset_writes_the_dump_and_detaches);
	MU_RUN(a_bootloader_that_never_reports_its_state_ends_the_read_back);
	MU_RUN(a_bootloader_that_never_reports_its_state_ends_a_bare_reset);
	MU_RUN(a_state_this_host_cannot_drive_ends_the_command);
	MU_RUN(a_half_finished_transfer_is_aborted_before_the_read_back);
	MU_RUN(a_read_back_failing_part_way_still_detaches_with_reset);
	MU_RUN(a_read_back_failing_without_reset_leaves_the_board_in_dfu);
	MU_RUN(a_transient_read_back_failure_is_retried_once_and_succeeds);
	MU_RUN(a_refused_detach_fails_the_command);

	remove_dump();
	rmdir(directory);
	return mu_report();
}
