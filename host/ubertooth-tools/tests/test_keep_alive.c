/*
 * What libubertooth does with the keep-alive a quiet capture gets.
 *
 * The firmware answers the bulk endpoint with a one-byte keep-alive when it
 * has no packet to send, so the host can tell a quiet board from a dead one.
 * One byte sets pkt_type in the 64-byte receive slot and leaves the rest of
 * the slot as it was. The receive callback used to publish the slot anyway,
 * because it never looked at how much had arrived, so a capture that went
 * quiet put a packet nothing had filled in into the ring buffer -- and
 * ubertooth-dump -d wrote it out as a record a reader cannot tell from data.
 *
 * Two things are checked here, matching the two places a keep-alive is
 * stopped:
 *
 *  - the receive callback publishes a whole packet and nothing shorter, which
 *    is the fix: a short transfer keeps the slot for the next real packet;
 *  - the consumers that write records out skip a keep-alive that reaches them
 *    anyway, which a replay of a capture file recorded before the fix does.
 *
 * The real ubertooth.c, ubertooth_callback.c and ubertooth_control.c are
 * compiled in; the test defines the libusb calls and the fifo calls itself,
 * so no bulk endpoint, 64 MB ring buffer or board is involved.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pthread.h>

#include <btbb.h>
#include <libusb.h>

#include "../../../firmware/tests/minunit.h"
#include "ubertooth.h"
#include "ubertooth_fifo.h"

/* What a receive slot holds before anything is received into it: the stale
 * bytes a real malloced slot would have. Recognisable in a record that should
 * never have been written. */
#define SLOT_JUNK 0xa5

/* ---- fifo fake ---------------------------------------------------------- */
/* A ring of a few slots, so a case can see exactly which slots a capture
 * published and what was in them. The real ubertooth_fifo.c is not linked:
 * its ring is 64 MB. Nothing here fills the ring, so unlike the real one this
 * fake always advances rather than refusing to when full. */

#define FAKE_SLOTS 8

static struct {
	usb_pkt_rx slots[FAKE_SLOTS];
	size_t write_ptr;
	size_t read_ptr;
	int published;      /* slots handed to the consumer */
	int stop_when_empty;/* stop the capture once nothing is left to pop */
} ring;

/* The handle a capture under test is using, so the faked fifo can stop it. */
static ubertooth_t* running_ut;

static void reset_ring(void)
{
	memset(&ring, 0, sizeof(ring));
	memset(ring.slots, SLOT_JUNK, sizeof(ring.slots));
}

fifo_t* fifo_init(void)
{
	return (fifo_t*)malloc(1);
}

usb_pkt_rx* fifo_get_write_element(fifo_t* fifo)
{
	(void)fifo;
	return &ring.slots[ring.write_ptr];
}

void fifo_inc_write_ptr(fifo_t* fifo)
{
	(void)fifo;
	ring.published++;
	ring.write_ptr = (ring.write_ptr + 1) % FAKE_SLOTS;
}

void fifo_push(fifo_t* fifo, const usb_pkt_rx* packet)
{
	ring.slots[ring.write_ptr] = *packet;
	fifo_inc_write_ptr(fifo);
}

uint8_t fifo_empty(fifo_t* fifo)
{
	(void)fifo;
	if (ring.read_ptr != ring.write_ptr)
		return 0;
	if (ring.stop_when_empty && running_ut != NULL)
		running_ut->stop_ubertooth = 1;
	return 1;
}

usb_pkt_rx fifo_pop(fifo_t* fifo)
{
	size_t selected = ring.read_ptr;

	(void)fifo;
	ring.read_ptr = (ring.read_ptr + 1) % FAKE_SLOTS;
	return ring.slots[selected];
}

/* ---- libusb fakes ------------------------------------------------------- */
/* Defined here, so they win over the libusb library symbols. */

/* Guards the transfer state, which the poll thread a whole capture starts
 * also reaches. */
static pthread_mutex_t fake_lock = PTHREAD_MUTEX_INITIALIZER;

static struct {
	struct libusb_transfer* in_flight;
	int submits;
	int cancelled;
} fake;

static void reset_fake(void)
{
	pthread_mutex_lock(&fake_lock);
	memset(&fake, 0, sizeof(fake));
	pthread_mutex_unlock(&fake_lock);
}

int LIBUSB_CALL libusb_init(libusb_context** ctx) { (void)ctx; return 0; }
void LIBUSB_CALL libusb_exit(libusb_context* ctx) { (void)ctx; }

int LIBUSB_CALL libusb_control_transfer(libusb_device_handle* devh,
	uint8_t request_type, uint8_t bRequest, uint16_t wValue, uint16_t wIndex,
	unsigned char* data, uint16_t wLength, unsigned int timeout)
{
	(void)devh;
	(void)request_type;
	(void)bRequest;
	(void)wValue;
	(void)wIndex;
	(void)timeout;
	if (data != NULL && wLength > 0)
		memset(data, 0, wLength);
	return wLength;
}

/* The capture clears the receive pipe before it reads, to drop packets left in
 * the board's endpoint buffers by an earlier mode. No case here depends on it,
 * but the real call would reach a device this test does not have. */
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
	pthread_mutex_lock(&fake_lock);
	if (xfer == fake.in_flight)
		fake.in_flight = NULL;
	pthread_mutex_unlock(&fake_lock);
	free(xfer);
}

int LIBUSB_CALL libusb_submit_transfer(struct libusb_transfer* xfer)
{
	pthread_mutex_lock(&fake_lock);
	fake.submits++;
	fake.in_flight = xfer;
	pthread_mutex_unlock(&fake_lock);
	return 0;
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

/* The poll loop: nothing arrives on its own here, so the only completion it
 * delivers is a cancellation the teardown asked for. */
int LIBUSB_CALL libusb_handle_events_timeout(libusb_context* ctx,
                                             struct timeval* tv)
{
	struct libusb_transfer* xfer = NULL;

	(void)ctx;
	(void)tv;
	pthread_mutex_lock(&fake_lock);
	if (fake.cancelled && fake.in_flight != NULL) {
		fake.cancelled = 0;
		xfer = fake.in_flight;
		fake.in_flight = NULL;
	}
	pthread_mutex_unlock(&fake_lock);
	if (xfer != NULL) {
		xfer->status = LIBUSB_TRANSFER_CANCELLED;
		xfer->actual_length = 0;
		xfer->callback(xfer);
		return 0;
	}
	usleep(1000);
	return 0;
}

/* ---- helpers ------------------------------------------------------------ */

static int fake_devh;

static ubertooth_t* new_handle(void)
{
	ubertooth_t* ut = ubertooth_init();

	if (ut == NULL)
		return NULL;
	ut->devh = (struct libusb_device_handle*)&fake_devh;
	running_ut = ut;
	return ut;
}

static void release_handle(ubertooth_t* ut)
{
	struct libusb_transfer* left;

	if (ut == NULL)
		return;
	left = ut->rx_xfer;
	ut->rx_xfer = NULL;
	libusb_free_transfer(left);
	ut->devh = NULL;
	running_ut = NULL;
	ubertooth_free(ut);
}

static void reset(void)
{
	reset_ring();
	reset_fake();
	running_ut = NULL;
}

/* Hands the transfer in flight back to libubertooth's callback the way libusb
 * does when a bulk read completes, having transferred `length` bytes. No poll
 * thread runs in these cases, so this is the whole of the event delivery. */
static void deliver_bytes(int length)
{
	struct libusb_transfer* xfer = fake.in_flight;

	mu_assert_fatal(xfer != NULL, "a transfer was in flight");
	xfer->status = LIBUSB_TRANSFER_COMPLETED;
	xfer->actual_length = length;
	xfer->callback(xfer);
}

/* Writes a whole packet into the slot the transfer is reading into, as the
 * board would have, and reports how many bytes that is. */
static int fill_slot_with_a_packet(uint8_t channel)
{
	usb_pkt_rx* slot = (usb_pkt_rx*)fake.in_flight->buffer;

	memset(slot, 0, sizeof(*slot));
	slot->pkt_type = BR_PACKET;
	slot->channel = channel;
	return PKT_LEN;
}

static usb_pkt_rx keep_alive_record(void)
{
	usb_pkt_rx rx;

	/* As a keep-alive leaves a slot: its type, and stale bytes behind it. */
	memset(&rx, SLOT_JUNK, sizeof(rx));
	rx.pkt_type = KEEP_ALIVE;
	/* Those stale bytes can read as a valid channel, which is what makes
	 * such a record impossible to tell from data; a consumer must not fall
	 * back on a sanity check of some other field to leave it out. */
	rx.channel = 37;
	return rx;
}

static usb_pkt_rx packet_record(uint8_t channel)
{
	usb_pkt_rx rx;

	memset(&rx, 0, sizeof(rx));
	rx.pkt_type = BR_PACKET;
	rx.channel = channel;
	return rx;
}

static FILE* capture_out;
static FILE* capture_err;
static int saved_stdout = -1;
static int saved_stderr = -1;

/* Both of the tool's streams, kept out of the test's own output and read
 * back. A keep-alive is an ordinary part of a quiet capture, so the cases can
 * require that nothing is said about one on either stream. */
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
	saved_stdout = -1;
	saved_stderr = -1;
	rewind(capture_out);
	n = fread(out_text, 1, out_len - 1, capture_out);
	out_text[n] = '\0';
	rewind(capture_err);
	n = fread(err_text, 1, err_len - 1, capture_err);
	err_text[n] = '\0';
	fclose(capture_out);
	fclose(capture_err);
}

static int count_of(const char* text, const char* needle)
{
	int n = 0;
	const char* at = text;

	while ((at = strstr(at, needle)) != NULL) {
		n++;
		at += strlen(needle);
	}
	return n;
}

/* Writes one record of a saved capture: the host time, big-endian, then the
 * packet, as ubertooth-dump -d and ubertooth-rx -d write them. */
static void write_record(FILE* fp, const usb_pkt_rx* rx)
{
	uint32_t when = htobe32(1700000000u);

	mu_assert_fatal(fwrite(&when, 1, sizeof(when), fp) == sizeof(when),
	                "the record's time");
	mu_assert_fatal(fwrite(rx, 1, PKT_LEN, fp) == PKT_LEN,
	                "the record's packet");
}

/* ---- what the receive callback publishes -------------------------------- */

/* The fix. A one-byte keep-alive is not a packet: the slot it touched stays
 * where it is, nothing is published, and the capture carries on with nothing
 * said. */
MU_TEST(a_keep_alive_transfer_is_not_published)
{
	ubertooth_t* ut;
	char err[256] = "";
	char out[256] = "";
	uint8_t* slot_before;
	int i;

	reset();
	ut = new_handle();
	mu_assert_fatal(ut != NULL, "a handle");

	capture_output();
	mu_assert_int_eq(0, ubertooth_bulk_init(ut));
	slot_before = fake.in_flight->buffer;
	for (i = 0; i < 5; i++) {
		((usb_pkt_rx*)fake.in_flight->buffer)->pkt_type = KEEP_ALIVE;
		deliver_bytes(1);
	}
	release_output(out, sizeof(out), err, sizeof(err));

	mu_assert_int_eq(0, ring.published);
	/* The same slot is still the one being read into, so the next real
	 * packet overwrites every byte of it. */
	mu_assert(fake.in_flight != NULL && fake.in_flight->buffer == slot_before,
	          "the slot was kept for the next packet");
	/* One submit to start the capture, and one per keep-alive. */
	mu_assert_int_eq(6, fake.submits);
	mu_assert(!ubertooth_stream_failed(ut), "the capture is still running");
	mu_assert(err[0] == '\0', err);
	mu_assert(out[0] == '\0', out);

	release_handle(ut);
}

/* The case that must keep working: a whole packet is published, once. */
MU_TEST(a_whole_packet_is_published)
{
	ubertooth_t* ut;

	reset();
	ut = new_handle();
	mu_assert_fatal(ut != NULL, "a handle");

	mu_assert_int_eq(0, ubertooth_bulk_init(ut));
	deliver_bytes(fill_slot_with_a_packet(17));

	mu_assert_int_eq(1, ring.published);
	mu_assert_int_eq(BR_PACKET, ring.slots[0].pkt_type);
	mu_assert_int_eq(17, ring.slots[0].channel);

	release_handle(ut);
}

/* A quiet spell in the middle of a capture: the packet that follows the
 * keep-alives is published, and it is the only thing published. The record
 * holds the packet, not the bytes a keep-alive left in that slot. */
MU_TEST(a_packet_after_a_quiet_spell_is_the_only_record)
{
	ubertooth_t* ut;
	int i;

	reset();
	ut = new_handle();
	mu_assert_fatal(ut != NULL, "a handle");

	mu_assert_int_eq(0, ubertooth_bulk_init(ut));
	for (i = 0; i < 3; i++) {
		((usb_pkt_rx*)fake.in_flight->buffer)->pkt_type = KEEP_ALIVE;
		deliver_bytes(1);
	}
	deliver_bytes(fill_slot_with_a_packet(42));

	mu_assert_int_eq(1, ring.published);
	mu_assert_int_eq(BR_PACKET, ring.slots[0].pkt_type);
	mu_assert_int_eq(42, ring.slots[0].channel);

	release_handle(ut);
}

/* A transfer cut short anywhere other than at a keep-alive is not a packet
 * either: a record 63 bytes of data short is not something a consumer can
 * read. */
MU_TEST(a_transfer_shorter_than_a_packet_is_not_published)
{
	static const int lengths[] = { 0, 1, 2, 32, PKT_LEN - 1 };
	size_t i;

	for (i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
		ubertooth_t* ut;
		uint8_t* slot_before;

		reset();
		ut = new_handle();
		mu_assert_fatal(ut != NULL, "a handle");

		mu_assert_int_eq(0, ubertooth_bulk_init(ut));
		slot_before = fake.in_flight->buffer;
		deliver_bytes(lengths[i]);

		mu_assert_int_eq(0, ring.published);
		mu_assert(fake.in_flight != NULL
		          && fake.in_flight->buffer == slot_before,
		          "the slot was kept for the next packet");
		mu_assert(!ubertooth_stream_failed(ut),
		          "the capture is still running");

		release_handle(ut);
	}
}

/* ---- what a dump writes ------------------------------------------------- */

/* The belt. A keep-alive that reaches the consumer anyway -- which replaying
 * a capture file recorded before the fix does -- is left out of the dump, so
 * every record in the file is a packet. */
MU_TEST(a_dump_leaves_out_a_keep_alive_record)
{
	ubertooth_t* ut;
	usb_pkt_rx ka;
	usb_pkt_rx pkt;
	usb_pkt_rx written;
	uint32_t when;
	char err[256] = "";
	char out[256] = "";
	long size;
	FILE* dump;

	reset();
	ut = new_handle();
	mu_assert_fatal(ut != NULL, "a handle");
	dump = tmpfile();
	mu_assert_fatal(dump != NULL, "a dump file");
	dumpfile = dump;

	/* A quiet spell, then a packet, as a capture file holds them. */
	ka = keep_alive_record();
	pkt = packet_record(23);
	fifo_push(ut->fifo, &ka);
	fifo_push(ut->fifo, &pkt);
	ring.stop_when_empty = 1;

	capture_output();
	mu_assert_int_eq(0, rx_dump(ut, 0));
	release_output(out, sizeof(out), err, sizeof(err));

	dumpfile = NULL;
	fflush(dump);
	mu_assert_fatal(fseek(dump, 0, SEEK_END) == 0, "seek the dump");
	size = ftell(dump);

	/* One record, the packet. Two would mean the keep-alive was written,
	 * and a size that is not a whole number of records would mean the dump
	 * cannot be read back at all. */
	mu_assert_int_eq(RX_FILE_RECORD_LEN, (int)size);
	mu_assert_int_eq(0, (int)(size % RX_FILE_RECORD_LEN));
	rewind(dump);
	mu_assert_fatal(fread(&when, 1, sizeof(when), dump) == sizeof(when),
	                "the record's time");
	mu_assert_fatal(fread(&written, 1, sizeof(written), dump)
	                == sizeof(written), "the record's packet");
	mu_assert_int_eq(BR_PACKET, written.pkt_type);
	mu_assert_int_eq(23, written.channel);
	/* The dump went to the file, so the tool's own stream carries only its
	 * one timestamp line per packet -- and none for the keep-alive -- while
	 * standard output carries nothing at all. */
	mu_assert(strstr(err, "rx block timestamp") != NULL, err);
	mu_assert(strchr(err, '\n') == strrchr(err, '\n'), "said once");
	mu_assert(out[0] == '\0', out);

	fclose(dump);
	release_handle(ut);
}

/* The bitstream dump (-b) the same way: a keep-alive carries no symbols, so
 * it adds no line of bits. */
MU_TEST(a_bitstream_dump_leaves_out_a_keep_alive)
{
	ubertooth_t* ut;
	usb_pkt_rx ka;
	usb_pkt_rx pkt;
	char err[256] = "";
	char out[256] = "";
	long size;
	FILE* dump;

	reset();
	ut = new_handle();
	mu_assert_fatal(ut != NULL, "a handle");
	dump = tmpfile();
	mu_assert_fatal(dump != NULL, "a dump file");
	dumpfile = dump;

	ka = keep_alive_record();
	pkt = packet_record(23);
	fifo_push(ut->fifo, &ka);
	fifo_push(ut->fifo, &pkt);
	ring.stop_when_empty = 1;

	capture_output();
	mu_assert_int_eq(0, rx_dump(ut, 1));
	release_output(out, sizeof(out), err, sizeof(err));

	dumpfile = NULL;
	fflush(dump);
	mu_assert_fatal(fseek(dump, 0, SEEK_END) == 0, "seek the dump");
	size = ftell(dump);

	/* One line of symbols and its newline: the packet's. The bits went to
	 * the file, so standard output carries nothing, and the tool's own
	 * stream carries its one timestamp line for the packet and none for the
	 * keep-alive. */
	mu_assert_int_eq(BANK_LEN + 1, (int)size);
	mu_assert(out[0] == '\0', out);
	mu_assert(strstr(err, "rx block timestamp") != NULL, err);
	mu_assert(strchr(err, '\n') == strrchr(err, '\n'), "said once");

	fclose(dump);
	release_handle(ut);
}

/* The same belt on the replay path, which is how a keep-alive still reaches a
 * consumer: a capture file recorded before the fix holds one. Replaying it
 * neither decodes it as a packet nor copies it into the capture being
 * written. */
MU_TEST(a_replayed_keep_alive_is_not_decoded_or_recorded)
{
	usb_pkt_rx ka = keep_alive_record();
	usb_pkt_rx pkt = packet_record(11);
	usb_pkt_rx written;
	uint32_t when;
	char err[512] = "";
	char out[4096] = "";
	FILE* replay;
	FILE* dump;
	long size;
	int r;

	reset();
	replay = tmpfile();
	dump = tmpfile();
	mu_assert_fatal(replay != NULL && dump != NULL, "the two files");
	pkt.pkt_type = LE_PACKET;
	write_record(replay, &ka);
	write_record(replay, &pkt);
	rewind(replay);
	infile = replay;
	dumpfile = dump;

	capture_output();
	r = rx_btle_file(replay);
	release_output(out, sizeof(out), err, sizeof(err));
	infile = NULL;
	dumpfile = NULL;

	mu_assert_int_eq(0, r); /* a clean end of file */
	fflush(dump);
	mu_assert_fatal(fseek(dump, 0, SEEK_END) == 0, "seek the capture");
	size = ftell(dump);
	mu_assert_int_eq(RX_FILE_RECORD_LEN, (int)size);
	rewind(dump);
	mu_assert_fatal(fread(&when, 1, sizeof(when), dump) == sizeof(when),
	                "the record's time");
	mu_assert_fatal(fread(&written, 1, sizeof(written), dump)
	                == sizeof(written), "the record's packet");
	mu_assert_int_eq(LE_PACKET, written.pkt_type);
	mu_assert_int_eq(11, written.channel);
	/* One packet was printed, so the keep-alive was not decoded into a
	 * second one. */
	mu_assert_int_eq(1, count_of(out, "systime="));

	fclose(replay);
	fclose(dump);
}

int main(void)
{
	/* Nothing here should look for a board; make sure of it. */
	setenv("UBERTOOTH_TEST_NO_DEVICES", "1", 1);

	MU_RUN(a_keep_alive_transfer_is_not_published);
	MU_RUN(a_whole_packet_is_published);
	MU_RUN(a_packet_after_a_quiet_spell_is_the_only_record);
	MU_RUN(a_transfer_shorter_than_a_packet_is_not_published);
	MU_RUN(a_dump_leaves_out_a_keep_alive_record);
	MU_RUN(a_bitstream_dump_leaves_out_a_keep_alive);
	MU_RUN(a_replayed_keep_alive_is_not_decoded_or_recorded);
	return mu_report();
}
