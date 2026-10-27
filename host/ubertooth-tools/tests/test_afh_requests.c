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
 * The faked transfer calls also model what libusb does with a transfer once it
 * has one: a submitted transfer is in flight, a cancelled one stays in flight
 * until its callback runs, and a second cancel of that one is refused even
 * though libusb is still holding it. Freeing a transfer in either of those
 * states is undefined behaviour, so the fake records it rather than doing it,
 * and keeps the body until the case ends so a completion or a cancel that
 * arrives afterwards is caught here too. That is what lets the teardown cases
 * below pin a transfer's lifetime without a board and without a sanitizer.
 */
#include <pthread.h>
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
/* Transfers one case can have: a two-phase AFH run allocates two. */
#define MAX_XFERS 4
#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* The channel the AFH paths set: not a real channel, it is how the firmware
 * is told to take its channel from the AFH hopping it is about to do. */
#define AFH_DETECTION_CHANNEL 9999

/* libubertooth's poll-thread flag: ubertooth_bulk_thread_start() clears it
 * and ubertooth_bulk_thread_stop() sets it, so it says whether the thread the
 * stream started was joined again. */
extern int do_exit;

/* Where one transfer is, as libusb would see it. */
enum fake_xfer_state {
	FX_OURS,       /* allocated, or handed back by its callback */
	FX_IN_FLIGHT,  /* submitted: libusb is holding it */
	FX_CANCELLING, /* submitted and cancelled: libusb is still holding it */
	FX_FREED
};

struct fake_xfer {
	struct libusb_transfer* xfer;
	enum fake_xfer_state state;
};

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
	struct fake_xfer xfers[MAX_XFERS];
	int n_xfers;
	int too_many_xfers;             /* more transfers than this models */
	int freed_in_flight;            /* freed while libusb was holding it */
	int freed_twice;
	int used_after_free;            /* submitted or cancelled once freed */
	int refuse_submit;              /* fail libusb_submit_transfer() */
	int cancel_rc;                  /* if set, what a cancel of an in-flight
	                                 * transfer returns; it stays in flight */
	int time_out_once;              /* the next completion is a bulk timeout
	                                 * rather than the cancellation */
	int complete_once;              /* ... or a packet */
	int packet_pending;             /* fifo_empty() reports a packet waiting */
	int poll_thread_idle;           /* completions are delivered only to the
	                                 * thread that asks for them */
	int device_gone;                /* the board has left the bus: a transfer
	                                 * libusb is holding comes back with
	                                 * LIBUSB_TRANSFER_NO_DEVICE, and every
	                                 * control request is refused */
	int closes;                     /* libusb_close() calls */
	int exits;                      /* libusb_exit() calls */
	pthread_t main_thread;
	int clear_halts;                /* libusb_clear_halt() calls */
	int clear_halt_at_submit;       /* bulk transfers submitted by then */
	unsigned char clear_halt_ep;    /* the endpoint of the last one */
	int refuse_clear_halt;          /* fail libusb_clear_halt() */
} fake;

/* The handle the receive loop under test is using, so the faked fifo can stop
 * it, and how many empty polls to allow first. */
static ubertooth_t* running_ut;
static int empty_polls;

/* Released here rather than by the faked free, so that a transfer freed while
 * libusb was holding it, or one used after it was freed, is still caught by
 * the case that did it. */
static void reset(void)
{
	int i;

	for (i = 0; i < fake.n_xfers; i++)
		free(fake.xfers[i].xfer);
	memset(&fake, 0, sizeof(fake));
	fake.main_thread = pthread_self();
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
	/* A case that needs the receive loop to stop with a packet in hand says
	 * so: that is the path which cancels the transfer itself, so the
	 * teardown's own cancel then meets a cancellation already under way. */
	return fake.packet_pending ? 0 : 1;
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
	fake.exits++;
}

void LIBUSB_CALL libusb_close(libusb_device_handle* devh)
{
	(void)devh;
	fake.closes++;
}

int LIBUSB_CALL libusb_release_interface(libusb_device_handle* devh,
                                         int interface_number)
{
	(void)devh;
	(void)interface_number;
	return 0;
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
	if (fake.device_gone)
		return LIBUSB_ERROR_NO_DEVICE;
	if (bRequest == fake.refuse_request)
		return LIBUSB_ERROR_PIPE; /* the board stalled it */
	if (data != NULL && wLength > 0)
		memset(data, 0, wLength);
	return wLength;
}

/* Clearing the halt feature is how a stream start empties the board's bulk IN
 * endpoint, which can still hold packets from a capture that has ended. */
int LIBUSB_CALL libusb_clear_halt(libusb_device_handle* devh,
	unsigned char endpoint)
{
	(void)devh;
	fake.clear_halts++;
	fake.clear_halt_ep = endpoint;
	fake.clear_halt_at_submit = fake.bulk_submits;
	return fake.refuse_clear_halt ? LIBUSB_ERROR_PIPE : 0;
}

/* The record of a transfer the fake knows about, or NULL for one it does
 * not. */
static struct fake_xfer* find_xfer(const struct libusb_transfer* xfer)
{
	int i;

	for (i = 0; i < fake.n_xfers; i++)
		if (fake.xfers[i].xfer == xfer)
			return &fake.xfers[i];
	return NULL;
}

struct libusb_transfer* LIBUSB_CALL libusb_alloc_transfer(int iso_packets)
{
	struct libusb_transfer* xfer = calloc(1, sizeof(struct libusb_transfer)
		+ (size_t)iso_packets * sizeof(struct libusb_iso_packet_descriptor));

	fake.xfer_allocs++;
	if (xfer == NULL)
		return NULL;
	if (fake.n_xfers >= MAX_XFERS) {
		fake.too_many_xfers = 1;
		free(xfer);
		return NULL;
	}
	fake.xfers[fake.n_xfers].xfer = xfer;
	fake.xfers[fake.n_xfers].state = FX_OURS;
	fake.n_xfers++;
	return xfer;
}

/* A transfer libusb is holding must not be freed, and neither must one that
 * has been freed already: both are recorded instead of done, and the body is
 * kept until the case ends (see reset()) so that anything arriving for it
 * afterwards lands here rather than in freed memory. */
void LIBUSB_CALL libusb_free_transfer(struct libusb_transfer* xfer)
{
	struct fake_xfer* fx;

	if (xfer == NULL)
		return;
	fake.xfer_frees++;
	fx = find_xfer(xfer);
	if (fx == NULL) {
		free(xfer); /* not one of ours: nothing is recorded about it */
		return;
	}
	if (fx->state == FX_FREED)
		fake.freed_twice = 1;
	else if (fx->state != FX_OURS)
		fake.freed_in_flight = 1;
	fx->state = FX_FREED;
}

/* Drops a transfer the library left behind, without counting it as the
 * library's own free: a case that ends with one still in flight is tidying
 * up, not making the mistake it is testing for. */
static void discard_xfer(struct libusb_transfer* xfer)
{
	struct fake_xfer* fx = find_xfer(xfer);

	if (fx != NULL)
		fx->state = FX_FREED;
}

/* Only the bulk receive transfer is submitted this way; the control requests
 * above are counted as they are made. */
int LIBUSB_CALL libusb_submit_transfer(struct libusb_transfer* xfer)
{
	struct fake_xfer* fx = find_xfer(xfer);

	fake.bulk_submits++;
	if (fx == NULL || fx->state == FX_FREED) {
		fake.used_after_free = 1;
		return LIBUSB_ERROR_NO_DEVICE;
	}
	if (fake.refuse_submit)
		return LIBUSB_ERROR_IO;
	fx->state = FX_IN_FLIGHT;
	return 0;
}

/* As libusb does. A transfer it is not holding cannot be cancelled -- the
 * caller may free that one itself. Neither can one whose cancellation is
 * already under way, and that one is still in flight with its callback to
 * come, which is why the result of this cannot decide what may be freed.
 * cancel_rc stands in for the other refusal with a transfer still in flight:
 * a device that has left the bus. */
int LIBUSB_CALL libusb_cancel_transfer(struct libusb_transfer* xfer)
{
	struct fake_xfer* fx = find_xfer(xfer);

	if (fx == NULL || fx->state == FX_FREED) {
		if (fx != NULL)
			fake.used_after_free = 1;
		return LIBUSB_ERROR_NOT_FOUND;
	}
	if (fx->state != FX_IN_FLIGHT)
		return LIBUSB_ERROR_NOT_FOUND;
	if (fake.cancel_rc != 0)
		return fake.cancel_rc;
	fx->state = FX_CANCELLING;
	return 0;
}

/* Delivers one completion to its transfer's callback, which is how a real
 * teardown gets its transfer back; with nothing to deliver this just waits its
 * turn, as it does on the poll thread.
 *
 * A cancelled transfer comes back cancelled, unless the case asked for a bulk
 * timeout or a packet instead: libusb reports either for a transfer whose
 * completion beat the cancellation, and either used to put a cancelled
 * transfer back in flight. A case with device_gone set gets NO_DEVICE for any
 * transfer libusb is holding.
 *
 * poll_thread_idle keeps the delivery on the calling thread, which is the
 * timing a teardown sees when the poll thread has already been joined. */
int LIBUSB_CALL libusb_handle_events_timeout(libusb_context* ctx,
                                             struct timeval* tv)
{
	int i;

	(void)ctx;
	(void)tv;
	if (fake.poll_thread_idle && !pthread_equal(pthread_self(),
	                                            fake.main_thread)) {
		usleep(1000);
		return 0;
	}
	for (i = 0; i < fake.n_xfers; i++) {
		struct fake_xfer* fx = &fake.xfers[i];
		struct libusb_transfer* xfer = fx->xfer;

		if (fx->state != FX_IN_FLIGHT && fx->state != FX_CANCELLING)
			continue;
		if (fx->state == FX_IN_FLIGHT && !fake.device_gone
		    && !fake.time_out_once && !fake.complete_once)
			continue; /* nothing has happened to it yet */
		if (fake.device_gone) {
			/* A board that has left the bus ends the transfer this way
			 * whether or not a cancellation was already under way. */
			xfer->status = LIBUSB_TRANSFER_NO_DEVICE;
		} else if (fake.time_out_once) {
			fake.time_out_once = 0;
			xfer->status = LIBUSB_TRANSFER_TIMED_OUT;
		} else if (fake.complete_once) {
			fake.complete_once = 0;
			xfer->status = LIBUSB_TRANSFER_COMPLETED;
			xfer->actual_length = PKT_LEN;
		} else {
			xfer->status = LIBUSB_TRANSFER_CANCELLED;
		}
		fx->state = FX_OURS;
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
	discard_xfer(ut->rx_xfer);
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

/* What no run may ever do, whatever else a case checks: free a transfer
 * libusb is holding, free one twice, or use one that has been freed. */
static void assert_transfer_lifetimes(void)
{
	mu_assert(!fake.freed_in_flight,
	          "a transfer was freed while libusb was holding it");
	mu_assert(!fake.freed_twice, "a transfer was freed twice");
	mu_assert(!fake.used_after_free, "a freed transfer was used again");
	mu_assert(!fake.too_many_xfers, "more transfers than the fake models");
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

	assert_transfer_lifetimes();
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

	assert_transfer_lifetimes();
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

	assert_transfer_lifetimes();
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
	int r;

	reset();
	ut = new_handle();
	pn = new_piconet();
	mu_assert(ut != NULL && pn != NULL, "a handle and a piconet");
	if (ut == NULL || pn == NULL)
		return;

	capture_output();
	r = rx_afh_r(ut, pn, 0);
	release_output(err, sizeof(err));

	mu_assert_int_eq(0, r); /* stopped normally */
	assert_requests(expected, ARRAY_LEN(expected));
	mu_assert_int_eq(AFH_DETECTION_CHANNEL, fake.values[0]);
	mu_assert(err[0] == '\0', err);

	assert_transfer_lifetimes();
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

	assert_transfer_lifetimes();
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
	r = rx_afh_r(ut, pn, 0);
	release_output(err, sizeof(err));

	mu_assert_int_eq(LIBUSB_ERROR_PIPE, r);
	assert_requests(expected, ARRAY_LEN(expected));
	mu_assert_int_eq(1, do_exit);

	assert_transfer_lifetimes();
	release_handle(ut);
	btbb_piconet_unref(pn);
}

/* The band sweep is what makes the map a map: without it the board stays on
 * one channel, so a refused request ends the receive phase instead of being
 * carried past it. Both paths used to send it and look at nothing, then
 * stream and print a map of the one channel the board happened to sit on. No
 * stream starts, and the library's own report is the only line. */
MU_TEST(a_refused_band_sweep_ends_the_receive_phase)
{
	static const uint8_t expected[] = { UBERTOOTH_SET_CHANNEL };
	char err[256] = "";
	ubertooth_t* ut;
	btbb_piconet* pn;
	int r;

	reset();
	fake.refuse_request = UBERTOOTH_SET_CHANNEL;
	ut = new_handle();
	pn = new_piconet();
	mu_assert(ut != NULL && pn != NULL, "a handle and a piconet");
	if (ut == NULL || pn == NULL)
		return;

	capture_output();
	r = rx_afh(ut, pn, 30);
	alarm(0); /* in case the detection phase was reached after all */
	release_output(err, sizeof(err));

	mu_assert_int_eq(LIBUSB_ERROR_PIPE, r);
	assert_requests(expected, ARRAY_LEN(expected));
	mu_assert_int_eq(AFH_DETECTION_CHANNEL, fake.values[0]);
	mu_assert_int_eq(0, fake.bulk_submits);
	/* the library reports the refusal; the caller adds nothing */
	mu_assert(strstr(err, "control message unsupported") != NULL, err);

	assert_transfer_lifetimes();
	release_handle(ut);

	/* and the -r loop, whose return value is how its caller finds out */
	reset();
	fake.refuse_request = UBERTOOTH_SET_CHANNEL;
	ut = new_handle();
	mu_assert(ut != NULL, "a handle");
	if (ut == NULL) {
		btbb_piconet_unref(pn);
		return;
	}

	capture_output();
	r = rx_afh_r(ut, pn, 0);
	release_output(err, sizeof(err));

	mu_assert_int_eq(LIBUSB_ERROR_PIPE, r);
	assert_requests(expected, ARRAY_LEN(expected));
	mu_assert_int_eq(0, fake.bulk_submits);
	mu_assert(strstr(err, "control message unsupported") != NULL, err);

	assert_transfer_lifetimes();
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
	assert_transfer_lifetimes();

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
	assert_transfer_lifetimes();

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

	assert_transfer_lifetimes();
	release_handle(ut);
}

/* The ordinary interrupted capture: the receive loop stops with a packet in
 * hand, so it cancels the transfer itself, and the teardown's own cancel is
 * then refused because a cancellation is already under way. Reading that
 * refusal as "not in flight" freed a transfer libusb was still holding, which
 * is undefined behaviour and, in a two-phase run, dropped the next phase's
 * transfer as well. */
MU_TEST(a_stop_with_a_packet_in_hand_gives_the_transfer_back)
{
	char err[1024] = "";
	ubertooth_t* ut;

	reset();
	fake.packet_pending = 1;
	/* so the cancellation is still outstanding when the teardown runs, as
	 * it is whenever the poll thread is joined before reaping it */
	fake.poll_thread_idle = 1;
	ut = new_handle();
	mu_assert(ut != NULL, "a handle");
	if (ut == NULL)
		return;

	capture_output();
	rx_dump(ut, 1);
	release_output(err, sizeof(err));

	assert_transfer_lifetimes();
	mu_assert_int_eq(1, fake.xfer_allocs);
	mu_assert_int_eq(1, fake.xfer_frees);
	mu_assert(ut->rx_xfer == NULL, "no transfer left on the handle");
	/* the packets it had are reported; nothing goes wrong */
	mu_assert(strstr(err, "error:") == NULL, err);

	release_handle(ut);
}

/* The refusals that do not mean the transfer is back. libusb reports
 * NOT_FOUND for a transfer whose cancellation is already under way, which is
 * still in flight with its callback to come, and NO_DEVICE for a board that
 * has left the bus, which is no guarantee either. Reading either as "not in
 * flight" freed a transfer libusb was still holding. Such a transfer is left
 * allocated instead, so the run ends without touching it and the next phase
 * on the same handle refuses to start. */
MU_TEST(a_cancel_the_device_refuses_leaves_the_transfer_alone)
{
	static const int refusals[] = {
		LIBUSB_ERROR_NOT_FOUND, LIBUSB_ERROR_NO_DEVICE
	};
	char err[1024] = "";
	ubertooth_t* ut;
	size_t i;

	for (i = 0; i < ARRAY_LEN(refusals); i++) {
		reset();
		fake.cancel_rc = refusals[i];
		fake.poll_thread_idle = 1;
		ut = new_handle();
		mu_assert(ut != NULL, "a handle");
		if (ut == NULL)
			return;

		capture_output();
		rx_dump(ut, 1);
		release_output(err, sizeof(err));

		assert_transfer_lifetimes();
		mu_assert_int_eq(1, fake.xfer_allocs);
		mu_assert_int_eq(0, fake.xfer_frees);
		mu_assert(ut->rx_xfer != NULL, "the transfer is still libusb's");

		/* and the next phase on the same handle refuses to start rather
		 * than replacing a transfer libusb is holding */
		capture_output();
		mu_assert_int_eq(-1, ubertooth_bulk_init(ut));
		release_output(err, sizeof(err));
		assert_transfer_lifetimes();
		mu_assert(strstr(err, "the previous receive transfer did not finish")
		          != NULL, err);

		release_handle(ut);
	}
}

/* A transfer libusb never hands back keeps the device and the libusb session
 * alive: both are things libusb may still need to finish it, and the
 * transfer's callback reaches this handle. The teardown says so and leaves
 * them, rather than closing the device and ending the session underneath a
 * transfer that is still in flight.
 *
 * It asks the board for nothing either, not even the stop: a transfer libusb
 * has neither completed nor cancelled is a board that has gone or stopped
 * answering, which no request can reach. The run is still marked failed, so
 * a tool exits non-zero on a capture that lost its stream. */
MU_TEST(a_transfer_that_never_comes_back_keeps_the_device_open)
{
	char err[1024] = "";
	ubertooth_t* ut;

	reset();
	/* refused for a transfer that stays in flight, and nothing delivers its
	 * completion: the bounded wait in the teardown runs out */
	fake.cancel_rc = LIBUSB_ERROR_NOT_FOUND;
	fake.poll_thread_idle = 1;
	ut = new_handle();
	mu_assert(ut != NULL, "a handle");
	if (ut == NULL)
		return;

	capture_output();
	mu_assert_int_eq(0, ubertooth_bulk_init(ut));
	ubertooth_stop(ut);
	release_output(err, sizeof(err));

	assert_transfer_lifetimes();
	mu_assert_int_eq(0, fake.xfer_frees);
	mu_assert_int_eq(0, fake.closes);
	mu_assert_int_eq(0, fake.exits);
	mu_assert(ut->devh != NULL, "the device is left open");
	/* nothing was asked of the board, the stop least of all */
	mu_assert_int_eq(0, sent(UBERTOOTH_STOP));
	mu_assert(fake.n == 0, requests_text());
	/* a warning, on standard error: the teardown itself did what it could,
	 * and the exit status below is what says the capture fell short */
	mu_assert(strstr(err, "warning: the receive transfer did not finish; the "
	          "Ubertooth is left open and may still be capturing") != NULL,
	          err);
	mu_assert(strstr(err, "error:") == NULL, err);
	/* what ubertooth-dump and the other streaming tools turn into exit 1 */
	mu_assert(ubertooth_stream_failed(ut), "the run is marked failed");
	/* ubertooth_free() leaves this handle alone too, so the callback of a
	 * transfer libusb is still holding does not land in freed memory. Not
	 * asserted here: every way of observing it reads the handle after the
	 * call, which is exactly the freed memory the guard exists to avoid. */

	release_handle(ut);

	/* and an ordinary stop does close the device and end the session */
	reset();
	ut = new_handle();
	mu_assert(ut != NULL, "a handle");
	if (ut == NULL)
		return;

	capture_output();
	mu_assert_int_eq(0, ubertooth_bulk_init(ut));
	ubertooth_stop(ut);
	release_output(err, sizeof(err));

	assert_transfer_lifetimes();
	mu_assert_int_eq(1, fake.xfer_frees);
	mu_assert_int_eq(1, fake.closes);
	mu_assert_int_eq(1, fake.exits);
	mu_assert(ut->devh == NULL, "the device is closed");

	release_handle(ut);
}

/* The detection phase takes the board out of AFH mode before the monitor
 * phase starts it again. Not when its own transfer was never handed back,
 * though: that is the same gone or unanswering board the teardown finds, and
 * the monitor phase cannot start on a handle whose transfer is still in
 * flight, so the run ends there. */
MU_TEST(a_detection_phase_that_lost_its_transfer_sends_no_stop)
{
	char err[1024] = "";
	ubertooth_t* ut;
	btbb_piconet* pn;
	int r;

	reset();
	/* refused for a transfer that stays in flight, and nothing delivers its
	 * completion: the bounded wait in the teardown runs out */
	fake.cancel_rc = LIBUSB_ERROR_NO_DEVICE;
	fake.poll_thread_idle = 1;
	ut = new_handle();
	pn = new_piconet();
	mu_assert(ut != NULL && pn != NULL, "a handle and a piconet");
	if (ut == NULL || pn == NULL)
		return;
	mark_a_channel_seen(pn);

	capture_output();
	r = rx_afh(ut, pn, 30);
	alarm(0); /* rx_afh() set one for the detection phase */
	release_output(err, sizeof(err));

	assert_transfer_lifetimes();
	mu_assert_int_eq(0, sent(UBERTOOTH_STOP));
	mu_assert(ut->rx_xfer != NULL, "the transfer is still libusb's");
	mu_assert(r < 0, "the run reports a failure");
	mu_assert(strstr(err, "the previous receive transfer did not finish")
	          != NULL, err);

	release_handle(ut);
	btbb_piconet_unref(pn);
}

/* A board that leaves the bus during a capture: the teardown's own wait for
 * events gets the transfer back with NO_DEVICE, so libusb is holding nothing
 * by the time the device is closed, and it closes as usual. The stop on the
 * way out is refused, as every request to a board that has gone is; the
 * refusal is reported and the teardown finishes it rather than stopping
 * there, so the capture files are still closed and the session still
 * ended. */
MU_TEST(a_board_that_leaves_the_bus_is_still_closed)
{
	char err[1024] = "";
	char refused[32];
	ubertooth_t* ut;

	reset();
	fake.poll_thread_idle = 1;
	ut = new_handle();
	mu_assert(ut != NULL, "a handle");
	if (ut == NULL)
		return;

	capture_output();
	mu_assert_int_eq(0, ubertooth_bulk_init(ut)); /* a capture is running */
	fake.device_gone = 1;                         /* and the board goes */
	ubertooth_stop(ut);
	release_output(err, sizeof(err));

	assert_transfer_lifetimes();
	mu_assert(ut->rx_xfer == NULL, "the transfer came back");
	mu_assert_int_eq(1, fake.xfer_frees);
	mu_assert_int_eq(1, fake.closes);
	mu_assert_int_eq(1, fake.exits);
	mu_assert(ut->devh == NULL, "the device is closed");
	mu_assert_int_eq(1, sent(UBERTOOTH_STOP));
	/* the lost transfer, and the stop the gone board could not answer. The
	 * code is built rather than spelled out, so the text libusb supplies
	 * for it is left to libusb. */
	snprintf(refused, sizeof(refused), "(%d)", LIBUSB_ERROR_NO_DEVICE);
	mu_assert(strstr(err, "error: receive transfer failed: Device "
	          "disconnected.") != NULL, err);
	mu_assert(strstr(err, "libUSB Error:") != NULL, err);
	mu_assert(strstr(err, refused) != NULL, err);
	mu_assert(ubertooth_stream_failed(ut), "the run is marked failed");

	release_handle(ut);
}

/* A completion that beats the teardown's cancellation -- a bulk timeout, or a
 * last packet -- does not put the transfer back in flight. Re-arming it left
 * the teardown waiting for a completion that was no longer coming, so the wait
 * ran out, the device was never closed and the next phase refused to start.
 *
 * The teardown here runs with stop_ubertooth clear, which is what a teardown
 * between the phases of a run, or after a mode request the board refused,
 * looks like: the cancellation is the only thing that says a teardown is
 * waiting. */
MU_TEST(a_completion_during_the_teardown_does_not_re_arm_the_transfer)
{
	static const char* const what[] = { "a bulk timeout", "a packet" };
	char err[1024] = "";
	ubertooth_t* ut;
	size_t i;

	for (i = 0; i < ARRAY_LEN(what); i++) {
		reset();
		fake.poll_thread_idle = 1;
		ut = new_handle();
		mu_assert(ut != NULL, "a handle");
		if (ut == NULL)
			return;

		capture_output();
		mu_assert_int_eq(0, ubertooth_bulk_init(ut));
		if (i == 0)
			fake.time_out_once = 1;
		else
			fake.complete_once = 1;
		mu_assert_int_eq(0, ut->stop_ubertooth);
		ubertooth_stop(ut);
		release_output(err, sizeof(err));

		assert_transfer_lifetimes();
		mu_assert_int_eq(1, fake.bulk_submits); /* not re-armed */
		mu_assert_int_eq(1, fake.xfer_frees);
		mu_assert(ut->rx_xfer == NULL, what[i]);
		mu_assert_int_eq(1, fake.closes); /* so the device is closed */
		mu_assert_int_eq(1, fake.exits);
		/* neither is a failure, so neither is reported */
		mu_assert(strstr(err, "error:") == NULL, err);
		mu_assert(strstr(err, "warning:") == NULL, err);

		release_handle(ut);
	}
}

/* The other side of it: a completion during a running capture does put the
 * transfer back in flight, with the ring buffer advanced to the next slot. */
MU_TEST(a_packet_during_a_capture_re_arms_the_transfer)
{
	struct timeval tv = { 0, 1000 };
	char err[1024] = "";
	ubertooth_t* ut;

	reset();
	fake.poll_thread_idle = 1;
	ut = new_handle();
	mu_assert(ut != NULL, "a handle");
	if (ut == NULL)
		return;

	capture_output();
	mu_assert_int_eq(0, ubertooth_bulk_init(ut));
	fake.complete_once = 1;
	libusb_handle_events_timeout(NULL, &tv);
	release_output(err, sizeof(err));

	assert_transfer_lifetimes();
	mu_assert_int_eq(2, fake.bulk_submits); /* submitted, then re-armed */
	mu_assert_int_eq(0, fake.xfer_frees);
	mu_assert(ut->rx_xfer != NULL, "the transfer is back in flight");
	mu_assert(err[0] == '\0', err);

	/* and the teardown still gets it back */
	capture_output();
	ubertooth_stop(ut);
	release_output(err, sizeof(err));
	assert_transfer_lifetimes();
	mu_assert_int_eq(1, fake.xfer_frees);
	mu_assert(ut->rx_xfer == NULL, "no transfer left on the handle");
	mu_assert(err[0] == '\0', err);

	release_handle(ut);
}

/* A re-arm libusb refuses: the transfer is not in flight any more, so it is
 * freed instead of left to refuse the next phase of the same run. The failure
 * is reported, and on standard error. */
MU_TEST(a_refused_re_arm_gives_the_transfer_back)
{
	struct timeval tv = { 0, 1000 };
	char err[1024] = "";
	ubertooth_t* ut;

	reset();
	fake.poll_thread_idle = 1;
	ut = new_handle();
	mu_assert(ut != NULL, "a handle");
	if (ut == NULL)
		return;

	capture_output();
	mu_assert_int_eq(0, ubertooth_bulk_init(ut));
	fake.time_out_once = 1;
	fake.refuse_submit = 1;
	libusb_handle_events_timeout(NULL, &tv);
	release_output(err, sizeof(err));

	assert_transfer_lifetimes();
	mu_assert_int_eq(1, fake.xfer_frees);
	mu_assert(ut->rx_xfer == NULL, "no transfer left on the handle");
	mu_assert(strstr(err, "resubmitting the receive transfer failed") != NULL,
	          err);

	/* and the next phase on the same handle starts */
	fake.refuse_submit = 0;
	capture_output();
	mu_assert_int_eq(0, ubertooth_bulk_init(ut));
	release_output(err, sizeof(err));
	mu_assert(err[0] == '\0', err);
	assert_transfer_lifetimes();

	release_handle(ut);
}

/*
 * A stream starts from an empty pipe. The board's bulk IN endpoint holds two
 * packets in hardware, past the firmware's queue, so a capture that ended
 * with packets this host never collected left one in each -- and they reached
 * the next capture before its own mode had started. Clearing the halt feature
 * discards them and resets the data toggle on both sides.
 *
 * It has to happen before the receive transfer is submitted, and before the
 * mode request: after the request, the clear would take the caller's own first
 * packets with it.
 */
MU_TEST(a_stream_clears_the_receive_pipe_before_it_reads)
{
	char err[256] = "";
	ubertooth_t* ut;

	reset();
	ut = new_handle();
	mu_assert(ut != NULL, "a handle");
	if (ut == NULL)
		return;

	capture_output();
	mu_assert_int_eq(0, ubertooth_bulk_init(ut));
	release_output(err, sizeof(err));

	mu_assert_int_eq(1, fake.clear_halts);
	mu_assert_int_eq(DATA_IN, fake.clear_halt_ep);
	mu_assert_int_eq(0, fake.clear_halt_at_submit);
	mu_assert_int_eq(1, fake.bulk_submits);
	mu_assert(err[0] == '\0', err);

	release_handle(ut);
}

/* A board that refuses it is a warning on standard error, not a failure: the
 * stream still works, it may just open with a packet or two from before. */
MU_TEST(a_refused_pipe_clear_warns_and_still_streams)
{
	char err[256] = "";
	ubertooth_t* ut;

	reset();
	fake.refuse_clear_halt = 1;
	ut = new_handle();
	mu_assert(ut != NULL, "a handle");
	if (ut == NULL)
		return;

	capture_output();
	mu_assert_int_eq(0, ubertooth_bulk_init(ut));
	release_output(err, sizeof(err));

	mu_assert_int_eq(1, fake.bulk_submits);
	mu_assert(ut->rx_xfer != NULL, "a transfer in place");
	mu_assert(strstr(err, "warning: could not clear the receive endpoint")
	          != NULL, err);

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
	MU_RUN(a_refused_band_sweep_ends_the_receive_phase);
	MU_RUN(a_two_phase_run_frees_every_receive_transfer);
	MU_RUN(a_single_phase_stream_frees_its_receive_transfer);
	MU_RUN(a_refused_submit_leaves_no_transfer_behind);
	MU_RUN(a_stop_with_a_packet_in_hand_gives_the_transfer_back);
	MU_RUN(a_cancel_the_device_refuses_leaves_the_transfer_alone);
	MU_RUN(a_transfer_that_never_comes_back_keeps_the_device_open);
	MU_RUN(a_detection_phase_that_lost_its_transfer_sends_no_stop);
	MU_RUN(a_board_that_leaves_the_bus_is_still_closed);
	MU_RUN(a_completion_during_the_teardown_does_not_re_arm_the_transfer);
	MU_RUN(a_packet_during_a_capture_re_arms_the_transfer);
	MU_RUN(a_refused_re_arm_gives_the_transfer_back);
	MU_RUN(a_stream_clears_the_receive_pipe_before_it_reads);
	MU_RUN(a_refused_pipe_clear_warns_and_still_streams);
	reset(); /* release the last case's transfer bodies */
	return mu_report();
}
