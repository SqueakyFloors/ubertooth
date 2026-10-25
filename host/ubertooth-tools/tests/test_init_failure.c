/*
 * Hardware-independent tests for ubertooth_init() when the ring buffer cannot
 * be allocated.
 *
 * The real ubertooth.c is compiled into this test, which defines the fifo
 * calls itself (ubertooth_fifo.c is not linked) so the allocation can be made
 * to fail on demand. A handle with no ring buffer must not be handed out: the
 * first fifo_* call on it would dereference NULL.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libusb.h>

#include "../../../firmware/tests/minunit.h"
#include "ubertooth.h"
#include "ubertooth_fifo.h"

static struct {
	int fifo_fails; /* make fifo_init() return NULL */
	int fifo_inits; /* fifo_init() calls */
	int usb_inits;  /* libusb_init() calls */
} fake;

/* ---- fifo fake ---------------------------------------------------------- */

/* A real fifo_t is 64 MB, and nothing here reads one: this stands in for the
 * ring buffer as an opaque, free()able pointer. */
fifo_t* fifo_init()
{
	fake.fifo_inits++;
	if (fake.fifo_fails)
		return NULL;
	return (fifo_t*)malloc(1);
}

/* The rest of the fifo interface, for the library's other references. None of
 * it runs here: no capture is started. */
void fifo_inc_write_ptr(fifo_t* fifo) { (void)fifo; }
void fifo_push(fifo_t* fifo, const usb_pkt_rx* packet) { (void)fifo; (void)packet; }
usb_pkt_rx fifo_pop(fifo_t* fifo) { usb_pkt_rx p; (void)fifo; memset(&p, 0, sizeof(p)); return p; }
usb_pkt_rx* fifo_get_write_element(fifo_t* fifo) { (void)fifo; return NULL; }
uint8_t fifo_empty(fifo_t* fifo) { (void)fifo; return 1; }

/* ---- libusb fake -------------------------------------------------------- */

/* Defined here, so it takes precedence over the libusb library symbol: a
 * failed ubertooth_init() must return before anything touches USB. */
int LIBUSB_CALL libusb_init(libusb_context** ctx)
{
	(void)ctx;
	fake.usb_inits++;
	return 0;
}

/* ---- helpers ------------------------------------------------------------ */

static FILE* capture;
static int saved_stderr;

static void capture_stderr(void)
{
	capture = tmpfile();
	saved_stderr = capture != NULL ? dup(fileno(stderr)) : -1;
	mu_assert(capture != NULL && saved_stderr >= 0, "capture stderr");
	/* A plain mu_assert records the failure and carries on, and the lines
	 * below would then redirect to a NULL stream. This is a helper, not a
	 * test body, so it cannot use mu_assert_fatal: it leaves stderr alone
	 * instead, and release_stderr() below restores nothing. */
	if (saved_stderr < 0)
		return;
	fflush(stderr);
	dup2(fileno(capture), fileno(stderr));
}

static void release_stderr(char* text, size_t len)
{
	size_t n;

	text[0] = '\0';
	if (saved_stderr < 0) {
		/* nothing was redirected; close whatever the failed capture got */
		if (capture != NULL) {
			fclose(capture);
			capture = NULL;
		}
		return;
	}
	fflush(stderr);
	dup2(saved_stderr, fileno(stderr));
	close(saved_stderr);
	rewind(capture);
	n = fread(text, 1, len - 1, capture);
	text[n] = '\0';
	fclose(capture);
}

static void reset(int fifo_fails)
{
	memset(&fake, 0, sizeof(fake));
	fake.fifo_fails = fifo_fails;
}

/* ---- tests -------------------------------------------------------------- */

/* It used to print the message and return the handle anyway, with
 * ut->fifo == NULL. */
MU_TEST(init_fails_when_the_ring_buffer_cannot_be_allocated)
{
	ubertooth_t* ut;
	char text[256] = "";

	reset(1);
	capture_stderr();
	ut = ubertooth_init();
	release_stderr(text, sizeof(text));

	mu_assert(ut == NULL, "no handle without a ring buffer");
	mu_assert_int_eq(1, fake.fifo_inits);
	mu_assert(strcmp(text, "error: unable to initialize ringbuffer\n") == 0, text);
}

MU_TEST(init_returns_a_usable_handle_when_the_ring_buffer_is_allocated)
{
	ubertooth_t* ut;

	reset(0);
	ut = ubertooth_init();
	mu_assert(ut != NULL, "a handle");
	if (ut == NULL)
		return;
	mu_assert(ut->fifo != NULL, "with a ring buffer");
	mu_assert(ut->devh == NULL, "no device yet");
	mu_assert(ut->rx_xfer == NULL, "no transfer yet");
	mu_assert(ut->h_pcap_bredr == NULL && ut->h_pcap_le == NULL
	          && ut->h_pcapng_bredr == NULL && ut->h_pcapng_le == NULL,
	          "no capture files yet");
	mu_assert_int_eq(0, ut->stop_ubertooth);
	ubertooth_free(ut);
}

/* The functions that build a handle for the caller pass the failure on, and
 * none of them goes near USB once it has failed. */
MU_TEST(the_start_functions_fail_with_the_ring_buffer)
{
	char text[512] = "";

	reset(1);
	capture_stderr();
	mu_assert(ubertooth_start(-1) == NULL, "ubertooth_start");
	mu_assert(ubertooth_start_serial("05f0") == NULL, "ubertooth_start_serial");
	mu_assert_int_eq(-ENOMEM, rx_btle_file(NULL));
	release_stderr(text, sizeof(text));

	mu_assert_int_eq(3, fake.fifo_inits);
	mu_assert_int_eq(0, fake.usb_inits);
	/* each of the three reports the failure, once */
	mu_assert(strcmp(text, "error: unable to initialize ringbuffer\n"
	                       "error: unable to initialize ringbuffer\n"
	                       "error: unable to initialize ringbuffer\n") == 0, text);
}

int main(void)
{
	/* Nothing here should look for a board; make sure of it. */
	setenv("UBERTOOTH_TEST_NO_DEVICES", "1", 1);

	MU_RUN(init_fails_when_the_ring_buffer_cannot_be_allocated);
	MU_RUN(init_returns_a_usable_handle_when_the_ring_buffer_is_allocated);
	MU_RUN(the_start_functions_fail_with_the_ring_buffer);
	return mu_report();
}
