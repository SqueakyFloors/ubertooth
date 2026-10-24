/*
 * Hardware-independent tests for libubertooth device selection (-U<index>).
 *
 * The real ubertooth.c is compiled into this test and the test defines the
 * libusb device-list calls itself, so the library enumerates a faked USB bus
 * instead of the real one. That makes the multi-device cases testable without
 * two boards: an index that is not present must open nothing, and no index may
 * ever be used to read past the end of the device list.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libusb.h>

#include "../../../firmware/tests/minunit.h"
#include "ubertooth.h"

#define FAKE_MAX 8

/* One faked USB device: its descriptor, the serial number it reports, and a
 * handle the library may open. */
struct fake_dev {
	uint16_t vid, pid;
	uint8_t serial[16]; /* the 16 bytes -s prints as 32 hex digits */
	int handle; /* opaque body for the handle the fake libusb_open() hands out */
};

static struct {
	struct fake_dev devs[FAKE_MAX];
	int n;                  /* devices on the faked bus */
	ssize_t list_rc;        /* <0 to fail libusb_get_device_list() */
	int descriptor_fails;   /* bus index whose descriptor read fails, or -1 */
	int open_rc;            /* returned by libusb_open() */
	struct fake_dev *opened; /* the device the library opened, or NULL */
	int opens;              /* libusb_open() calls */
	int closes;             /* libusb_close() calls */
	int lists;              /* outstanding libusb_get_device_list() lists */
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
	}
	fake.n = n + 1;
	fake.list_rc = fake.n;
}

/* Give the nth Ubertooth on the faked bus the serial number hex (32 lowercase
 * hex digits, as -s prints it), which its serial read then reports. */
static void set_serial(int n, const char *hex)
{
	int i;

	mu_assert(strlen(hex) == 32, hex);
	for (i = 0; i < 16; i++) {
		unsigned byte;

		sscanf(hex + i * 2, "%2x", &byte);
		fake.devs[1 + n].serial[i] = (uint8_t)byte;
	}
}

/* The faked bus used by the serial tests: two boards whose serials differ in
 * their leading digits and share their trailing ones, as real ones do. */
#define SERIAL_A "1cc001038088cb956c43ad61c52000f5"
#define SERIAL_B "05f00014e380df95f4436562c32000f5"

static void bus_with_two_serials(void)
{
	bus_with_uberteeth(2);
	set_serial(0, SERIAL_A);
	set_serial(1, SERIAL_B);
}

/* ---- libusb fakes ------------------------------------------------------- */
/* Defined here, so they take precedence over the libusb library symbols. */

int LIBUSB_CALL libusb_init(libusb_context **ctx)
{
	(void)ctx;
	return 0;
}

void LIBUSB_CALL libusb_exit(libusb_context *ctx)
{
	(void)ctx;
}

ssize_t LIBUSB_CALL libusb_get_device_list(libusb_context *ctx,
                                           libusb_device ***list)
{
	int i;

	(void)ctx;
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
	return 0;
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
	if (bRequest == UBERTOOTH_GET_SERIAL && wLength == 17 && fd != NULL)
		memcpy(data + 1, fd->serial, sizeof(fd->serial)); /* data[0] is the status */
	return wLength;
}

/* ---- helpers ------------------------------------------------------------ */

/* Run ubertooth_start_select(u_arg) with stderr captured into msg. */
static ubertooth_t *select_capturing(const char *u_arg, char *msg, size_t len)
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
	ut = ubertooth_start_select(u_arg);
	fflush(stderr);
	dup2(saved, fileno(stderr));
	close(saved);
	rewind(capture);
	n = fread(msg, 1, len - 1, capture);
	msg[n] = '\0';
	fclose(capture);
	return ut;
}

/* ---- tests -------------------------------------------------------------- */

MU_TEST(an_index_in_range_opens_that_device)
{
	char msg[256];
	ubertooth_t *ut;

	bus_with_ubertooths(3);
	ut = select_capturing("1", msg, sizeof(msg));
	mu_assert(ut != NULL, "-U1 of three devices opens one");
	mu_assert(fake.opened == &fake.devs[2], "-U1 opens the second Ubertooth");
	mu_assert(msg[0] == '\0', msg);
	ubertooth_stop(ut);
	ubertooth_free(ut);

	bus_with_ubertooths(3);
	ut = select_capturing("2", msg, sizeof(msg));
	mu_assert(ut != NULL, "-U2 of three devices opens one");
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

	bus_with_ubertooths(2);
	mu_assert(select_capturing("2147483647", msg, sizeof(msg)) == NULL, msg);
	mu_assert(strcmp(msg,
	    "error: no Ubertooth at index 2147483647 (2 attached)\n"
	    "indexes change when boards are added or removed; select a board by serial with -U<serial>\n"
	    "could not open Ubertooth device\n") == 0, msg);

	/* Leading zeros are still just digits. */
	bus_with_ubertooths(2);
	mu_assert(select_capturing("01", msg, sizeof(msg)) != NULL, msg);
	mu_assert(fake.opened == &fake.devs[2], "-U01 is index 1");
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
	mu_assert(ut != NULL, "-U0 opens the only device");
	mu_assert(fake.opened == &fake.devs[1], "and it is the Ubertooth");
	mu_assert(msg[0] == '\0', msg);
	ubertooth_stop(ut);
	ubertooth_free(ut);

	bus_with_ubertooths(1);
	ut = select_capturing(NULL, msg, sizeof(msg));
	mu_assert(ut != NULL, "no -U opens the only device");
	mu_assert(fake.opened == &fake.devs[1], "and it is the Ubertooth");
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
	    "indexes change when boards are added or removed; select a board by serial with -U<serial>\n"
	    "could not open Ubertooth device\n") == 0, msg);
}

MU_TEST(several_devices_with_no_index_still_list_them)
{
	char msg[512];

	bus_with_ubertooths(2);
	mu_assert(select_capturing(NULL, msg, sizeof(msg)) == NULL, "no -U of two");
	mu_assert(strstr(msg, "multiple Ubertooth devices found") != NULL, msg);
}

MU_TEST(no_device_attached_opens_nothing)
{
	char msg[256];

	bus_with_ubertooths(0); /* the hub only */
	mu_assert(select_capturing("0", msg, sizeof(msg)) == NULL, "-U0 of none");
	mu_assert_int_eq(0, fake.opens);
	mu_assert(strstr(msg, "could not open Ubertooth device") != NULL, msg);

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

	bus_with_ubertooths(0);
	fake.list_rc = LIBUSB_ERROR_IO;
	mu_assert(select_capturing("0", msg, sizeof(msg)) == NULL, "failed list");
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
	mu_assert(ut != NULL, "-U0 opens the Ubertooth that answered");
	mu_assert(fake.opened == &fake.devs[2], "and it is the second one");
	mu_assert(strcmp(msg,
	    "couldn't get usb descriptor for dev #1!\n") == 0, msg);
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
	mu_assert(ut != NULL, "quiet -U1 of two opens one");
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
	mu_assert(ut != NULL, "quiet -U0 of one opens it");
	ubertooth_stop(ut);
	ubertooth_free(ut);
}

/* ---- selecting by serial number ----------------------------------------- */

MU_TEST(a_full_serial_opens_that_board)
{
	char msg[256];
	ubertooth_t *ut;

	bus_with_two_serials();
	ut = select_capturing(SERIAL_A, msg, sizeof(msg));
	mu_assert(ut != NULL, "the full serial of the first board");
	mu_assert(fake.opened == &fake.devs[1], "and it is that board");
	mu_assert(msg[0] == '\0', msg);
	ubertooth_stop(ut);
	ubertooth_free(ut);

	bus_with_two_serials();
	ut = select_capturing(SERIAL_B, msg, sizeof(msg));
	mu_assert(ut != NULL, "the full serial of the second board");
	mu_assert(fake.opened == &fake.devs[2], "and it is that board");
	ubertooth_stop(ut);
	ubertooth_free(ut);
	mu_assert_int_eq(0, fake.lists);
}

/* The serials differ at the start and share their ending, so a short prefix
 * picks a board while the distinctive part of the tail is longer. */
MU_TEST(a_prefix_or_a_suffix_of_a_serial_opens_that_board)
{
	char msg[256];
	ubertooth_t *ut;

	bus_with_two_serials();
	ut = select_capturing("1cc", msg, sizeof(msg));
	mu_assert(ut != NULL, "a prefix of the first serial");
	mu_assert(fake.opened == &fake.devs[1], "opens the first board");
	mu_assert(msg[0] == '\0', msg);
	ubertooth_stop(ut);
	ubertooth_free(ut);

	bus_with_two_serials();
	ut = select_capturing("05f", msg, sizeof(msg));
	mu_assert(ut != NULL, "a prefix of the second serial");
	mu_assert(fake.opened == &fake.devs[2], "opens the second board");
	ubertooth_stop(ut);
	ubertooth_free(ut);

	bus_with_two_serials();
	ut = select_capturing("c52000f5", msg, sizeof(msg));
	mu_assert(ut != NULL, "the distinctive tail of the first serial");
	mu_assert(fake.opened == &fake.devs[1], "opens the first board");
	ubertooth_stop(ut);
	ubertooth_free(ut);

	bus_with_two_serials();
	ut = select_capturing("c32000f5", msg, sizeof(msg));
	mu_assert(ut != NULL, "the distinctive tail of the second serial");
	mu_assert(fake.opened == &fake.devs[2], "opens the second board");
	ubertooth_stop(ut);
	ubertooth_free(ut);
}

MU_TEST(a_serial_matches_in_upper_case)
{
	char msg[256];
	ubertooth_t *ut;

	bus_with_two_serials();
	ut = select_capturing("1CC001038088CB956C43AD61C52000F5", msg, sizeof(msg));
	mu_assert(ut != NULL, "an upper-case full serial");
	mu_assert(fake.opened == &fake.devs[1], "opens the first board");
	mu_assert(msg[0] == '\0', msg);
	ubertooth_stop(ut);
	ubertooth_free(ut);

	bus_with_two_serials();
	ut = select_capturing("05F", msg, sizeof(msg));
	mu_assert(ut != NULL, "an upper-case prefix");
	mu_assert(fake.opened == &fake.devs[2], "opens the second board");
	ubertooth_stop(ut);
	ubertooth_free(ut);
}

/* A string that matches two attached boards opens neither: it used to open
 * whichever enumerated first, so a shared tail silently targeted one board or
 * the other. */
MU_TEST(a_string_matching_two_boards_is_refused)
{
	char msg[256];

	bus_with_two_serials();
	mu_assert(select_capturing("2000f5", msg, sizeof(msg)) == NULL,
	          "a tail both serials share");
	mu_assert(strcmp(msg,
	    "error: '2000f5' matches 2 Ubertooth devices: "
	    SERIAL_A ", " SERIAL_B "\n"
	    "select one of them by a longer part of its serial number\n") == 0, msg);
	mu_assert_int_eq(fake.opens, fake.closes); /* no handle left open */
	mu_assert_int_eq(0, fake.lists);

	/* Matching both ends means a string can match one board's start and
	 * another's end; that is ambiguous too. These serials are made up to put
	 * the same digits at opposite ends. */
	bus_with_uberteeth(2);
	set_serial(0, "1cc001038088cb956c43ad61c52000f5");
	set_serial(1, "f5c001038088cb956c43ad61c5200012");
	mu_assert(select_capturing("f5", msg, sizeof(msg)) == NULL,
	          "one board's tail and another's head");
	mu_assert(strstr(msg, "matches 2 Ubertooth devices") != NULL, msg);
	mu_assert_int_eq(fake.opens, fake.closes);

	/* The upper-case form is refused the same way. */
	bus_with_two_serials();
	mu_assert(select_capturing("2000F5", msg, sizeof(msg)) == NULL,
	          "an upper-case shared tail");
	mu_assert(strstr(msg, "matches 2 Ubertooth devices") != NULL, msg);
}

MU_TEST(a_serial_that_matches_nothing_is_refused)
{
	char msg[256];

	bus_with_two_serials();
	mu_assert(select_capturing("deadbeef", msg, sizeof(msg)) == NULL,
	          "a serial no board has");
	mu_assert(strcmp(msg,
	    "no Ubertooth found with serial matching 'deadbeef'\n") == 0, msg);
	mu_assert_int_eq(fake.opens, fake.closes);
	mu_assert_int_eq(0, fake.lists);

	/* The middle of a serial is not matched: only a prefix or a suffix is. */
	bus_with_two_serials();
	mu_assert(select_capturing("8088cb95", msg, sizeof(msg)) == NULL,
	          "the middle of a serial");
	mu_assert(strstr(msg, "no Ubertooth found with serial matching") != NULL,
	          msg);

	/* A string longer than a serial cannot match one. */
	bus_with_two_serials();
	mu_assert(select_capturing(SERIAL_A "00", msg, sizeof(msg)) == NULL,
	          "longer than a serial");

	bus_with_uberteeth(0);
	mu_assert(select_capturing("1cc", msg, sizeof(msg)) == NULL,
	          "no board attached");
	mu_assert_int_eq(0, fake.lists);
}

/* An all-digits argument is an index, even when it would match a serial as a
 * prefix or a suffix: the two cannot both be auto-detected, and an index is
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
	mu_assert(ut != NULL, "-U1 of two boards");
	mu_assert(fake.opened == &fake.devs[2], "is the second board, not the first");
	ubertooth_stop(ut);
	ubertooth_free(ut);

	/* An all-digits tail is an index too: 12 is not attached. */
	bus_with_uberteeth(2);
	set_serial(0, "1cc001038088cb956c43ad61c5200012");
	set_serial(1, "05f00014e380df95f4436562c3200034");
	mu_assert(select_capturing("12", msg, sizeof(msg)) == NULL, "-U12");
	mu_assert(strstr(msg, "no Ubertooth at index 12 (2 attached)") != NULL, msg);
}

/* The quiet selection used by the reset wait matches serials the same way,
 * without printing anything. */
MU_TEST(the_quiet_selection_matches_the_same_serials)
{
	ubertooth_t *ut;

	bus_with_two_serials();
	ut = ubertooth_start_select_quiet("05f");
	mu_assert(ut != NULL, "quiet prefix match");
	mu_assert(fake.opened == &fake.devs[2], "the second board");
	ubertooth_stop(ut);
	ubertooth_free(ut);

	bus_with_two_serials();
	ut = ubertooth_start_select_quiet(SERIAL_A);
	mu_assert(ut != NULL, "quiet full serial");
	mu_assert(fake.opened == &fake.devs[1], "the first board");
	ubertooth_stop(ut);
	ubertooth_free(ut);

	bus_with_two_serials();
	mu_assert(ubertooth_start_select_quiet("2000f5") == NULL,
	          "quiet ambiguous match");
	mu_assert_int_eq(fake.opens, fake.closes);
	mu_assert_int_eq(0, fake.lists);

	bus_with_two_serials();
	mu_assert(ubertooth_start_select_quiet("deadbeef") == NULL,
	          "quiet no match");
	mu_assert_int_eq(0, fake.lists);
}

int main(void)
{
	MU_RUN(an_index_in_range_opens_that_device);
	MU_RUN(an_index_past_several_devices_opens_nothing);
	MU_RUN(an_index_too_large_for_an_int_opens_nothing);
	MU_RUN(the_largest_representable_index_is_an_ordinary_refusal);
	MU_RUN(a_nonzero_index_with_one_device_opens_nothing);
	MU_RUN(index_zero_and_no_index_open_the_only_device);
	MU_RUN(the_error_names_the_index_and_the_count);
	MU_RUN(several_devices_with_no_index_still_list_them);
	MU_RUN(no_device_attached_opens_nothing);
	MU_RUN(an_empty_or_failing_bus_is_refused);
	MU_RUN(a_device_without_a_descriptor_is_skipped);
	MU_RUN(the_quiet_selection_refuses_the_same_indices);
	MU_RUN(a_full_serial_opens_that_board);
	MU_RUN(a_prefix_or_a_suffix_of_a_serial_opens_that_board);
	MU_RUN(a_serial_matches_in_upper_case);
	MU_RUN(a_string_matching_two_boards_is_refused);
	MU_RUN(a_serial_that_matches_nothing_is_refused);
	MU_RUN(an_all_digits_string_is_still_an_index);
	MU_RUN(the_quiet_selection_matches_the_same_serials);
	return mu_report();
}
