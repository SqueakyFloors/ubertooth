/*
 * Hardware-independent tests for ubertooth_start_select_quiet(), the silent
 * selection the reset wait polls a re-enumerating board with.
 *
 * The real ubertooth.c is compiled into this test and the test defines the
 * libusb calls itself (as test_control_partnum does for the control
 * requests), so the library enumerates, opens and reads a faked USB bus. What
 * matters about this selector is what it does NOT do: it prints nothing on
 * either stream, and a device it cannot claim is closed without a STOP, so a
 * poll that arrives mid-reset leaves the board's state alone. Both are
 * checked here against the loud path, which does print and does send STOP.
 *
 * The ring buffer is faked too (see fifo_init() below): a real one is 64 MB
 * per selection and no case here reads a packet.
 */
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
#define MAX_REQUESTS 16

/* The timeouts the serial read is given: the quiet caller is polling a board
 * that may be absent, so it waits briefly rather than for the loud path's
 * second. */
#define QUIET_SERIAL_TIMEOUT_MS 200
#define LOUD_SERIAL_TIMEOUT_MS 1000

/* One faked USB device: its descriptor, the serial it reports, and a handle
 * the library may open. */
struct fake_dev {
	uint16_t vid, pid;
	uint8_t serial[16]; /* the 16 bytes -s prints as 32 hex digits */
	int handle;         /* opaque body for the handle libusb_open() hands out */
};

static struct {
	struct fake_dev devs[FAKE_MAX];
	int n;                   /* devices on the faked bus */
	ssize_t list_rc;         /* <0 to fail libusb_get_device_list() */
	int open_rc;             /* returned by libusb_open() */
	int claim_rc;            /* returned by libusb_claim_interface() */
	struct fake_dev *opened; /* the device left open, or NULL */
	int opens, closes, claims, releases;
	int lists;               /* outstanding device lists */
	uint8_t requests[MAX_REQUESTS]; /* bRequest of each control transfer */
	unsigned timeouts[MAX_REQUESTS]; /* and the timeout it was given */
	int requests_n;
} fake;

/* Put n Ubertooth One devices on the bus, preceded by one non-Ubertooth
 * device so that a bus index and an Ubertooth index are never the same
 * number. */
static void bus_with_ubertooths(int n)
{
	int i;

	memset(&fake, 0, sizeof(fake));
	fake.devs[0].vid = 0x1d6b; /* a root hub */
	fake.devs[0].pid = 0x0001;
	for (i = 0; i < n; i++) {
		fake.devs[1 + i].vid = U1_VENDORID;
		fake.devs[1 + i].pid = U1_PRODUCTID;
	}
	fake.n = n + 1;
	fake.list_rc = fake.n;
}

/* Give the nth Ubertooth on the faked bus the serial number hex (32 hex
 * digits, as -s prints it), which its serial read then reports. */
static void set_serial(int n, const char *hex)
{
	int i;

	mu_assert(strlen(hex) == 32
	          && strspn(hex, "0123456789abcdefABCDEF") == 32, hex);
	for (i = 0; i < 16; i++) {
		unsigned byte;

		sscanf(hex + i * 2, "%2x", &byte);
		fake.devs[1 + n].serial[i] = (uint8_t)byte;
	}
}

/* Two boards whose serials differ in their leading digits and share their
 * trailing ones, as real ones do. */
#define SERIAL_A "1cc001038088cb956c43ad61c52000f5"
#define SERIAL_B "05f00014e380df95f4436562c32000f5"

static void bus_with_two_serials(void)
{
	bus_with_ubertooths(2);
	set_serial(0, SERIAL_A);
	set_serial(1, SERIAL_B);
}

/* ---- fifo fake ---------------------------------------------------------- */
/* ubertooth_fifo.c is not linked, so these are the fifo calls the library
 * gets. Nothing here reads a packet, so a one-byte stand-in does, and a
 * selection no longer depends on 64 MB being available. */
fifo_t* fifo_init(void)
{
	return (fifo_t*)malloc(1);
}

void fifo_inc_write_ptr(fifo_t* fifo) { (void)fifo; }
void fifo_push(fifo_t* fifo, const usb_pkt_rx* packet) { (void)fifo; (void)packet; }
usb_pkt_rx fifo_pop(fifo_t* fifo) { usb_pkt_rx p; (void)fifo; memset(&p, 0, sizeof(p)); return p; }
usb_pkt_rx* fifo_get_write_element(fifo_t* fifo) { (void)fifo; return NULL; }
uint8_t fifo_empty(fifo_t* fifo) { (void)fifo; return 1; }

/* ---- libusb fakes ------------------------------------------------------- */
/* Defined here, so they take precedence over the libusb library symbols: no
 * case can reach a real bus or a board another process is using. */

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

void LIBUSB_CALL libusb_free_device_list(libusb_device **list, int unref)
{
	(void)unref;
	if (list != NULL) {
		free(list);
		fake.lists--;
	}
}

int LIBUSB_CALL libusb_get_device_descriptor(libusb_device *dev,
                                             struct libusb_device_descriptor *desc)
{
	struct fake_dev *fd = (struct fake_dev *)dev;

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
	fake.opened = NULL;
}

int LIBUSB_CALL libusb_claim_interface(libusb_device_handle *devh, int interface)
{
	(void)devh;
	(void)interface;
	fake.claims++;
	return fake.claim_rc;
}

int LIBUSB_CALL libusb_release_interface(libusb_device_handle *devh, int interface)
{
	(void)devh;
	(void)interface;
	fake.releases++;
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

/* Records every request the library sends, with the timeout it gave, and
 * answers the serial read with the device's own serial. */
int LIBUSB_CALL libusb_control_transfer(libusb_device_handle *devh,
	uint8_t request_type, uint8_t bRequest, uint16_t wValue, uint16_t wIndex,
	unsigned char *data, uint16_t wLength, unsigned int timeout)
{
	struct fake_dev *fd = dev_of_handle(devh);

	(void)request_type;
	(void)wValue;
	(void)wIndex;
	if (fake.requests_n < MAX_REQUESTS) {
		fake.requests[fake.requests_n] = bRequest;
		fake.timeouts[fake.requests_n] = timeout;
		fake.requests_n++;
	}
	if (data != NULL && wLength > 0)
		memset(data, 0, wLength);
	if (bRequest == UBERTOOTH_GET_SERIAL && wLength == 17 && fd != NULL)
		memcpy(data + 1, fd->serial, sizeof(fd->serial)); /* data[0] is status */
	return wLength;
}

/* ---- helpers ------------------------------------------------------------ */

/* How many times request was sent, and the timeout the first of them had. */
static int sent(uint8_t request)
{
	int i, n = 0;

	for (i = 0; i < fake.requests_n; i++)
		if (fake.requests[i] == request)
			n++;
	return n;
}

static unsigned timeout_of(uint8_t request)
{
	int i;

	for (i = 0; i < fake.requests_n; i++)
		if (fake.requests[i] == request)
			return fake.timeouts[i];
	return 0;
}

/* Run the quiet (or the loud) selection of u_arg with both output streams
 * captured, so a case can require that nothing at all was written. */
static ubertooth_t *select_capturing(const char *u_arg, int quiet,
                                     char *text, size_t len)
{
	ubertooth_t *ut;
	FILE *capture = tmpfile();
	int saved_out = capture != NULL ? dup(fileno(stdout)) : -1;
	int saved_err = capture != NULL ? dup(fileno(stderr)) : -1;
	size_t n;

	text[0] = '\0';
	if (capture == NULL || saved_out < 0 || saved_err < 0) {
		mu_assert(0, "capture both streams");
		return NULL;
	}
	fflush(stdout);
	fflush(stderr);
	dup2(fileno(capture), fileno(stdout));
	dup2(fileno(capture), fileno(stderr));
	ut = quiet ? ubertooth_start_select_quiet(u_arg)
	           : ubertooth_start_select(u_arg);
	fflush(stdout);
	fflush(stderr);
	dup2(saved_out, fileno(stdout));
	dup2(saved_err, fileno(stderr));
	close(saved_out);
	close(saved_err);
	rewind(capture);
	n = fread(text, 1, len - 1, capture);
	text[n] = '\0';
	fclose(capture);
	return ut;
}

/* Release a handle the quiet path returned. ubertooth_close() rather than
 * ubertooth_stop(), so the test's own teardown does not add the STOP the
 * cases below require the library not to send. */
static void release(ubertooth_t *ut)
{
	if (ut == NULL)
		return;
	ubertooth_close(ut);
	ubertooth_free(ut);
}

/* ---- tests -------------------------------------------------------------- */

/* Its callers poll with it while printing their own progress, so it must say
 * nothing on either stream, whatever the outcome. */
MU_TEST(the_quiet_selection_prints_nothing)
{
	char text[512];
	ubertooth_t *ut;

	/* a device it finds */
	bus_with_ubertooths(1);
	ut = select_capturing("0", 1, text, sizeof(text));
	mu_assert_fatal(ut != NULL, text);
	mu_assert(text[0] == '\0', text);
	release(ut);

	/* an index that is not attached */
	bus_with_ubertooths(2);
	mu_assert(select_capturing("5", 1, text, sizeof(text)) == NULL, text);
	mu_assert(text[0] == '\0', text);

	/* an index no device could have */
	bus_with_ubertooths(1);
	mu_assert(select_capturing("4294967296", 1, text, sizeof(text)) == NULL,
	          text);
	mu_assert(text[0] == '\0', text);

	/* several devices and no index */
	bus_with_ubertooths(2);
	mu_assert(select_capturing(NULL, 1, text, sizeof(text)) == NULL, text);
	mu_assert(text[0] == '\0', text);

	/* a serial no board reports */
	bus_with_two_serials();
	mu_assert(select_capturing("deadbeef", 1, text, sizeof(text)) == NULL,
	          text);
	mu_assert(text[0] == '\0', text);

	/* a string that matches two boards */
	bus_with_ubertooths(2);
	set_serial(0, SERIAL_A);
	set_serial(1, SERIAL_A);
	mu_assert(select_capturing("1cc", 1, text, sizeof(text)) == NULL, text);
	mu_assert(text[0] == '\0', text);

	/* a bus that cannot be listed */
	bus_with_ubertooths(1);
	fake.list_rc = LIBUSB_ERROR_IO;
	mu_assert(select_capturing("0", 1, text, sizeof(text)) == NULL, text);
	mu_assert(text[0] == '\0', text);

	/* a board that cannot be opened */
	bus_with_ubertooths(1);
	fake.open_rc = LIBUSB_ERROR_ACCESS;
	mu_assert(select_capturing("0", 1, text, sizeof(text)) == NULL, text);
	mu_assert(text[0] == '\0', text);

	/* The loud path, for contrast: it does report what it found, so the
	 * silence above is this selector's own doing. */
	bus_with_ubertooths(2);
	mu_assert(select_capturing("5", 0, text, sizeof(text)) == NULL, text);
	mu_assert(strstr(text, "no Ubertooth at index 5") != NULL, text);
}

/* A board that is claimed by something else -- or still coming back from a
 * reset -- is closed again without a STOP, so a poll cannot take a board out
 * of the mode it is in. */
MU_TEST(a_board_that_cannot_be_claimed_gets_no_stop)
{
	char text[512];

	bus_with_ubertooths(1);
	fake.claim_rc = LIBUSB_ERROR_BUSY;
	mu_assert(select_capturing("0", 1, text, sizeof(text)) == NULL, text);
	mu_assert_int_eq(1, fake.opens);
	mu_assert_int_eq(1, fake.claims);
	mu_assert_int_eq(1, fake.closes); /* the handle is not left open */
	mu_assert(fake.opened == NULL, "no device left open");
	mu_assert_int_eq(0, sent(UBERTOOTH_STOP));
	mu_assert_int_eq(0, fake.requests_n); /* no request at all */
	mu_assert_int_eq(0, fake.lists);
	mu_assert(text[0] == '\0', text);

	/* The same board claimed by something else on the loud path does get a
	 * STOP, from the ubertooth_stop() its failure path calls: that is what
	 * the quiet selection exists to avoid. */
	bus_with_ubertooths(1);
	fake.claim_rc = LIBUSB_ERROR_BUSY;
	mu_assert(select_capturing("0", 0, text, sizeof(text)) == NULL, text);
	mu_assert_int_eq(1, sent(UBERTOOTH_STOP));
	mu_assert(strstr(text, "claiming the USB interface failed") != NULL, text);
}

/* An index is a position in the list of attached Ubertooths, so one past the
 * end selects nothing -- and the quiet path refuses it without opening or
 * reading anything. */
MU_TEST(an_index_past_several_devices_opens_nothing)
{
	static const char *const past_end[] = { "3", "4", "7", "99" };
	char text[512];
	size_t i;

	for (i = 0; i < sizeof(past_end) / sizeof(past_end[0]); i++) {
		bus_with_ubertooths(3);
		mu_assert(select_capturing(past_end[i], 1, text, sizeof(text)) == NULL,
		          past_end[i]);
		mu_assert_int_eq(0, fake.opens);
		mu_assert_int_eq(0, fake.requests_n);
		mu_assert_int_eq(0, fake.lists); /* the device list is freed */
		mu_assert(text[0] == '\0', text);
	}

	/* Several devices and no index: the loud path lists their serials, which
	 * means a request to every board. The quiet one opens nothing. */
	bus_with_ubertooths(3);
	mu_assert(select_capturing(NULL, 1, text, sizeof(text)) == NULL, text);
	mu_assert_int_eq(0, fake.opens);
	mu_assert_int_eq(0, fake.requests_n);
	mu_assert_int_eq(0, fake.lists);
}

/* With one board attached, index 0 and no index open it; a nonzero index does
 * not. The index is a position in a list of one, and only 0 is one: before
 * that was fixed, any index opened the only device, so -U1 meant the second
 * board with two attached and the only board with one. */
MU_TEST(the_only_device_is_opened_at_index_zero_and_by_default)
{
	static const char *const nonzero[] = { "1", "2", "9" };
	char text[512];
	ubertooth_t *ut;
	size_t i;

	bus_with_ubertooths(1);
	ut = select_capturing("0", 1, text, sizeof(text));
	mu_assert_fatal(ut != NULL, "-U0 of one board");
	mu_assert(fake.opened == &fake.devs[1], "and it is the Ubertooth");
	mu_assert_int_eq(1, fake.claims);
	mu_assert_int_eq(0, fake.requests_n); /* an index needs no request */
	release(ut);

	bus_with_ubertooths(1);
	ut = select_capturing(NULL, 1, text, sizeof(text));
	mu_assert_fatal(ut != NULL, "no -U of one board");
	mu_assert(fake.opened == &fake.devs[1], "and it is the Ubertooth");
	release(ut);

	for (i = 0; i < sizeof(nonzero) / sizeof(nonzero[0]); i++) {
		bus_with_ubertooths(1);
		mu_assert(select_capturing(nonzero[i], 1, text, sizeof(text)) == NULL,
		          nonzero[i]);
		mu_assert_int_eq(0, fake.opens);
		mu_assert(text[0] == '\0', text);
	}
}

/* Matching a serial means opening every attached board and reading it, so the
 * quiet path gives that read a short timeout: a board that does not answer
 * must not stall a caller that is polling for one that will. */
MU_TEST(serial_matching_uses_the_short_timeout)
{
	char text[512];
	ubertooth_t *ut;

	bus_with_two_serials();
	ut = select_capturing("05f", 1, text, sizeof(text));
	mu_assert_fatal(ut != NULL, "a prefix of the second board's serial");
	mu_assert(fake.opened == &fake.devs[2], "and it opens that board");
	mu_assert_int_eq(2, sent(UBERTOOTH_GET_SERIAL)); /* both boards read */
	mu_assert_int_eq(QUIET_SERIAL_TIMEOUT_MS,
	                 (int)timeout_of(UBERTOOTH_GET_SERIAL));
	mu_assert(text[0] == '\0', text);
	release(ut);

	/* The loud path keeps the longer timeout, so this is the quiet path's own
	 * choice and not the only one in the library. */
	bus_with_two_serials();
	ut = select_capturing("05f", 0, text, sizeof(text));
	mu_assert_fatal(ut != NULL, text);
	mu_assert_int_eq(LOUD_SERIAL_TIMEOUT_MS,
	                 (int)timeout_of(UBERTOOTH_GET_SERIAL));
	release(ut);
}

int main(void)
{
	MU_RUN(the_quiet_selection_prints_nothing);
	MU_RUN(a_board_that_cannot_be_claimed_gets_no_stop);
	MU_RUN(an_index_past_several_devices_opens_nothing);
	MU_RUN(the_only_device_is_opened_at_index_zero_and_by_default);
	MU_RUN(serial_matching_uses_the_short_timeout);
	return mu_report();
}
