/*
 * What libubertooth does when a capture's USB stream fails part-way.
 *
 * A board that stops answering, or drops off the bus, ends the receive
 * transfer with an error status. That used to be printed and then forgotten:
 * the receive loop kept spinning with nothing arriving until the tool's own
 * timeout or a Ctrl-C, and the run reported success, so a script could not
 * tell a capture that died from one that finished. The failure is now kept on
 * the handle, which stops the capture at once and gives every streaming tool
 * a non-zero exit status to report.
 *
 * The real ubertooth.c, ubertooth_callback.c and ubertooth_control.c are
 * compiled in; the test defines the libusb and fifo calls itself, so no bulk
 * endpoint, ring buffer or board is involved. Two ways in:
 *
 *  - the transfer callback on its own, driven straight from the test with the
 *    status libusb would have set, which is where the decision is made;
 *  - a whole stream (rx_dump(), rx_afh()), with the scripted event below
 *    delivered to the poll thread, which is where the loop and the return
 *    value a tool reads are.
 *
 * The cases that must NOT be read as a failure matter as much as the one that
 * must: a bulk timeout is what a quiet board looks like, the teardown's own
 * cancellation is how a stream ends, and a Ctrl-C is a user stopping a
 * capture that was working.
 */
/* RTLD_NEXT, used below to reach the real pthread_create, is a GNU extension.
 * Nothing in the build asks for the GNU set: the build asks for
 * _DEFAULT_SOURCE, which does not include it, and no compiler dialect
 * predefines _GNU_SOURCE. Current glibc declares RTLD_NEXT anyway, which is
 * why only the older ones (2.31 and 2.35, Ubuntu 20.04 and 22.04) stopped the
 * build here. This has to come before any include, because the first one
 * pulls in <features.h>, which is where the choice is read. */
#define _GNU_SOURCE 1

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
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

/* What the faked poll loop does to the transfer in flight the next time
 * libubertooth asks it for events. */
enum scripted_event {
	EVENT_NOTHING,       /* no events; the capture just keeps waiting */
	EVENT_TRANSFER_ERROR,
	EVENT_DEVICE_GONE,
	EVENT_TIMED_OUT,     /* a bulk timeout: the board had nothing to send */
	EVENT_INTERRUPT      /* a Ctrl-C reaching this process */
};

/* Guards the fields the poll thread and the capture's own thread both touch.
 * The library's own threading is what is under test here; the fake must not
 * add a race of its own to it. */
static pthread_mutex_t fake_lock = PTHREAD_MUTEX_INITIALIZER;

static struct {
	struct libusb_transfer* in_flight;
	int submits;
	/* 1 to have the board stall the teardown's STOP, which is what a board
	 * that will not leave its mode looks like, and 1 to have it gone from
	 * the bus as well. */
	int refuse_stop;
	int board_has_left_the_bus;
	int cancelled;
	int frees;
	int refuse_submit;        /* fail libusb_submit_transfer() */
	int refuse_resubmit_after;/* ...from this submit on, so a stream starts */
	/* The errno pthread_create() hands back, 0 to let a real thread start. */
	int refuse_thread;
	int threads_started;      /* pthread_create() calls */
	int threads_created;      /* ...that really made a thread */
	int threads_joined;       /* pthread_join() calls */
	int control_requests;     /* control requests the library sent */
	enum scripted_event event;
	int events_delivered;
	int published;            /* packets handed to the ring buffer */
} fake;

/* The handle the stream under test is using, and how many empty polls to
 * allow before the faked fifo stops the loop the way a timeout would. 0
 * leaves the loop running until something else ends it. */
static ubertooth_t* running_ut;
static int stop_after_polls;
static int empty_polls;

static void reset(void)
{
	pthread_mutex_lock(&fake_lock);
	memset(&fake, 0, sizeof(fake));
	pthread_mutex_unlock(&fake_lock);
	running_ut = NULL;
	stop_after_polls = 0;
	empty_polls = 0;
	/* The library keeps "a stop was asked for" in a global, which the Ctrl-C
	 * case below sets for real. Left set, it would end the next case's
	 * receive loop at its first turn, so a case meant to run until something
	 * stopped it would pass without ever proving that something did. */
	ubertooth_reset_stop_signals();
}

/* ---- fifo fake ---------------------------------------------------------- */
/* ubertooth_fifo.c is not linked: a real ring buffer is 72 MB and no case
 * here receives a packet. */

static usb_pkt_rx one_slot;

fifo_t* fifo_init(void)
{
	return (fifo_t*)malloc(1);
}

uint8_t fifo_empty(fifo_t* fifo)
{
	(void)fifo;
	if (stop_after_polls && ++empty_polls >= stop_after_polls
	    && running_ut != NULL)
		running_ut->stop_ubertooth = 1;
	return 1;
}

usb_pkt_rx* fifo_get_write_element(fifo_t* fifo) { (void)fifo; return &one_slot; }
/* Counted, because whether a packet was published is the whole of what the
 * cases below about a full-length transfer are asking. */
void fifo_inc_write_ptr(fifo_t* fifo, uint64_t arrival_ns)
{
	(void)fifo;
	(void)arrival_ns;
	pthread_mutex_lock(&fake_lock);
	fake.published++;
	pthread_mutex_unlock(&fake_lock);
}
void fifo_push(fifo_t* fifo, const usb_pkt_rx* packet, uint64_t arrival_ns)
{ (void)fifo; (void)packet; (void)arrival_ns; }
uint64_t fifo_popped_arrival_ns(const fifo_t* fifo) { (void)fifo; return 0; }
usb_pkt_rx fifo_pop(fifo_t* fifo)
{
	usb_pkt_rx p;

	(void)fifo;
	memset(&p, 0, sizeof(p));
	return p;
}

/* ---- pthread fakes ------------------------------------------------------ */

/* Defined here, so they win over the C library's: every pthread_create() and
 * pthread_join() in this program comes through them, including the pair
 * libubertooth makes for the thread that collects a capture.
 *
 * A process only fails to create a thread when it is out of threads or
 * memory, which a test cannot reach without putting the whole run at risk, so
 * the failure is scripted instead. Anything the case has not asked to fail
 * goes to the real call, looked up past this definition, so the streams that
 * do run get a real thread.
 */
int pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                   void* (*start)(void*), void* arg)
{
	static int (*real_pthread_create)(pthread_t*, const pthread_attr_t*,
	                                  void* (*)(void*), void*);

	fake.threads_started++;
	if (fake.refuse_thread != 0)
		return fake.refuse_thread;
	fake.threads_created++;
	if (real_pthread_create == NULL)
		real_pthread_create = (int (*)(pthread_t*, const pthread_attr_t*,
		                               void* (*)(void*), void*))
		                      dlsym(RTLD_NEXT, "pthread_create");
	/* Nothing in this program could start a thread then, which the cases
	 * that need one report as the failure it is. */
	if (real_pthread_create == NULL)
		return ENOSYS;
	return real_pthread_create(thread, attr, start, arg);
}

/* Counted the same way, and passed through for the same reason: joining a
 * pthread_t that was never filled in is undefined, so what a case has to be
 * able to see is whether a join was attempted at all. */
int pthread_join(pthread_t thread, void** retval)
{
	static int (*real_pthread_join)(pthread_t, void**);

	fake.threads_joined++;
	if (real_pthread_join == NULL)
		real_pthread_join = (int (*)(pthread_t, void**))
		                    dlsym(RTLD_NEXT, "pthread_join");
	if (real_pthread_join == NULL)
		return ESRCH;
	return real_pthread_join(thread, retval);
}

/* Stands in for the board's USB handle, and for the board itself in the
 * device list the fakes below keep. Nothing dereferences it. */
static int fake_devh;

/* ---- libusb fakes ------------------------------------------------------- */
/* Defined here, so they win over the libusb library symbols. */

int LIBUSB_CALL libusb_init(libusb_context** ctx) { (void)ctx; return 0; }
void LIBUSB_CALL libusb_exit(libusb_context* ctx) { (void)ctx; }

int LIBUSB_CALL libusb_control_transfer(libusb_device_handle* devh,
	uint8_t request_type, uint8_t bRequest, uint16_t wValue, uint16_t wIndex,
	unsigned char* data, uint16_t wLength, unsigned int timeout)
{
	(void)devh;
	(void)request_type;
	(void)wValue;
	(void)wIndex;
	(void)timeout;
	pthread_mutex_lock(&fake_lock);
	fake.control_requests++;
	pthread_mutex_unlock(&fake_lock);
	/* A board stalls a request it will not take. The teardown's STOP is the
	 * one that matters here: a board that stalls it is left running the mode
	 * the capture started. */
	if (bRequest == UBERTOOTH_STOP && fake.refuse_stop)
		return LIBUSB_ERROR_PIPE;
	if (data != NULL && wLength > 0)
		memset(data, 0, wLength);
	return wLength;
}

/* The board behind the handle, and the bus it is on. The report about a
 * stalled request asks whether the board has gone before it blames the
 * board's firmware, and the real check lists the bus; the handle here is not
 * a real one, so the list is this fake's. */
libusb_device* LIBUSB_CALL libusb_get_device(libusb_device_handle* devh)
{
	(void)devh;
	return (libusb_device*)&fake_devh;
}

ssize_t LIBUSB_CALL libusb_get_device_list(libusb_context* ctx,
	libusb_device*** list)
{
	static libusb_device* devices[1];

	(void)ctx;
	devices[0] = (libusb_device*)&fake_devh;
	*list = devices;
	return fake.board_has_left_the_bus ? 0 : 1;
}

void LIBUSB_CALL libusb_free_device_list(libusb_device** list,
	int unref_devices)
{
	(void)list;
	(void)unref_devices;
}

/* The teardown gives the interface and the handle back; neither reaches a
 * board here. */
int LIBUSB_CALL libusb_release_interface(libusb_device_handle* devh,
	int interface_number)
{
	(void)devh;
	(void)interface_number;
	return 0;
}

void LIBUSB_CALL libusb_close(libusb_device_handle* devh)
{
	(void)devh;
}

/* The stream clears the receive pipe before it reads; nothing here depends on
 * it, but the real call would reach a device this test does not have. */
int LIBUSB_CALL libusb_clear_halt(libusb_device_handle* devh,
	unsigned char endpoint)
{
	(void)devh;
	(void)endpoint;
	return 0;
}

struct libusb_transfer* LIBUSB_CALL libusb_alloc_transfer(int iso_packets)
{
	return calloc(1, sizeof(struct libusb_transfer)
		+ (size_t)iso_packets * sizeof(struct libusb_iso_packet_descriptor));
}

void LIBUSB_CALL libusb_free_transfer(struct libusb_transfer* xfer)
{
	/* A transfer must not still be reachable from the handle when it is
	 * freed. The capture's own loop cancels ut->rx_xfer as soon as it sees
	 * the stop, and the failure path is what sets that stop, so a free
	 * before the pointer is cleared is a transfer cancelled after it was
	 * freed. */
	if (xfer != NULL && running_ut != NULL)
		mu_assert(running_ut->rx_xfer != xfer,
		          "the handle still pointed at the transfer being freed");
	pthread_mutex_lock(&fake_lock);
	if (xfer != NULL)
		fake.frees++;
	if (xfer == fake.in_flight)
		fake.in_flight = NULL;
	pthread_mutex_unlock(&fake_lock);
	free(xfer);
}

int LIBUSB_CALL libusb_submit_transfer(struct libusb_transfer* xfer)
{
	int refused;

	/* As libusb does: the count of bytes received starts at zero, so a
	 * transfer put back in flight cannot carry the last packet's count
	 * forward. The callback reads that count to decide whether a packet
	 * arrived, so a backend that left it alone would publish the slot
	 * again. */
	if (xfer != NULL)
		xfer->actual_length = 0;
	pthread_mutex_lock(&fake_lock);
	fake.submits++;
	refused = fake.refuse_submit
	          || (fake.refuse_resubmit_after
	              && fake.submits > fake.refuse_resubmit_after);
	if (!refused)
		fake.in_flight = xfer;
	pthread_mutex_unlock(&fake_lock);
	return refused ? LIBUSB_ERROR_IO : 0;
}

/* As libusb does: only a transfer that is in flight can be cancelled, and it
 * completes later. */
int LIBUSB_CALL libusb_cancel_transfer(struct libusb_transfer* xfer)
{
	int found;

	pthread_mutex_lock(&fake_lock);
	found = xfer != NULL && xfer == fake.in_flight;
	if (found)
		fake.cancelled = 1;
	pthread_mutex_unlock(&fake_lock);
	return found ? 0 : LIBUSB_ERROR_NOT_FOUND;
}

/* The poll loop: delivers the scripted event, then any pending cancellation,
 * to the transfer's callback, the way libusb hands a completion back. Each
 * scripted event is delivered once, so a capture that survives one (a bulk
 * timeout) carries on instead of being fed the same completion forever.
 *
 * The callback is called with the lock released: it is the code under test,
 * and it calls back into these fakes. */
int LIBUSB_CALL libusb_handle_events_timeout(libusb_context* ctx,
                                             struct timeval* tv)
{
	struct libusb_transfer* xfer = NULL;
	enum libusb_transfer_status status = LIBUSB_TRANSFER_COMPLETED;

	(void)ctx;
	(void)tv;
	pthread_mutex_lock(&fake_lock);
	if (fake.in_flight != NULL && fake.event != EVENT_NOTHING) {
		switch (fake.event) {
		case EVENT_TRANSFER_ERROR:
			status = LIBUSB_TRANSFER_ERROR;
			break;
		case EVENT_DEVICE_GONE:
			status = LIBUSB_TRANSFER_NO_DEVICE;
			break;
		case EVENT_TIMED_OUT:
			status = LIBUSB_TRANSFER_TIMED_OUT;
			break;
		case EVENT_INTERRUPT:
			raise(SIGINT);
			break;
		case EVENT_NOTHING:
			break;
		}
		fake.event = EVENT_NOTHING;
		fake.events_delivered++;
		if (status != LIBUSB_TRANSFER_COMPLETED) {
			xfer = fake.in_flight;
			fake.in_flight = NULL;
		}
	} else if (fake.cancelled && fake.in_flight != NULL) {
		fake.cancelled = 0;
		xfer = fake.in_flight;
		fake.in_flight = NULL;
		status = LIBUSB_TRANSFER_CANCELLED;
	}
	pthread_mutex_unlock(&fake_lock);
	if (xfer != NULL) {
		xfer->status = status;
		xfer->callback(xfer);
		return 0;
	}
	usleep(1000);
	return 0;
}

/* ---- helpers ------------------------------------------------------------ */

static ubertooth_t* new_handle(void)
{
	ubertooth_t* ut = ubertooth_init();

	if (ut == NULL)
		return NULL;
	ut->devh = (struct libusb_device_handle*)&fake_devh;
	/* As ubertooth_connect() sets it: this handle is built by hand, and the
	 * teardown ends the libusb session only of a handle that opened one. */
	ut->usb_session_open = 1;
	running_ut = ut;
	return ut;
}

static void release_handle(ubertooth_t* ut)
{
	struct libusb_transfer* left;

	if (ut == NULL)
		return;
	/* Cleared first, so this does not trip the check in the fake free. */
	left = ut->rx_xfer;
	ut->rx_xfer = NULL;
	libusb_free_transfer(left);
	ut->devh = NULL;
	running_ut = NULL;
	ubertooth_free(ut);
}

static FILE* capture_out;
static FILE* capture_err;
static int saved_stdout = -1;
static int saved_stderr = -1;

/* A capture writes its packets to standard output, and the library its
 * diagnostics to standard error. Both are kept out of the test's own output
 * and read back, so a case can pin what was said and on which stream -- and
 * that a diagnostic never lands on the stream a capture's data goes to. */
static void capture_output(void)
{
	capture_out = tmpfile();
	capture_err = tmpfile();
	saved_stdout = capture_out != NULL ? dup(fileno(stdout)) : -1;
	saved_stderr = capture_err != NULL ? dup(fileno(stderr)) : -1;
	mu_assert(capture_out != NULL && capture_err != NULL
	          && saved_stdout >= 0 && saved_stderr >= 0, "capture output");
	if (saved_stdout < 0 || saved_stderr < 0)
		return;
	fflush(stdout);
	fflush(stderr);
	dup2(fileno(capture_out), fileno(stdout));
	dup2(fileno(capture_err), fileno(stderr));
}

static void release_output(char* out_text, size_t out_len,
                           char* err_text, size_t err_len)
{
	size_t n;

	out_text[0] = '\0';
	err_text[0] = '\0';
	if (saved_stdout < 0 || saved_stderr < 0) {
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
	rewind(capture_out);
	n = fread(out_text, 1, out_len - 1, capture_out);
	out_text[n] = '\0';
	rewind(capture_err);
	n = fread(err_text, 1, err_len - 1, capture_err);
	err_text[n] = '\0';
	fclose(capture_out);
	fclose(capture_err);
}

/* Hands the transfer in flight back to libubertooth's callback with the
 * status given, from the test's own thread: no poll thread is running in the
 * callback cases, so this is the whole of the event delivery. */
static void deliver(enum libusb_transfer_status status)
{
	struct libusb_transfer* xfer = fake.in_flight;

	mu_assert_fatal(xfer != NULL, "a transfer was in flight");
	if (status != LIBUSB_TRANSFER_TIMED_OUT)
		fake.in_flight = NULL;
	xfer->status = status;
	xfer->callback(xfer);
}

/* The same, for a transfer that brought bytes: libusb fills in how many it
 * received, which is what decides whether this is a packet. */
static void deliver_bytes(enum libusb_transfer_status status, int length,
                          uint8_t first_byte)
{
	mu_assert_fatal(fake.in_flight != NULL, "a transfer was in flight");
	if (length > 0 && fake.in_flight->buffer != NULL)
		fake.in_flight->buffer[0] = first_byte;
	fake.in_flight->actual_length = length;
	deliver(status);
}

static btbb_piconet* new_piconet(void)
{
	btbb_piconet* pn = btbb_piconet_new();

	if (pn == NULL)
		return NULL;
	btbb_init_piconet(pn, 0x446688);
	btbb_piconet_set_uap(pn, 0x22);
	return pn;
}

/* ---- the callback's decision ------------------------------------------- */

/* A transfer that failed: reported once, on standard error, and recorded, so
 * the capture stops here rather than waiting for a packet that cannot come. */
MU_TEST(a_failed_transfer_is_reported_and_recorded)
{
	static const enum libusb_transfer_status failures[] = {
		LIBUSB_TRANSFER_ERROR, LIBUSB_TRANSFER_NO_DEVICE,
		LIBUSB_TRANSFER_STALL, LIBUSB_TRANSFER_OVERFLOW
	};
	size_t i;

	for (i = 0; i < sizeof(failures) / sizeof(failures[0]); i++) {
		ubertooth_t* ut;
		char err[256] = "";
		char out[256] = "";

		reset();
		ut = new_handle();
		mu_assert_fatal(ut != NULL, "a handle");

		capture_output();
		mu_assert_int_eq(0, ubertooth_bulk_init(ut));
		deliver(failures[i]);
		release_output(out, sizeof(out), err, sizeof(err));

		mu_assert(ubertooth_stream_failed(ut), "the stream is marked failed");
		mu_assert_int_eq(failures[i], ut->rx_stream_status);
		/* The capture's loop watches this, so it ends at once. */
		mu_assert_int_eq(1, ut->stop_ubertooth);
		/* A stalled receive endpoint is the board refusing to send the
		 * capture at all, so it gets the shared report for a request the
		 * firmware has not got, which names what was asked for. */
		if (failures[i] == LIBUSB_TRANSFER_STALL)
			mu_assert(strcmp(err, "error: could not receive the capture: "
			          "the board's firmware does not support it "
			          "(stalled)\n") == 0, err);
		else
			mu_assert(strncmp(err, "error: receive transfer failed: ", 32)
			          == 0, err);
		mu_assert(strchr(err, '\n') == strrchr(err, '\n'), "said once");
		/* The library never writes to standard output: that is the
		 * capture's stream, and a tool may be piping it. */
		mu_assert(out[0] == '\0', out);

		release_handle(ut);
	}
}

/* A bulk timeout is what a board with nothing to send looks like: the same
 * transfer goes back in flight and the capture carries on, unmarked and with
 * nothing said. */
MU_TEST(a_bulk_timeout_is_not_a_failure)
{
	ubertooth_t* ut;
	char err[256] = "";
	char out[256] = "";
	int i;

	reset();
	ut = new_handle();
	mu_assert_fatal(ut != NULL, "a handle");

	capture_output();
	mu_assert_int_eq(0, ubertooth_bulk_init(ut));
	for (i = 0; i < 5; i++)
		deliver(LIBUSB_TRANSFER_TIMED_OUT);
	release_output(out, sizeof(out), err, sizeof(err));

	mu_assert(!ubertooth_stream_failed(ut), "the stream is still running");
	mu_assert_int_eq(0, ut->stop_ubertooth);
	mu_assert(err[0] == '\0', err);
	/* One submit to start it, and one per timeout. */
	mu_assert_int_eq(6, fake.submits);

	release_handle(ut);
}

/* A transfer that brought a whole 64-byte packet is published whichever of
 * the two statuses libusb gave it. A board cannot produce the timed-out one
 * on a full transfer to order -- libusb reports a transfer that received
 * every byte as completed -- so it is scripted here. It used to be dropped,
 * and the truncation warning passes over a full-length transfer, so a packet
 * the host already held was lost in silence. */
MU_TEST(a_whole_packet_is_published_whatever_status_came_with_it)
{
	static const enum libusb_transfer_status statuses[] = {
		LIBUSB_TRANSFER_COMPLETED, LIBUSB_TRANSFER_TIMED_OUT
	};
	size_t i;

	for (i = 0; i < sizeof(statuses) / sizeof(statuses[0]); i++) {
		ubertooth_t* ut;
		char err[256] = "";
		char out[256] = "";

		reset();
		ut = new_handle();
		mu_assert_fatal(ut != NULL, "a handle");

		capture_output();
		mu_assert_int_eq(0, ubertooth_bulk_init(ut));
		deliver_bytes(statuses[i], PKT_LEN, 0);
		release_output(out, sizeof(out), err, sizeof(err));

		mu_assert_int_eq(1, fake.published);
		/* A packet is what a running capture looks like, so nothing is
		 * said about it, and the capture carries on reading. */
		mu_assert(err[0] == '\0', err);
		mu_assert(out[0] == '\0', out);
		mu_assert(!ubertooth_stream_failed(ut), "the stream is running");
		mu_assert_int_eq(0, ut->stop_ubertooth);
		mu_assert_int_eq(2, fake.submits);

		release_handle(ut);
	}
}

/* A capture that is stopping keeps the packets it already has rather than
 * reading into another slot of a ring buffer nobody is draining, so a whole
 * packet arriving after the teardown has cancelled its transfer is not
 * published. The transfer is not put back in flight either: the teardown is
 * waiting for it. */
MU_TEST(a_whole_packet_is_not_published_once_a_teardown_wants_the_transfer)
{
	ubertooth_t* ut;
	char err[256] = "";
	char out[256] = "";

	reset();
	ut = new_handle();
	mu_assert_fatal(ut != NULL, "a handle");

	capture_output();
	mu_assert_int_eq(0, ubertooth_bulk_init(ut));
	/* As the capture's own thread does when it sees the stop. */
	ut->stop_ubertooth = 1;
	deliver_bytes(LIBUSB_TRANSFER_TIMED_OUT, PKT_LEN, 0);
	release_output(out, sizeof(out), err, sizeof(err));

	mu_assert_int_eq(0, fake.published);
	mu_assert_int_eq(1, fake.submits);  /* only the one that started it */
	mu_assert(err[0] == '\0', err);
	mu_assert(out[0] == '\0', out);
	mu_assert(!ubertooth_stream_failed(ut), "the stop is not a failure");

	release_handle(ut);
}

/* Short of a packet is still not a packet, and the keep-alive is still the
 * one short transfer that says nothing: the status it arrives with does not
 * change either. */
MU_TEST(a_short_transfer_is_not_published_whatever_status_came_with_it)
{
	static const enum libusb_transfer_status statuses[] = {
		LIBUSB_TRANSFER_COMPLETED, LIBUSB_TRANSFER_TIMED_OUT
	};
	size_t i;

	for (i = 0; i < sizeof(statuses) / sizeof(statuses[0]); i++) {
		ubertooth_t* ut;
		char err[256] = "";
		char out[256] = "";

		reset();
		ut = new_handle();
		mu_assert_fatal(ut != NULL, "a handle");

		capture_output();
		mu_assert_int_eq(0, ubertooth_bulk_init(ut));
		/* The keep-alive a quiet capture gets. */
		deliver_bytes(statuses[i], 1, KEEP_ALIVE);
		/* A packet cut short, which is a fault and is reported. */
		deliver_bytes(statuses[i], PKT_LEN - 1, 0);
		release_output(out, sizeof(out), err, sizeof(err));

		mu_assert_int_eq(0, fake.published);
		mu_assert(strstr(err, "warning: a receive transfer arrived with 63 "
		          "of a packet's 64 bytes and was dropped") != NULL, err);
		mu_assert(out[0] == '\0', out);
		mu_assert(!ubertooth_stream_failed(ut), "the stream is running");

		release_handle(ut);
	}
}

/* The teardown's own cancellation is how a stream ends. Reading it as a
 * failure would turn every normal end of capture into a non-zero exit. */
MU_TEST(a_cancellation_is_not_a_failure)
{
	ubertooth_t* ut;
	char err[256] = "";
	char out[256] = "";

	reset();
	ut = new_handle();
	mu_assert_fatal(ut != NULL, "a handle");

	capture_output();
	mu_assert_int_eq(0, ubertooth_bulk_init(ut));
	deliver(LIBUSB_TRANSFER_CANCELLED);
	release_output(out, sizeof(out), err, sizeof(err));

	mu_assert(!ubertooth_stream_failed(ut), "no failure recorded");
	mu_assert_int_eq(0, ut->stop_ubertooth);
	mu_assert(err[0] == '\0', err);

	release_handle(ut);
}

/* A re-arm libusb refuses leaves nothing in flight, so the board has nowhere
 * to send to: as dead a stream as a failed transfer. No completion is coming
 * either, so only the recorded failure can end the loop. */
MU_TEST(a_refused_re_arm_ends_the_capture)
{
	ubertooth_t* ut;
	char err[256] = "";
	char out[256] = "";

	reset();
	ut = new_handle();
	mu_assert_fatal(ut != NULL, "a handle");

	capture_output();
	mu_assert_int_eq(0, ubertooth_bulk_init(ut));
	fake.refuse_resubmit_after = 1; /* the one that started it was accepted */
	deliver(LIBUSB_TRANSFER_COMPLETED);
	release_output(out, sizeof(out), err, sizeof(err));

	mu_assert(ubertooth_stream_failed(ut), "the stream is marked failed");
	mu_assert_int_eq(1, ut->stop_ubertooth);
	mu_assert(strstr(err, "error: resubmitting the receive transfer failed")
	          != NULL, err);
	mu_assert(out[0] == '\0', out);

	release_handle(ut);

	/* The same when the refusal follows a bulk timeout, which is the other
	 * place the transfer is put back in flight. */
	reset();
	ut = new_handle();
	mu_assert_fatal(ut != NULL, "a handle");

	capture_output();
	mu_assert_int_eq(0, ubertooth_bulk_init(ut));
	fake.refuse_resubmit_after = 1;
	deliver(LIBUSB_TRANSFER_TIMED_OUT);
	release_output(out, sizeof(out), err, sizeof(err));

	mu_assert(ubertooth_stream_failed(ut), "the stream is marked failed");
	mu_assert_int_eq(1, ut->stop_ubertooth);
	mu_assert(strstr(err, "error: resubmitting the receive transfer failed")
	          != NULL, err);

	release_handle(ut);
}

/* What ended the stream is what explains it, so a later status does not
 * overwrite the first. */
MU_TEST(the_first_failure_is_the_one_kept)
{
	ubertooth_t* ut;
	char err[256] = "";
	char out[256] = "";

	reset();
	ut = new_handle();
	mu_assert_fatal(ut != NULL, "a handle");

	capture_output();
	mu_assert_int_eq(0, ubertooth_bulk_init(ut));
	deliver(LIBUSB_TRANSFER_NO_DEVICE);
	mu_assert_int_eq(0, ubertooth_bulk_init(ut)); /* a second phase tries */
	deliver(LIBUSB_TRANSFER_ERROR);
	release_output(out, sizeof(out), err, sizeof(err));

	mu_assert_int_eq(LIBUSB_TRANSFER_NO_DEVICE, ut->rx_stream_status);

	release_handle(ut);
}

/* ---- a whole stream ---------------------------------------------------- */

/* A symbol capture whose stream dies: the loop ends where the failure was,
 * and the caller is told rather than left to read the clean-end return. */
MU_TEST(a_stream_that_dies_ends_the_capture)
{
	ubertooth_t* ut;
	char err[256] = "";
	char out[256] = "";

	reset();
	/* Nothing else stops this loop: if the failure did not, the test would
	 * hang here rather than pass. */
	stop_after_polls = 0;
	fake.event = EVENT_TRANSFER_ERROR;
	ut = new_handle();
	mu_assert_fatal(ut != NULL, "a handle");

	capture_output();
	rx_dump(ut, 1);
	release_output(out, sizeof(out), err, sizeof(err));

	mu_assert(ubertooth_stream_failed(ut), "the stream is marked failed");
	mu_assert(strstr(err, "error: receive transfer failed") != NULL, err);
	/* The bitstream a capture does write goes there, and no packet
	 * arrived, so the failure left standard output untouched. */
	mu_assert(out[0] == '\0', out);

	release_handle(ut);
}

/* The teardown's STOP is what takes the board out of the mode a capture
 * started. A board that would not take it is left in that mode, and no other
 * tool can open it until it is reset or re-plugged, so the run did not finish
 * what it was asked: the handle keeps what the request came back with, and
 * every streaming tool turns that into exit 1.
 *
 * This is the fault a lab board produced by itself while a capture was being
 * repeated: the capture ran its whole timeout, so the run looked like one that
 * worked, and only the next command found a board nothing could open. */
MU_TEST(a_refused_teardown_is_recorded)
{
	ubertooth_t* ut;
	char err[512] = "";
	char out[256] = "";

	reset();
	fake.refuse_stop = 1;
	ut = new_handle();
	mu_assert_fatal(ut != NULL, "a handle");

	capture_output();
	ubertooth_stop(ut);
	release_output(out, sizeof(out), err, sizeof(err));

	mu_assert(ubertooth_stop_failed(ut), "the refused stop is recorded");
	/* The stream itself was never asked for, so it is not what failed. */
	mu_assert(!ubertooth_stream_failed(ut), "the stream did not fail");
	/* The library names the request, so no tool has to. */
	mu_assert(strstr(err, "error: could not stop the running mode") != NULL,
	          err);
	mu_assert(out[0] == '\0', out);

	release_handle(ut);
}

/* A teardown the board took leaves nothing recorded, which is the regression
 * that matters: every ordinary end of a capture goes this way.
 * ubertooth_close() sends no STOP at all, so it cannot record one
 * either. */
MU_TEST(a_teardown_the_board_took_is_not_a_failure)
{
	ubertooth_t* ut;
	char err[256] = "";
	char out[256] = "";

	reset();
	ut = new_handle();
	mu_assert_fatal(ut != NULL, "a handle");
	capture_output();
	ubertooth_stop(ut);
	release_output(out, sizeof(out), err, sizeof(err));
	mu_assert(!ubertooth_stop_failed(ut), "nothing was refused");
	mu_assert(err[0] == '\0', err);
	release_handle(ut);

	reset();
	fake.refuse_stop = 1;
	ut = new_handle();
	mu_assert_fatal(ut != NULL, "a handle");
	capture_output();
	ubertooth_close(ut);
	release_output(out, sizeof(out), err, sizeof(err));
	mu_assert(!ubertooth_stop_failed(ut), "a close sends no stop");
	mu_assert(err[0] == '\0', err);
	release_handle(ut);
}

/* A board that has left the USB bus stalls the teardown too on some hosts,
 * and its firmware is not the reason: saying so sent the user to reflash a
 * board that was never the problem. One line, naming what happened, and no
 * note about firmware API versions. The status is still a failure. */
MU_TEST(a_teardown_of_a_board_that_has_gone_does_not_blame_the_firmware)
{
	ubertooth_t* ut;
	char err[512] = "";
	char out[256] = "";

	reset();
	fake.refuse_stop = 1;
	fake.board_has_left_the_bus = 1;
	ut = new_handle();
	mu_assert_fatal(ut != NULL, "a handle");
	/* As opening a board records it: the report has a version to quote, and
	 * a board to ask about. */
	ubertooth_note_api_version(ut->devh, UBERTOOTH_API_VERSION);

	capture_output();
	ubertooth_stop(ut);
	release_output(out, sizeof(out), err, sizeof(err));

	mu_assert(ubertooth_stop_failed(ut), "the refused stop is recorded");
	mu_assert(strcmp(err, "error: could not stop the running mode: the board "
	                 "is no longer attached\n") == 0, err);
	mu_assert(out[0] == '\0', out);

	release_handle(ut);
	ubertooth_forget_api_version(&fake_devh);
}

/* A capture stopped the ordinary way -- a timeout, or a Ctrl-C -- is not a
 * failure, and it is not cut short either: this is the regression that
 * matters, since every normal run goes this way. */
MU_TEST(a_capture_that_was_stopped_is_not_a_failure)
{
	ubertooth_t* ut;
	char err[256] = "";
	char out[256] = "";

	reset();
	stop_after_polls = 3; /* as a -t timeout would */
	ut = new_handle();
	mu_assert_fatal(ut != NULL, "a handle");

	capture_output();
	rx_dump(ut, 1);
	release_output(out, sizeof(out), err, sizeof(err));

	mu_assert(!ubertooth_stream_failed(ut), "no failure recorded");
	mu_assert(err[0] == '\0', err);
	mu_assert(ut->rx_xfer == NULL, "the transfer was given back");
	/* A real thread collected it and was joined on the way out, so the
	 * scripted pthread_create() and pthread_join() above stand in for the C
	 * library's rather than replacing them. */
	mu_assert_int_eq(1, fake.threads_created);
	mu_assert_int_eq(1, fake.threads_joined);

	release_handle(ut);
}

/* A Ctrl-C reaching a capture that was working: the handler stops it, and the
 * cancellation that ends the transfer must not be read as the board failing. */
MU_TEST(an_interrupt_is_not_a_failure)
{
	ubertooth_t* ut;
	char err[256] = "";
	char out[256] = "";

	reset();
	stop_after_polls = 0;
	fake.event = EVENT_INTERRUPT;
	ut = new_handle();
	mu_assert_fatal(ut != NULL, "a handle");
	register_cleanup_handler(ut, 0);

	capture_output();
	rx_dump(ut, 1);
	release_output(out, sizeof(out), err, sizeof(err));

	mu_assert(!ubertooth_stream_failed(ut), "no failure recorded");
	mu_assert_int_eq(1, ut->stop_ubertooth);
	mu_assert(err[0] == '\0', err);

	signal(SIGINT, SIG_DFL);
	release_handle(ut);
}

/* AFH detection reads "no packet arrived" from an empty map, and a stream
 * that died leaves one too: it must not be reported as a quiet piconet, which
 * is a distinct outcome with its own exit status. */
MU_TEST(a_detection_phase_that_lost_its_stream_is_not_a_quiet_piconet)
{
	ubertooth_t* ut;
	btbb_piconet* pn;
	char err[256] = "";
	char out[256] = "";
	int r;

	reset();
	stop_after_polls = 0;
	fake.event = EVENT_DEVICE_GONE;
	ut = new_handle();
	pn = new_piconet();
	mu_assert_fatal(ut != NULL && pn != NULL, "a handle and a piconet");
	/* One channel detected before the stream died, as cb_afh_initial()
	 * would have set from a packet. */
	btbb_piconet_get_afh_map(pn)[0] = 0x01;

	capture_output();
	r = rx_afh(ut, pn, 30);
	alarm(0); /* rx_afh() set one for the detection phase */
	release_output(out, sizeof(out), err, sizeof(err));

	mu_assert_int_eq(UBERTOOTH_ERROR_STREAM_FAILED, r);
	mu_assert(ubertooth_stream_failed(ut), "the stream is marked failed");
	mu_assert(strstr(err, "error: receive transfer failed") != NULL, err);
	/* What it did detect is still printed, as a capture that died prints
	 * its partial results; the error is what says they are incomplete. */
	mu_assert(out[0] != '\0', "the partial map was printed");

	release_handle(ut);
	btbb_piconet_unref(pn);
}

/* A capture that could not be started is reported to the caller too: the
 * handle alone cannot say so, because no stream ever ran to fail. */
MU_TEST(a_capture_that_could_not_start_is_reported)
{
	ubertooth_t* ut;
	char err[256] = "";
	char out[256] = "";

	reset();
	fake.refuse_submit = 1;
	ut = new_handle();
	mu_assert_fatal(ut != NULL, "a handle");

	capture_output();
	mu_assert(rx_dump(ut, 1) < 0, "the dump reports it could not start");
	release_output(out, sizeof(out), err, sizeof(err));

	mu_assert(strstr(err, "error: submitting the receive transfer failed")
	          != NULL, err);
	mu_assert(out[0] == '\0', out);

	release_handle(ut);
}

/*
 * A capture whose collecting thread could not be started. pthread_create()
 * reports a failure as a positive errno, and a "< 0" test let that through:
 * the capture was started with nobody to complete its transfers, so it waited
 * forever with nothing arriving and said nothing about why. Both receive
 * loops the library runs itself are covered, since each starts the thread.
 *
 * The capture is also scripted to stop after a few empty polls. Nothing here
 * reaches that -- the capture is refused before its loop -- but it keeps a
 * regression that ran the loop anyway from hanging the suite until its
 * timeout.
 */
MU_TEST(a_capture_whose_collecting_thread_failed_is_reported)
{
	char expected[256];
	size_t i;

	snprintf(expected, sizeof(expected),
	         "error: could not start the thread that collects the capture:"
	         " %s\n", strerror(EAGAIN));

	/* rx_dump() for stream_rx_usb(), which every ubertooth-dump and
	 * ubertooth-afh capture goes through, and rx_afh_r() for the
	 * one-line-per-second form, which has a receive loop of its own. */
	for (i = 0; i < 2; i++) {
		ubertooth_t* ut;
		btbb_piconet* pn = NULL;
		char err[256] = "";
		char out[256] = "";
		int r;

		reset();
		fake.refuse_thread = EAGAIN;
		stop_after_polls = 3;
		ut = new_handle();
		mu_assert_fatal(ut != NULL, "a handle");
		if (i == 1) {
			pn = new_piconet();
			mu_assert_fatal(pn != NULL, "a piconet");
		}

		capture_output();
		r = i == 0 ? rx_dump(ut, 1) : rx_afh_r(ut, pn, 0);
		release_output(out, sizeof(out), err, sizeof(err));

		/* Negative, so the caller reads it as the failure it is: a
		 * positive errno handed straight back would have read as the
		 * normal end of a capture. */
		mu_assert(r < 0, "the capture reports it could not start");
		mu_assert(strcmp(err, expected) == 0, err);
		/* The library never writes to standard output: that is the
		 * capture's stream, and a tool may be piping it. */
		mu_assert(out[0] == '\0', out);
		mu_assert_int_eq(1, fake.threads_started);
		mu_assert_int_eq(0, fake.threads_created);
		/* No thread was created, so none was waited on: joining a
		 * pthread_t pthread_create() never filled in is undefined. */
		mu_assert_int_eq(0, fake.threads_joined);
		/* Which holds for a caller that reaches the stop on its own, as
		 * the Kismet plugin's teardown does. */
		ubertooth_bulk_thread_stop();
		mu_assert_int_eq(0, fake.threads_joined);
		/* Nothing was left behind: the transfer the setup had put in
		 * flight was given back rather than left for the next capture on
		 * this handle to refuse. */
		mu_assert(ut->rx_xfer == NULL, "the transfer was given back");
		/* Nor was the board asked to send: with nobody to collect it, a
		 * started mode would stream into a dead stream. The one request
		 * the second case did send is rx_afh_r()'s band sweep, which it
		 * sets before the capture is built. */
		mu_assert_int_eq((int)i, fake.control_requests);

		release_handle(ut);
		if (pn != NULL)
			btbb_piconet_unref(pn);
	}
}

int main(void)
{
	/* Nothing here should look for a board; make sure of it. */
	setenv("UBERTOOTH_TEST_NO_DEVICES", "1", 1);

	MU_RUN(a_failed_transfer_is_reported_and_recorded);
	MU_RUN(a_bulk_timeout_is_not_a_failure);
	MU_RUN(a_whole_packet_is_published_whatever_status_came_with_it);
	MU_RUN(a_whole_packet_is_not_published_once_a_teardown_wants_the_transfer);
	MU_RUN(a_short_transfer_is_not_published_whatever_status_came_with_it);
	MU_RUN(a_cancellation_is_not_a_failure);
	MU_RUN(a_refused_re_arm_ends_the_capture);
	MU_RUN(the_first_failure_is_the_one_kept);
	MU_RUN(a_stream_that_dies_ends_the_capture);
	MU_RUN(a_refused_teardown_is_recorded);
	MU_RUN(a_teardown_the_board_took_is_not_a_failure);
	MU_RUN(a_teardown_of_a_board_that_has_gone_does_not_blame_the_firmware);
	MU_RUN(a_capture_that_was_stopped_is_not_a_failure);
	MU_RUN(an_interrupt_is_not_a_failure);
	MU_RUN(a_detection_phase_that_lost_its_stream_is_not_a_quiet_piconet);
	MU_RUN(a_capture_that_could_not_start_is_reported);
	MU_RUN(a_capture_whose_collecting_thread_failed_is_reported);
	return mu_report();
}
