/*
 * What the low-energy callback writes, and on which stream, when a capture has
 * taken standard output.
 *
 * `ubertooth-btle -q -` hands the capture the tool's standard output and
 * points the process's own standard output at /dev/null for the rest of the
 * run, so the file holds nothing but the capture. The callback still ran the
 * whole per-packet dump into /dev/null, and the last thing that dump does is
 * ask libbtbb to print the packet -- which parses the advertising data and
 * reports a field it cannot read on standard error, so that a dump redirected
 * to a file stays clean. Standard error is not redirected, so a capture of
 * ordinary noise filled the user's terminal with complaints about a dump
 * nobody could see.
 *
 * The real cb_btle() runs here, with the real decoder, over one crafted
 * packet whose advertising data claims more bytes than arrived. Only the ring
 * buffer is faked. The cases cover both directions: an ordinary run prints
 * the dump on standard output and the complaint on standard error, and a run
 * whose capture took standard output prints neither while the capture still
 * gets the packet.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <btbb.h>

#include "../../../firmware/tests/minunit.h"
#include "ubertooth.h"
#include "ubertooth_callback.h"
#include "ubertooth_fifo.h"

/* ---- fifo fake ---------------------------------------------------------- */
/* One slot: each case hands the callback a single record. The real
 * ubertooth_fifo.c is not linked, because its ring is 72 MB. */

static usb_pkt_rx pending;
static int pending_count;

fifo_t* fifo_init(void) { return (fifo_t*)malloc(1); }
usb_pkt_rx* fifo_get_write_element(fifo_t* fifo) { (void)fifo; return &pending; }
void fifo_inc_write_ptr(fifo_t* fifo, uint64_t arrival_ns)
{ (void)fifo; (void)arrival_ns; }
void fifo_push(fifo_t* fifo, const usb_pkt_rx* packet, uint64_t arrival_ns)
{
	(void)fifo;
	(void)arrival_ns;
	pending = *packet;
	pending_count = 1;
}
uint8_t fifo_empty(fifo_t* fifo) { (void)fifo; return pending_count == 0; }
uint64_t fifo_popped_arrival_ns(const fifo_t* fifo) { (void)fifo; return 0; }
usb_pkt_rx fifo_pop(fifo_t* fifo)
{
	(void)fifo;
	pending_count = 0;
	return pending;
}

/* ---- the packet --------------------------------------------------------- */

/* An advertising packet whose first advertising field claims more bytes than
 * the packet carries. libbtbb decodes it as an advertisement and its
 * advertising-data parser refuses that field, which is the complaint a
 * capture of a quiet band produces by the hundred: noise decoded as an
 * advertisement mostly looks like this, and the parser is right to refuse it.
 */
static usb_pkt_rx advertisement_with_an_unreadable_field(void)
{
	usb_pkt_rx rx;
	uint8_t* data;

	memset(&rx, 0, sizeof(rx));
	rx.pkt_type = LE_PACKET;
	rx.channel = 0;            /* 2402 MHz: advertising channel 37 */
	rx.clk100ns = htole32(1234);
	rx.rssi_min = rx.rssi_max = rx.rssi_avg = 10;
	rx.rssi_count = 1;

	data = rx.data;
	/* The advertising access address, little endian. */
	data[0] = 0xd6;
	data[1] = 0xbe;
	data[2] = 0x89;
	data[3] = 0x8e;
	data[4] = 0x00;            /* PDU header: ADV_IND, public address */
	data[5] = 6 + 11;          /* the address, then 11 bytes of data */
	memset(data + 6, 0x11, 6); /* AdvA */
	/* The first field: 15 bytes of a complete local name, of which 9
	 * arrived. */
	data[12] = 15;
	data[13] = 0x09;
	memset(data + 14, 'a', 9);
	return rx;
}

/* ---- both streams ------------------------------------------------------- */

static FILE* capture_out;
static FILE* capture_err;
static int saved_stdout = -1;
static int saved_stderr = -1;

/* Both of the tool's streams, kept out of the test's own output and read back
 * afterwards. Standard output stands in for the user's `> capture.pcap`.
 */
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

/* The bytes each stream was given. Binary-safe for standard output, which in
 * one case holds a capture file: *out_len is set to how much arrived.
 */
static void release_output(char* out_text, size_t out_size, size_t* out_len,
                           char* err_text, size_t err_size)
{
	size_t n;

	out_text[0] = '\0';
	err_text[0] = '\0';
	if (out_len != NULL)
		*out_len = 0;
	if (saved_stdout < 0 || saved_stderr < 0)
		return;
	fflush(stdout);
	fflush(stderr);
	dup2(saved_stdout, fileno(stdout));
	dup2(saved_stderr, fileno(stderr));
	close(saved_stdout);
	close(saved_stderr);
	saved_stdout = -1;
	saved_stderr = -1;
	rewind(capture_out);
	n = fread(out_text, 1, out_size - 1, capture_out);
	out_text[n] = '\0';
	if (out_len != NULL)
		*out_len = n;
	rewind(capture_err);
	n = fread(err_text, 1, err_size - 1, capture_err);
	err_text[n] = '\0';
	fclose(capture_out);
	fclose(capture_err);
}

/* Whether the bytes given hold the text named. Standard output is binary in
 * one case below (a capture file), so it is searched by length rather than as
 * a string. */
static int holds_text(const char* bytes, size_t length, const char* text)
{
	size_t want = strlen(text);
	size_t at;

	if (length < want)
		return 0;
	for (at = 0; at + want <= length; at++) {
		if (memcmp(bytes + at, text, want) == 0)
			return 1;
	}
	return 0;
}

/* The one complaint libbtbb makes about the packet above, and the first word
 * of the dump the callback writes itself. */
#define DECODER_COMPLAINT "attempt to read past end of buffer"
#define DUMP_LINE "systime="

/* ---- an ordinary run ---------------------------------------------------- */

/* The dump is what the user asked for, so it goes to standard output; the
 * decoder's complaint about a field it could not read goes to standard error,
 * where a user who redirected the dump to a file still sees it.
 */
MU_TEST(a_dump_prints_the_packet_and_the_complaint_on_standard_error)
{
	/* A handle as a capturing tool has it: no device, and the fifo above. */
	ubertooth_t* ut = ubertooth_init();
	btle_options opts = { .allowed_access_address_errors = 32 };
	usb_pkt_rx rx = advertisement_with_an_unreadable_field();
	char out[8192] = "";
	char err[2048] = "";

	mu_assert_fatal(ut != NULL, "a handle");
	fifo_push(ut->fifo, &rx, 0);
	capture_output();
	cb_btle(ut, &opts);
	release_output(out, sizeof(out), NULL, err, sizeof(err));

	mu_assert(strstr(out, DUMP_LINE) != NULL,
	          "the dump names the packet on standard output");
	mu_assert(strstr(err, DECODER_COMPLAINT) != NULL,
	          "the decoder complains on standard error");
	mu_assert(strstr(out, DECODER_COMPLAINT) == NULL,
	          "the complaint is not on standard output");

	ubertooth_free(ut);
}

/* ---- a capture on standard output --------------------------------------- */

/* As `ubertooth-btle -q -` leaves things: the capture holds the tool's
 * standard output, the process's own standard output is /dev/null, and the
 * callback is told so. The dump then costs nothing and says nothing -- and
 * the capture still gets the packet, on the stream the user redirected.
 */
MU_TEST(a_capture_on_standard_output_prints_no_dump_and_no_complaint)
{
	ubertooth_t* ut = ubertooth_init();
	btle_options opts = { .allowed_access_address_errors = 32,
	                      .capture_took_stdout = 1 };
	usb_pkt_rx rx = advertisement_with_an_unreadable_field();
	char out[8192] = "";
	char err[2048] = "";
	size_t out_len = 0;
	char path[64];
	int capture_fd, null_fd;

	mu_assert_fatal(ut != NULL, "a handle");
	fifo_push(ut->fifo, &rx, 0);
	capture_output();

	/* What ubertooth-btle does for a bare "-": keep the descriptor the
	 * capture is to be written to, then point standard output at
	 * /dev/null. */
	capture_fd = dup(fileno(stdout));
	null_fd = open("/dev/null", O_WRONLY);
	mu_assert_fatal(capture_fd >= 0 && null_fd >= 0, "the two descriptors");
	fflush(stdout);
	dup2(null_fd, fileno(stdout));
	close(null_fd);
	snprintf(path, sizeof(path), "/dev/fd/%d", capture_fd);
	mu_assert_fatal(lell_pcap_create_file(path, &ut->h_pcap_le) == 0,
	                "a capture on the saved descriptor");

	cb_btle(ut, &opts);

	lell_pcap_close(ut->h_pcap_le);
	ut->h_pcap_le = NULL;
	close(capture_fd);
	release_output(out, sizeof(out), &out_len, err, sizeof(err));

	/* The capture: a PCAP file holding more than its 24-byte header. */
	mu_assert(out_len > 24, "the capture carries a packet");
	mu_assert(memcmp(out, "\x4d\x3c\xb2\xa1", 4) == 0,
	          "the capture starts with the PCAP magic");
	/* Nothing else reached either stream: no dump into the capture, and no
	 * complaint about a dump nobody would have seen. */
	mu_assert(!holds_text(out, out_len, DUMP_LINE),
	          "no dump in the capture");
	mu_assert_int_eq(0, (int)strlen(err));

	ubertooth_free(ut);
}

int main(void)
{
	/* Nothing here should look for a board; make sure of it. */
	setenv("UBERTOOTH_TEST_NO_DEVICES", "1", 1);

	MU_RUN(a_dump_prints_the_packet_and_the_complaint_on_standard_error);
	MU_RUN(a_capture_on_standard_output_prints_no_dump_and_no_complaint);
	return mu_report();
}
