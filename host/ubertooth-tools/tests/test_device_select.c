/*
 * Hardware-independent tests for libubertooth device selection (-U<index>).
 *
 * The real ubertooth.c is compiled into this test and the test defines the
 * libusb device-list calls itself, so the library enumerates a faked USB bus
 * instead of the real one. That makes the multi-device cases testable without
 * two boards: an index that is not present must open nothing, and no index may
 * ever be used to read past the end of the device list.
 *
 * The ring buffer is faked too (see fifo_init() below), so a selection does
 * not depend on 72 MB being free. An assertion about a handle the case then
 * uses is fatal: a plain one would record the failure, return, and let the
 * case segfault on the NULL handle, which reports a crash instead of the
 * assertion that failed.
 */
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libusb.h>

#include "../../../firmware/tests/minunit.h"
#include "ubertooth.h"
#include "ubertooth_fifo.h"

#define FAKE_MAX 8

/* One faked USB device: its descriptor, the serial number it reports, and a
 * handle the library may open. */
struct fake_dev {
	uint16_t vid, pid;
	uint16_t bcd;       /* the firmware API version the board reports */
	uint8_t serial[16]; /* the 16 bytes -s prints as 32 hex digits */
	int serial_fails;   /* set to fail this device's serial read */
	/* The firmware image this board reports in its info block. NULL stalls
	 * the read, which is how firmware built before the block answers. */
	const char *image;
	int handle; /* opaque body for the handle the fake libusb_open() hands out */
};

static struct {
	struct fake_dev devs[FAKE_MAX];
	int n;                  /* devices on the faked bus */
	ssize_t list_rc;        /* <0 to fail libusb_get_device_list() */
	int descriptor_fails;   /* bus index whose descriptor read fails, or -1 */
	int open_rc;            /* returned by libusb_open() */
	int claim_rc;           /* returned by libusb_claim_interface() */
	struct fake_dev *opened; /* the device the library opened, or NULL */
	int opens;              /* libusb_open() calls */
	int closes;             /* libusb_close() calls */
	int lists;              /* outstanding libusb_get_device_list() lists */
	int stops;              /* STOP requests sent to any board */
	int list_calls;         /* libusb_get_device_list() calls, not freed again */
	int usb_sessions;       /* libusb_init() minus libusb_exit() */
	int usb_inits;          /* libusb_init() calls */
	int usb_exits;          /* libusb_exit() calls */
} fake;

/* Put n Ubertooth One devices on the bus, preceded by one non-Ubertooth device
 * so that a bus index and an Ubertooth index are never the same number. */
static void bus_with_ubertooths(int n)
{
	int i;

	memset(&fake, 0, sizeof(fake));
	fake.descriptor_fails = -1;
	fake.devs[0].vid = 0x1d6b; /* a root hub: the kind of device an unchecked index opened */
	fake.devs[0].pid = 0x0001;
	for (i = 0; i < n; i++) {
		fake.devs[1 + i].vid = U1_VENDORID;
		fake.devs[1 + i].pid = U1_PRODUCTID;
		/* One version behind this tree, the state a development build
		 * exists for. */
		fake.devs[1 + i].bcd = UBERTOOTH_API_VERSION - 1;
		fake.devs[1 + i].image = "bluetooth_rxtx";
	}
	fake.n = n + 1;
	fake.list_rc = fake.n;
}

/* Put count boards in the DFU bootloader on the faked bus, after whatever
 * bus_with_ubertooths() left there. The bootloader is a USB device of its own:
 * no tool can open it, and it is what a board being flashed looks like. */
static void bus_with_bootloaders(int count)
{
	int i;

	for (i = 0; i < count; i++) {
		fake.devs[fake.n].vid = U1_DFU_VENDORID;
		fake.devs[fake.n].pid = U1_DFU_PRODUCTID;
		fake.n++;
	}
	fake.list_rc = fake.n;
}

/* Give the nth Ubertooth on the faked bus the serial number hex (32 lowercase
 * hex digits, as -s prints it), which its serial read then reports. */
static void set_serial(int n, const char *hex)
{
	int i;

	/* Checked once here, so the conversion below cannot fail. */
	mu_assert(strlen(hex) == 32
	          && strspn(hex, "0123456789abcdefABCDEF") == 32, hex);
	for (i = 0; i < 16; i++) {
		unsigned byte;

		sscanf(hex + i * 2, "%2x", &byte);
		fake.devs[1 + n].serial[i] = (uint8_t)byte;
	}
}

/* The faked bus used by the serial tests: two boards whose serials differ in
 * their leading digits and share their trailing ones, as real ones do. */
#define SERIAL_A "aa1100112233445566778899aabbccdd"
#define SERIAL_B "19bb00112233445566778899aabbccdd"

static void bus_with_two_serials(void)
{
	bus_with_ubertooths(2);
	set_serial(0, SERIAL_A);
	set_serial(1, SERIAL_B);
}

/* Two boards whose serials start alike (made up): "aa" is the start of both. */
#define SERIAL_C "aa2200112233445566778899aabbccdd"

static void bus_with_shared_prefix(void)
{
	bus_with_ubertooths(2);
	set_serial(0, SERIAL_A);
	set_serial(1, SERIAL_C);
}

/* ---- fifo fake ---------------------------------------------------------- */
/* ubertooth_fifo.c is not linked, so these are the fifo calls the library
 * gets. A real ring buffer is 72 MB (FIFO_SIZE packets) and every successful
 * selection allocates one; the cases below make several selections each, and
 * on a loaded machine one of those allocations can fail even though the faked
 * bus is fine. Nothing here reads a packet, so a one-byte stand-in does, and
 * selection no longer depends on 72 MB being available.
 *
 * Counted, not reset between cases, so one case can check that selection
 * allocated through this fake at all. (Linking ubertooth_fifo.c again would
 * not reach that check: it fails the link with duplicate symbols.) */
static int fifo_inits;

fifo_t* fifo_init()
{
	fifo_inits++;
	return (fifo_t*)malloc(1);
}

/* The rest of the interface, for the library's other references. None of it
 * runs here: no capture is started. */
void fifo_inc_write_ptr(fifo_t* fifo, uint64_t arrival_ns)
{ (void)fifo; (void)arrival_ns; }
void fifo_push(fifo_t* fifo, const usb_pkt_rx* packet, uint64_t arrival_ns)
{ (void)fifo; (void)packet; (void)arrival_ns; }
uint64_t fifo_popped_arrival_ns(const fifo_t* fifo) { (void)fifo; return 0; }
usb_pkt_rx fifo_pop(fifo_t* fifo) { usb_pkt_rx p; (void)fifo; memset(&p, 0, sizeof(p)); return p; }
usb_pkt_rx* fifo_get_write_element(fifo_t* fifo) { (void)fifo; return NULL; }
uint8_t fifo_empty(fifo_t* fifo) { (void)fifo; return 1; }

/* ---- libusb fakes ------------------------------------------------------- */
/* Defined here, so they take precedence over the libusb library symbols. */

int LIBUSB_CALL libusb_init(libusb_context **ctx)
{
	(void)ctx;
	fake.usb_sessions++;
	fake.usb_inits++;
	return 0;
}

void LIBUSB_CALL libusb_exit(libusb_context *ctx)
{
	(void)ctx;
	fake.usb_sessions--;
	fake.usb_exits++;
}

ssize_t LIBUSB_CALL libusb_get_device_list(libusb_context *ctx,
                                           libusb_device ***list)
{
	int i;

	(void)ctx;
	fake.list_calls++;
	if (fake.list_rc < 0) {
		*list = NULL;
		return fake.list_rc;
	}
	*list = calloc(fake.n + 1, sizeof(libusb_device *));
	if (*list == NULL)
		return LIBUSB_ERROR_NO_MEM;
	for (i = 0; i < fake.n; i++)
		(*list)[i] = (libusb_device *)&fake.devs[i];
	fake.lists++;
	return fake.list_rc;
}

void LIBUSB_CALL libusb_free_device_list(libusb_device **list,
                                         int unref_devices)
{
	(void)unref_devices;
	if (list != NULL) {
		free(list);
		fake.lists--;
	}
}

int LIBUSB_CALL libusb_get_device_descriptor(libusb_device *dev,
                                             struct libusb_device_descriptor *desc)
{
	struct fake_dev *fd = (struct fake_dev *)dev;

	if (fd - fake.devs == fake.descriptor_fails)
		return LIBUSB_ERROR_IO; /* desc left untouched, as libusb does */
	memset(desc, 0, sizeof(*desc));
	desc->idVendor = fd->vid;
	desc->idProduct = fd->pid;
	desc->bcdDevice = fd->bcd;
	return 0;
}

int LIBUSB_CALL libusb_open(libusb_device *dev, libusb_device_handle **devh)
{
	struct fake_dev *fd = (struct fake_dev *)dev;

	fake.opens++;
	if (fake.open_rc != 0) {
		*devh = NULL;
		return fake.open_rc;
	}
	fake.opened = fd;
	*devh = (libusb_device_handle *)&fd->handle;
	return 0;
}

void LIBUSB_CALL libusb_close(libusb_device_handle *devh)
{
	(void)devh;
	fake.closes++;
}

int LIBUSB_CALL libusb_claim_interface(libusb_device_handle *devh,
                                       int interface_number)
{
	(void)devh;
	(void)interface_number;
	return fake.claim_rc;
}

int LIBUSB_CALL libusb_release_interface(libusb_device_handle *devh,
                                         int interface_number)
{
	(void)devh;
	(void)interface_number;
	return 0;
}

/* Which faked device a handle belongs to, so a read answers with that
 * device's own data. */
static struct fake_dev *dev_of_handle(libusb_device_handle *devh)
{
	int i;

	for (i = 0; i < FAKE_MAX; i++)
		if (devh == (libusb_device_handle *)&fake.devs[i].handle)
			return &fake.devs[i];
	return NULL;
}

/* The device a handle was opened from, which is how ubertooth_get_api() finds
 * the descriptor libusb read while it enumerated. */
libusb_device * LIBUSB_CALL libusb_get_device(libusb_device_handle *devh)
{
	return (libusb_device *)dev_of_handle(devh);
}

/* Answers the serial-number read with the selected device's serial; every
 * other request succeeds with a zeroed reply, which is all selection needs. */
int LIBUSB_CALL libusb_control_transfer(libusb_device_handle *devh,
	uint8_t request_type, uint8_t bRequest, uint16_t wValue, uint16_t wIndex,
	unsigned char *data, uint16_t wLength, unsigned int timeout)
{
	struct fake_dev *fd = dev_of_handle(devh);

	(void)request_type;
	(void)wValue;
	(void)wIndex;
	(void)timeout;
	memset(data, 0, wLength);
	if (bRequest == UBERTOOTH_GET_SERIAL && wLength == 17 && fd != NULL) {
		if (fd->serial_fails)
			return LIBUSB_ERROR_TIMEOUT; /* a board that does not answer */
		memcpy(data + 1, fd->serial, sizeof(fd->serial)); /* data[0] is the status */
	}
	if (bRequest == UBERTOOTH_STOP)
		fake.stops++;
	if (bRequest == UBERTOOTH_GET_FW_INFO && fd != NULL) {
		if (fd->image == NULL)
			return LIBUSB_ERROR_PIPE; /* firmware without the block */
		/* The 64 bytes the firmware returns, built by offset as
		 * ubertooth_interface.h lays them out. */
		memcpy(data + UBERTOOTH_FW_INFO_OFF_MAGIC,
		       UBERTOOTH_FW_INFO_MAGIC, UBERTOOTH_FW_INFO_MAGIC_LEN);
		data[UBERTOOTH_FW_INFO_OFF_FORMAT] = UBERTOOTH_FW_INFO_FORMAT;
		data[UBERTOOTH_FW_INFO_OFF_API] = fd->bcd & 0xff;
		data[UBERTOOTH_FW_INFO_OFF_API + 1] = (fd->bcd >> 8) & 0xff;
		snprintf((char *)data + UBERTOOTH_FW_INFO_OFF_IMAGE,
		         UBERTOOTH_FW_INFO_IMAGE_LEN, "%s", fd->image);
		snprintf((char *)data + UBERTOOTH_FW_INFO_OFF_REVISION,
		         UBERTOOTH_FW_INFO_REVISION_LEN, "git-test");
	}
	return wLength;
}

/* ---- helpers ------------------------------------------------------------ */

/* Run the loud or the quiet selection of u_arg with stderr captured into
 * msg. */
static ubertooth_t *select_capturing_opt(const char *u_arg, int quiet,
                                         char *msg, size_t len)
{
	ubertooth_t *ut;
	FILE *capture = tmpfile();
	int saved = dup(fileno(stderr));
	size_t n;

	msg[0] = '\0';
	if (capture == NULL || saved < 0) {
		mu_assert(0, "capture stderr");
		return NULL;
	}
	fflush(stderr);
	dup2(fileno(capture), fileno(stderr));
	ut = quiet ? ubertooth_start_select_quiet(u_arg)
	           : ubertooth_start_select(u_arg);
	fflush(stderr);
	dup2(saved, fileno(stderr));
	close(saved);
	rewind(capture);
	n = fread(msg, 1, len - 1, capture);
	msg[n] = '\0';
	fclose(capture);
	return ut;
}

/* Run ubertooth_start_select(u_arg) with stderr captured into msg. */
static ubertooth_t *select_capturing(const char *u_arg, char *msg, size_t len)
{
	return select_capturing_opt(u_arg, 0, msg, len);
}

/* ubertooth_count(), with whatever it printed returned in msg. */
static int count_capturing(char *msg, size_t len)
{
	FILE *capture = tmpfile();
	int saved = dup(fileno(stderr));
	size_t n;
	int count;

	msg[0] = '\0';
	if (capture == NULL || saved < 0) {
		mu_assert(0, "capture stderr");
		return 0;
	}
	fflush(stderr);
	dup2(fileno(capture), fileno(stderr));
	count = ubertooth_count();
	fflush(stderr);
	dup2(saved, fileno(stderr));
	close(saved);
	rewind(capture);
	n = fread(msg, 1, len - 1, capture);
	msg[n] = '\0';
	fclose(capture);
	return count;
}

/* Run the shared report for a refused request with stderr captured, to see
 * which board's firmware API version it names. */
static void report_capturing(char *msg, size_t len)
{
	FILE *capture = tmpfile();
	int saved = dup(fileno(stderr));
	size_t n;

	msg[0] = '\0';
	if (capture == NULL || saved < 0) {
		mu_assert(0, "capture stderr");
		return;
	}
	fflush(stderr);
	dup2(fileno(capture), fileno(stderr));
	ubertooth_report_unsupported_request("set the channel", -9);
	fflush(stderr);
	dup2(saved, fileno(stderr));
	close(saved);
	rewind(capture);
	n = fread(msg, 1, len - 1, capture);
	msg[n] = '\0';
	fclose(capture);
}

/* ---- tests -------------------------------------------------------------- */

/* Opening a board keeps its firmware API version, so a report about a request
 * that board refuses can name it -- including from a tool that never asks for
 * the version, which ubertooth-util is for every option but -I. The version
 * is the board's USB device version, which libusb has already read, so no
 * request is sent to get it. Releasing the board takes it away again. */
MU_TEST(an_open_keeps_the_boards_api_version_for_later_reports)
{
	char msg[256], expected[128];
	ubertooth_t *ut;
	uint16_t board = UBERTOOTH_API_VERSION - 1;

	snprintf(expected, sizeof(expected),
	         "note: the board reports firmware API version %x.%02x,",
	         (board >> 8) & 0xFF, board & 0xFF);

	bus_with_ubertooths(1);
	ut = select_capturing(NULL, msg, sizeof(msg));
	mu_assert_fatal(ut != NULL, "the only device opens");

	report_capturing(msg, sizeof(msg));
	mu_assert(strstr(msg, expected) != NULL, msg);

	ubertooth_stop(ut);
	ubertooth_free(ut);

	report_capturing(msg, sizeof(msg));
	mu_assert(strstr(msg, "firmware API version") == NULL,
	          "the released board's version is still being quoted");
}

/* Runs ubertooth_check_api(ut) with stderr captured into msg. */
static int check_api_capturing(ubertooth_t *ut, char *msg, size_t len)
{
	FILE *capture = tmpfile();
	int saved = dup(fileno(stderr));
	size_t n;
	int result;

	msg[0] = '\0';
	if (capture == NULL || saved < 0) {
		mu_assert(0, "capture stderr");
		return 0;
	}
	fflush(stderr);
	dup2(fileno(capture), fileno(stderr));
	result = ubertooth_check_api(ut);
	fflush(stderr);
	dup2(saved, fileno(stderr));
	close(saved);
	rewind(capture);
	n = fread(msg, 1, len - 1, capture);
	msg[n] = '\0';
	fclose(capture);
	return result;
}

/* A board running this tree's other firmware image is refused by name.
 *
 * Both images report the same API version, so the version check lets such a
 * board through; the tool then failed on the first request btbr does not
 * answer, and reported a libusb error about a board that was simply carrying
 * the wrong firmware. */
MU_TEST(a_board_running_the_other_image_is_refused_by_name)
{
	char msg[512];
	ubertooth_t *ut;

	bus_with_ubertooths(1);
	fake.devs[1].bcd = UBERTOOTH_API_VERSION;
	fake.devs[1].image = "btbr";
	ut = select_capturing(NULL, msg, sizeof(msg));
	mu_assert_fatal(ut != NULL, "the only device opens");

	mu_assert_int_eq(-1, check_api_capturing(ut, msg, sizeof(msg)));
	mu_assert(strcmp(msg,
	                 "error: the Ubertooth is running btbr; this tool needs "
	                 "bluetooth_rxtx\n"
	                 "note: flash it with ubertooth-dfu -d "
	                 "firmware/bluetooth_rxtx/bluetooth_rxtx.dfu -r\n")
	          == 0, msg);
	/* released here, so a tool that returns next does not leave it claimed */
	mu_assert_int_eq(1, fake.closes);
	/* and released without a STOP: no mode has been started, and btbr is
	 * one of the images that does not answer STOP, so sending it would add
	 * a second failure -- about the teardown -- to this report */
	mu_assert_int_eq(0, fake.stops);
	ubertooth_free(ut);

	/* The note reflashes the board the user named and no other. */
	bus_with_two_serials();
	fake.devs[1].bcd = UBERTOOTH_API_VERSION;
	fake.devs[2].bcd = UBERTOOTH_API_VERSION;
	fake.devs[1].image = "btbr";
	ut = select_capturing(SERIAL_A, msg, sizeof(msg));
	mu_assert_fatal(ut != NULL, "the named board opens");
	mu_assert_int_eq(-1, check_api_capturing(ut, msg, sizeof(msg)));
	mu_assert(strstr(msg, "note: flash it with ubertooth-dfu -U "
	                 SERIAL_A " -d "
	                 "firmware/bluetooth_rxtx/bluetooth_rxtx.dfu -r\n")
	          != NULL, msg);
	ubertooth_free(ut);
}

/* The image every tool here needs passes, silently, and so does a board that
 * will not say which image it runs: firmware without the block is older than
 * this tree, which the version check refuses anyway, and what is left is a
 * board too busy to answer this read. Refusing that would turn a working
 * board away. */
MU_TEST(the_expected_image_and_an_unreadable_one_both_pass)
{
	char msg[512];
	ubertooth_t *ut;

	bus_with_ubertooths(1);
	fake.devs[1].bcd = UBERTOOTH_API_VERSION;
	ut = select_capturing(NULL, msg, sizeof(msg));
	mu_assert_fatal(ut != NULL, "the only device opens");
	mu_assert_int_eq(0, check_api_capturing(ut, msg, sizeof(msg)));
	mu_assert(msg[0] == '\0', msg);
	ubertooth_stop(ut);
	ubertooth_free(ut);

	bus_with_ubertooths(1);
	fake.devs[1].bcd = UBERTOOTH_API_VERSION;
	fake.devs[1].image = NULL; /* the read stalls */
	ut = select_capturing(NULL, msg, sizeof(msg));
	mu_assert_fatal(ut != NULL, "the only device opens");
	mu_assert_int_eq(0, check_api_capturing(ut, msg, sizeof(msg)));
	mu_assert(msg[0] == '\0', msg);
	ubertooth_stop(ut);
	ubertooth_free(ut);
}

/* Runs ubertooth_check_image(ut) with stderr captured into msg. */
static int check_image_capturing(ubertooth_t *ut, char *msg, size_t len)
{
	FILE *capture = tmpfile();
	int saved = dup(fileno(stderr));
	size_t n;
	int result;

	msg[0] = '\0';
	if (capture == NULL || saved < 0) {
		mu_assert(0, "capture stderr");
		return 0;
	}
	fflush(stderr);
	dup2(fileno(capture), fileno(stderr));
	result = ubertooth_check_image(ut);
	fflush(stderr);
	dup2(saved, fileno(stderr));
	close(saved);
	rewind(capture);
	n = fread(msg, 1, len - 1, capture);
	msg[n] = '\0';
	fclose(capture);
	return result;
}

/* The image check on its own, which is what ubertooth-debug calls: it refuses
 * the other image with the same message, and says nothing about the firmware
 * version, so a board carrying an older revision of the right image is read
 * as the debugging work it is. */
MU_TEST(the_image_check_alone_ignores_the_firmware_version)
{
	char msg[512];
	ubertooth_t *ut;

	bus_with_ubertooths(1);
	fake.devs[1].bcd = UBERTOOTH_API_VERSION - 1;
	fake.devs[1].image = "btbr";
	ut = select_capturing(NULL, msg, sizeof(msg));
	mu_assert_fatal(ut != NULL, "the only device opens");
	mu_assert_int_eq(-1, check_image_capturing(ut, msg, sizeof(msg)));
	mu_assert(strcmp(msg,
	                 "error: the Ubertooth is running btbr; this tool needs "
	                 "bluetooth_rxtx\n"
	                 "note: flash it with ubertooth-dfu -d "
	                 "firmware/bluetooth_rxtx/bluetooth_rxtx.dfu -r\n")
	          == 0, msg);
	mu_assert_int_eq(1, fake.closes);
	mu_assert_int_eq(0, fake.stops);
	ubertooth_free(ut);

	bus_with_ubertooths(1);
	fake.devs[1].bcd = UBERTOOTH_API_VERSION - 1;
	ut = select_capturing(NULL, msg, sizeof(msg));
	mu_assert_fatal(ut != NULL, "the only device opens");
	mu_assert_int_eq(0, check_image_capturing(ut, msg, sizeof(msg)));
	mu_assert(msg[0] == '\0', msg);
	mu_assert_int_eq(0, fake.closes);
	ubertooth_stop(ut);
	ubertooth_free(ut);
}

MU_TEST(an_index_in_range_opens_that_device)
{
	char msg[256];
	ubertooth_t *ut;

	bus_with_ubertooths(3);
	ut = select_capturing("1", msg, sizeof(msg));
	mu_assert_fatal(ut != NULL, "-U1 of three devices opens one");
	mu_assert(fake.opened == &fake.devs[2], "-U1 opens the second Ubertooth");
	mu_assert(msg[0] == '\0', msg);
	ubertooth_stop(ut);
	ubertooth_free(ut);

	bus_with_ubertooths(3);
	ut = select_capturing("2", msg, sizeof(msg));
	mu_assert_fatal(ut != NULL, "-U2 of three devices opens one");
	mu_assert(fake.opened == &fake.devs[3], "-U2 opens the third Ubertooth");
	ubertooth_stop(ut);
	ubertooth_free(ut);
}

/* With several devices attached, -U2 used to open usb_list[0] (whatever USB
 * device enumerated first) and the tool exited 0. */
MU_TEST(an_index_past_several_devices_opens_nothing)
{
	static const int past_end[] = { 2, 3, 5, 20, 99 };
	char msg[256];
	size_t i;

	for (i = 0; i < sizeof(past_end) / sizeof(past_end[0]); i++) {
		char arg[8];

		bus_with_ubertooths(2);
		snprintf(arg, sizeof(arg), "%d", past_end[i]);
		mu_assert(select_capturing(arg, msg, sizeof(msg)) == NULL, arg);
		mu_assert(fake.opened == NULL, "no device opened");
		mu_assert_int_eq(0, fake.opens);
		mu_assert(strstr(msg, "no Ubertooth at index") != NULL, msg);
	}
}

/* An all-digits index too large for an int must not be converted into one
 * that selects a device: atoi() read 4294967296 as index 0 and 2147483648 as
 * a negative index, which means "the only device". */
MU_TEST(an_index_too_large_for_an_int_opens_nothing)
{
	static const char *const too_large[] = {
		"2147483648",           /* INT_MAX + 1 */
		"4294967296",           /* 2^32 */
		"9223372036854775808",  /* LONG_MAX + 1 on 64-bit */
		"99999999999999999999999999"
	};
	char msg[512], expected[512];
	size_t i;

	for (i = 0; i < sizeof(too_large) / sizeof(too_large[0]); i++) {
		bus_with_ubertooths(1);
		mu_assert(select_capturing(too_large[i], msg, sizeof(msg)) == NULL,
		          too_large[i]);
		mu_assert(fake.opened == NULL, too_large[i]);
		mu_assert_int_eq(0, fake.opens);
		/* The whole message: the index as given, the count and the hint, and
		 * not the "could not open" line, since no device was looked for. */
		snprintf(expected, sizeof(expected),
		         "error: no Ubertooth at index %s (1 attached)\n"
		         "indexes change when boards are added or removed;"
		         " select a board by serial with -U<serial>\n",
		         too_large[i]);
		mu_assert(strcmp(msg, expected) == 0, msg);

		bus_with_ubertooths(2);
		mu_assert(select_capturing(too_large[i], msg, sizeof(msg)) == NULL,
		          too_large[i]);
		mu_assert_int_eq(0, fake.opens);
		mu_assert_int_eq(0, fake.lists);

		/* and the quiet path refuses it without a message */
		bus_with_ubertooths(1);
		mu_assert(ubertooth_start_select_quiet(too_large[i]) == NULL,
		          too_large[i]);
		mu_assert_int_eq(0, fake.opens);
	}
}

/* The largest index that does fit is still refused, but as an ordinary index
 * that is not attached. */
MU_TEST(the_largest_representable_index_is_an_ordinary_refusal)
{
	char msg[256];
	ubertooth_t *ut;

	bus_with_ubertooths(2);
	mu_assert(select_capturing("2147483647", msg, sizeof(msg)) == NULL, msg);
	mu_assert(strcmp(msg,
	    "error: no Ubertooth at index 2147483647 (2 attached)\n"
	    "indexes change when boards are added or removed; select a board by serial with -U<serial>\n") == 0, msg);

	/* Leading zeros are still just digits. */
	bus_with_ubertooths(2);
	ut = select_capturing("01", msg, sizeof(msg));
	mu_assert_fatal(ut != NULL, msg);
	mu_assert(fake.opened == &fake.devs[2], "-U01 is index 1");
	ubertooth_stop(ut);
	ubertooth_free(ut);
}

/* The same refusal with exactly one device attached: the index is a position
 * in the list of found Ubertooths, and only 0 is a position in a list of one.
 * This used to open that device whatever the index, which meant -U1 targeted
 * the second board with two attached and the only board with one. */
MU_TEST(a_nonzero_index_with_one_device_opens_nothing)
{
	char msg[256];

	bus_with_ubertooths(1);
	mu_assert(select_capturing("1", msg, sizeof(msg)) == NULL, "-U1 of one");
	mu_assert_int_eq(0, fake.opens);
	mu_assert(strstr(msg, "no Ubertooth at index 1 (1 attached)") != NULL, msg);

	bus_with_ubertooths(1);
	mu_assert(select_capturing("5", msg, sizeof(msg)) == NULL, "-U5 of one");
	mu_assert(strstr(msg, "no Ubertooth at index 5 (1 attached)") != NULL, msg);
}

MU_TEST(index_zero_and_no_index_open_the_only_device)
{
	char msg[256];
	ubertooth_t *ut;

	bus_with_ubertooths(1);
	ut = select_capturing("0", msg, sizeof(msg));
	mu_assert_fatal(ut != NULL, "-U0 opens the only device");
	mu_assert(fake.opened == &fake.devs[1], "and it is the Ubertooth");
	mu_assert(msg[0] == '\0', msg);
	ubertooth_stop(ut);
	ubertooth_free(ut);

	bus_with_ubertooths(1);
	ut = select_capturing(NULL, msg, sizeof(msg));
	mu_assert_fatal(ut != NULL, "no -U opens the only device");
	mu_assert(fake.opened == &fake.devs[1], "and it is the Ubertooth");
	ubertooth_stop(ut);
	ubertooth_free(ut);
}

/* A handle remembers the board the user named, as they wrote it: that is what
 * lets a message about this one board say which it is, rather than offering to
 * reflash every board attached. Where they named none, there is no board to
 * name, and the message says so instead of naming the one the library
 * happened to open. */
MU_TEST(a_handle_remembers_the_board_the_user_named)
{
	char msg[256];
	ubertooth_t *ut;

	bus_with_ubertooths(3);
	ut = select_capturing("1", msg, sizeof(msg));
	mu_assert_fatal(ut != NULL, "-U1 of three devices opens one");
	mu_assert(ut->selector != NULL && strcmp(ut->selector, "1") == 0,
	          "the index the user gave");
	ubertooth_stop(ut);
	ubertooth_free(ut);

	bus_with_two_serials();
	ut = select_capturing(SERIAL_A, msg, sizeof(msg));
	mu_assert_fatal(ut != NULL, "a full serial opens that board");
	mu_assert(ut->selector != NULL && strcmp(ut->selector, SERIAL_A) == 0,
	          "the serial the user gave");
	ubertooth_stop(ut);
	ubertooth_free(ut);

	bus_with_ubertooths(1);
	ut = select_capturing(NULL, msg, sizeof(msg));
	mu_assert_fatal(ut != NULL, "no -U opens the only device");
	mu_assert(ut->selector == NULL, "no board was named");
	ubertooth_stop(ut);
	ubertooth_free(ut);

	/* The other form of the same selection, for the tools that allocate a
	 * handle and then connect it. */
	bus_with_ubertooths(3);
	ut = ubertooth_init();
	mu_assert_fatal(ut != NULL, "cannot allocate a handle");
	mu_assert(ubertooth_connect_select(ut, "2") >= 0, "-U2 of three connects");
	mu_assert(ut->selector != NULL && strcmp(ut->selector, "2") == 0,
	          "the index the user gave");
	ubertooth_stop(ut);
	ubertooth_free(ut);

	/* A handle that has opened no device names no board. Nothing has set
	 * the field at that point, so a message about "this one board" printed
	 * from such a handle would print whatever the heap last held there. */
	bus_with_ubertooths(1);
	ut = ubertooth_init();
	mu_assert_fatal(ut != NULL, "cannot allocate a handle");
	mu_assert(ut->selector == NULL, "a fresh handle names no board");
	ubertooth_free(ut);

	/* And the quiet selection the reset wait polls with, which reopens the
	 * board the user named. */
	bus_with_two_serials();
	ut = ubertooth_start_select_quiet(SERIAL_B);
	mu_assert_fatal(ut != NULL, "the quiet selection opens that board");
	mu_assert(ut->selector != NULL && strcmp(ut->selector, SERIAL_B) == 0,
	          "the serial the user gave");
	ubertooth_stop(ut);
	ubertooth_free(ut);

	bus_with_ubertooths(1);
	ut = ubertooth_start_select_quiet(NULL);
	mu_assert_fatal(ut != NULL, "the quiet selection opens the only board");
	mu_assert(ut->selector == NULL, "no board was named");
	ubertooth_stop(ut);
	ubertooth_free(ut);
}

MU_TEST(the_error_names_the_index_and_the_count)
{
	char msg[256];

	bus_with_ubertooths(2);
	mu_assert(select_capturing("7", msg, sizeof(msg)) == NULL, "-U7 of two");
	mu_assert(strcmp(msg,
	    "error: no Ubertooth at index 7 (2 attached)\n"
	    "indexes change when boards are added or removed; select a board by serial with -U<serial>\n") == 0, msg);
}

/*
 * Several boards and no -U: one error: line saying what to do, then the
 * boards, so the list is something to read rather than part of the refusal.
 * It used to be "multiple Ubertooth devices found! Use '-U' to specify device
 * number" -- no prefix, and no mention that -U takes a serial as well -- and
 * the connect added a second unprefixed line, "could not open Ubertooth
 * device", that named no cause of its own.
 */
MU_TEST(several_devices_with_no_index_still_list_them)
{
	char msg[512];

	bus_with_two_serials();
	mu_assert(select_capturing(NULL, msg, sizeof(msg)) == NULL, "no -U of two");
	mu_assert(strcmp(msg,
	    "error: multiple Ubertooth devices found; select one with "
	    "-U <index|serial>\n"
	    "  Device 0: " SERIAL_A "\n"
	    "  Device 1: " SERIAL_B "\n") == 0, msg);

	/* A board whose serial could not be read is still on the list, because
	 * it is still a board the next command has to choose between: it is
	 * named by its index, which is how it can be selected. */
	bus_with_two_serials();
	fake.devs[1].serial_fails = 1; /* the first Ubertooth on the bus */
	mu_assert(select_capturing(NULL, msg, sizeof(msg)) == NULL, "no -U of two");
	mu_assert(strcmp(msg,
	    "error: multiple Ubertooth devices found; select one with "
	    "-U <index|serial>\n"
	    "  Device 0: serial unreadable\n"
	    "  Device 1: " SERIAL_B "\n") == 0, msg);

	/* A board that would not open at all is listed with libusb's reason,
	 * because that is the useful part: on Linux it is usually the udev
	 * rules not being installed. */
	bus_with_two_serials();
	fake.open_rc = LIBUSB_ERROR_ACCESS;
	mu_assert(select_capturing(NULL, msg, sizeof(msg)) == NULL, "no -U of two");
	mu_assert(strstr(msg, "\n  Device 0: could not be opened: ") != NULL, msg);
	mu_assert(strstr(msg, "(libusb -3)\n") != NULL, msg);
	mu_assert(strstr(msg + 7, "error: ") == NULL, msg);
}

MU_TEST(no_device_attached_opens_nothing)
{
	char msg[256];

	bus_with_ubertooths(0); /* the hub only */
	mu_assert(select_capturing("0", msg, sizeof(msg)) == NULL, "-U0 of none");
	mu_assert_int_eq(0, fake.opens);
	mu_assert(strstr(msg, "error: no Ubertooth devices found") != NULL, msg);

	bus_with_ubertooths(0);
	mu_assert(select_capturing(NULL, msg, sizeof(msg)) == NULL, "no -U of none");
	mu_assert_int_eq(0, fake.opens);
}

/* An empty or failing bus must not be enumerated or freed as if it had
 * devices, and the device list must be freed on every path. */
MU_TEST(an_empty_or_failing_bus_is_refused)
{
	char msg[256];

	bus_with_ubertooths(0);
	fake.n = 0;
	fake.list_rc = 0;
	mu_assert(select_capturing("0", msg, sizeof(msg)) == NULL, "empty bus");
	mu_assert_int_eq(0, fake.lists);

	/* A bus that could not be listed is not an empty one, and used to be
	 * reported as one -- or, by index, as nothing at all. */
	bus_with_ubertooths(0);
	fake.list_rc = LIBUSB_ERROR_IO;
	mu_assert(select_capturing("0", msg, sizeof(msg)) == NULL, "failed list");
	mu_assert_int_eq(0, fake.lists);
	mu_assert(strstr(msg, "error: could not list USB devices") == msg, msg);
	mu_assert(strstr(msg, "no Ubertooth") == NULL, msg);

	/* The serial path says the same thing, and records the same reason. */
	bus_with_ubertooths(0);
	fake.list_rc = LIBUSB_ERROR_IO;
	mu_assert(select_capturing("aa11", msg, sizeof(msg)) == NULL, "failed list");
	mu_assert_int_eq(0, fake.lists);
	mu_assert(strstr(msg, "error: could not list USB devices") == msg, msg);
	mu_assert(strstr(msg, "no Ubertooth") == NULL, msg);
	mu_assert(strcmp(ubertooth_open_failure_reason(),
	                 "the USB devices could not be listed") == 0,
	          ubertooth_open_failure_reason());

	/* And the count is one the caller can tell from an empty bus. It used to
	 * be 0, so -U all said "no Ubertooth devices found" for a bus nobody
	 * could read. The line is said once: this count is polled every 100 ms
	 * while ubertooth-util -r waits for a board, and the failure persists. */
	bus_with_ubertooths(0);
	fake.list_rc = LIBUSB_ERROR_IO;
	mu_assert_int_eq(-1, count_capturing(msg, sizeof(msg)));
	mu_assert(strstr(msg, "error: could not list USB devices") == msg, msg);
	mu_assert_int_eq(-1, count_capturing(msg, sizeof(msg)));
	mu_assert(msg[0] == '\0', msg);
	mu_assert_int_eq(0, fake.lists);

	bus_with_ubertooths(2);
	mu_assert(select_capturing("9", msg, sizeof(msg)) == NULL, "past the end");
	mu_assert_int_eq(0, fake.lists);
}

/* A device whose descriptor cannot be read is not counted as an Ubertooth, so
 * it cannot shift the indices of the devices after it. */
MU_TEST(a_device_without_a_descriptor_is_skipped)
{
	char msg[256];
	ubertooth_t *ut;

	bus_with_ubertooths(2);
	fake.descriptor_fails = 1; /* the first Ubertooth on the bus */
	ut = select_capturing("0", msg, sizeof(msg));
	mu_assert_fatal(ut != NULL, "-U0 opens the Ubertooth that answered");
	mu_assert(fake.opened == &fake.devs[2], "and it is the second one");
	mu_assert(strcmp(msg,
	    "warning: could not read the USB descriptor of device 1, so it is "
	    "skipped\n") == 0, msg);
	ubertooth_stop(ut);
	ubertooth_free(ut);

	bus_with_ubertooths(2);
	fake.descriptor_fails = 1;
	mu_assert(select_capturing("1", msg, sizeof(msg)) == NULL, "-U1 of one left");
	mu_assert(strstr(msg, "no Ubertooth at index 1 (1 attached)") != NULL, msg);
}

/* The quiet selection used by the reset wait makes the same choice, without
 * printing anything. */
MU_TEST(the_quiet_selection_refuses_the_same_indices)
{
	ubertooth_t *ut;

	bus_with_ubertooths(2);
	ut = ubertooth_start_select_quiet("1");
	mu_assert_fatal(ut != NULL, "quiet -U1 of two opens one");
	mu_assert(fake.opened == &fake.devs[2], "the second Ubertooth");
	ubertooth_stop(ut);
	ubertooth_free(ut);

	bus_with_ubertooths(2);
	mu_assert(ubertooth_start_select_quiet("2") == NULL, "quiet -U2 of two");
	mu_assert_int_eq(0, fake.opens);
	mu_assert_int_eq(0, fake.lists);

	bus_with_ubertooths(1);
	mu_assert(ubertooth_start_select_quiet("1") == NULL, "quiet -U1 of one");
	mu_assert_int_eq(0, fake.opens);

	bus_with_ubertooths(1);
	ut = ubertooth_start_select_quiet("0");
	mu_assert_fatal(ut != NULL, "quiet -U0 of one opens it");
	ubertooth_stop(ut);
	ubertooth_free(ut);
}

/* UBERTOOTH_TEST_NO_DEVICES, the switch the tool tests set, makes selection
 * behave as if nothing were attached without listing or opening anything, so a
 * test run cannot open a board another process is using. Unset, every
 * selection works as before. */
MU_TEST(the_test_switch_hides_every_device)
{
	/* Values that are not exactly "1" leave selection alone. */
	static const char *const off[] = { "0", "", "2", "true", "yes" };
	char msg[256];
	ubertooth_t *ut;
	size_t i;

	setenv("UBERTOOTH_TEST_NO_DEVICES", "1", 1);

	/* The quiet selection stays silent, as its callers require. The note is
	 * printed once per process, so this has to come before the loud case
	 * below, and this case before any other that sets the switch. */
	bus_with_ubertooths(1);
	mu_assert(select_capturing_opt("0", 1, msg, sizeof(msg)) == NULL,
	          "quiet -U0");
	mu_assert_int_eq(0, fake.opens);
	mu_assert_int_eq(0, fake.list_calls);
	mu_assert(msg[0] == '\0', msg);

	/* The first loud selection to hide a device names the switch. */
	bus_with_ubertooths(1);
	mu_assert(select_capturing(NULL, msg, sizeof(msg)) == NULL, "no -U");
	mu_assert_int_eq(0, fake.opens);
	mu_assert_int_eq(0, fake.list_calls);
	mu_assert(strstr(msg, "UBERTOOTH_TEST_NO_DEVICES is set") != NULL, msg);
	mu_assert(strstr(msg, "error: no Ubertooth devices found") != NULL, msg);

	bus_with_ubertooths(2);
	mu_assert_int_eq(0, ubertooth_count());
	mu_assert_int_eq(0, fake.list_calls);

	bus_with_ubertooths(2);
	mu_assert(select_capturing("1", msg, sizeof(msg)) == NULL, "-U1");
	mu_assert_int_eq(0, fake.opens);
	mu_assert_int_eq(0, fake.list_calls);
	mu_assert(strstr(msg, "error: no Ubertooth devices found") != NULL, msg);

	/* A serial is matched by opening each board and reading its serial; with
	 * the switch set, none is opened and the report is unchanged. */
	bus_with_ubertooths(2);
	mu_assert(select_capturing("c52000f5", msg, sizeof(msg)) == NULL, "-U serial");
	mu_assert_int_eq(0, fake.opens);
	mu_assert_int_eq(0, fake.list_calls);
	mu_assert(strstr(msg, "error: no Ubertooth found with serial matching "
	          "'c52000f5'") != NULL, msg);

	/* Only "1" turns it on: a user whose environment holds the name for some
	 * other reason still gets their board. The switch is restored before each
	 * selection below, so a fatal assertion in this loop cannot leave a value
	 * that hides devices from a later case. */
	for (i = 0; i < sizeof(off) / sizeof(off[0]); i++) {
		setenv("UBERTOOTH_TEST_NO_DEVICES", off[i], 1);
		bus_with_ubertooths(1);
		ut = select_capturing("0", msg, sizeof(msg));
		mu_assert_fatal(ut != NULL, off[i]);
		ubertooth_stop(ut);
		ubertooth_free(ut);
	}

	unsetenv("UBERTOOTH_TEST_NO_DEVICES");
	bus_with_ubertooths(1);
	mu_assert_int_eq(1, ubertooth_count());
	ut = select_capturing("0", msg, sizeof(msg));
	mu_assert_fatal(ut != NULL, "-U0 without the switch opens the device");
	mu_assert(fake.opened == &fake.devs[1], "and it is the Ubertooth");
	mu_assert(fake.list_calls > 0, "and the bus was listed");
	ubertooth_stop(ut);
	ubertooth_free(ut);
}

/*
 * The bootloader note runs where no board was found, and -V asks for it after
 * the count has given its libusb session back, so the note has to open one of
 * its own: listing devices with no session open is undefined, and crashed
 * every tool's -V with no board attached.
 */
MU_TEST(the_bootloader_note_opens_a_session_of_its_own)
{
	char msg[256];
	FILE *capture;
	int saved;
	size_t n;

	/* One board in the DFU bootloader, and no Ubertooth to work with. */
	bus_with_ubertooths(0);
	bus_with_bootloaders(1);
	capture = tmpfile();
	saved = dup(fileno(stderr));
	msg[0] = '\0';
	mu_assert_fatal(capture != NULL && saved >= 0, "capture stderr");
	fflush(stderr);
	dup2(fileno(capture), fileno(stderr));
	ubertooth_note_dfu_bootloader();
	fflush(stderr);
	dup2(saved, fileno(stderr));
	close(saved);
	rewind(capture);
	n = fread(msg, 1, sizeof(msg) - 1, capture);
	msg[n] = '\0';
	fclose(capture);

	mu_assert(strstr(msg, "in the DFU bootloader") != NULL, msg);
	mu_assert_int_eq(1, fake.usb_inits);    /* it opened a session */
	mu_assert_int_eq(0, fake.usb_sessions); /* ...and gave it back */
	mu_assert(fake.list_calls > 0, "the bus was listed");
}

/* ---- selecting by serial number ----------------------------------------- */

MU_TEST(a_full_serial_opens_that_board)
{
	char msg[256];
	ubertooth_t *ut;

	bus_with_two_serials();
	ut = select_capturing(SERIAL_A, msg, sizeof(msg));
	mu_assert_fatal(ut != NULL, "the full serial of the first board");
	mu_assert(fake.opened == &fake.devs[1], "and it is that board");
	mu_assert(msg[0] == '\0', msg);
	ubertooth_stop(ut);
	ubertooth_free(ut);

	bus_with_two_serials();
	ut = select_capturing(SERIAL_B, msg, sizeof(msg));
	mu_assert_fatal(ut != NULL, "the full serial of the second board");
	mu_assert(fake.opened == &fake.devs[2], "and it is that board");
	ubertooth_stop(ut);
	ubertooth_free(ut);
	mu_assert_int_eq(0, fake.lists);
}

/* The serials differ at the start, so a short prefix picks a board. Only the
 * start is matched: the end of a serial selects nothing. */
MU_TEST(a_prefix_of_a_serial_opens_that_board)
{
	char msg[256];
	ubertooth_t *ut;

	bus_with_two_serials();
	ut = select_capturing("aa1", msg, sizeof(msg));
	mu_assert_fatal(ut != NULL, "a prefix of the first serial");
	mu_assert(fake.opened == &fake.devs[1], "opens the first board");
	mu_assert(msg[0] == '\0', msg);
	ubertooth_stop(ut);
	ubertooth_free(ut);

	bus_with_two_serials();
	ut = select_capturing("19b", msg, sizeof(msg));
	mu_assert_fatal(ut != NULL, "a prefix of the second serial");
	mu_assert(fake.opened == &fake.devs[2], "opens the second board");
	ubertooth_stop(ut);
	ubertooth_free(ut);

	bus_with_two_serials();
	mu_assert(select_capturing("c52000f5", msg, sizeof(msg)) == NULL,
	          "the end of a serial is not matched");
	mu_assert(strcmp(msg,
	    "error: no Ubertooth found with serial matching 'c52000f5'\n") == 0, msg);
	mu_assert_int_eq(fake.opens, fake.closes);
}

MU_TEST(a_serial_matches_in_upper_case)
{
	char msg[256];
	ubertooth_t *ut;

	bus_with_two_serials();
	ut = select_capturing("AA1100112233445566778899AABBCCDD", msg, sizeof(msg));
	mu_assert_fatal(ut != NULL, "an upper-case full serial");
	mu_assert(fake.opened == &fake.devs[1], "opens the first board");
	mu_assert(msg[0] == '\0', msg);
	ubertooth_stop(ut);
	ubertooth_free(ut);

	bus_with_two_serials();
	ut = select_capturing("19B", msg, sizeof(msg));
	mu_assert_fatal(ut != NULL, "an upper-case prefix");
	mu_assert(fake.opened == &fake.devs[2], "opens the second board");
	ubertooth_stop(ut);
	ubertooth_free(ut);
}

/* A string that matches two attached boards opens neither: it used to open
 * whichever enumerated first, so a shared part silently targeted one board or
 * the other. */
MU_TEST(a_string_matching_two_boards_is_refused)
{
	char msg[256];

	bus_with_shared_prefix();
	mu_assert(select_capturing("aa", msg, sizeof(msg)) == NULL,
	          "a start both serials share");
	mu_assert(strcmp(msg,
	    "error: 'aa' matches 2 Ubertooth devices: "
	    SERIAL_A ", " SERIAL_C "\n"
	    "select one of them by a longer prefix of its serial number\n") == 0, msg);
	mu_assert_int_eq(fake.opens, fake.closes); /* no handle left open */
	mu_assert_int_eq(0, fake.lists);

	/* The upper-case form is refused the same way. */
	bus_with_shared_prefix();
	mu_assert(select_capturing("AA", msg, sizeof(msg)) == NULL,
	          "an upper-case shared start");
	mu_assert(strstr(msg, "matches 2 Ubertooth devices") != NULL, msg);

	/* One more digit tells them apart. */
	bus_with_shared_prefix();
	{
		ubertooth_t *ut = select_capturing("aa2", msg, sizeof(msg));
		mu_assert_fatal(ut != NULL, "a longer start");
		mu_assert(fake.opened == &fake.devs[2], "opens the second board");
		ubertooth_stop(ut);
		ubertooth_free(ut);
	}
}

MU_TEST(a_serial_that_matches_nothing_is_refused)
{
	char msg[256];

	bus_with_two_serials();
	mu_assert(select_capturing("deadbeef", msg, sizeof(msg)) == NULL,
	          "a serial no board has");
	mu_assert(strcmp(msg,
	    "error: no Ubertooth found with serial matching 'deadbeef'\n") == 0, msg);
	mu_assert_int_eq(fake.opens, fake.closes);
	mu_assert_int_eq(0, fake.lists);

	/* The middle of a serial is not matched: only its start is. A run that
	 * includes a non-digit, since an all-digits string is an index. */
	bus_with_two_serials();
	mu_assert(select_capturing("8899aabb", msg, sizeof(msg)) == NULL,
	          "the middle of a serial");
	mu_assert(strstr(msg, "error: no Ubertooth found with serial matching")
	          != NULL, msg);

	/* A string longer than a serial cannot match one. */
	bus_with_two_serials();
	mu_assert(select_capturing(SERIAL_A "00", msg, sizeof(msg)) == NULL,
	          "longer than a serial");

	bus_with_ubertooths(0);
	mu_assert(select_capturing("aa1", msg, sizeof(msg)) == NULL,
	          "no board attached");
	mu_assert_int_eq(0, fake.lists);

	/* and an empty bus, which is not enumerated at all */
	bus_with_ubertooths(0);
	fake.n = 0;
	fake.list_rc = 0;
	mu_assert(select_capturing("aa1", msg, sizeof(msg)) == NULL, "empty bus");
	mu_assert(strcmp(msg,
	    "error: no Ubertooth found with serial matching 'aa1'\n") == 0, msg);
	mu_assert_int_eq(0, fake.lists);
}

/* An all-digits argument is an index, even when it would match the start of a
 * serial: the two cannot both be auto-detected, and an index is
 * what -U has always meant for digits. Use a longer string, or one that
 * includes a non-digit, to mean a serial. */
MU_TEST(an_all_digits_string_is_still_an_index)
{
	char msg[256];
	ubertooth_t *ut;

	/* "05" is the start of the second board's serial, but index 5. */
	bus_with_two_serials();
	mu_assert(select_capturing("05", msg, sizeof(msg)) == NULL, "-U05");
	mu_assert(strstr(msg, "no Ubertooth at index 5 (2 attached)") != NULL, msg);

	/* "1" is the start of the first board's serial, but index 1. */
	bus_with_two_serials();
	ut = select_capturing("1", msg, sizeof(msg));
	mu_assert_fatal(ut != NULL, "-U1 of two boards");
	mu_assert(fake.opened == &fake.devs[2], "is the second board, not the first");
	ubertooth_stop(ut);
	ubertooth_free(ut);

	/* An all-digits start is an index too: 12 is not attached. */
	bus_with_ubertooths(2);
	set_serial(0, "12ff00112233445566778899aabbccdd");
	set_serial(1, "19bb00112233445566778899aabbccdd");
	mu_assert(select_capturing("12", msg, sizeof(msg)) == NULL, "-U12");
	mu_assert(strstr(msg, "no Ubertooth at index 12 (2 attached)") != NULL, msg);
}

/* Matching a serial means reading every attached board, so a board that does
 * not answer must not report an error of its own while another board is being
 * selected -- but it does change what "no match" means, so it is counted. */
MU_TEST(a_board_that_does_not_answer_is_skipped_quietly)
{
	/* 512: the note that goes with this report is a long line. */
	char msg[512];
	ubertooth_t *ut;

	bus_with_two_serials();
	fake.devs[1].serial_fails = 1;
	ut = select_capturing("19b", msg, sizeof(msg));
	mu_assert_fatal(ut != NULL, "the board that answered is still selected");
	mu_assert(fake.opened == &fake.devs[2], "and it is that board");
	mu_assert(msg[0] == '\0', msg); /* nothing about the silent board */
	ubertooth_stop(ut);
	ubertooth_free(ut);

	bus_with_two_serials();
	fake.devs[1].serial_fails = 1;
	mu_assert(select_capturing("aa1", msg, sizeof(msg)) == NULL,
	          "the serial of the silent board");
	mu_assert(strcmp(msg,
	    "error: no Ubertooth found with serial matching 'aa1'\n"
	    "1 attached Ubertooth device did not answer the serial read\n"
	    "note: a board that does not answer may be in use by another "
	    "program, or busy in a mode; stop a mode with "
	    "ubertooth-util -U <index> -S, or reset a board that answers "
	    "nothing with -r\n") == 0,
	    msg);

	bus_with_two_serials();
	fake.devs[1].serial_fails = 1;
	fake.devs[2].serial_fails = 1;
	mu_assert(select_capturing("aa1", msg, sizeof(msg)) == NULL, "both silent");
	mu_assert(strstr(msg, "2 attached Ubertooth devices did not answer") != NULL,
	          msg);
	mu_assert_int_eq(fake.opens, fake.closes);
}

/* A Ctrl-C while the bus is being read for serials ends the search. Reading
 * them is the slow part of starting a tool -- every attached board is opened
 * and asked, and one that does not answer costs a second -- so a search that
 * worked through the rest of the bus would hold the stop up for that long.
 *
 * No board is opened, nothing is printed, and the reason says the run was
 * stopped rather than that no board matched: that is what lets a tool exit 0
 * instead of reporting a failure to find a board the user never waited for.
 *
 * The handlers are installed first because that is where a tool has them
 * from: before it opens anything. */
MU_TEST(a_serial_search_the_user_stopped_opens_nothing)
{
	char msg[256];
	ubertooth_t *ut;

	bus_with_two_serials();
	ubertooth_catch_stop_signals();
	raise(SIGINT);

	mu_assert(select_capturing(SERIAL_A, msg, sizeof(msg)) == NULL,
	          "a search the user stopped selects no board");
	mu_assert(msg[0] == '\0', msg);
	mu_assert_int_eq(1, ubertooth_open_stopped());
	/* Not even to read a serial: the board the user named is on this bus. */
	mu_assert_int_eq(0, fake.opens);

	signal(SIGINT, SIG_DFL);
	ubertooth_reset_stop_signals();

	/* And the next search is an ordinary one, which is what makes the case
	 * above mean something. */
	bus_with_two_serials();
	ut = select_capturing(SERIAL_A, msg, sizeof(msg));
	mu_assert_fatal(ut != NULL, "the same serial, with nothing interrupting");
	mu_assert_int_eq(0, ubertooth_open_stopped());
	ubertooth_stop(ut);
	ubertooth_free(ut);
}

/* Two boards reporting the same serial cannot be told apart by this selector,
 * so the refusal does not advise a longer string. */
MU_TEST(two_boards_with_one_serial_are_refused_without_bad_advice)
{
	char msg[256];

	bus_with_ubertooths(2);
	set_serial(0, SERIAL_A);
	set_serial(1, SERIAL_A);
	mu_assert(select_capturing(SERIAL_A, msg, sizeof(msg)) == NULL,
	          "a serial two boards report");
	mu_assert(strcmp(msg,
	    "error: '" SERIAL_A "' matches 2 Ubertooth devices: "
	    SERIAL_A ", " SERIAL_A "\n"
	    "these Ubertooth devices report the same serial number; select one by "
	    "index with -U<index>\n") == 0, msg);

	/* The advice follows the serials, not the length of what was typed: a
	 * short string that matches those two boards is just as impossible. */
	bus_with_ubertooths(2);
	set_serial(0, SERIAL_A);
	set_serial(1, SERIAL_A);
	mu_assert(select_capturing("aa1", msg, sizeof(msg)) == NULL,
	          "a short string matching both");
	mu_assert(strstr(msg, "report the same serial number") != NULL, msg);
	mu_assert(strstr(msg, "longer prefix") == NULL, msg);

	/* Two different serials still get the advice that can be followed. */
	bus_with_shared_prefix();
	mu_assert(select_capturing("aa", msg, sizeof(msg)) == NULL,
	          "a start two different serials share");
	mu_assert(strstr(msg, "longer prefix") != NULL, msg);
}

/* A board that cannot be opened at all -- the usual Linux symptom of missing
 * udev rules -- is counted and reported, so a non-match is not mistaken for
 * "that serial is not attached". */
MU_TEST(boards_that_cannot_be_opened_are_reported)
{
	char msg[256];

	bus_with_two_serials();
	fake.open_rc = LIBUSB_ERROR_ACCESS;
	mu_assert(select_capturing("aa1", msg, sizeof(msg)) == NULL, "no access");
	mu_assert(strstr(msg,
	    "error: no Ubertooth found with serial matching 'aa1'\n"
	    "2 attached Ubertooth devices could not be opened\n") != NULL, msg);
	/* Why one of those boards could not be opened, in libusb's words, as a
	 * note rather than a second error: the refusal is the first line. */
	mu_assert(strstr(msg, "\nnote: ") != NULL, msg);
	mu_assert(strstr(msg, "Access denied") != NULL, msg); /* the libusb error */
	mu_assert(strstr(msg + 7, "error: ") == NULL, msg);
	mu_assert_int_eq(0, fake.lists);

	/* One board unreadable and one unopenable: both are reported. */
	bus_with_two_serials();
	fake.devs[1].serial_fails = 1;
	mu_assert(select_capturing("deadbeef", msg, sizeof(msg)) == NULL, "mixed");
	mu_assert(strstr(msg, "1 attached Ubertooth device did not answer") != NULL,
	          msg);

	/* Quiet selection says nothing about either. */
	bus_with_two_serials();
	fake.open_rc = LIBUSB_ERROR_ACCESS;
	mu_assert(ubertooth_start_select_quiet("aa1") == NULL, "quiet no access");
}

/* The phrase a tool puts on standard output for a board it could not open,
 * beside the library's own error line. */
MU_TEST(a_failed_open_says_why_in_a_phrase_a_report_can_use)
{
	char msg[256];
	ubertooth_t *ut;

	/* The board opens but something else holds its interface. */
	bus_with_two_serials();
	fake.claim_rc = LIBUSB_ERROR_BUSY;
	mu_assert(select_capturing("aa1", msg, sizeof(msg)) == NULL, "busy board");
	mu_assert(strcmp(ubertooth_open_failure_reason(),
	                 "in use by another program") == 0,
	          ubertooth_open_failure_reason());

	/* Any other claim failure is not blamed on another program. */
	bus_with_two_serials();
	fake.claim_rc = LIBUSB_ERROR_NO_DEVICE;
	mu_assert(select_capturing("aa1", msg, sizeof(msg)) == NULL, "claim failed");
	mu_assert(strcmp(ubertooth_open_failure_reason(),
	                 "its USB interface could not be claimed") == 0,
	          ubertooth_open_failure_reason());

	/* No board matched the string at all. */
	bus_with_two_serials();
	mu_assert(select_capturing("deadbeef", msg, sizeof(msg)) == NULL, "no match");
	mu_assert(strcmp(ubertooth_open_failure_reason(),
	                 "no board matched that serial") == 0,
	          ubertooth_open_failure_reason());

	/* A bus whose boards cannot be opened at all is a permissions problem,
	 * not a serial that is not there: no board's serial could be read, so
	 * none could match. */
	bus_with_two_serials();
	fake.open_rc = LIBUSB_ERROR_ACCESS;
	mu_assert(select_capturing("aa11", msg, sizeof(msg)) == NULL, "no access");
	mu_assert(strcmp(ubertooth_open_failure_reason(),
	                 "the attached boards could not be opened") == 0,
	          ubertooth_open_failure_reason());

	/* By index, which is what the per-device listing uses, the same failure
	 * is reported where the open happens. */
	bus_with_ubertooths(2);
	fake.open_rc = LIBUSB_ERROR_ACCESS;
	mu_assert(select_capturing("1", msg, sizeof(msg)) == NULL, "no access by index");
	mu_assert(strcmp(ubertooth_open_failure_reason(),
	                 "access to the board was denied") == 0,
	          ubertooth_open_failure_reason());

	/* Nothing attached at all. */
	bus_with_ubertooths(0);
	mu_assert(select_capturing("0", msg, sizeof(msg)) == NULL, "empty bus");
	mu_assert(strcmp(ubertooth_open_failure_reason(),
	                 "no Ubertooth is attached") == 0,
	          ubertooth_open_failure_reason());

	/* Several attached and none selected. */
	bus_with_ubertooths(2);
	mu_assert(select_capturing(NULL, msg, sizeof(msg)) == NULL, "no selector");
	mu_assert(strcmp(ubertooth_open_failure_reason(),
	                 "more than one board is attached and none was selected") == 0,
	          ubertooth_open_failure_reason());

	/* An index with no board behind it. */
	bus_with_ubertooths(1);
	mu_assert(select_capturing("1", msg, sizeof(msg)) == NULL, "index past end");
	mu_assert(strcmp(ubertooth_open_failure_reason(),
	                 "no board answered at that index") == 0,
	          ubertooth_open_failure_reason());

	/* An open that succeeded leaves no stale reason behind. */
	bus_with_ubertooths(1);
	ut = select_capturing("0", msg, sizeof(msg));
	mu_assert_fatal(ut != NULL, "index 0 opened");
	mu_assert(strcmp(ubertooth_open_failure_reason(),
	                 "it could not be opened") == 0,
	          ubertooth_open_failure_reason());
	ubertooth_close(ut);
	ubertooth_free(ut);
}

/* A bus that cannot be listed is not a serial that is not attached. */
MU_TEST(a_bus_that_cannot_be_listed_is_not_reported_as_no_match)
{
	char msg[256];

	bus_with_two_serials();
	fake.list_rc = LIBUSB_ERROR_IO;
	mu_assert(select_capturing("aa1", msg, sizeof(msg)) == NULL, "failed list");
	mu_assert(strstr(msg, "could not list USB devices") != NULL, msg);
	mu_assert(strstr(msg, "no Ubertooth found with serial") == NULL, msg);
	mu_assert_int_eq(0, fake.opens);
	mu_assert_int_eq(0, fake.lists);

	bus_with_two_serials();
	fake.list_rc = LIBUSB_ERROR_IO;
	mu_assert(ubertooth_start_select_quiet("aa1") == NULL, "quiet failed list");
	mu_assert_int_eq(0, fake.opens);
}

/* The quiet selection used by the reset wait matches serials the same way,
 * without printing anything. */
MU_TEST(the_quiet_selection_matches_the_same_serials)
{
	ubertooth_t *ut;

	bus_with_two_serials();
	ut = ubertooth_start_select_quiet("19b");
	mu_assert_fatal(ut != NULL, "quiet prefix match");
	mu_assert(fake.opened == &fake.devs[2], "the second board");
	ubertooth_stop(ut);
	ubertooth_free(ut);

	bus_with_two_serials();
	ut = ubertooth_start_select_quiet(SERIAL_A);
	mu_assert_fatal(ut != NULL, "quiet full serial");
	mu_assert(fake.opened == &fake.devs[1], "the first board");
	ubertooth_stop(ut);
	ubertooth_free(ut);

	bus_with_shared_prefix();
	mu_assert(ubertooth_start_select_quiet("aa") == NULL,
	          "quiet ambiguous match");
	mu_assert_int_eq(fake.opens, fake.closes);
	mu_assert_int_eq(0, fake.lists);

	bus_with_two_serials();
	mu_assert(ubertooth_start_select_quiet("deadbeef") == NULL,
	          "quiet no match");
	mu_assert_int_eq(0, fake.lists);
}

/* A board another program is using opens, and then cannot be claimed: that is
 * how both systems report it, macOS as ACCESS and Linux as BUSY. Say so in
 * words, naming the board as the selector named it, and keep the libusb code
 * for bug reports. It used to read "error: claiming the USB interface failed
 * (-3)", which says nothing about what is going on or what to do. */
MU_TEST(a_board_another_program_is_using_says_so)
{
	static const int in_use[] = { LIBUSB_ERROR_ACCESS, LIBUSB_ERROR_BUSY };
	char msg[256], expected[256];
	size_t i;

	for (i = 0; i < sizeof(in_use) / sizeof(in_use[0]); i++) {
		/* selected by serial: named as the user named it */
		bus_with_two_serials();
		fake.claim_rc = in_use[i];
		mu_assert(select_capturing("19b", msg, sizeof(msg)) == NULL, msg);
		/* the whole serial of the board that matched, not the few characters
		 * of it the user typed */
		snprintf(expected, sizeof(expected),
		         "error: Ubertooth " SERIAL_B " is in use by another program "
		         "(libusb %d)\n", in_use[i]);
		mu_assert(strcmp(msg, expected) == 0, msg);

		/* and by index, which is all a -U<index> run knows about it */
		bus_with_ubertooths(2);
		fake.claim_rc = in_use[i];
		mu_assert(select_capturing("1", msg, sizeof(msg)) == NULL, msg);
		snprintf(expected, sizeof(expected),
		         "error: Ubertooth at index 1 is in use by another program "
		         "(libusb %d)\n", in_use[i]);
		mu_assert(strcmp(msg, expected) == 0, msg);
	}
}

/* An open libusb refuses as busy gets the plain wording too. It cannot name
 * the board: the report is the library's shared sink for libusb failures,
 * which has no handle to look at. */
MU_TEST(an_open_refused_as_busy_says_the_board_is_in_use)
{
	char msg[256];

	bus_with_ubertooths(2);
	fake.open_rc = LIBUSB_ERROR_BUSY;
	mu_assert(select_capturing("1", msg, sizeof(msg)) == NULL, msg);
	mu_assert(strstr(msg, "error: the Ubertooth is in use by another program "
	          "(libusb -6)") != NULL, msg);

	/* An open refused for want of permission keeps libusb's own naming: that
	 * is the usual cause there, and it is a different problem. */
	bus_with_ubertooths(2);
	fake.open_rc = LIBUSB_ERROR_ACCESS;
	mu_assert(select_capturing("1", msg, sizeof(msg)) == NULL, msg);
	mu_assert(strstr(msg, "in use by another program") == NULL, msg);
	/* libusb's own words for it, which are localizable, so only the prefix
	 * and the code are pinned. */
	mu_assert(strstr(msg, "error: ") == msg, msg);
	mu_assert(strstr(msg, "(libusb -3)") != NULL, msg);
}

/* Any other claim failure is still reported as itself, naming the board, so
 * the plain wording above is not put on causes it does not fit. With no
 * selector there is no name to use. */
MU_TEST(another_claim_failure_is_reported_as_itself)
{
	char msg[256];

	bus_with_ubertooths(1);
	fake.claim_rc = LIBUSB_ERROR_NO_DEVICE;
	mu_assert(select_capturing("0", msg, sizeof(msg)) == NULL, msg);
	mu_assert(strcmp(msg, "error: claiming the USB interface of Ubertooth at "
	          "index 0 failed (libusb -4)\n") == 0, msg);

	bus_with_ubertooths(1);
	fake.claim_rc = LIBUSB_ERROR_NO_DEVICE;
	mu_assert(select_capturing(NULL, msg, sizeof(msg)) == NULL, msg);
	mu_assert(strcmp(msg, "error: claiming the USB interface of the Ubertooth "
	          "failed (libusb -4)\n") == 0, msg);
}

/* Selection still allocates a ring buffer, and it came from this fake rather
 * than from the library's 72 MB one. */
MU_TEST(the_selections_used_the_faked_ring_buffer)
{
	mu_assert(fifo_inits > 0, "fifo_init() came from this test");
}

/* A board in the DFU bootloader is being flashed, or a flash left it there.
 * It is not an Ubertooth any tool can open, so the selection still finds
 * nothing and still fails -- but it is not a board that went away either, and
 * the two used to read alike: a "no Ubertooth found" line in an overnight
 * regression log was the one event nobody could explain. */
MU_TEST(a_board_in_the_bootloader_is_reported_as_such)
{
	char msg[512];

	/* By serial, which is what a regression log selects with. */
	bus_with_two_serials();
	bus_with_bootloaders(1);
	mu_assert(select_capturing("deadbeef", msg, sizeof(msg)) == NULL,
	          "a serial no board has");
	mu_assert(strcmp(msg,
	    "error: no Ubertooth found with serial matching 'deadbeef'\n"
	    "note: a board is in the DFU bootloader, so no tool can open it: "
	    "it is being flashed, or a flash left it there\n") == 0, msg);

	/* And by index, where the selection's own message comes from the
	 * connect: the note follows it. */
	bus_with_ubertooths(0);
	bus_with_bootloaders(1);
	mu_assert(select_capturing("0", msg, sizeof(msg)) == NULL, "-U0 of none");
	mu_assert(strcmp(msg,
	    "error: no Ubertooth devices found\n"
	    "note: a board is in the DFU bootloader, so no tool can open it: "
	    "it is being flashed, or a flash left it there\n") == 0, msg);

	/* Several say how many. */
	bus_with_ubertooths(0);
	bus_with_bootloaders(2);
	mu_assert(select_capturing(NULL, msg, sizeof(msg)) == NULL, "no -U");
	mu_assert(strstr(msg, "note: 2 boards are in the DFU bootloader")
	          != NULL, msg);

	/* A board that is simply not attached reads as it always did. */
	bus_with_two_serials();
	mu_assert(select_capturing("deadbeef", msg, sizeof(msg)) == NULL,
	          "a serial no board has");
	mu_assert(strcmp(msg,
	    "error: no Ubertooth found with serial matching 'deadbeef'\n") == 0, msg);

	/* The note is extra information, not a board: a bootloader on the bus
	 * does not become the device a selection opens. */
	bus_with_ubertooths(0);
	bus_with_bootloaders(1);
	mu_assert(select_capturing(NULL, msg, sizeof(msg)) == NULL, "no -U");
	mu_assert_int_eq(0, fake.opens);
	mu_assert_int_eq(0, fake.lists);
}

/* Closing a handle twice does nothing the second time, so a tool does not
 * have to work out whether the library has closed the board already -- which
 * it does for a board whose firmware version it refused. The libusb session
 * used to be ended outside the device check, so the second close ended a
 * session that was no longer this handle's. */
MU_TEST(closing_a_handle_twice_is_harmless)
{
	char msg[256];
	ubertooth_t *ut;

	bus_with_ubertooths(1);
	ut = select_capturing("0", msg, sizeof(msg));
	mu_assert_fatal(ut != NULL, "-U0 opens the only device");
	mu_assert_int_eq(1, fake.usb_sessions);

	ubertooth_close(ut);
	mu_assert_int_eq(1, fake.closes);
	mu_assert_int_eq(1, fake.usb_exits);
	mu_assert_int_eq(0, fake.usb_sessions);

	ubertooth_close(ut);
	ubertooth_stop(ut);
	mu_assert_int_eq(1, fake.closes); /* the device was closed once */
	mu_assert_int_eq(1, fake.usb_exits); /* and the session ended once */
	ubertooth_free(ut);

	/* A handle that never opened a board ends no session at all: a replay
	 * reads a file, and closing its handle used to end whatever session
	 * another handle held. */
	bus_with_ubertooths(1);
	ut = ubertooth_init();
	mu_assert_fatal(ut != NULL, "a handle");
	ubertooth_close(ut);
	ubertooth_stop(ut);
	mu_assert_int_eq(0, fake.usb_exits);
	mu_assert_int_eq(0, fake.closes);
	ubertooth_free(ut);
}

/* Every selection that opens a board, and every one that fails, leaves the
 * libusb session count where it started. */
MU_TEST(a_selection_ends_every_session_it_starts)
{
	char msg[256];
	ubertooth_t *ut;

	bus_with_ubertooths(1);
	ut = select_capturing("0", msg, sizeof(msg));
	mu_assert_fatal(ut != NULL, msg);
	ubertooth_stop(ut);
	ubertooth_free(ut);
	mu_assert_int_eq(0, fake.usb_sessions);

	/* No board matches: the library gives the session back itself. */
	bus_with_ubertooths(1);
	mu_assert(select_capturing("7", msg, sizeof(msg)) == NULL, msg);
	mu_assert_int_eq(0, fake.usb_sessions);

	bus_with_two_serials();
	mu_assert(select_capturing("deadbeef", msg, sizeof(msg)) == NULL, msg);
	mu_assert_int_eq(0, fake.usb_sessions);

	/* And a board that opens but cannot be claimed. */
	bus_with_ubertooths(1);
	fake.claim_rc = LIBUSB_ERROR_BUSY;
	mu_assert(select_capturing("0", msg, sizeof(msg)) == NULL, msg);
	mu_assert_int_eq(0, fake.usb_sessions);

	/* The quiet selection the reset wait polls with, both ways. */
	bus_with_ubertooths(1);
	ut = ubertooth_start_select_quiet("0");
	mu_assert_fatal(ut != NULL, "the quiet selection opens it");
	ubertooth_stop(ut);
	ubertooth_free(ut);
	mu_assert_int_eq(0, fake.usb_sessions);

	bus_with_ubertooths(1);
	mu_assert(ubertooth_start_select_quiet("3") == NULL, "quiet -U3 of one");
	mu_assert_int_eq(0, fake.usb_sessions);
}

int main(void)
{
	MU_RUN(a_board_running_the_other_image_is_refused_by_name);
	MU_RUN(the_expected_image_and_an_unreadable_one_both_pass);
	MU_RUN(the_image_check_alone_ignores_the_firmware_version);
	MU_RUN(an_index_in_range_opens_that_device);
	MU_RUN(an_index_past_several_devices_opens_nothing);
	MU_RUN(an_index_too_large_for_an_int_opens_nothing);
	MU_RUN(the_largest_representable_index_is_an_ordinary_refusal);
	MU_RUN(a_nonzero_index_with_one_device_opens_nothing);
	MU_RUN(index_zero_and_no_index_open_the_only_device);
	MU_RUN(a_handle_remembers_the_board_the_user_named);
	MU_RUN(an_open_keeps_the_boards_api_version_for_later_reports);
	MU_RUN(the_error_names_the_index_and_the_count);
	MU_RUN(several_devices_with_no_index_still_list_them);
	MU_RUN(no_device_attached_opens_nothing);
	MU_RUN(an_empty_or_failing_bus_is_refused);
	MU_RUN(a_device_without_a_descriptor_is_skipped);
	MU_RUN(the_quiet_selection_refuses_the_same_indices);
	MU_RUN(the_bootloader_note_opens_a_session_of_its_own);
	MU_RUN(a_full_serial_opens_that_board);
	MU_RUN(a_prefix_of_a_serial_opens_that_board);
	MU_RUN(a_serial_matches_in_upper_case);
	MU_RUN(a_string_matching_two_boards_is_refused);
	MU_RUN(a_serial_that_matches_nothing_is_refused);
	MU_RUN(an_all_digits_string_is_still_an_index);
	MU_RUN(a_board_that_does_not_answer_is_skipped_quietly);
	MU_RUN(a_serial_search_the_user_stopped_opens_nothing);
	MU_RUN(two_boards_with_one_serial_are_refused_without_bad_advice);
	MU_RUN(boards_that_cannot_be_opened_are_reported);
	MU_RUN(a_failed_open_says_why_in_a_phrase_a_report_can_use);
	MU_RUN(a_bus_that_cannot_be_listed_is_not_reported_as_no_match);
	MU_RUN(the_quiet_selection_matches_the_same_serials);
	MU_RUN(the_test_switch_hides_every_device);
	MU_RUN(a_board_another_program_is_using_says_so);
	MU_RUN(an_open_refused_as_busy_says_the_board_is_in_use);
	MU_RUN(another_claim_failure_is_reported_as_itself);
	MU_RUN(a_board_in_the_bootloader_is_reported_as_such);
	MU_RUN(closing_a_handle_twice_is_harmless);
	MU_RUN(a_selection_ends_every_session_it_starts);
	MU_RUN(the_selections_used_the_faked_ring_buffer);
	return mu_report();
}
