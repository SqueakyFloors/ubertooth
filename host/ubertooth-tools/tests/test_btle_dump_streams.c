/*
 * What the low-energy callback writes, and on which stream, when a capture has
 * taken standard output.
 *
 * `ubertooth-btle -q -` hands the capture the tool's standard output and
 * points the process's own standard output at /dev/null for the rest of the
 * run, so the file holds nothing but the capture. The dump that goes to that
 * /dev/null must not be produced at all: it ends in lell_print(), which
 * reports a field it cannot read on standard error, and standard error still
 * reaches the user.
 *
 * The real cb_btle() runs here, with the real decoder, over one crafted
 * packet whose advertising data claims more bytes than arrived. Only the ring
 * buffer is faked. The stream cases cover both directions: an ordinary run
 * prints the dump on standard output and the complaint on standard error, and
 * a run whose capture took standard output prints neither while the capture
 * still gets the packet. A last case reads the signal level out of both the
 * dump and the capture of one packet, which must be the same number.
 */
#include <fcntl.h>
#include <limits.h>
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
	int ready;

	mu_assert_fatal(ut != NULL, "a handle");
	fifo_push(ut->fifo, &rx, 0);
	capture_output();

	/* What ubertooth-btle does for a bare "-": keep the descriptor the
	 * capture is to be written to, then point standard output at
	 * /dev/null.
	 *
	 * Nothing here is asserted until the streams are back, below: a check
	 * that failed while standard error still went to a temporary file
	 * would report an empty failure and leave the reader guessing that
	 * /dev/fd or /dev/null was the problem. */
	capture_fd = dup(fileno(stdout));
	null_fd = open("/dev/null", O_WRONLY);
	ready = capture_fd >= 0 && null_fd >= 0;
	if (ready) {
		fflush(stdout);
		dup2(null_fd, fileno(stdout));
		snprintf(path, sizeof(path), "/dev/fd/%d", capture_fd);
		ready = lell_pcap_create_file(path, &ut->h_pcap_le) == 0;
	}
	/* dup2() above made its own descriptor, so this one has no further use
	 * on either path. */
	if (null_fd >= 0)
		close(null_fd);
	if (ready) {
		cb_btle(ut, &opts);
		lell_pcap_close(ut->h_pcap_le);
		ut->h_pcap_le = NULL;
	}
	if (capture_fd >= 0)
		close(capture_fd);
	release_output(out, sizeof(out), &out_len, err, sizeof(err));
	mu_assert_fatal(ready, "a capture on the saved descriptor");

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

/* ---- the promiscuous mode's status lines -------------------------------- */

/* One of the four records `ubertooth-btle -p` sends as it works a
 * connection's parameters out: this one carries the access address. */
static usb_pkt_rx promiscuous_access_address(uint32_t aa)
{
	usb_pkt_rx rx;

	memset(&rx, 0, sizeof(rx));
	rx.pkt_type = LE_PROMISC;
	rx.data[0] = 0;            /* the state: an access address follows */
	memcpy(&rx.data[1], &aa, sizeof(aa));
	return rx;
}

/*
 * The lines promiscuous mode reports are not part of the capture and not
 * noise: they are what the mode is for, and a capture written to a file has
 * no other way to say which connection it is of. So they go to the stream the
 * tool reports on, which is standard output in an ordinary run and standard
 * error once a capture has taken standard output -- where `-p -q -` used to
 * send them to the /dev/null the tool's own standard output had become, and
 * the user saw nothing.
 */
MU_TEST(the_promiscuous_report_follows_the_tools_report_stream)
{
	ubertooth_t* ut = ubertooth_init();
	usb_pkt_rx rx = promiscuous_access_address(0xdeadbeef);
	char out[8192] = "";
	char err[2048] = "";

	mu_assert_fatal(ut != NULL, "a handle");

	/* An ordinary run: the tool reports on standard output. */
	{
		btle_options opts = { .allowed_access_address_errors = 32,
		                      .report_stream = stdout };

		fifo_push(ut->fifo, &rx, 0);
		capture_output();
		cb_btle(ut, &opts);
		fflush(stdout);
		release_output(out, sizeof(out), NULL, err, sizeof(err));

		mu_assert(strstr(out, "Access Address: deadbeef") != NULL, out);
		mu_assert_int_eq(0, (int)strlen(err));
	}

	/* A capture that took standard output: the tool reports on standard
	 * error, and so does the callback. */
	{
		btle_options opts = { .allowed_access_address_errors = 32,
		                      .capture_took_stdout = 1,
		                      .report_stream = stderr };

		fifo_push(ut->fifo, &rx, 0);
		capture_output();
		cb_btle(ut, &opts);
		fflush(stderr);
		release_output(out, sizeof(out), NULL, err, sizeof(err));

		mu_assert(strstr(err, "Access Address: deadbeef") != NULL, err);
		mu_assert_int_eq(0, (int)strlen(out));
	}

	ubertooth_free(ut);
}

/* ---- the level printed beside a packet ---------------------------------- */

/* Where the signal level sits in a PCAP file holding one low-energy packet:
 * past the 24-byte file header and the 16-byte record header, the pseudo-
 * header's advertising channel index comes first and the signal level second.
 * The index is checked as well, so a pseudo-header that grows a field in
 * front of these two is caught as such rather than as a level nobody
 * expected. */
#define CAPTURED_CHANNEL_AT (24 + 16)
#define CAPTURED_LEVEL_AT (CAPTURED_CHANNEL_AT + 1)

/* The dBm figure on a `systime=` line, or INT_MIN if there is none. */
static int printed_level(const char* text)
{
	const char* at = strstr(text, "rssi=");
	int level;

	if (at == NULL || sscanf(at, "rssi=%d", &level) != 1)
		return INT_MIN;
	return level;
}

/* The signal level the one record of a capture file carries, or INT_MIN if
 * the file is too short to hold one or its channel byte is not where this
 * expects the pseudo-header to start. */
static int captured_level(const char* path, unsigned channel)
{
	unsigned char bytes[CAPTURED_LEVEL_AT + 1];
	FILE* file = fopen(path, "rb");
	size_t n;

	if (file == NULL)
		return INT_MIN;
	n = fread(bytes, 1, sizeof(bytes), file);
	fclose(file);
	if (n < sizeof(bytes) || bytes[CAPTURED_CHANNEL_AT] != channel)
		return INT_MIN;
	return (signed char) bytes[CAPTURED_LEVEL_AT];
}

/* The dump and the capture describe the same packet, so the level they report
 * for it is the same number: the strongest of the readings taken while the
 * packet's symbols were collected, converted to dBm the way the datasheet's
 * curve says.
 *
 * The readings here are far apart on purpose. The weakest one used to be what
 * the dump printed, and through a cruder conversion, so it sat far below the
 * packet's own level: the two figures disagreed by tens of dB, and anyone
 * comparing a dump against a capture of the same run read that as a receiver
 * fault.
 */
MU_TEST(the_dump_reports_the_level_the_capture_records)
{
	ubertooth_t* ut = ubertooth_init();
	btle_options opts = { .allowed_access_address_errors = 32 };
	usb_pkt_rx rx = advertisement_with_an_unreadable_field();
	char dir[] = "/tmp/ubertooth-btle-level-XXXXXX";
	char capture[sizeof(dir) + 16];
	char out[8192] = "";
	char err[2048] = "";
	int level;

	mu_assert_fatal(ut != NULL, "a handle");
	mu_assert_fatal(mkdtemp(dir) != NULL, "a directory to write in");
	snprintf(capture, sizeof(capture), "%s/capture.pcap", dir);
	mu_assert_fatal(lell_pcap_create_file(capture, &ut->h_pcap_le) == 0,
	                "a capture file");

	/* 70 raw counts apart, which the datasheet's curve puts 63 dB apart:
	 * -28 dBm for the strongest reading of the packet, -91 dBm for the
	 * weakest. A real packet's readings spread like this, because the
	 * radio's reading has not caught up with the packet when its first
	 * bytes arrive. */
	rx.rssi_max = 30;
	rx.rssi_min = -40;
	/* 2480 MHz, advertising channel 39, so the channel byte the check
	 * below reads is a value nothing else in the record could be. */
	rx.channel = 78;

	fifo_push(ut->fifo, &rx, 0);
	capture_output();
	cb_btle(ut, &opts);
	release_output(out, sizeof(out), NULL, err, sizeof(err));
	lell_pcap_close(ut->h_pcap_le);
	ut->h_pcap_le = NULL;

	level = printed_level(out);
	mu_assert(level != INT_MIN, out);
	mu_assert(captured_level(capture, 39) != INT_MIN,
	          "the capture holds a record to read a level from");
	mu_assert_int_eq(captured_level(capture, 39), level);
	/* And it is the packet's own level, not the floor of the window: the
	 * weakest reading through the old approximation would read -94. */
	mu_assert_int_eq(-28, level);

	unlink(capture);
	rmdir(dir);
	ubertooth_free(ut);
}

int main(void)
{
	/* Nothing here should look for a board; make sure of it. */
	setenv("UBERTOOTH_TEST_NO_DEVICES", "1", 1);

	MU_RUN(a_dump_prints_the_packet_and_the_complaint_on_standard_error);
	MU_RUN(a_capture_on_standard_output_prints_no_dump_and_no_complaint);
	MU_RUN(the_promiscuous_report_follows_the_tools_report_stream);
	MU_RUN(the_dump_reports_the_level_the_capture_records);
	return mu_report();
}
