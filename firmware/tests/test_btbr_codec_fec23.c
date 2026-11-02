/*
 * Unit tests for how the BR/EDR codec counts the air bits of a 2/3 FEC
 * payload (firmware/btbr/src/codec/codec.c).
 *
 * The encoder works in 10-bit groups and writes 15 air bits per group, so a
 * payload whose byte count is not a multiple of 5 ends in a group that is
 * only part full. The Bluetooth specification pads that group with zeros
 * (Bluetooth Core, Vol 2 Part B 7.4) and puts it on the air whole, which is
 * 15 air bits, not the 12 bits per byte the rest of the payload averages.
 *
 * Transmit, receive and the packet's total air size each have to count that
 * group the same way: the transmitter stops sending at the total, so a total
 * that is short cuts the last group off the air, and a receiver that expects
 * fewer bits than the transmitter sent reads the following payload bytes from
 * the wrong place.
 *
 * codec.c and bitstream_proc.c are pure logic, so the real ones are linked
 * here and whole packets go through them: encoded chunk by chunk the way the
 * transmit task does, then decoded chunk by chunk the way the receive task
 * does, with the air buffer cut at air_bytes the way the radio cuts it and
 * noise after that point.
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

/* ---- the packets these cases send ---- */

/* An air buffer big enough for the longest packet, plus room for the noise
 * after it: a receiver reads on past the end of a packet, and the codec's
 * first chunk is a whole CODEC_RX_CHUNK_SIZE even when the payload is
 * shorter than that. */
#define AIR_BUF_SIZE	512
/* The longest payload the codec counts: a DM5's 2 header bytes, 224 payload
 * bytes and 2 checksum bytes, plus the byte unfec23() writes above a part
 * chunk. */
#define DATA_BUF_SIZE	256

/* Values a real packet carries; nothing here depends on them. */
#define TEST_UAP	0x9e
#define TEST_WHITEN	0x4b
#define TEST_TRAILER	0x5
#define TEST_LT_ADDR	1

/* The codec's table of payload types, which codec.c defines and no header
 * declares; the table is the firmware's own account of what each type
 * carries, so the sizes below come from it. */
extern const bbcodec_types_t bbcodec_acl_types[16];

/*
 * Every 2/3 FEC payload type, with the payload lengths each one can carry.
 *
 * FHS carries no payload header, so its length is fixed at the one the type
 * says and min and max are the same. It is the type paging uses, and the only
 * one this change leaves alone: 18 payload bytes and a 2-byte checksum are 20,
 * a multiple of 5, so its air size was already right. That is why a board
 * running an older image can still be paged by one running this code, and it
 * is pinned here so a change to the FHS size does not quietly take that away.
 */
static const struct {
	const char *name;
	unsigned type;
	unsigned min_payload;
	unsigned max_payload;
} fec23_types[] = {
	{ "FHS", BB_TYPE_FHS, 18, 18 },
	{ "DM1", BB_TYPE_DM1,  0,  17 },
	{ "DM3", BB_TYPE_DM3,  0, 121 },
	{ "DM5", BB_TYPE_DM5,  0, 224 },
};

/* Every type and every length it can carry, as the cases below walk them. */
#define FOR_EVERY_PAYLOAD(i, length) \
	for ((i) = 0; (i) < sizeof(fec23_types) / sizeof(fec23_types[0]); (i)++) \
		for ((length) = fec23_types[i].min_payload; \
		     (length) <= fec23_types[i].max_payload; (length)++)

/* The air bits a 2/3 FEC payload of this many bytes occupies, counted here
 * from the group size rather than from the codec's macro, so the two are
 * independent accounts of the same thing. */
static unsigned reference_air_bits(unsigned bytes)
{
	unsigned groups = (bytes * 8 + 9) / 10;

	return groups * 15;
}

static void fill_pattern(uint8_t *buf, unsigned size, uint32_t seed)
{
	uint32_t x = seed | 1;
	unsigned i;

	for (i = 0; i < size; i++) {
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		buf[i] = (uint8_t)(x >> 7);
	}
}

/*
 * The host-side bytes of one packet: the payload header carrying the length,
 * then the payload, then two bytes the codec fills with the checksum.
 */
static unsigned build_payload(uint8_t *data, unsigned type, unsigned length,
                              uint32_t seed)
{
	const bbcodec_types_t *t = &bbcodec_acl_types[type & 0xf];
	unsigned hdr_len = t->payload_header_bytes;
	unsigned total = hdr_len + length + (t->has_crc ? 2 : 0);

	memset(data, 0, DATA_BUF_SIZE);
	fill_pattern(data + hdr_len, length, seed);

	/* The length sits above the three flow and logical-channel bits, in
	 * one byte or two, as bbcodec_calc_payload_length() reads it. */
	if (hdr_len == 1) {
		data[0] = (uint8_t)(length << 3);
	} else if (hdr_len == 2) {
		data[0] = (uint8_t)(length << 3);
		data[1] = (uint8_t)(length >> 5);
	}
	return total;
}

/*
 * Encode a packet the way tx_task.c does: the header, then chunks until the
 * payload is used up. Returns the codec, so a case can read what it counted.
 */
static void encode_packet(bbcodec_t *codec, uint8_t *air, uint8_t *data,
                          unsigned type)
{
	bbhdr_t hdr;

	memset(&hdr, 0, sizeof(hdr));
	hdr.lt_addr = TEST_LT_ADDR;
	hdr.type = (uint8_t)type;

	memset(air, 0, AIR_BUF_SIZE);
	bbcodec_init(codec, TEST_WHITEN, TEST_UAP, 1, 0);
	bbcodec_encode_header(codec, air, &hdr, TEST_TRAILER, data);
	while (codec->coded_pos < codec->coded_total)
		bbcodec_encode_chunk(codec, air, &hdr, data);
}

/*
 * Decode that air buffer the way rx_task.c does: the header, then chunks
 * until the codec says it has the whole payload. in_length is how many air
 * bytes the receiver has, which here is the whole buffer -- the radio keeps
 * delivering after the packet ends, and the bytes past the packet are noise.
 * in_length is how many air bytes the receiver has. Returns what the last
 * chunk said: BBCODEC_DONE for a whole payload, or BBCODEC_SHORT when the
 * bytes ran out. On BBCODEC_DONE the flags bbcodec_decode_finalize() reports
 * come back in *flags.
 */
static int decode_packet(bbcodec_t *codec, uint8_t *air, unsigned in_length,
                         uint8_t *out, uint16_t *out_size, uint8_t *flags)
{
	bbhdr_t hdr;
	int rc;

	memset(&hdr, 0, sizeof(hdr));
	memset(out, 0, DATA_BUF_SIZE);
	bbcodec_init(codec, TEST_WHITEN, TEST_UAP, 1, 0);
	if (bbcodec_decode_header(codec, &hdr, air) != 0) {
		/* Not mu_assert_fatal: that returns from the function it is in,
		 * which here is a helper rather than the test body. */
		mu_assert(0, "the packet header did not decode");
		return BBCODEC_SHORT;
	}
	do {
		rc = bbcodec_decode_chunk(codec, out, &hdr, air, in_length);
	} while (rc == 0);
	if (rc == BBCODEC_DONE)
		*flags = bbcodec_decode_finalize(codec, out, out_size);
	return rc;
}

/* ---- the air size of a packet ---- */

/*
 * The total the transmitter stops sending at, and the receiver waits for,
 * counts the part group whole. Checked at every payload length of every 2/3
 * FEC type, because which lengths are wrong is the whole point: a total that
 * is 12 bits per byte is right only when the byte count is a multiple of 5.
 */
MU_TEST(test_air_size_counts_the_part_group_whole)
{
	unsigned i, length;

	FOR_EVERY_PAYLOAD(i, length) {
		uint8_t air[AIR_BUF_SIZE], data[DATA_BUF_SIZE];
		bbcodec_t codec;
		unsigned total, want;
		char message[96];

		total = build_payload(data, fec23_types[i].type, length,
			0x1b3d7 + length);
		encode_packet(&codec, air, data, fec23_types[i].type);

		mu_assert_int_eq((int)total, (int)codec.coded_total);
		want = BYTE_ALIGN(58 + reference_air_bits(total));
		snprintf(message, sizeof(message),
			"%s payload %u: air size %u, not %u",
			fec23_types[i].name, length, codec.air_bytes,
			want);
		mu_assert(codec.air_bytes == want, message);
	}
}

/*
 * And the encoder writes exactly that many bits. The air offset it leaves
 * behind is where the next chunk would go, so after the last chunk it is the
 * end of the payload: anything less and the total above promises air the
 * encoder never filled.
 */
MU_TEST(test_the_encoder_fills_the_air_it_asks_for)
{
	unsigned i, length;

	FOR_EVERY_PAYLOAD(i, length) {
		uint8_t air[AIR_BUF_SIZE], data[DATA_BUF_SIZE];
		bbcodec_t codec;
		unsigned total;
		char message[96];

		total = build_payload(data, fec23_types[i].type, length,
			0x5c10b + length);
		encode_packet(&codec, air, data, fec23_types[i].type);

		snprintf(message, sizeof(message),
			"%s payload %u: encoder stopped at bit %u",
			fec23_types[i].name, length, codec.air_off_b);
		mu_assert(codec.air_off_b ==
				58 + reference_air_bits(total), message);
		/* Which is what the air size holds. */
		mu_assert(codec.air_bytes == BYTE_ALIGN(codec.air_off_b),
			message);
	}
}

/* ---- a whole packet, out and back ---- */

/*
 * Every payload length of every 2/3 FEC type survives being encoded and
 * decoded, with the air cut where the transmitter stops. The bytes past that
 * point are noise, as they are on the air, so a decoder that reads the last
 * group from beyond the cut gets a payload that fails its checksum -- which
 * is what the lengths whose byte count is not a multiple of 5 used to do.
 */
MU_TEST(test_every_payload_length_survives_the_round_trip)
{
	unsigned i, length;

	FOR_EVERY_PAYLOAD(i, length) {
		uint8_t air[AIR_BUF_SIZE], data[DATA_BUF_SIZE];
		uint8_t sent[DATA_BUF_SIZE], out[DATA_BUF_SIZE];
		bbcodec_t enc, dec;
		unsigned total;
		uint16_t out_size = 0;
		uint8_t flags = 0;
		char message[96];
		int rc;

		total = build_payload(data, fec23_types[i].type, length,
			0x2a74f + length);
		encode_packet(&enc, air, data, fec23_types[i].type);
		/* What went out, less the checksum: the encoder keeps that of
		 * its own and leaves the caller's buffer alone, so only the
		 * payload is here to compare. The checksum is checked by the
		 * decoder's own answer below. */
		memcpy(sent, data, total - 2);

		/* Noise past the point the transmitter stops at. */
		fill_pattern(air + enc.air_bytes,
			AIR_BUF_SIZE - enc.air_bytes, 0x7e3b1 + length);

		rc = decode_packet(&dec, air, AIR_BUF_SIZE, out, &out_size,
			&flags);

		snprintf(message, sizeof(message),
			"%s payload %u: not decoded (rc %d)",
			fec23_types[i].name, length, rc);
		mu_assert(rc == BBCODEC_DONE, message);

		snprintf(message, sizeof(message),
			"%s payload %u: bad checksum",
			fec23_types[i].name, length);
		mu_assert(flags & (1 << BBPKT_F_GOOD_CRC), message);

		snprintf(message, sizeof(message),
			"%s payload %u: wrong payload size %u",
			fec23_types[i].name, length, out_size);
		mu_assert(out_size == total - 2, message);

		snprintf(message, sizeof(message),
			"%s payload %u: payload changed",
			fec23_types[i].name, length);
		mu_assert(memcmp(sent, out, total - 2) == 0, message);
	}
}

/*
 * A capture one byte short of the packet is reported short, not decoded.
 *
 * This is the other half of the air size: the decoder compares the bytes it
 * has against the bits the next chunk needs, and that need is the group count
 * too. A chunk whose bits are not all there has to say BBCODEC_SHORT and wait
 * for the rest, because the alternative is decoding a group from bytes the
 * radio has not delivered. One byte less than the packet is short for the
 * last chunk at every payload size, so every size is a case here.
 */
MU_TEST(test_a_capture_a_byte_short_is_reported_short)
{
	unsigned i, length;

	FOR_EVERY_PAYLOAD(i, length) {
		uint8_t air[AIR_BUF_SIZE], data[DATA_BUF_SIZE];
		uint8_t out[DATA_BUF_SIZE];
		bbcodec_t enc, dec;
		uint16_t out_size = 0;
		uint8_t flags = 0;
		char message[96];
		int rc;

		build_payload(data, fec23_types[i].type, length,
			0x4d9e3 + length);
		encode_packet(&enc, air, data, fec23_types[i].type);

		rc = decode_packet(&dec, air, enc.air_bytes - 1, out,
			&out_size, &flags);

		snprintf(message, sizeof(message),
			"%s payload %u: %u air bytes of %u decoded as whole",
			fec23_types[i].name, length, enc.air_bytes - 1,
			enc.air_bytes);
		mu_assert(rc == BBCODEC_SHORT, message);

		/* And the whole packet is not short, so the byte that was
		 * missing is the only reason above.
		 *
		 * Except for a packet smaller than one receive chunk: the
		 * receive side decodes a whole CODEC_RX_CHUNK_SIZE first,
		 * before it knows the payload length, so such a packet is
		 * short on its own air size. A real receiver has the bytes
		 * anyway, because the radio goes on delivering after the
		 * packet ends, which is what the round trip above gives it. */
		if (enc.air_bytes <
		    BYTE_ALIGN(58 + BBCODEC_FEC23_AIR_BITS(CODEC_RX_CHUNK_SIZE)))
			continue;
		rc = decode_packet(&dec, air, enc.air_bytes, out, &out_size,
			&flags);
		snprintf(message, sizeof(message),
			"%s payload %u: %u air bytes reported short",
			fec23_types[i].name, length, enc.air_bytes);
		mu_assert(rc == BBCODEC_DONE, message);
	}
}

/* ---- the pad bits of the part group ---- */

/*
 * Leaves a pattern on the stack below the caller, where the encoder's own
 * temporary buffer goes. volatile, so the writes are not dropped as dead.
 */
static void dirty_the_stack(uint8_t fill)
{
	volatile uint8_t scratch[1024];
	unsigned i;

	for (i = 0; i < sizeof(scratch); i++)
		scratch[i] = fill;
}

/*
 * The pad bits of the last group are zeros, not whatever was in memory.
 *
 * The specification pads with zeros, and the pad bits are not whitened, so
 * they reach the air as plain zeros. This decodes the last transmit chunk
 * back out of the air buffer and looks at the bits above the payload in it,
 * with a pattern left on the stack first: the encoder's input is a temporary
 * there, and whitening fills it only as far as the payload goes.
 */
MU_TEST(test_the_pad_bits_are_zeros)
{
	unsigned i, length;

	FOR_EVERY_PAYLOAD(i, length) {
		uint8_t air[AIR_BUF_SIZE], data[DATA_BUF_SIZE];
		uint8_t decoded[DATA_BUF_SIZE];
		bbcodec_t enc;
		unsigned total, whole, last, groups, pad, start, bit;
		char message[96];

		total = build_payload(data, fec23_types[i].type, length,
			0x6f281 + length);
		dirty_the_stack(0xff);
		encode_packet(&enc, air, data, fec23_types[i].type);

		/* Where the last transmit chunk is. The chunks before
		 * it are whole ones, and a whole chunk is a whole
		 * number of groups. */
		whole = (total - 1) / CODEC_TX_CHUNK_SIZE;
		last = total - whole * CODEC_TX_CHUNK_SIZE;
		start = 58 + whole *
			BBCODEC_FEC23_AIR_BITS(CODEC_TX_CHUNK_SIZE);

		groups = (last * 8 + 9) / 10;
		pad = groups * 10 - last * 8;
		if (pad == 0)
			continue;

		/* Decoding the chunk gives back its groups, so the
		 * bits above the payload are the pad bits. unfec23()
		 * ORs its output in and clears only as far as the
		 * payload, so the buffer is cleared here. */
		memset(decoded, 0, sizeof(decoded));
		unfec23(decoded, air, start, last * 8);

		snprintf(message, sizeof(message),
			"%s payload %u: %u pad bits are not zeros",
			fec23_types[i].name, length, pad);
		for (bit = last * 8; bit < groups * 10; bit++)
			mu_assert((decoded[bit / 8] >> (bit % 8) & 1) == 0,
				message);
	}
}

int main(void)
{
	printf("btbr 2/3 FEC payload length tests\n");
	MU_RUN(test_air_size_counts_the_part_group_whole);
	MU_RUN(test_the_encoder_fills_the_air_it_asks_for);
	MU_RUN(test_every_payload_length_survives_the_round_trip);
	MU_RUN(test_a_capture_a_byte_short_is_reported_short);
	MU_RUN(test_the_pad_bits_are_zeros);
	return mu_report();
}
