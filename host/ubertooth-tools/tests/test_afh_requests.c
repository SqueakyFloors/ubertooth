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
 *
 * The faked transfer calls also count what a run allocates and frees, and the
 * faked event handling delivers a cancellation to the transfer's callback the
 * way libusb does, so the cases can require that a stream gives its receive
 * transfer back when it ends.
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
#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* The channel the AFH paths set: not a real channel, it is how the firmware
 * is told to take its channel from the AFH hopping it is about to do. */
#define AFH_DETECTION_CHANNEL 9999

/* libubertooth's poll-thread flag: ubertooth_bulk_thread_start() clears it
 * and ubertooth_bulk_thread_stop() sets it, so it says whether the thread the
 * stream started was joined again. */
extern int do_exit;

static struct {
	uint8_t requests[MAX_REQUESTS]; /* bRequest of each control transfer */
	uint16_t values[MAX_REQUESTS];  /* its wValue */
	int bulk_at[MAX_REQUESTS];      /* bulk transfers submitted by then */
	int n;
	int overflow;                   /* more requests than this records */
	int bulk_submits;
	uint8_t refuse_request;         /* fail this request, as a stall does */
	int xfer_allocs;                /* libusb_alloc_transfer() calls */
	int xfer_frees;                 /* libusb_free_transfer() calls */
	struct libusb_transfer* in_flight; /* the submitted transfer, or NULL */
	int cancelled;                  /* a cancellation waiting to complete */
	int refuse_submit;              /* fail libusb_submit_transfer() */
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
	(void)wIndex;
	(void)timeout;
	if (fake.n < MAX_REQUESTS) {
		fake.requests[fake.n] = bRequest;
		fake.values[fake.n] = wValue;
		fake.bulk_at[fake.n] = fake.bulk_submits;
		fake.n++;
	} else {
		fake.overflow = 1;
	}
	if (bRequest == fake.refuse_request)
		return LIBUSB_ERROR_PIPE; /* the board stalled it */
	if (data != NULL && wLength > 0)
		memset(data, 0, wLength);
	return wLength;
}

struct libusb_transfer* LIBUSB_CALL libusb_alloc_transfer(int iso_packets)
{
	fake.xfer_allocs++;
	return calloc(1, sizeof(struct libusb_transfer)
		+ (size_t)iso_packets * sizeof(struct libusb_iso_packet_descriptor));
}

void LIBUSB_CALL libusb_free_transfer(struct libusb_transfer* xfer)
{
	if (xfer != NULL)
		fake.xfer_frees++;
	free(xfer);
}

/* Only the bulk receive transfer is submitted this way; the control requests
 * above are counted as they are made. */
int LIBUSB_CALL libusb_submit_transfer(struct libusb_transfer* xfer)
{
	fake.bulk_submits++;
	if (fake.refuse_submit)
		return LIBUSB_ERROR_IO;
	fake.in_flight = xfer;
	return 0;
}

/* As libusb does: a submitted transfer is cancelled and completes later, and
 * a transfer that is not in flight cannot be cancelled. The caller is allowed
 * to free that one itself, so telling the two apart is what the teardown
 * under test depends on. */
int LIBUSB_CALL libusb_cancel_transfer(struct libusb_transfer* xfer)
{
	if (xfer == NULL || xfer != fake.in_flight)
		return LIBUSB_ERROR_NOT_FOUND;
	fake.cancelled = 1;
	return 0;
}

/* Delivers a pending cancellation to the transfer's callback, which is how a
 * real teardown gets its transfer back; with nothing pending this just waits
 * its turn, as it does on the poll thread. */
int LIBUSB_CALL libusb_handle_events_timeout(libusb_context* ctx,
                                             struct timeval* tv)
{
	struct libusb_transfer* xfer = fake.in_flight;

	(void)ctx;
	(void)tv;
	if (fake.cancelled && xfer != NULL) {
		fake.cancelled = 0;
		fake.in_flight = NULL;
		xfer->status = LIBUSB_TRANSFER_CANCELLED;
		xfer->callback(xfer);
		return 0;
	}
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
	saved_stdout = capture_out != NULL ? dup(fileno(stdout)) : -1;
	saved_stderr = capture_err != NULL ? dup(fileno(stderr)) : -1;
	mu_assert(capture_out != NULL && capture_err != NULL
	          && saved_stdout >= 0 && saved_stderr >= 0, "capture output");
	if (saved_stdout < 0 || saved_stderr < 0)
		return; /* nothing is redirected, so nothing is restored below */
	fflush(stdout);
	fflush(stderr);
	dup2(fileno(capture_out), fileno(stdout));
	dup2(fileno(capture_err), fileno(stderr));
}

static void release_output(char* err_text, size_t len)
{
	size_t n;

	err_text[0] = '\0';
	if (saved_stdout < 0 || saved_stderr < 0) {
		/* nothing was redirected; close whatever the failed capture did get */
		if (saved_stdout >= 0)
			close(saved_stdout);
		if (saved_stderr >= 0)
			close(saved_stderr);
		if (capture_out != NULL)
			fclose(capture_out);
		if (capture_err != NULL)
			fclose(capture_err);
		return;
	}
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
	size_t used = 0;
	int i;

	text[0] = '\0';
	for (i = 0; i < fake.n && used + 1 < sizeof(text); i++) {
		int wrote = snprintf(text + used, sizeof(text) - used, "%s%u",
		                     i == 0 ? "" : ",",
		                     (unsigned int)fake.requests[i]);

		if (wrote < 0)
			break;
		/* snprintf returns what it would have written, not what it did */
		used += (size_t)wrote;
		if (used >= sizeof(text))
			used = sizeof(text) - 1;
	}
	if (fake.overflow && used + 1 < sizeof(text))
		snprintf(text + used, sizeof(text) - used, ",...");
	return text;
}

static void assert_requests(const uint8_t* expected, size_t n)
{
	size_t i;

	mu_assert(!fake.overflow, requests_text());
	mu_assert_int_eq(n, fake.n);
	for (i = 0; i < n && i < (size_t)fake.n; i++) {
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
	assert_requests(expected, ARRAY_LEN(expected));
	mu_assert_int_eq(AFH_DETECTION_CHANNEL, fake.values[0]);
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
	assert_requests(expected, ARRAY_LEN(expected));
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
	assert_requests(expected, ARRAY_LEN(expected));
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

	assert_requests(expected, ARRAY_LEN(expected));
	mu_assert_int_eq(AFH_DETECTION_CHANNEL, fake.values[0]);
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

/* A board that refuses the mode request leaves a poll thread running and a
 * libusb context about to be freed under it, so both paths have to join the
 * thread before they return. Nothing else here reaches those branches: every
 * faked transfer succeeds. */
MU_TEST(a_refused_mode_request_joins_the_poll_thread)
{
	static const uint8_t expected[] = {
		UBERTOOTH_SET_CHANNEL, UBERTOOTH_AFH
	};
	char err[256] = "";
	ubertooth_t* ut;
	btbb_piconet* pn;
	int r;

	reset();
	fake.refuse_request = UBERTOOTH_AFH;
	ut = new_handle();
	pn = new_piconet();
	mu_assert(ut != NULL && pn != NULL, "a handle and a piconet");
	if (ut == NULL || pn == NULL)
		return;

	capture_output();
	r = rx_afh(ut, pn, 0);
	release_output(err, sizeof(err));

	mu_assert_int_eq(LIBUSB_ERROR_PIPE, r);
	assert_requests(expected, ARRAY_LEN(expected));
	mu_assert_int_eq(1, do_exit); /* the poll thread was stopped */
	/* the library reports the refusal; the caller adds nothing */
	mu_assert(strstr(err, "control message unsupported") != NULL, err);

	release_handle(ut);

	/* and the -r loop, which joins the thread itself */
	reset();
	fake.refuse_request = UBERTOOTH_AFH;
	ut = new_handle();
	mu_assert(ut != NULL, "a handle");
	if (ut == NULL) {
		btbb_piconet_unref(pn);
		return;
	}

	capture_output();
	rx_afh_r(ut, pn, 0);
	release_output(err, sizeof(err));

	assert_requests(expected, ARRAY_LEN(expected));
	mu_assert_int_eq(1, do_exit);

	release_handle(ut);
	btbb_piconet_unref(pn);
}

/* Every receive transfer a run allocates is freed again. ubertooth_bulk_init()
 * used to assign ut->rx_xfer whatever was already there, so the two-phase AFH
 * run -- detect a map, then monitor it -- dropped the first phase's transfer:
 * the allocation leaks reported 80 bytes at, once per extra phase. Whether
 * the first phase's cancellation was ever handled depended on timing, so the
 * teardown is now done where the phase ends. */
MU_TEST(a_two_phase_run_frees_every_receive_transfer)
{
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
	mark_a_channel_seen(pn); /* so the monitor phase follows the detection */

	capture_output();
	r = rx_afh(ut, pn, 30);
	alarm(0); /* rx_afh() set one for the detection phase */
	release_output(err, sizeof(err));

	mu_assert_int_eq(0, r);
	mu_assert_int_eq(2, fake.bulk_submits); /* one transfer per phase */
	mu_assert_int_eq(2, fake.xfer_allocs);
	mu_assert_int_eq(2, fake.xfer_frees);
	mu_assert(ut->rx_xfer == NULL, "no transfer left on the handle");
	mu_assert(err[0] == '\0', err);

	release_handle(ut);
	btbb_piconet_unref(pn);
}

/* A stream with one phase ends the same way. Its transfer used to stay
 * allocated for the rest of the process: ubertooth_stop() cancelled it and
 * closed the device without waiting for the cancellation. */
MU_TEST(a_single_phase_stream_frees_its_receive_transfer)
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

	mu_assert_int_eq(1, fake.bulk_submits);
	mu_assert_int_eq(1, fake.xfer_allocs);
	mu_assert_int_eq(1, fake.xfer_frees);
	mu_assert(ut->rx_xfer == NULL, "no transfer left on the handle");
	mu_assert(err[0] == '\0', err);

	release_handle(ut);
}

/* A transfer libusb refused to submit was never in flight: it is freed rather
 * than left to refuse the next phase of the same run. */
MU_TEST(a_refused_submit_leaves_no_transfer_behind)
{
	char err[256] = "";
	ubertooth_t* ut;

	reset();
	fake.refuse_submit = 1;
	ut = new_handle();
	mu_assert(ut != NULL, "a handle");
	if (ut == NULL)
		return;

	capture_output();
	mu_assert_int_eq(-1, ubertooth_bulk_init(ut));
	release_output(err, sizeof(err));

	mu_assert_int_eq(1, fake.xfer_allocs);
	mu_assert_int_eq(1, fake.xfer_frees);
	mu_assert(ut->rx_xfer == NULL, "no transfer left on the handle");
	mu_assert(strstr(err, "submitting the receive transfer failed") != NULL,
	          err);

	/* and the next one on the same handle is accepted */
	fake.refuse_submit = 0;
	capture_output();
	mu_assert_int_eq(0, ubertooth_bulk_init(ut));
	release_output(err, sizeof(err));
	mu_assert(err[0] == '\0', err);
	mu_assert(ut->rx_xfer != NULL, "a transfer in place");

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
	MU_RUN(a_refused_mode_request_joins_the_poll_thread);
	MU_RUN(a_two_phase_run_frees_every_receive_transfer);
	MU_RUN(a_single_phase_stream_frees_its_receive_transfer);
	MU_RUN(a_refused_submit_leaves_no_transfer_behind);
	return mu_report();
}
