/*
 * Unit tests for the Header Error Check (HEC) on a received BR/EDR packet
 * header (firmware/btbr/src/codec/codec.c, bbcodec_decode_header()).
 *
 * A BR/EDR header is 10 bits of header information plus an 8-bit HEC
 * (Bluetooth Core, Vol 2 Part B 6.4). The HEC is computed over those 10 bits
 * starting from the piconet's UAP, so it is the only thing that says a header
 * belongs to this piconet: the receiver matched an access code, decoded the
 * 1/3 FEC and unwhitened the header, and nothing else is checked before the
 * payload is decoded and the packet reported.
 *
 * So these cases hold two things. A header this piconet sent matches on all
 * eight bits, whitening on or off and whatever the UAP is -- the path from
 * the air to the comparison keeps every one of them. And a header whose HEC
 * differs in any single bit is refused, the top bit included: one that passed
 * would become a packet line with a wrong type, lt_addr or flags.
 *
 * codec.c and bitstream_proc.c are pure logic, so the real ones are linked
 * here. The air buffer is built here rather than by the encoder, so a case
 * can put a HEC of its choosing on the air; the encoder's own output is
 * checked against it, which keeps the two descriptions of a header honest.
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
 * stubs-btbr/ubtbr/debug.h. The codec calls DIE() when a packet is handed to
 * it in a state it does not expect, which in a test means the test asked for
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

/* Room for the 4-bit trailer and the 54 air bits of the header, and for the
 * byte unfec13_hdr() reads past the last of them. */
#define AIR_BUF_SIZE	16

/* One packet's worth of payload, which no case here decodes. */
#define DATA_BUF_SIZE	8

/* The trailer that follows the access code. bbcodec_decode_header() does not
 * read it; it is here so the air buffer looks like a real one. */
#define TEST_TRAILER	0x5

/* The whitening seed a real receiver takes from the piconet clock. */
#define TEST_WHITEN	0x4b

/* The 10 bits of header information, as a header a receiver would see. */
typedef struct {
	unsigned lt_addr;
	unsigned type;
	unsigned flags;
} hdr_info_t;

/* Enough headers to cover each field's range and a few combinations, rather
 * than all 1024: the HEC is computed over all 10 bits at once, so a bit the
 * comparison dropped would show on any of them. */
static const hdr_info_t test_headers[] = {
	{ 0, BB_TYPE_NULL, 0 },
	{ 1, BB_TYPE_POLL, 0 },
	{ 1, BB_TYPE_FHS,  1 },
	{ 2, BB_TYPE_DM1,  2 },
	{ 3, BB_TYPE_DH1,  4 },
	{ 4, BB_TYPE_DM3,  7 },
	{ 5, BB_TYPE_DH3,  5 },
	{ 6, BB_TYPE_DM5,  3 },
	{ 7, BB_TYPE_DH5,  7 },
	{ 7, BB_TYPE_AUX1, 6 },
};
#define NUM_TEST_HEADERS (sizeof(test_headers)/sizeof(test_headers[0]))

/* UAPs to compute the HEC from. 0x00 and 0xff are the ends of the range, and
 * the HEC register is seeded with the UAP, so a seed of all zeros or all ones
 * is worth having among the arbitrary ones. */
static const uint8_t test_uaps[] = { 0x00, 0x01, 0x0d, 0x41, 0x9e, 0xff };
#define NUM_TEST_UAPS (sizeof(test_uaps)/sizeof(test_uaps[0]))

/* The 10 bits of header information, packed the way the air carries them. */
static uint16_t pack_header_info(const hdr_info_t *info)
{
	return (uint16_t)((info->lt_addr & 7)
		| ((info->type & 0xf) << 3)
		| ((info->flags & 7) << 7));
}

/*
 * Put one header on the air: the 10 bits of header information and the given
 * 8-bit HEC, whitened if the piconet whitens, then 1/3 FEC encoded after the
 * trailer. This is what a transmitter does, with the HEC left to the caller.
 */
static void put_header(uint8_t *air, const hdr_info_t *info, uint8_t hec,
                       int use_whiten)
{
	uint8_t hdr_bytes[3], whiten_state = TEST_WHITEN;
	uint32_t h32 = pack_header_info(info) | ((uint32_t)hec << 10);

	hdr_bytes[0] = 0xff & h32;
	hdr_bytes[1] = 0xff & (h32 >> 8);
	hdr_bytes[2] = 0xff & (h32 >> 16);

	if (use_whiten)
		whiten(hdr_bytes, hdr_bytes, 18, &whiten_state);

	memset(air, 0, AIR_BUF_SIZE);
	air[0] = TEST_TRAILER;
	fec13(air, hdr_bytes, 4, 18);
}

/*
 * Decode that air buffer the way rx_task.c does. Returns what
 * bbcodec_decode_header() returned, with the header it read in *out_hdr.
 */
static int decode_header(uint8_t *air, uint8_t uap, int use_whiten,
                         bbhdr_t *out_hdr)
{
	bbcodec_t codec;

	memset(out_hdr, 0, sizeof(*out_hdr));
	bbcodec_init(&codec, TEST_WHITEN, uap, use_whiten, 0);
	return bbcodec_decode_header(&codec, out_hdr, air);
}

/* ---- a header of this piconet ---- */

/*
 * The HEC a header arrives with reaches the comparison whole. Checked bit by
 * bit rather than through the accept/refuse answer, so a bit lost on the way
 * from the air shows here even if the comparison that follows ignores it.
 */
MU_TEST(test_the_whole_hec_survives_the_air)
{
	unsigned h, u, w;

	for (w = 0; w < 2; w++)
	for (u = 0; u < NUM_TEST_UAPS; u++)
	for (h = 0; h < NUM_TEST_HEADERS; h++) {
		const hdr_info_t *info = &test_headers[h];
		uint8_t air[AIR_BUF_SIZE], hec;
		bbhdr_t got;
		char message[128];

		hec = hec_compute(pack_header_info(info), test_uaps[u]);
		put_header(air, info, hec, (int)w);

		mu_assert(decode_header(air, test_uaps[u], (int)w, &got) == 0,
			"a header of this piconet was refused");

		snprintf(message, sizeof(message),
			"uap %02x whiten %u header %u: hec %02x, not %02x",
			test_uaps[u], w, h, got.hec, hec);
		mu_assert(got.hec == hec, message);

		snprintf(message, sizeof(message),
			"uap %02x whiten %u header %u: header information "
			"came back as %u/%u/%u",
			test_uaps[u], w, h, got.lt_addr, got.type, got.flags);
		mu_assert(got.lt_addr == info->lt_addr
			&& got.type == info->type
			&& got.flags == info->flags, message);
	}
}

/*
 * And the encoder computes the same HEC the receiver expects. The cases above
 * build the air buffer themselves, so without this they could agree with each
 * other and with neither the encoder nor the air.
 */
MU_TEST(test_the_encoder_writes_the_hec_the_receiver_expects)
{
	unsigned h, u;

	for (u = 0; u < NUM_TEST_UAPS; u++)
	for (h = 0; h < NUM_TEST_HEADERS; h++) {
		const hdr_info_t *info = &test_headers[h];
		uint8_t air[AIR_BUF_SIZE], data[DATA_BUF_SIZE];
		uint8_t mine[AIR_BUF_SIZE], hec;
		bbcodec_t codec;
		bbhdr_t hdr;
		char message[96];

		memset(data, 0, sizeof(data));
		memset(&hdr, 0, sizeof(hdr));
		hdr.lt_addr = (uint8_t)info->lt_addr;
		hdr.type = (uint8_t)info->type;
		hdr.flags = (uint8_t)info->flags;

		memset(air, 0, sizeof(air));
		bbcodec_init(&codec, TEST_WHITEN, test_uaps[u], 1, 0);
		bbcodec_encode_header(&codec, air, &hdr, TEST_TRAILER, data);

		hec = hec_compute(pack_header_info(info), test_uaps[u]);
		put_header(mine, info, hec, 1);

		snprintf(message, sizeof(message),
			"uap %02x header %u: the encoder put different bits "
			"on the air", test_uaps[u], h);
		/* The header occupies the trailer's byte and the seven above
		 * it; what the encoder leaves past that is not the header. */
		mu_assert(memcmp(air, mine, 8) == 0, message);
	}
}

/* ---- a header that is not this piconet's ---- */

/*
 * A HEC that differs in one bit is refused, whichever bit it is. The top bit
 * is the point of this case: a comparison that dropped it would accept one
 * header in 128 from another piconet rather than one in 256, and an accepted
 * header is reported as a packet with whatever type, lt_addr and flags the
 * other piconet's header happened to carry.
 */
MU_TEST(test_a_hec_wrong_in_one_bit_is_refused)
{
	unsigned h, u, w, bit;

	for (w = 0; w < 2; w++)
	for (u = 0; u < NUM_TEST_UAPS; u++)
	for (h = 0; h < NUM_TEST_HEADERS; h++)
	for (bit = 0; bit < 8; bit++) {
		const hdr_info_t *info = &test_headers[h];
		uint8_t air[AIR_BUF_SIZE], hec;
		bbhdr_t got;
		char message[128];

		hec = hec_compute(pack_header_info(info), test_uaps[u]);
		put_header(air, info, hec ^ (uint8_t)(1u << bit), (int)w);

		snprintf(message, sizeof(message),
			"uap %02x whiten %u header %u: a hec with bit %u "
			"flipped was accepted", test_uaps[u], w, h, bit);
		mu_assert(decode_header(air, test_uaps[u], (int)w, &got) != 0,
			message);
	}
}

/*
 * The same header read with the wrong UAP is refused. That is the case the
 * HEC is there for: two piconets on one channel, and a packet of the other
 * one matching this one's access code closely enough to be decoded.
 */
MU_TEST(test_a_header_from_another_piconet_is_refused)
{
	unsigned h, u, other;

	for (u = 0; u < NUM_TEST_UAPS; u++)
	for (h = 0; h < NUM_TEST_HEADERS; h++)
	for (other = 0; other < NUM_TEST_UAPS; other++) {
		const hdr_info_t *info = &test_headers[h];
		uint8_t air[AIR_BUF_SIZE], hec;
		bbhdr_t got;
		char message[128];

		if (other == u)
			continue;

		hec = hec_compute(pack_header_info(info), test_uaps[u]);
		put_header(air, info, hec, 1);

		/* Only a UAP that gives this header a different HEC can be
		 * told apart from the right one: an 8-bit check over 10 bits
		 * of header information cannot separate every pair. */
		if (hec_compute(pack_header_info(info), test_uaps[other])
				== hec)
			continue;

		snprintf(message, sizeof(message),
			"header %u sent with uap %02x was accepted as uap "
			"%02x's", h, test_uaps[u], test_uaps[other]);
		mu_assert(decode_header(air, test_uaps[other], 1, &got) != 0,
			message);
	}
}

/*
 * A whitened header read as an unwhitened one, and the other way round, are
 * both refused. The whitening seed comes from the piconet clock, so this is
 * what a receiver whose clock estimate is wrong sees, and the HEC is what
 * stops it reporting the packet.
 */
MU_TEST(test_a_header_unwhitened_the_wrong_way_is_refused)
{
	unsigned h, u, w;

	for (w = 0; w < 2; w++)
	for (u = 0; u < NUM_TEST_UAPS; u++)
	for (h = 0; h < NUM_TEST_HEADERS; h++) {
		const hdr_info_t *info = &test_headers[h];
		uint8_t air[AIR_BUF_SIZE], hec;
		bbhdr_t got;
		char message[128];

		hec = hec_compute(pack_header_info(info), test_uaps[u]);
		put_header(air, info, hec, (int)w);

		/* A whitening word can leave the 18 bits alone, and then
		 * there is nothing for the receiver to get wrong. */
		if (decode_header(air, test_uaps[u], (int)(1 - w), &got) == 0
				&& got.hec == hec)
			continue;

		snprintf(message, sizeof(message),
			"uap %02x header %u: a header sent with whitening %u "
			"was accepted with whitening %u",
			test_uaps[u], h, w, 1 - w);
		mu_assert(decode_header(air, test_uaps[u], (int)(1 - w), &got)
			!= 0, message);
	}
}

int main(void)
{
	printf("btbr packet header HEC tests\n");
	MU_RUN(test_the_whole_hec_survives_the_air);
	MU_RUN(test_the_encoder_writes_the_hec_the_receiver_expects);
	MU_RUN(test_a_hec_wrong_in_one_bit_is_refused);
	MU_RUN(test_a_header_from_another_piconet_is_refused);
	MU_RUN(test_a_header_unwhitened_the_wrong_way_is_refused);
	return mu_report();
}
