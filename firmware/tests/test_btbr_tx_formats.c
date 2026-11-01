/*
 * Unit tests for the basic-rate packet formats btbr can be asked to transmit
 * (firmware/btbr/src/codec/codec.c, bbcodec_tx_format() and the encoder).
 *
 * A stack sends DM1, DH1 and their longer relatives and little else, so the
 * decoders for the rest -- AUX1, DV, the HV voice packets and the EV
 * synchronous ones -- are rarely given a real packet to read. The host can
 * now ask this firmware for any of them, which means the air layout of each
 * has to be right whether or not a peer ever answers one.
 *
 * Two things are checked for every format. The packet occupies exactly the
 * number of air bits the specification gives it (Bluetooth Core, Vol 2 Part
 * B, Table 6.5 and Table 6.6), which the cases below spell out rather than
 * recompute from the table under test. And the bytes come off the air as
 * they went on: each packet is decoded again through the same codec, with the
 * CRC checked where the format has one, so a whitening, error-correction or
 * length mistake shows up as a payload that does not match.
 *
 * The lengths include the ends of each format's range and odd values in
 * between, because those are where the rate 2/3 code's 10-bit groups and the
 * payload header's length field go wrong if they go wrong at all.
 *
 * codec.c and bitstream_proc.c are pure logic, so the real ones are linked
 * here, as test_btbr_codec_header.c does.
 *
 * This file is part of Project Ubertooth and is released under the terms of
 * the GPL. Refer to COPYING for more information.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "minunit.h"

#include <ubtbr/bb.h>
#include <ubtbr/btphy.h>
#include <ubtbr/codec.h>

/* The two diagnostics every btbr source can reach, declared by
 * stubs-btbr/ubtbr/debug.h. A DIE() here means the test asked the codec for
 * something the firmware never asks for, so failing there is the point. */
void cprintf(char *fmt, ...)
{
	(void)fmt;
}

void __attribute__((noreturn)) die(char *fmt, ...)
{
	va_list args;

	fflush(stdout);
	fputs("die: ", stderr);
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
	exit(1);
}

/* The longest packet any of these formats makes is a DH5: 339 data bytes, a
 * two-byte payload header and a two-byte CRC, with no error correction, so
 * 58 + 343*8 air bits. The buffers are sized for the rate 2/3 code's larger
 * output and for the bytes fec23() and fec13() write past the last group. */
#define AIR_BUF_SIZE	1024
#define DATA_BUF_SIZE	512

/* The piconet this packet belongs to, and the whitening the air carries. Any
 * values will do: what the cases check is that the two ends agree. */
#define TEST_UAP	0x5a
#define TEST_WHITEN	0x37
#define TEST_TRAILER	0x5
#define TEST_LT_ADDR	3

/* The formats, in the order of btctl_tx_format_t. */
static const struct {
	unsigned format;
	const char *name;
} formats[] = {
	{ BTCTL_FMT_DM1,  "DM1"  },
	{ BTCTL_FMT_DH1,  "DH1"  },
	{ BTCTL_FMT_DM3,  "DM3"  },
	{ BTCTL_FMT_DH3,  "DH3"  },
	{ BTCTL_FMT_DM5,  "DM5"  },
	{ BTCTL_FMT_DH5,  "DH5"  },
	{ BTCTL_FMT_AUX1, "AUX1" },
	{ BTCTL_FMT_DV,   "DV"   },
	{ BTCTL_FMT_HV1,  "HV1"  },
	{ BTCTL_FMT_HV2,  "HV2"  },
	{ BTCTL_FMT_HV3,  "HV3"  },
	{ BTCTL_FMT_EV3,  "EV3"  },
	{ BTCTL_FMT_EV4,  "EV4"  },
	{ BTCTL_FMT_EV5,  "EV5"  },
};

/* One packet, built by the encoder. */
typedef struct {
	uint8_t air[AIR_BUF_SIZE];
	uint8_t data[DATA_BUF_SIZE];
	bbcodec_t codec;
	bbhdr_t hdr;
	const bbcodec_types_t *t;
	uint8_t air_type;
	unsigned data_bytes;
} packet_t;

/* Fill the data field with bytes a wrong length or a wrong whitening state
 * cannot reproduce by accident, and write the payload header the format
 * carries. */
static void fill_payload(packet_t *p, unsigned data_bytes)
{
	unsigned i;
	uint8_t *data = p->data + p->t->voice_bytes;

	memset(p->data, 0, sizeof(p->data));
	for (i = 0; i < p->t->voice_bytes; i++)
		p->data[i] = 0xc0 ^ (uint8_t)(i * 7 + 1);
	if (p->t->payload_header_bytes == 1)
	{
		/* length, flow, LLID (Vol 2 Part B 6.5.4.1) */
		data[0] = (uint8_t)((data_bytes << 3) | (1 << 2) | 2);
	}
	else if (p->t->payload_header_bytes == 2)
	{
		uint16_t h = (uint16_t)((data_bytes << 3) | (1 << 2) | 2);

		data[0] = 0xff & h;
		data[1] = 0xff & (h >> 8);
	}
	for (i = 0; i < data_bytes; i++)
		data[p->t->payload_header_bytes + i] =
			(uint8_t)(0x5a ^ (i * 31 + 3));
}

/* Build one packet of the given format with that many data bytes. */
static void encode(packet_t *p, unsigned format, unsigned data_bytes)
{
	memset(p->air, 0, sizeof(p->air));
	p->t = bbcodec_tx_format(format, &p->air_type);
	mu_assert(p->t != NULL, "every format has an air layout");
	p->data_bytes = data_bytes;
	fill_payload(p, data_bytes);

	p->hdr.lt_addr = TEST_LT_ADDR;
	p->hdr.type = p->air_type;
	p->hdr.flags = 0;
	p->hdr.hec = 0;

	bbcodec_init(&p->codec, TEST_WHITEN, TEST_UAP, 1, 0);
	bbcodec_set_tx_format(&p->codec, p->t,
		p->t->payload_header_bytes ? 0 : data_bytes);
	bbcodec_encode_header(&p->codec, p->air, &p->hdr, TEST_TRAILER,
		p->data);
	while (p->codec.coded_pos < p->codec.coded_total)
		bbcodec_encode_chunk(&p->codec, p->air, &p->hdr, p->data);
}

/* Read the packet back off the air and compare it with what went on.
 *
 * The decoder is given the same format, because a TYPE code does not say
 * which format a synchronous packet is and the air carries no length for the
 * EV packets; everything else -- the whitening, the error correction, the
 * payload header and the CRC -- it works out for itself.
 */
static void decode_and_compare(packet_t *p, const char *name)
{
	static uint8_t out[DATA_BUF_SIZE];
	bbcodec_t rx;
	bbhdr_t rx_hdr;
	uint16_t out_size = 0;
	uint8_t flags;
	int rc = 0;
	unsigned chunk, expect_total;
	char message[128];

	memset(out, 0, sizeof(out));
	bbcodec_init(&rx, TEST_WHITEN, TEST_UAP, 1, 0);
	rc = bbcodec_decode_header(&rx, &rx_hdr, p->air);
	snprintf(message, sizeof(message), "%s: the header decodes", name);
	mu_assert(rc == 0, message);
	snprintf(message, sizeof(message), "%s: the TYPE code survives", name);
	mu_assert(rx_hdr.type == p->air_type, message);
	snprintf(message, sizeof(message),
		"%s: the transport address survives", name);
	mu_assert(rx_hdr.lt_addr == TEST_LT_ADDR, message);

	/* The header decode looked the TYPE code up in the asynchronous
	 * table; this is the format the sender chose. The voice field comes
	 * off the air here, because it carries no length and no CRC and the
	 * chunk decoder below is about the data field. */
	bbcodec_set_tx_format(&rx, p->t,
		p->t->payload_header_bytes ? 0 : p->data_bytes);
	if (p->t->voice_bytes)
	{
		null_decode(out, p->air, rx.air_off_b, p->t->voice_bytes);
		whiten(out, out, 8*p->t->voice_bytes, &rx.whiten_state);
		rx.air_off_b += 8*p->t->voice_bytes;
	}
	/* The whole buffer, as the receive task hands over a whole bank of air
	 * bytes rather than the packet's own length: the first chunk is
	 * decoded before any payload header has said how long the packet is,
	 * so a buffer cut to the packet would come up short. */
	for (chunk = 0; chunk < DATA_BUF_SIZE/CODEC_RX_CHUNK_SIZE + 2; chunk++)
	{
		rc = bbcodec_decode_chunk(&rx, out, &rx_hdr, p->air,
			sizeof(p->air));
		if (rc == BBCODEC_DONE)
			break;
		snprintf(message, sizeof(message),
			"%s: the payload is not cut short", name);
		mu_assert_fatal(rc != BBCODEC_SHORT, message);
	}
	snprintf(message, sizeof(message), "%s: the payload decodes", name);
	mu_assert_fatal(rc == BBCODEC_DONE, message);
	flags = bbcodec_decode_finalize(&rx, out, &out_size);

	expect_total = p->t->voice_bytes + p->t->payload_header_bytes
		+ p->data_bytes;
	snprintf(message, sizeof(message),
		"%s: the payload is as long as it was sent", name);
	mu_assert(out_size + p->t->voice_bytes == expect_total, message);
	snprintf(message, sizeof(message),
		"%s: every payload byte comes back", name);
	mu_assert(memcmp(out, p->data, expect_total) == 0, message);
	if (p->t->has_crc)
	{
		snprintf(message, sizeof(message), "%s: the CRC checks out",
			name);
		mu_assert((flags & (1<<BBPKT_F_GOOD_CRC)) != 0, message);
	}
}

/* Each format's packet occupies the air bits the specification gives it, and
 * comes back off the air as it went on. */
MU_TEST(test_air_size_and_round_trip)
{
	/* format, data bytes, payload air bits (Vol 2 Part B, Tables 6.5 and
	 * 6.6). The voice formats and the longest of each data format are the
	 * sizes those tables state outright; the shorter data lengths are
	 * what the payload, its header and its CRC come to under the format's
	 * error correction. */
	static const struct {
		unsigned format;
		unsigned data_bytes;
		unsigned air_bits;
	} cases[] = {
		/* a DM1's 17 data bytes, its header and its CRC make 20 bytes
		 * = 16 groups of 10 bits = 240 air bits */
		{ BTCTL_FMT_DM1,   17, 240 },
		{ BTCTL_FMT_DM1,    0,  45 },
		{ BTCTL_FMT_DM1,    1,  60 },
		{ BTCTL_FMT_DH1,   27, 240 },
		{ BTCTL_FMT_DH1,    0,  24 },
		{ BTCTL_FMT_DH1,    7,  80 },
		{ BTCTL_FMT_DM3,  121, 1500 },
		{ BTCTL_FMT_DM3,   67, 855 },
		{ BTCTL_FMT_DH3,  183, 1496 },
		{ BTCTL_FMT_DH3,   99, 824 },
		{ BTCTL_FMT_DM5,  224, 2745 },
		{ BTCTL_FMT_DM5,  113, 1410 },
		{ BTCTL_FMT_DH5,  339, 2744 },
		{ BTCTL_FMT_DH5,  171, 1400 },
		/* AUX1 is a DH1 with no CRC: 29 data bytes and a one-byte
		 * header, 240 air bits, and nothing to detect a bit error */
		{ BTCTL_FMT_AUX1,  29, 240 },
		{ BTCTL_FMT_AUX1,   1,  16 },
		{ BTCTL_FMT_AUX1,  13, 112 },
		/* DV: 80 bits of voice, then a data field of up to 9 bytes
		 * built like a DM1's, so 80 + 15 groups of ten bits */
		{ BTCTL_FMT_DV,     9, 230 },
		{ BTCTL_FMT_DV,     0, 125 },
		{ BTCTL_FMT_DV,     5, 185 },
		/* the three SCO voice packets, all 240 air bits: 10 bytes
		 * under the 1/3 code, 20 under the 2/3 code, 30 bare */
		{ BTCTL_FMT_HV1,   10, 240 },
		{ BTCTL_FMT_HV2,   20, 240 },
		{ BTCTL_FMT_HV3,   30, 240 },
		/* the eSCO packets, whose length is agreed when the link is
		 * set up rather than carried in the packet */
		{ BTCTL_FMT_EV3,   30, 256 },
		{ BTCTL_FMT_EV3,    1,  24 },
		{ BTCTL_FMT_EV3,   17, 152 },
		{ BTCTL_FMT_EV4,  120, 1470 },
		{ BTCTL_FMT_EV4,    1,  45 },
		{ BTCTL_FMT_EV4,   63, 780 },
		{ BTCTL_FMT_EV5,  180, 1456 },
		{ BTCTL_FMT_EV5,    1,  24 },
		{ BTCTL_FMT_EV5,  111, 904 },
	};
	static packet_t p;
	unsigned i;
	char message[128];

	for (i = 0; i < sizeof(cases)/sizeof(cases[0]); i++)
	{
		unsigned expected = (58 + cases[i].air_bits + 7) / 8;

		encode(&p, cases[i].format, cases[i].data_bytes);
		snprintf(message, sizeof(message),
			"format %d of %d data bytes takes %d air bytes",
			cases[i].format, cases[i].data_bytes, expected);
		mu_assert(p.codec.air_bytes == expected, message);
		snprintf(message, sizeof(message), "format %d, %d data bytes",
			cases[i].format, cases[i].data_bytes);
		decode_and_compare(&p, message);
	}

}

/* Every format the host can name has an air layout, and nothing beyond them
 * does: a format number out of range is refused rather than transmitted as
 * whatever the table holds next to it. */
MU_TEST(test_every_format_is_known)
{
	unsigned i;
	uint8_t air_type;
	char message[128];

	mu_assert(sizeof(formats)/sizeof(formats[0]) == BTCTL_FMT_COUNT,
		"the table covers every format");
	for (i = 0; i < sizeof(formats)/sizeof(formats[0]); i++)
	{
		snprintf(message, sizeof(message), "%s has an air layout",
			formats[i].name);
		mu_assert(bbcodec_tx_format(formats[i].format, &air_type) != NULL,
			message);
	}
	mu_assert(bbcodec_tx_format(BTCTL_FMT_COUNT, &air_type) == NULL,
		"a format beyond the last is refused");
	mu_assert(bbcodec_tx_format(255, &air_type) == NULL,
		"a wildly wrong format is refused");

}

/* HV3 and EV3 share a TYPE code and differ only in what the payload holds,
 * which is what libbtbb has to tell apart when it reads one off the air. The
 * host names the format, so the two are built differently: an HV3 is 30 bytes
 * with nothing to check them, an EV3 any length the link agreed, with a CRC.
 */
MU_TEST(test_hv3_and_ev3_share_a_type_code)
{
	uint8_t hv3_type, ev3_type;
	const bbcodec_types_t *hv3 = bbcodec_tx_format(BTCTL_FMT_HV3, &hv3_type);
	const bbcodec_types_t *ev3 = bbcodec_tx_format(BTCTL_FMT_EV3, &ev3_type);

	mu_assert(hv3_type == ev3_type,
		"HV3 and EV3 go out with the same TYPE code");
	mu_assert(hv3->has_crc == 0, "an HV3 carries no CRC");
	mu_assert(ev3->has_crc != 0, "an EV3 carries a CRC");
	mu_assert(hv3->payload_bytes == 30, "an HV3 is one length");

}

/* What each format will carry, which is what the host checks a length
 * against before the board has to refuse it. A voice packet is one length; an
 * eSCO payload needs at least the one information byte its CRC covers; a
 * format with a payload header can carry an empty data field. */
MU_TEST(test_format_limits)
{
	static const struct {
		unsigned format;
		unsigned min;
		unsigned max;
	} cases[] = {
		{ BTCTL_FMT_DM1,    0,  17 },
		{ BTCTL_FMT_DH5,    0, 339 },
		{ BTCTL_FMT_AUX1,   0,  29 },
		{ BTCTL_FMT_DV,     0,   9 },
		{ BTCTL_FMT_HV1,   10,  10 },
		{ BTCTL_FMT_HV2,   20,  20 },
		{ BTCTL_FMT_HV3,   30,  30 },
		{ BTCTL_FMT_EV3,    1,  30 },
		{ BTCTL_FMT_EV4,    1, 120 },
		{ BTCTL_FMT_EV5,    1, 180 },
	};
	unsigned i;
	char message[128];

	for (i = 0; i < sizeof(cases)/sizeof(cases[0]); i++)
	{
		unsigned min = 0, max = 0;
		uint8_t air_type;
		const bbcodec_types_t *t =
			bbcodec_tx_format(cases[i].format, &air_type);

		bbcodec_tx_format_limits(t, &min, &max);
		snprintf(message, sizeof(message),
			"format %d carries %d to %d data bytes",
			cases[i].format, cases[i].min, cases[i].max);
		mu_assert(min == cases[i].min && max == cases[i].max, message);
	}

}

int main(void)
{
	printf("btbr transmit packet format tests\n");
	MU_RUN(test_every_format_is_known);
	MU_RUN(test_format_limits);
	MU_RUN(test_hv3_and_ev3_share_a_type_code);
	MU_RUN(test_air_size_and_round_trip);
	return mu_report();
}
