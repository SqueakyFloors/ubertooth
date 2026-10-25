/*
 * Hardware-independent tests for the control requests the AFH paths send.
 *
 * AFH detection runs in the board's own AFH mode: cmd_afh() clears the map,
 * turns on AFH hopping and asks for MODE_AFH, and it has to be the last mode
 * request the board gets. Sending cmd_rx_syms() on top of it -- which is what
 * every other receive path starts with -- left detection receiving under
 * MODE_RX_SYMBOLS, so the board never hopped and the map stayed empty.
 * Nothing but a board caught that, which is what these tests are for.
 *
 * The real ubertooth.c, ubertooth_callback.c and ubertooth_control.c are
 * compiled in, and the test defines the libusb transfer calls and the fifo
 * calls itself, so the library's requests are recorded instead of sent and no
 * ring buffer, bulk endpoint or board is involved. The receive loops are
 * stopped from the faked fifo_empty() after a few turns, the way an
 * interrupted run stops them.
 *
 * rx_dump() is here as the opposite case: it is a plain symbol capture, so it
 * must send UBERTOOTH_RX_SYMBOLS. Without it, "never UBERTOOTH_RX_SYMBOLS"
 * would also hold if the recorder saw nothing at all.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <btbb.h>
#include <libusb.h>

#include "../../../firmware/tests/minunit.h"
#include "ubertooth.h"
#include "ubertooth_fifo.h"

#define MAX_REQUESTS 16

static struct {
	uint8_t requests[MAX_REQUESTS]; /* bRequest of each control transfer */
	int bulk_at[MAX_REQUESTS];      /* bulk transfers submitted by then */
	int n;
	int overflow;                   /* more requests than this records */
	int bulk_submits;
	int cancels;
	int allocated;
	int freed;
} fake;

/* The handle the receive loop under test is using, so the faked fifo can stop
 * it, and how many empty polls to allow first. */
static ubertooth_t* running_ut;
static int empty_polls;

static void reset(void)
{
	memset(&fake, 0, sizeof(fake));
	running_ut = NULL;
	empty_polls = 0;
}

/* ---- fifo fake ---------------------------------------------------------- */
/* ubertooth_fifo.c is not linked: a real ring buffer is 64 MB and nothing
 * here receives a packet. */

static usb_pkt_rx one_slot;

fifo_t* fifo_init()
{
	return (fifo_t*)malloc(1);
}

/* The receive loops spin on this. It always says empty -- no packet arrives
 * -- and after a few turns it stops the loop, as an interrupt or a timeout
 * would. */
uint8_t fifo_empty(fifo_t* fifo)
{
	(void)fifo;
	if (++empty_polls >= 3 && running_ut != NULL)
		running_ut->stop_ubertooth = 1;
	return 1;
}

usb_pkt_rx* fifo_get_write_element(fifo_t* fifo) { (void)fifo; return &one_slot; }
void fifo_inc_write_ptr(fifo_t* fifo) { (void)fifo; }
void fifo_push(fifo_t* fifo, const usb_pkt_rx* packet) { (void)fifo; (void)packet; }
usb_pkt_rx fifo_pop(fifo_t* fifo) { usb_pkt_rx p; (void)fifo; memset(&p, 0, sizeof(p)); return p; }

/* ---- libusb fakes ------------------------------------------------------- */
/* Defined here, so they take precedence over the libusb library symbols. */

int LIBUSB_CALL libusb_init(libusb_context** ctx)
{
	(void)ctx;
	return 0;
}

void LIBUSB_CALL libusb_exit(libusb_context* ctx)
{
	(void)ctx;
}

int LIBUSB_CALL libusb_control_transfer(libusb_device_handle* devh,
	uint8_t request_type, uint8_t bRequest, uint16_t wValue, uint16_t wIndex,
	unsigned char* data, uint16_t wLength, unsigned int timeout)
{
	(void)devh;
	(void)request_type;
	(void)wValue;
	(void)wIndex;
	(void)timeout;
	if (fake.n < MAX_REQUESTS) {
		fake.requests[fake.n] = bRequest;
		fake.bulk_at[fake.n] = fake.bulk_submits;
		fake.n++;
	} else {
		fake.overflow = 1;
	}
	if (data != NULL && wLength > 0)
		memset(data, 0, wLength);
	return wLength;
}

struct libusb_transfer* LIBUSB_CALL libusb_alloc_transfer(int iso_packets)
{
	struct libusb_transfer* xfer = calloc(1, sizeof(*xfer)
		+ (size_t)iso_packets * sizeof(struct libusb_iso_packet_descriptor));

	if (xfer != NULL)
		fake.allocated++;
	return xfer;
}

void LIBUSB_CALL libusb_free_transfer(struct libusb_transfer* xfer)
{
	if (xfer == NULL)
		return;
	fake.freed++;
	free(xfer);
}

int LIBUSB_CALL libusb_submit_transfer(struct libusb_transfer* xfer)
{
	(void)xfer;
	fake.bulk_submits++;
	return 0;
}

int LIBUSB_CALL libusb_cancel_transfer(struct libusb_transfer* xfer)
{
	(void)xfer;
	fake.cancels++;
	return 0;
}

/* The poll thread calls this; nothing completes, so it just waits its turn. */
int LIBUSB_CALL libusb_handle_events_timeout(libusb_context* ctx,
                                             struct timeval* tv)
{
	(void)ctx;
	(void)tv;
	usleep(1000);
	return 0;
}

/* ---- helpers ------------------------------------------------------------ */

static int fake_devh;

/* A handle with a device but no board behind it. */
static ubertooth_t* new_handle(void)
{
	ubertooth_t* ut = ubertooth_init();

	if (ut == NULL)
		return NULL;
	ut->devh = (struct libusb_device_handle*)&fake_devh;
	running_ut = ut;
	return ut;
}

/* Release it without ubertooth_stop(), whose own UBERTOOTH_STOP would land in
 * the recorded sequence. */
static void release_handle(ubertooth_t* ut)
{
	if (ut == NULL)
		return;
	libusb_free_transfer(ut->rx_xfer);
	ut->rx_xfer = NULL;
	ut->devh = NULL;
	running_ut = NULL;
	ubertooth_free(ut);
}

static btbb_piconet* new_piconet(void)
{
	btbb_piconet* pn = btbb_piconet_new();

	if (pn == NULL)
		return NULL;
	btbb_init_piconet(pn, 0x446688);
	btbb_piconet_set_uap(pn, 0x22);
	btbb_piconet_set_flag(pn, BTBB_IS_AFH, 1);
	btbb_piconet_set_flag(pn, BTBB_LOOKS_LIKE_AFH, 1);
	return pn;
}

/* Make the detection phase look like it received something, so the monitor
 * phase after it runs. cb_afh_initial() would set these bits from packets. */
static void mark_a_channel_seen(btbb_piconet* pn)
{
	uint8_t* afh_map = btbb_piconet_get_afh_map(pn);

	afh_map[0] = 0x01;
}

static FILE* capture_out;
static FILE* capture_err;
static int saved_stdout;
static int saved_stderr;

/* The AFH paths print the map as they go; keep it out of the test output, and
 * keep stderr so a case can require that nothing was reported. */
static void capture_output(void)
{
	capture_out = tmpfile();
	capture_err = tmpfile();
	saved_stdout = dup(fileno(stdout));
	saved_stderr = dup(fileno(stderr));
	mu_assert(capture_out != NULL && capture_err != NULL
	          && saved_stdout >= 0 && saved_stderr >= 0, "capture output");
	fflush(stdout);
	fflush(stderr);
	dup2(fileno(capture_out), fileno(stdout));
	dup2(fileno(capture_err), fileno(stderr));
}

static void release_output(char* err_text, size_t len)
{
	size_t n;

	fflush(stdout);
	fflush(stderr);
	dup2(saved_stdout, fileno(stdout));
	dup2(saved_stderr, fileno(stderr));
	close(saved_stdout);
	close(saved_stderr);
	rewind(capture_err);
	n = fread(err_text, 1, len - 1, capture_err);
	err_text[n] = '\0';
	fclose(capture_out);
	fclose(capture_err);
}

/* How many times request was sent. */
static int sent(uint8_t request)
{
	int i, n = 0;

	for (i = 0; i < fake.n; i++)
		if (fake.requests[i] == request)
			n++;
	return n;
}

/* The recorded requests, in order, as a printable list for a failure. */
static const char* requests_text(void)
{
	static char text[128];
	int i, used = 0;

	text[0] = '\0';
	for (i = 0; i < fake.n; i++)
		used += snprintf(text + used, sizeof(text) - used, "%s%u",
		                 i == 0 ? "" : ",", (unsigned int)fake.requests[i]);
	if (fake.overflow)
		snprintf(text + used, sizeof(text) - used, ",...");
	return text;
}

static void assert_requests(const uint8_t* expected, int n)
{
	int i;

	mu_assert(!fake.overflow, requests_text());
	mu_assert_int_eq(n, fake.n);
	for (i = 0; i < n && i < fake.n; i++) {
		mu_assert(fake.requests[i] == expected[i], requests_text());
	}
	/* The one that must never be there: it is what took the board out of
	 * AFH mode. */
	mu_assert_int_eq(0, sent(UBERTOOTH_RX_SYMBOLS));
}

/* ---- tests -------------------------------------------------------------- */

/* The detection phase: the channel is set to the AFH detection value, the
 * stream starts with UBERTOOTH_AFH, and the phase ends with UBERTOOTH_STOP.
 * With no packets received there is no map, so the caller is told so. */
MU_TEST(the_detection_phase_starts_the_stream_with_afh)
{
	static const uint8_t expected[] = {
		UBERTOOTH_SET_CHANNEL, UBERTOOTH_AFH, UBERTOOTH_STOP
	};
	char err[256] = "";
	ubertooth_t* ut;
	btbb_piconet* pn;
	int r;

	reset();
	ut = new_handle();
	pn = new_piconet();
	mu_assert(ut != NULL && pn != NULL, "a handle and a piconet");
	if (ut == NULL || pn == NULL)
		return;

	capture_output();
	r = rx_afh(ut, pn, 30);
	alarm(0); /* rx_afh() set one for the detection phase */
	release_output(err, sizeof(err));

	mu_assert_int_eq(UBERTOOTH_AFH_NO_PACKETS, r);
	assert_requests(expected, 3);
	mu_assert(err[0] == '\0', err);

	/* The mode request goes out once the bulk transfer is in place, and the
	 * channel before any of it. */
	mu_assert_int_eq(0, fake.bulk_at[0]);
	mu_assert(fake.bulk_at[1] >= 1, "AFH is asked for after the bulk submit");

	release_handle(ut);
	btbb_piconet_unref(pn);
}

/* With a map found, the monitor phase follows, and it asks for AFH again
 * rather than for symbols. */
MU_TEST(the_monitor_phase_after_a_detected_map_asks_for_afh_again)
{
	static const uint8_t expected[] = {
		UBERTOOTH_SET_CHANNEL, UBERTOOTH_AFH, UBERTOOTH_STOP, UBERTOOTH_AFH
	};
	char err[256] = "";
	ubertooth_t* ut;
	btbb_piconet* pn;
	int r;

	reset();
	ut = new_handle();
	pn = new_piconet();
	mu_assert(ut != NULL && pn != NULL, "a handle and a piconet");
	if (ut == NULL || pn == NULL)
		return;
	mark_a_channel_seen(pn);

	capture_output();
	r = rx_afh(ut, pn, 30);
	alarm(0);
	release_output(err, sizeof(err));

	mu_assert_int_eq(0, r);
	assert_requests(expected, 4);
	mu_assert(err[0] == '\0', err);

	release_handle(ut);
	btbb_piconet_unref(pn);
}

/* Without a timeout there is no detection phase: the monitor starts straight
 * away, and in AFH mode. */
MU_TEST(the_monitor_alone_asks_for_afh)
{
	static const uint8_t expected[] = {
		UBERTOOTH_SET_CHANNEL, UBERTOOTH_AFH
	};
	char err[256] = "";
	ubertooth_t* ut;
	btbb_piconet* pn;
	int r;

	reset();
	ut = new_handle();
	pn = new_piconet();
	mu_assert(ut != NULL && pn != NULL, "a handle and a piconet");
	if (ut == NULL || pn == NULL)
		return;

	capture_output();
	r = rx_afh(ut, pn, 0);
	release_output(err, sizeof(err));

	mu_assert_int_eq(0, r);
	assert_requests(expected, 2);
	mu_assert(err[0] == '\0', err);

	release_handle(ut);
	btbb_piconet_unref(pn);
}

/* The -r output format has its own receive loop, which starts the same way. */
MU_TEST(the_r_format_loop_asks_for_afh)
{
	static const uint8_t expected[] = {
		UBERTOOTH_SET_CHANNEL, UBERTOOTH_AFH
	};
	char err[256] = "";
	ubertooth_t* ut;
	btbb_piconet* pn;

	reset();
	ut = new_handle();
	pn = new_piconet();
	mu_assert(ut != NULL && pn != NULL, "a handle and a piconet");
	if (ut == NULL || pn == NULL)
		return;

	capture_output();
	rx_afh_r(ut, pn, 0);
	release_output(err, sizeof(err));

	assert_requests(expected, 2);
	mu_assert(err[0] == '\0', err);

	release_handle(ut);
	btbb_piconet_unref(pn);
}

/* The opposite case: a plain symbol capture does send UBERTOOTH_RX_SYMBOLS,
 * so the absence of it above is the AFH paths' own doing and not a recorder
 * that sees nothing. */
MU_TEST(a_symbol_capture_still_asks_for_symbols)
{
	char err[256] = "";
	ubertooth_t* ut;

	reset();
	ut = new_handle();
	mu_assert(ut != NULL, "a handle");
	if (ut == NULL)
		return;

	capture_output();
	rx_dump(ut, 1);
	release_output(err, sizeof(err));

	mu_assert_int_eq(1, fake.n);
	mu_assert_int_eq(1, sent(UBERTOOTH_RX_SYMBOLS));
	mu_assert_int_eq(0, sent(UBERTOOTH_AFH));
	mu_assert(err[0] == '\0', err);

	release_handle(ut);
}

int main(void)
{
	/* Nothing here should look for a board; make sure of it. */
	setenv("UBERTOOTH_TEST_NO_DEVICES", "1", 1);

	MU_RUN(the_detection_phase_starts_the_stream_with_afh);
	MU_RUN(the_monitor_phase_after_a_detected_map_asks_for_afh_again);
	MU_RUN(the_monitor_alone_asks_for_afh);
	MU_RUN(the_r_format_loop_asks_for_afh);
	MU_RUN(a_symbol_capture_still_asks_for_symbols);
	return mu_report();
}
