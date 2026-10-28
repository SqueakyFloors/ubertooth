/*
 * Bounds on a generic transmit packet (UBERTOOTH_TX_GENERIC_PACKET).
 *
 * The request carries the radio registers, a payload length, an amplifier
 * level and the payload. The handler used to take the length byte on trust and
 * copy that many bytes into tx_pkt, which has room for DMA_SIZE payload bytes
 * and has other globals behind it; the vendor-request buffer is 258 bytes, so
 * a length byte of 0xff reached about 200 bytes past the packet. The length
 * then went to the radio's FIFO write unchecked as well. No host tool sends
 * the request, but any program on the bus can.
 *
 * The radio's own bound is separate and is here too: its FIFO holds
 * CC2400_FIFO_SIZE bytes, a quarter less than a packet may carry, so the
 * payload has to go in a piece at a time while the radio sends what it has.
 * The one write tx_generic() used to make pushed the last 18 bytes of a
 * 50-byte packet past the end of the FIFO.
 *
 * The handler and tx_generic() are hardware code that cannot run here, so the
 * fakes below are the shape of both -- the request buffer, the packet with the
 * globals behind it as a guard region, and the FIFO writes -- and the last
 * tests scan the firmware source, the way test_frend.c scans the transmit
 * paths, so the fakes cannot drift away from what the firmware does.
 */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cc2400.h"
#include "minunit.h"
#include "source_scan.h"
#include "ubertooth_interface.h"

#define MIN(x,y)	((x)<(y)?(x):(y))

/* The part of the request in front of the payload, as the firmware names it. */
#define TX_GENERIC_HEADER_LEN offsetof(generic_tx_packet, data)

/* abVendorReqData in firmware/bluetooth_rxtx/ubertooth_usb.c: the buffer the
 * USB stack hands the handler. The stack's data stage does not bound itself to
 * it, so it is the size of the buffer rather than a ceiling on the request. */
#define VENDOR_REQ_BUF 258

/*
 * tx_pkt with the globals that follow it in the image standing in behind it.
 * Written only through `pkt`, so a copy that stays inside the packet leaves
 * `behind` as it was.
 */
#define GUARD_LEN 200

static struct {
	generic_tx_packet pkt;
	uint8_t behind[GUARD_LEN];
} globals;

static uint8_t request[VENDOR_REQ_BUF];

/*
 * What reached the radio, written over SPI on real hardware: every byte in the
 * order it went out, and the size of each write, since the FIFO's own limit is
 * on one write rather than on the total.
 */
static uint8_t fifo[VENDOR_REQ_BUF * 2];
static unsigned fifo_len;
static uint8_t writes[VENDOR_REQ_BUF];
static unsigned write_count;

/* 1 when the radio empties its FIFO between pieces, 0 when the wait for it
 * gives up, which is what fifo_wait_empty() returns. */
static int fifo_drains = 1;

static void reset_state(void)
{
	memset(&globals, 0, sizeof(globals));
	memset(request, 0, sizeof(request));
	memset(fifo, 0, sizeof(fifo));
	fifo_len = 0;
	memset(writes, 0, sizeof(writes));
	write_count = 0;
	fifo_drains = 1;
}

/*
 * Build a request: `length` in its length byte, `payload` bytes of 0xa5 after
 * the header, and `data_in_len` as the length the host declared (wLength).
 */
static unsigned build_request(uint8_t length, unsigned payload)
{
	unsigned i;

	request[offsetof(generic_tx_packet, length)] = length;
	request[offsetof(generic_tx_packet, pa_level)] = 7;
	for (i = 0; i < payload && TX_GENERIC_HEADER_LEN + i < sizeof(request);
	     i++)
		request[TX_GENERIC_HEADER_LEN + i] = 0xa5;
	return TX_GENERIC_HEADER_LEN + payload;
}

/* What the handler does now. 1 accepts the request, 0 stalls it. */
static int tx_generic_request(unsigned data_in_len)
{
	uint8_t i;

	if (data_in_len < TX_GENERIC_HEADER_LEN)
		return 0;
	i = request[offsetof(generic_tx_packet, length)];
	if (i > DMA_SIZE || data_in_len - TX_GENERIC_HEADER_LEN < i)
		return 0;
	memcpy(&globals.pkt, request, TX_GENERIC_HEADER_LEN + i);
	return 1;
}

static void cc2400_fifo_write(uint8_t len, uint8_t *data)
{
	memcpy(fifo + fifo_len, data, len);
	fifo_len += len;
	if (write_count < sizeof(writes))
		writes[write_count] = len;
	write_count++;
}

/*
 * The writes tx_generic() makes to the radio: the payload in FIFO-sized
 * pieces, each one after the radio has sent the last. The wait between them is
 * on the radio and is not modelled; what is modelled is how the payload is
 * cut up, which is what the FIFO's size bounds.
 */
static void tx_generic(void)
{
	uint8_t len = MIN(globals.pkt.length, DMA_SIZE);
	uint8_t sent = 0, chunk;

	do {
		if (sent != 0 && !fifo_drains)
			break;
		chunk = MIN(len - sent, CC2400_FIFO_SIZE);
		cc2400_fifo_write(chunk, globals.pkt.data + sent);
		sent += chunk;
	} while (sent < len);
}

/* Is the guard region still as it was, i.e. did the copy stay in the packet? */
static int guard_is_clean(void)
{
	unsigned i;

	for (i = 0; i < GUARD_LEN; i++)
		if (globals.behind[i] != 0)
			return 0;
	return 1;
}

/* The packet holds exactly the payload the radio's FIFO write can take. */
MU_TEST(test_the_packet_holds_dma_size_payload_bytes)
{
	mu_assert_int_eq(DMA_SIZE, (int)sizeof(globals.pkt.data));
	mu_assert_int_eq(DMA_SIZE, (int)(sizeof(generic_tx_packet) -
	                                 TX_GENERIC_HEADER_LEN));
}

/* A full-length packet is accepted whole, payload and all. */
MU_TEST(test_a_payload_at_the_limit_is_accepted)
{
	unsigned len;
	int i;

	reset_state();
	len = build_request(DMA_SIZE, DMA_SIZE);
	mu_assert_int_eq(1, tx_generic_request(len));
	mu_assert_int_eq(DMA_SIZE, globals.pkt.length);
	for (i = 0; i < DMA_SIZE; i++)
		mu_assert_int_eq(0xa5, globals.pkt.data[i]);
	mu_assert(guard_is_clean(), "a packet at the limit wrote past itself");
}

/*
 * The last payload byte is one the old handler dropped: it copied the length
 * byte's worth from the length byte's own offset, one short of the payload.
 */
MU_TEST(test_the_last_payload_byte_arrives)
{
	unsigned len;

	reset_state();
	len = build_request(3, 3);
	request[TX_GENERIC_HEADER_LEN + 2] = 0x5a;
	mu_assert_int_eq(1, tx_generic_request(len));
	mu_assert_int_eq(0x5a, globals.pkt.data[2]);
}

/* One byte more than the packet holds is refused, and copies nothing. */
MU_TEST(test_a_payload_one_over_the_limit_is_refused)
{
	unsigned len;

	reset_state();
	len = build_request(DMA_SIZE + 1, DMA_SIZE + 1);
	mu_assert_int_eq(0, tx_generic_request(len));
	mu_assert_int_eq(0, globals.pkt.length);
	mu_assert(guard_is_clean(), "a refused packet wrote past the packet");
}

/* So is the worst case, a length byte of 0xff with a request to match. */
MU_TEST(test_the_longest_possible_length_is_refused)
{
	reset_state();
	build_request(0xff, 0xff);
	mu_assert_int_eq(0, tx_generic_request(sizeof(request)));
	mu_assert_int_eq(0, globals.pkt.length);
	mu_assert(guard_is_clean(), "0xff payload bytes wrote past the packet");
}

/*
 * A request that declares more payload than it carries is refused too, so the
 * copy cannot run past the bytes the host sent. What the buffer holds is
 * another matter: a device-to-host request of the same number reaches the
 * handler before any data stage, and these checks bound the copy without being
 * able to tell the previous request's bytes apart from this one's.
 */
MU_TEST(test_a_request_shorter_than_it_claims_is_refused)
{
	reset_state();
	build_request(DMA_SIZE, DMA_SIZE);
	mu_assert_int_eq(0, tx_generic_request(TX_GENERIC_HEADER_LEN +
	                                       DMA_SIZE - 1));
	mu_assert_int_eq(0, globals.pkt.length);
}

/* A request too short to hold the header is refused before the length is read. */
MU_TEST(test_a_request_shorter_than_the_header_is_refused)
{
	unsigned len;

	for (len = 0; len < TX_GENERIC_HEADER_LEN; len++) {
		reset_state();
		build_request(1, 1);
		mu_assert_int_eq(0, tx_generic_request(len));
		mu_assert_int_eq(0, globals.pkt.length);
	}
}

/*
 * The guard region is what makes the tests above mean something: the copy the
 * handler used to make, with the same request, reaches past the packet.
 * Carried out on a buffer big enough to hold it, so this test shows how far it
 * ran rather than running past an array itself.
 */
MU_TEST(test_the_old_unchecked_copy_ran_past_the_packet)
{
	uint8_t roomy[VENDOR_REQ_BUF * 2];
	unsigned copied, past = 0, i;

	reset_state();
	build_request(0xff, 0xff);
	memset(roomy, 0, sizeof(roomy));

	/* `i = 7 + data[6]; memcpy(&tx_pkt, data, i);` */
	copied = 7 + request[6];
	if (copied > sizeof(request))
		copied = sizeof(request);
	memcpy(roomy, request, copied);

	for (i = sizeof(generic_tx_packet); i < copied; i++)
		if (roomy[i] != 0)
			past++;
	mu_assert(past > 100, "the old copy should have run well past tx_pkt, "
	                      "so the guard region above is checking something");
}

/* Whatever tx_pkt holds, the radio is never handed more than DMA_SIZE bytes. */
MU_TEST(test_tx_generic_never_writes_more_than_dma_size)
{
	unsigned length;

	for (length = 0; length <= 0xff; length++) {
		reset_state();
		globals.pkt.length = (uint8_t)length;
		tx_generic();
		mu_assert(fifo_len <= DMA_SIZE,
		          "tx_generic() wrote more than DMA_SIZE bytes to the "
		          "radio's FIFO");
		mu_assert_int_eq((int)MIN(length, DMA_SIZE), (int)fifo_len);
	}
}

/*
 * No single write is larger than the FIFO, for any length the packet can
 * hold. This is the bound the radio imposes, and it is on one write: the
 * total may be larger, because the radio has sent the earlier pieces by the
 * time the later ones go in.
 */
MU_TEST(test_no_write_is_larger_than_the_fifo)
{
	unsigned length, i;

	for (length = 0; length <= 0xff; length++) {
		char msg[200];

		reset_state();
		globals.pkt.length = (uint8_t)length;
		tx_generic();

		for (i = 0; i < write_count; i++) {
			snprintf(msg, sizeof(msg),
			         "a packet of %u bytes: write %u is %u bytes, more "
			         "than the radio's %d-byte FIFO holds",
			         length, i, writes[i], CC2400_FIFO_SIZE);
			mu_assert(writes[i] <= CC2400_FIFO_SIZE, msg);
		}
	}
}

/* Cut into pieces, not dropped: every payload byte reaches the radio, once
 * and in order. */
MU_TEST(test_the_whole_payload_reaches_the_radio_in_order)
{
	unsigned length, i;

	for (length = 0; length <= DMA_SIZE; length++) {
		char msg[200];

		reset_state();
		globals.pkt.length = (uint8_t)length;
		for (i = 0; i < length; i++)
			globals.pkt.data[i] = (uint8_t)(i + 1);
		tx_generic();

		snprintf(msg, sizeof(msg),
		         "a packet of %u bytes: %u bytes reached the radio",
		         length, fifo_len);
		mu_assert_int_eq((int)length, (int)fifo_len);
		for (i = 0; i < length; i++) {
			snprintf(msg, sizeof(msg),
			         "a packet of %u bytes: byte %u reached the radio as "
			         "0x%02x, not 0x%02x", length, i, fifo[i],
			         (uint8_t)(i + 1));
			mu_assert(fifo[i] == (uint8_t)(i + 1), msg);
		}
	}
}

/*
 * The shape of the two cases that matter: the longest packet goes out as a
 * full FIFO and the remaining 18 bytes, and a packet that fits goes out whole.
 * A zero-length packet is still one write, which is what it was before --
 * the radio sends the preamble and sync word with no payload.
 */
MU_TEST(test_the_longest_packet_goes_out_as_two_pieces)
{
	reset_state();
	globals.pkt.length = DMA_SIZE;
	tx_generic();
	mu_assert_int_eq(2, (int)write_count);
	mu_assert_int_eq(CC2400_FIFO_SIZE, writes[0]);
	mu_assert_int_eq(DMA_SIZE - CC2400_FIFO_SIZE, writes[1]);

	reset_state();
	globals.pkt.length = CC2400_FIFO_SIZE;
	tx_generic();
	mu_assert_int_eq(1, (int)write_count);
	mu_assert_int_eq(CC2400_FIFO_SIZE, writes[0]);

	reset_state();
	globals.pkt.length = CC2400_FIFO_SIZE + 1;
	tx_generic();
	mu_assert_int_eq(2, (int)write_count);
	mu_assert_int_eq(CC2400_FIFO_SIZE, writes[0]);
	mu_assert_int_eq(1, writes[1]);

	reset_state();
	globals.pkt.length = 0;
	tx_generic();
	mu_assert_int_eq(1, (int)write_count);
	mu_assert_int_eq(0, writes[0]);
}

/*
 * A radio that never empties its FIFO ends the packet where it is. Carrying on
 * would write the next piece into a FIFO the radio still holds data in, which
 * is the overrun the loop exists to avoid: the packet is lost either way, and
 * this way nothing goes past the FIFO to lose it.
 */
MU_TEST(test_a_radio_that_does_not_drain_ends_the_packet)
{
	unsigned length;

	for (length = CC2400_FIFO_SIZE; length <= DMA_SIZE; length++) {
		char msg[200];

		reset_state();
		globals.pkt.length = (uint8_t)length;
		fifo_drains = 0;
		tx_generic();

		snprintf(msg, sizeof(msg),
		         "a packet of %u bytes with a radio that never empties its "
		         "FIFO: %u writes, %u bytes", length, write_count, fifo_len);
		mu_assert(write_count == 1, msg);
		mu_assert(fifo_len == CC2400_FIFO_SIZE, msg);
	}
}

/*
 * What the single write this replaces did, so the tests above are checking
 * something: a 50-byte packet went into the FIFO as one 50-byte write, 18
 * bytes past its end.
 */
MU_TEST(test_the_old_single_write_overran_the_fifo)
{
	reset_state();
	globals.pkt.length = DMA_SIZE;
	cc2400_fifo_write(MIN(globals.pkt.length, DMA_SIZE), globals.pkt.data);
	mu_assert_int_eq(1, (int)write_count);
	mu_assert(writes[0] > CC2400_FIFO_SIZE,
	          "the write this replaces should have been larger than the "
	          "FIFO, so the bound the tests above check means something");
}

/*
 * The fakes above are only worth something if the firmware has the same shape,
 * and neither the handler nor tx_generic() can run here. So: find the handler's
 * case, and check that it refuses a request before it copies anything, that the
 * one copy into tx_pkt is bounded by the header length plus the byte it
 * checked, and that the FIFO write is clamped.
 */
struct scan {
	int case_line;        /* the handler's `case UBERTOOTH_TX_...:` */
	int header_check;     /* the request is long enough for the header */
	int length_read;      /* the length byte is read at its own offset */
	int length_check;     /* the length byte is at most DMA_SIZE */
	int carried_check;    /* the request carries the payload it declares */
	int copy;             /* the memcpy into tx_pkt */
	int copies;           /* how many memcpy's into tx_pkt the file has */
	int last_return;      /* the last refusal before the copy */
	int header_define;    /* where TX_GENERIC_HEADER_LEN is defined */
	int fifo_write;       /* the FIFO write in tx_generic(), of one piece */
	int fifo_writes;      /* how many FIFO writes tx_generic() makes */
	int length_clamped;   /* the payload length clamped to DMA_SIZE */
	int chunked;          /* a piece bounded by the FIFO's size */
	int drain_wait;       /* the wait for the radio between pieces */
	int drain_wait_used;  /* ...with its result acted on */
	int loop_end;         /* the loop that keeps going while bytes are left */
};

static int scan_source(const char *path, struct scan *s)
{
	char line[512], flat[512];
	int lineno = 0, in_block = 0, in_case = 0;
	FILE *f = fopen(path, "r");

	if (f == NULL)
		return 0;

	memset(s, 0, sizeof(*s));

	while (fgets(line, sizeof(line), f) != NULL) {
		lineno++;
		strip_comments(line, &in_block);
		collapse_spaces(line, flat, sizeof(flat));

		if (strstr(flat, "#define TX_GENERIC_HEADER_LEN "
		                 "offsetof(generic_tx_packet, data)") != NULL)
			s->header_define = lineno;

		if (strstr(flat, "memcpy(&tx_pkt") != NULL) {
			s->copies++;
			if (strstr(flat, "TX_GENERIC_HEADER_LEN + i)") != NULL &&
			    s->copy == 0)
				s->copy = lineno;
		}

		if (strstr(flat, "len = MIN(tx_pkt.length, DMA_SIZE)") != NULL)
			s->length_clamped = lineno;
		if (strstr(flat, "chunk = MIN(len - sent, CC2400_FIFO_SIZE)") != NULL)
			s->chunked = lineno;
		if (strstr(flat, "cc2400_fifo_write(chunk, tx_pkt.data + sent)") !=
		    NULL) {
			s->fifo_write = lineno;
			s->fifo_writes++;
		}
		/* The first one after the clamp, so this is tx_generic()'s and
		 * not le_transmit()'s, which comes earlier in the file. */
		if (strstr(flat, "fifo_wait_empty()") != NULL &&
		    s->length_clamped != 0 && s->drain_wait == 0) {
			s->drain_wait = lineno;
			s->drain_wait_used = strstr(flat, "!fifo_wait_empty()") != NULL;
		}
		if (strstr(flat, "} while (sent < len);") != NULL)
			s->loop_end = lineno;

		if (strstr(flat, "case UBERTOOTH_TX_GENERIC_PACKET:") != NULL) {
			s->case_line = lineno;
			in_case = 1;
			continue;
		}
		if (!in_case)
			continue;

		if (strstr(flat, "data_in_len < TX_GENERIC_HEADER_LEN") != NULL)
			s->header_check = lineno;
		if (strstr(flat, "i = data[offsetof(generic_tx_packet, length)]") !=
		    NULL)
			s->length_read = lineno;
		if (strstr(flat, "i > DMA_SIZE") != NULL)
			s->length_check = lineno;
		if (strstr(flat, "data_in_len - TX_GENERIC_HEADER_LEN < i") != NULL)
			s->carried_check = lineno;
		if (strstr(flat, "return 0;") != NULL)
			s->last_return = lineno;
		if (strstr(flat, "break;") != NULL)
			in_case = 0;
	}

	fclose(f);
	return 1;
}

static struct scan s;
static int scanned;
static const char *path = FW_RXTX_DIR "/bluetooth_rxtx.c";

MU_TEST(test_source_was_scanned)
{
	char msg[512];

	snprintf(msg, sizeof(msg), "cannot open %s to scan", path);
	mu_assert(scanned, msg);

	snprintf(msg, sizeof(msg),
	         "%s: the scan did not find the generic transmit request, so the "
	         "checks below would pass without reading anything", path);
	mu_assert(s.case_line > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s: TX_GENERIC_HEADER_LEN is not defined as offsetof("
	         "generic_tx_packet, data), so the bound the handler copies to "
	         "is not the packet's own header length", path);
	mu_assert(s.header_define > 0, msg);
}

MU_TEST(test_the_handler_refuses_before_it_copies)
{
	char msg[640];

	snprintf(msg, sizeof(msg),
	         "%s:%d: the generic transmit request does not check that the "
	         "request is long enough to hold the header", path, s.case_line);
	mu_assert(s.header_check > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: the generic transmit request does not read its length "
	         "byte into `i` at the offset the struct puts it, so what the "
	         "checks below bound is not the length the copy uses",
	         path, s.case_line);
	mu_assert(s.length_read > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: the generic transmit request does not check its length "
	         "byte against DMA_SIZE, so a longer one would be copied past "
	         "tx_pkt", path, s.case_line);
	mu_assert(s.length_check > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: the generic transmit request does not check that it "
	         "carries as much payload as it declares", path, s.case_line);
	mu_assert(s.carried_check > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s: the only copy into tx_pkt must be bounded by "
	         "TX_GENERIC_HEADER_LEN plus the length byte that was checked; "
	         "the file has %d copies into it and the bounded one is at line "
	         "%d", path, s.copies, s.copy);
	mu_assert(s.copies == 1 && s.copy > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: the copy into tx_pkt comes before the length read at "
	         "line %d or the checks at lines %d/%d/%d, or a refusal follows "
	         "it, so a request is copied before it is refused",
	         path, s.copy, s.length_read, s.header_check,
	         s.length_check, s.carried_check);
	mu_assert(s.header_check < s.copy && s.length_read < s.copy &&
	          s.length_check < s.copy && s.carried_check < s.copy &&
	          s.last_return < s.copy, msg);
}

MU_TEST(test_the_fifo_write_is_clamped_and_chunked)
{
	char msg[640];

	snprintf(msg, sizeof(msg),
	         "%s: the scan did not find tx_generic()'s FIFO write, so the "
	         "checks below would pass without reading anything", path);
	mu_assert(s.fifo_write > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s: tx_generic() must write the payload in one place, so the "
	         "bound below covers every byte that reaches the radio; the scan "
	         "found %d writes", path, s.fifo_writes);
	mu_assert(s.fifo_writes == 1, msg);

	snprintf(msg, sizeof(msg),
	         "%s: tx_generic() hands the radio a length it has not clamped to "
	         "DMA_SIZE, so a length the handler did not set would read past "
	         "tx_pkt.data", path);
	mu_assert(s.length_clamped > 0 && s.length_clamped < s.fifo_write, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: tx_generic() does not bound each piece it writes by "
	         "CC2400_FIFO_SIZE, so a packet longer than the radio's FIFO is "
	         "pushed past the end of it", path, s.fifo_write);
	mu_assert(s.chunked > 0 && s.chunked < s.fifo_write, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: tx_generic() writes the next piece without waiting for "
	         "the radio to have sent the last one", path, s.fifo_write);
	mu_assert(s.drain_wait > 0 && s.drain_wait < s.chunked, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: tx_generic() ignores what the wait returned, so a radio "
	         "that never empties its FIFO gets the next piece written past "
	         "the end of it anyway", path, s.drain_wait);
	mu_assert(s.drain_wait_used, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: the write is not in a loop that runs until the whole "
	         "payload has gone out, so a packet longer than the FIFO loses "
	         "its tail", path, s.fifo_write);
	mu_assert(s.loop_end > s.fifo_write, msg);
}

int main(void)
{
	printf("generic transmit packet bounds tests\n");
	MU_RUN(test_the_packet_holds_dma_size_payload_bytes);
	MU_RUN(test_a_payload_at_the_limit_is_accepted);
	MU_RUN(test_the_last_payload_byte_arrives);
	MU_RUN(test_a_payload_one_over_the_limit_is_refused);
	MU_RUN(test_the_longest_possible_length_is_refused);
	MU_RUN(test_a_request_shorter_than_it_claims_is_refused);
	MU_RUN(test_a_request_shorter_than_the_header_is_refused);
	MU_RUN(test_the_old_unchecked_copy_ran_past_the_packet);
	MU_RUN(test_tx_generic_never_writes_more_than_dma_size);
	MU_RUN(test_no_write_is_larger_than_the_fifo);
	MU_RUN(test_the_whole_payload_reaches_the_radio_in_order);
	MU_RUN(test_the_longest_packet_goes_out_as_two_pieces);
	MU_RUN(test_a_radio_that_does_not_drain_ends_the_packet);
	MU_RUN(test_the_old_single_write_overran_the_fifo);

	/* Scanned once here rather than from the first of the three tests
	 * below, which would leave them depending on the order they run in. */
	scanned = scan_source(path, &s);
	MU_RUN(test_source_was_scanned);
	MU_RUN(test_the_handler_refuses_before_it_copies);
	MU_RUN(test_the_fifo_write_is_clamped_and_chunked);
	return mu_report();
}
