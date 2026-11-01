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
	/* A channel the synthesizer can reach, so every test above this one is
	 * about the length rather than about the channel. */
	request[offsetof(generic_tx_packet, channel)] = BT_FREQ_MIN & 0xff;
	request[offsetof(generic_tx_packet, channel) + 1] = BT_FREQ_MIN >> 8;
	for (i = 0; i < payload && TX_GENERIC_HEADER_LEN + i < sizeof(request);
	     i++)
		request[TX_GENERIC_HEADER_LEN + i] = 0xa5;
	return TX_GENERIC_HEADER_LEN + payload;
}

/* What the handler does now. 1 accepts the request, 0 stalls it. */
static int tx_generic_request(unsigned data_in_len)
{
	uint8_t i;
	uint16_t tx_channel;

	if (data_in_len < TX_GENERIC_HEADER_LEN)
		return 0;
	i = request[offsetof(generic_tx_packet, length)];
	if (i > DMA_SIZE || data_in_len - TX_GENERIC_HEADER_LEN < i)
		return 0;
	tx_channel = request[offsetof(generic_tx_packet, channel)]
	           | ((uint16_t)request[offsetof(generic_tx_packet, channel) + 1] << 8);
	if (tx_channel < MIN_FREQ || tx_channel > MAX_FREQ)
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
	/* And the payload a host tool offers is one FIFO load, which is the
	 * most that goes out as a frame of a known shape. The interface header
	 * cannot include the radio's own, so the two numbers meet here. */
	mu_assert_int_eq(CC2400_FIFO_SIZE, GENERIC_TX_FRAMED_PAYLOAD);
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

/*
 * The channel goes into FSDIV unchanged, so only a frequency the synthesizer
 * can reach is accepted. One outside that range used to be taken: the
 * transmit then waited out its lock timeout, sent nothing, and left that
 * frequency in the register for whatever ran next.
 */
MU_TEST(test_a_channel_the_radio_cannot_tune_is_refused)
{
	const uint16_t refused[] = { 0, 1, MIN_FREQ - 1, MAX_FREQ + 1, 0xffff };
	const uint16_t accepted[] = { MIN_FREQ, BT_FREQ_MIN, BT_FREQ_MAX,
	                              MAX_FREQ };
	unsigned len, i;

	for (i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
		reset_state();
		len = build_request(4, 4);
		request[offsetof(generic_tx_packet, channel)] = refused[i] & 0xff;
		request[offsetof(generic_tx_packet, channel) + 1] = refused[i] >> 8;
		mu_assert_int_eq(0, tx_generic_request(len));
		mu_assert_int_eq(0, globals.pkt.length);
	}
	for (i = 0; i < sizeof(accepted) / sizeof(accepted[0]); i++) {
		reset_state();
		len = build_request(4, 4);
		request[offsetof(generic_tx_packet, channel)] = accepted[i] & 0xff;
		request[offsetof(generic_tx_packet, channel) + 1] = accepted[i] >> 8;
		mu_assert_int_eq(1, tx_generic_request(len));
		mu_assert_int_eq(accepted[i], globals.pkt.channel);
	}
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
	int channel_check;    /* the channel is one the synthesizer can reach */
	int repeat_read;      /* the burst count read out of the request */
	int burst_label;      /* where a packet of the burst starts */
	int burst_loop;       /* the jump back for the next packet */
	int restore_first;    /* the first register the restore puts back */
	int tx_crc;           /* the transmit framing turns the radio CRC on */
	int rx_crc;           /* the receive records the radio CRC verdict */
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
	int gio_in_le;        /* le_transmit() puts FIFO-empty on GIO6 */
	int gio_in_tx;        /* ...and so does tx_generic() */
	int le_wait;          /* le_transmit()'s own wait */
	int bare_gio6_spin;   /* an unbounded wait on GIO6, anywhere in the file */
	int bare_gio6_line;
	int wait_body;        /* where fifo_wait_empty() is defined */
	int wait_bound;       /* ...and where its poll count is used */
	int waits_checked;    /* calls whose result is acted on, in both loops */
	int loop_end;         /* the loop that keeps going while bytes are left */
	int tx_grmdm;         /* tx_generic()'s own write of GRMDM */
	char tx_grmdm_text[256];
	long tx_grmdm_value;  /* the GRMDM value it writes, -1 if not a number */
};

/* The value of `cc2400_set(<reg>, 0x1234);`, or -1 when the line writes
 * something that is not a hex number. */
static long hex_value_written(const char *flat)
{
	const char *p = strstr(flat, "0x");

	if (p == NULL)
		return -1;
	return strtol(p, NULL, 16);
}

static int scan_source(const char *path, struct scan *s)
{
	char line[512], flat[512];
	int lineno = 0, in_block = 0, in_case = 0, in_fn = 0;
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

		/* The channel refusal. The handler reads the request's channel
		 * into a local of its own, so this cannot match
		 * UBERTOOTH_SET_CHANNEL's check, which shares the lower bound
		 * and takes anything above the upper one as the band sweep. */
		if (strstr(flat, "tx_channel < MIN_FREQ || tx_channel > MAX_FREQ")
		    != NULL)
			s->channel_check = lineno;

		if (strstr(flat, "tx_pkt_repeat = request_params[0]") != NULL)
			s->repeat_read = lineno;
		if (strstr(flat, "send_one:") != NULL)
			s->burst_label = lineno;
		if (strstr(flat, "goto send_one;") != NULL)
			s->burst_loop = lineno;
		if (s->restore_first == 0 &&
		    strstr(flat, "cc2400_set(SYNCH, synch)") != NULL)
			s->restore_first = lineno;
		if (strstr(flat, "cc2400_set(GRMDM, 0x0f61 | GRMDM_CRC)") != NULL)
			s->tx_crc = lineno;
		if (strstr(flat, "buf[RX_GENERIC_CRC_OFFSET] =") != NULL)
			s->rx_crc = lineno;

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
		/* Both transmit loops have to act on what the wait returned, not
		 * only tx_generic()'s: le_transmit()'s call comes earlier in the
		 * file, so the check above does not reach it. */
		if (strstr(flat, "!fifo_wait_empty()") != NULL)
			s->waits_checked++;

		/*
		 * The signal the two waits read. fifo_wait_empty() polls GIO6,
		 * and GIO6 carries whatever IOCFG was told to put on it, so a
		 * loop that waits for the FIFO to empty has to ask for the
		 * FIFO-empty signal: FIFO_FULL is a different one with the
		 * opposite sense, and the threshold in INT is a third.
		 *
		 * Looked for inside each of the two functions rather than across
		 * the file, because br_transmit() asks for FIFO_FULL on purpose
		 * and never waits on the pin at all: it writes 16 bytes once per
		 * pair of slots, which the FIFO always has room for.
		 */
		if (strstr(flat, "void le_transmit(") != NULL)
			in_fn = 1;
		else if (strstr(flat, "void tx_generic(") != NULL)
			in_fn = 2;
		else if (in_fn != 0 && line[0] == '}')
			in_fn = 0;  /* a closing brace in column 0 ends it */
		if (strstr(flat, "cc2400_set(IOCFG, (GIO_FIFO_EMPTY << 9)") !=
		    NULL) {
			if (in_fn == 1 && s->gio_in_le == 0)
				s->gio_in_le = lineno;
			else if (in_fn == 2 && s->gio_in_tx == 0)
				s->gio_in_tx = lineno;
		}
		if (in_fn == 1 && s->le_wait == 0 &&
		    strstr(flat, "fifo_wait_empty()") != NULL)
			s->le_wait = lineno;

		/*
		 * The framing tx_generic() sends with. The first write is the
		 * setup; the write at the end of the function puts the saved
		 * value back, which test_grmdm_rx_generic.c checks.
		 */
		if (in_fn == 2 && s->tx_grmdm == 0 &&
		    strstr(flat, "cc2400_set(GRMDM,") != NULL) {
			s->tx_grmdm = lineno;
			snprintf(s->tx_grmdm_text, sizeof(s->tx_grmdm_text),
			         "%s", flat);
			s->tx_grmdm_value = hex_value_written(flat);
		}
		if (strstr(flat, "} while (sent < len);") != NULL)
			s->loop_end = lineno;

		/* The wait's own bound. Without the poll count it is another
		 * unbounded spin, in a function whose name says otherwise. */
		if (strstr(flat, "static int fifo_wait_empty(") != NULL)
			s->wait_body = lineno;
		if (s->wait_body != 0 && s->wait_bound == 0 &&
		    strstr(flat, "FIFO_EMPTY_POLLS") != NULL)
			s->wait_bound = lineno;

		/* The wait this file must not spin on. `while (!GIO6);` is how
		 * both transmit loops waited for the radio to empty its FIFO,
		 * and it has no end: the loop it sits in is the one that answers
		 * the host. test_cc2400_wait.c's scan for unbounded radio spins
		 * does not see this one, because it looks for the names of the
		 * register reads (cc2400_status(), FSMSTATE) rather than a GPIO. */
		if (strstr(flat, "while (!GIO6)") != NULL ||
		    strstr(flat, "while(!GIO6)") != NULL) {
			s->bare_gio6_spin++;
			if (s->bare_gio6_line == 0)
				s->bare_gio6_line = lineno;
		}

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
	         "%s:%d: the generic transmit request does not check its channel "
	         "against MIN_FREQ and MAX_FREQ, so a frequency the synthesizer "
	         "cannot reach would go into FSDIV and be left there",
	         path, s.case_line);
	mu_assert(s.channel_check > 0, msg);

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
	          s.channel_check < s.copy && s.last_return < s.copy, msg);
}

/*
 * One request can ask for a burst, and every packet of it has to go out with
 * the framing the first one had. The loop therefore sits inside the register
 * save and restore: a first attempt put it after the restore, so packets two
 * and on went out with the Bluetooth registers the restore had put back, and
 * a board set to this framing read one packet of thirty.
 */
MU_TEST(test_the_burst_loop_is_inside_the_register_restore)
{
	char msg[640];

	snprintf(msg, sizeof(msg),
	         "%s:%d: the generic transmit request does not read a packet "
	         "count out of its wValue, so one request cannot carry a burst "
	         "and the host is back to a request per packet",
	         path, s.case_line);
	mu_assert(s.repeat_read > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s: the scan did not find the burst loop (label at line %d, "
	         "jump at line %d), so the checks below would pass without "
	         "reading anything", path, s.burst_label, s.burst_loop);
	mu_assert(s.burst_label > 0 && s.burst_loop > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s: the burst loop at line %d jumps back to line %d, and the "
	         "register restore starts at line %d: the loop has to be inside "
	         "the restore, or every packet after the first goes out with "
	         "the registers the restore put back",
	         path, s.burst_loop, s.burst_label, s.restore_first);
	mu_assert(s.restore_first > 0 && s.burst_label < s.burst_loop &&
	          s.burst_loop < s.restore_first, msg);
}

/*
 * The framing the transmit programs turns the radio's CRC on, and the receive
 * writes down what the radio made of it. Without the CRC a payload with a bit
 * error in it reads as a payload: this framing has no other check.
 */
MU_TEST(test_the_framing_carries_a_crc_and_the_receive_records_it)
{
	char msg[640];

	snprintf(msg, sizeof(msg),
	         "%s: the generic transmit does not write GRMDM with GRMDM_CRC, "
	         "so what it sends carries no CRC and a receiving board has "
	         "nothing to judge a packet by", path);
	mu_assert(s.tx_crc > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s: the generic receive does not record the radio CRC verdict "
	         "at RX_GENERIC_CRC_OFFSET, so a corrupt packet reads like a "
	         "good one", path);
	mu_assert(s.rx_crc > 0, msg);
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

	snprintf(msg, sizeof(msg),
	         "%s: both transmit loops have to end the packet when the wait "
	         "gives up, rather than writing the next piece into a FIFO the "
	         "radio still holds; %d of the two act on what it returned",
	         path, s.waits_checked);
	mu_assert(s.waits_checked == 2, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: fifo_wait_empty() does not use FIFO_EMPTY_POLLS, so the "
	         "wait the loop relies on has no bound of its own", path,
	         s.wait_body);
	mu_assert(s.wait_body > 0 && s.wait_bound >= s.wait_body, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: a wait on GIO6 with no end (%d in the file); the loop it "
	         "is in is the one that answers the host, so a radio that stops "
	         "draining its FIFO stops the board until it is re-plugged -- use "
	         "fifo_wait_empty()", path, s.bare_gio6_line, s.bare_gio6_spin);
	mu_assert(s.bare_gio6_spin == 0, msg);
}

/*
 * The two loops that refill the FIFO wait for the radio to empty it, so each
 * has to put the FIFO-empty signal on the pin it polls. Waiting on a pin that
 * carries something else is the failure these two catch: with FIFO_FULL on it
 * the sense is inverted, and the next piece goes into a FIFO the radio still
 * holds. One test each, so a loop that has lost its setup cannot keep the
 * other from being checked.
 */
MU_TEST(test_le_transmit_waits_on_the_fifo_being_empty)
{
	char msg[640];

	snprintf(msg, sizeof(msg),
	         "%s: le_transmit() does not put the radio's FIFO-empty signal "
	         "on GIO6, which is the pin fifo_wait_empty() polls, so its wait "
	         "reads whatever signal was on the pin", path);
	mu_assert_fatal(s.gio_in_le > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: le_transmit() waits on GIO6 at line %d before asking "
	         "for the FIFO-empty signal", path, s.gio_in_le, s.le_wait);
	mu_assert(s.le_wait > 0 && s.gio_in_le < s.le_wait, msg);
}

MU_TEST(test_tx_generic_waits_on_the_fifo_being_empty)
{
	char msg[640];

	snprintf(msg, sizeof(msg),
	         "%s: tx_generic() does not put the radio's FIFO-empty signal on "
	         "GIO6, which is the pin fifo_wait_empty() polls, so its wait "
	         "reads whatever signal was on the pin", path);
	mu_assert_fatal(s.gio_in_tx > 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: tx_generic() waits on GIO6 at line %d before asking for "
	         "the FIFO-empty signal", path, s.gio_in_tx, s.drain_wait);
	mu_assert(s.gio_in_tx < s.drain_wait, msg);
}

/*
 * The radio frames a generic transmit itself, and the generic receive programs
 * nothing: it receives with whatever the host left in the registers. So the
 * GRMDM value this transmit writes says how the packet is framed -- preamble,
 * sync word, CRC, and whether the payload goes through the FIFO at all -- and
 * the host has to mirror it on the receiving board. The sync word, the
 * deviation and the channel are the transmit's other half, and they come from
 * the request and from MDMCTRL rather than from here.
 *
 * Each field checked below is one the generic receive needs a particular way,
 * with the reason in its message. The preamble is the exception: that path
 * needs a preamble but no particular length, so the check is only that there
 * is one.
 */
MU_TEST(test_the_transmits_framing_is_one_the_generic_receive_can_read)
{
	char msg[768];
	long g = s.tx_grmdm_value;

	snprintf(msg, sizeof(msg),
	         "%s: tx_generic() writes no GRMDM value of its own, so the "
	         "framing below cannot be checked", path);
	mu_assert_fatal(s.tx_grmdm > 0 && g >= 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: tx_generic() sends with GRMDM 0x%04lx, which is not "
	         "packet mode. The radio then puts a raw symbol stream on the "
	         "air instead of framing the FIFO, and the write loop below has "
	         "nothing to feed", path, s.tx_grmdm, g);
	mu_assert(grmdm_packet_mode((uint16_t)g), msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: tx_generic() sends with GRMDM 0x%04lx, which is not "
	         "buffered mode, so the payload does not go through the radio's "
	         "FIFO -- which is where this function writes it", path,
	         s.tx_grmdm, g);
	mu_assert((g & GRMDM_BUFFERED_MASK) == GRMDM_BUFFERED, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: tx_generic() sends with GRMDM 0x%04lx, which puts no "
	         "preamble in front of the sync word. The receiving board's modem "
	         "settles its frequency correction over the preamble -- the AFC "
	         "settling in MDMTST0, which for the generic receive is whatever "
	         "the host wrote -- so a packet with none leaves it nothing to "
	         "settle on", path, s.tx_grmdm, g);
	mu_assert((g & GRMDM_PREAMBLE_MASK) != 0, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: tx_generic() sends with GRMDM 0x%04lx, which matches "
	         "only part of the sync word. The request carries a whole 32-bit "
	         "sync word and the generic receive puts all four bytes of it in "
	         "every record, so both ends read all 32 bits", path, s.tx_grmdm,
	         g);
	mu_assert((g & GRMDM_SYNC_WORD_32) == GRMDM_SYNC_WORD_32, msg);

	snprintf(msg, sizeof(msg),
	         "%s:%d: tx_generic() sends with GRMDM 0x%04lx, which has the "
	         "radio append a CRC. The generic receive reads a fixed number of "
	         "bytes out of the FIFO and never looks at the radio's CRC "
	         "result, so the CRC buys nothing there and, for a payload "
	         "shorter than that read, lands among the bytes the record shows "
	         "as payload", path, s.tx_grmdm, g);
	mu_assert((g & GRMDM_CRC) == 0, msg);
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
	MU_RUN(test_a_channel_the_radio_cannot_tune_is_refused);
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
	MU_RUN(test_the_burst_loop_is_inside_the_register_restore);
	MU_RUN(test_the_framing_carries_a_crc_and_the_receive_records_it);
	MU_RUN(test_the_fifo_write_is_clamped_and_chunked);
	MU_RUN(test_le_transmit_waits_on_the_fifo_being_empty);
	MU_RUN(test_tx_generic_waits_on_the_fifo_being_empty);
	MU_RUN(test_the_transmits_framing_is_one_the_generic_receive_can_read);
	return mu_report();
}
