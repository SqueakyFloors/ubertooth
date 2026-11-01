#ifndef __CODEC_H
#define __CODEC_H
#include <stdint.h>
#include <ubtbr/bb.h>

#define MAX_ACL_SIZE 344

/* The error correction a payload carries (Bluetooth Core, Vol 2 Part B 7.4).
 * One payload has one of these, so they are a choice and not three flags. */
typedef enum bbcodec_fec_e {
	BB_FEC_NONE	= 0,
	/* each bit sent three times: the HV1 payload and every packet header */
	BB_FEC_13	= 1,
	/* 10 bits in, 15 out: DM, FHS, HV2 and EV4 payloads */
	BB_FEC_23	= 2,
} bbcodec_fec_t;

/* FIXME: pkt_type interpretation depends on channel's type.
*/
typedef struct bbcodec_types_s {
	uint8_t nslots;		// Slot occupancy
	uint8_t payload_header_bytes;	// Payload header bytes
	uint16_t payload_bytes;	// Payload bytes len
	uint8_t fec;		// bbcodec_fec_t over the data field
	uint8_t has_crc;	// CRC is applied on payload
	/* Bytes at the front of the payload that carry neither a length nor a
	 * CRC and are whitened but not error-corrected: the 10 voice bytes of
	 * a DV packet, whose data field follows them (Vol 2 Part B 6.5.4.4).
	 * Zero for every other format. */
	uint8_t voice_bytes;
} bbcodec_types_t;

/* The longest voice field any format has, which is the DV packet's ten bytes
 * (Vol 2 Part B 6.5.4.4). The transmit side whitens it in one piece, so this
 * is the size of the buffer it does that in. */
#define BB_VOICE_BYTES_MAX 10

typedef struct bbcodec_s {
	// common params 
	uint8_t uap;	
	int use_whiten;
	int rx_raw;
	uint8_t whiten_state;
	uint16_t crc_state;
	uint16_t crc_pos;
	// codec type
	const bbcodec_types_t *t;

	/* packet size */
	unsigned payload_length; // length of payload from spec or payload's header
	unsigned air_bytes;	// total bytes required for rx/tx, incl. (58bits header)+encoded(payload)
	unsigned coded_total;	// total payload bytes including payload_header, payload_daya, crc

	/* How many data bytes to transmit for a format that carries no length
	 * of its own, where the length is not the one the type always has: an
	 * eSCO payload's length is agreed when the link is set up, so the
	 * caller is the only one that knows it (Vol 2 Part B 6.5.3). Zero
	 * leaves the type's own length in force, which is what receive and
	 * every other transmit path want. */
	unsigned fixed_length;
	/* Where the data field starts in the caller's buffer, past the voice
	 * bytes above. The length, the CRC and the error correction are all
	 * about the data field, so they all count from here. */
	unsigned data_offset;

	// decoding state
	unsigned air_off_b;	// air bit offset (dec: in, enc: out)
	unsigned coded_pos;	// coded payload position
} bbcodec_t;

static inline void bbcodec_init(bbcodec_t *codec,
		uint32_t whiten_init,
		uint8_t uap,
		int use_whiten,
		int rx_raw)
{
	codec->uap = uap;
	codec->use_whiten = use_whiten;
	codec->rx_raw = rx_raw;
	codec->crc_state = reverse8(uap)<<8;
	codec->crc_pos = 0;
	codec->whiten_state = whiten_init;
	codec->t = NULL;
	codec->payload_length = 0;
	codec->air_off_b = 0;
	codec->air_bytes = 0;
	codec->coded_total = 0;
	codec->coded_pos = 0;
	codec->fixed_length = 0;
	codec->data_offset = 0;
}
/* Decode 10 host bytes at the time */
#define CODEC_RX_CHUNK_SIZE 10
/* Encode 80 host bytes at the time */
#define CODEC_TX_CHUNK_SIZE 80

/* How many air bits 2/3 FEC puts out for nbytes host bytes.
 *
 * The encoder takes 10 input bits at a time and writes 15 air bits, and a
 * payload that is not a whole number of 10-bit groups is padded with zeros to
 * the next group before encoding (Bluetooth Core, Vol 2 Part B 7.4). So the
 * last group of a byte count that is not a multiple of 5 occupies a whole 15
 * air bits, not the 12 bits its 8 input bits would suggest. Transmit, receive
 * and the packet's total air size all have to count it the same way, or one of
 * them cuts the last group short.
 */
#define BBCODEC_FEC23_AIR_BITS(nbytes)	((((nbytes)*8u + 9u) / 10u) * 15u)

/* Both chunk sizes above have to be whole numbers of 10-bit groups, which for
 * a byte count means a multiple of 5. Two things rest on that: only the last
 * chunk of a payload can end in a part group, so the chunked air size adds up
 * to the whole payload's; and the transmit side zeroes the pad byte above a
 * part chunk, which is inside its chunk-sized buffer only while a chunk that
 * needs padding is shorter than the buffer. */
_Static_assert(CODEC_TX_CHUNK_SIZE % 5 == 0,
	"a transmit chunk must be a whole number of 10-bit groups");
_Static_assert(CODEC_RX_CHUNK_SIZE % 5 == 0,
	"a receive chunk must be a whole number of 10-bit groups");

/* Transmit the next packet in the given format rather than in the one its
 * TYPE code means on an asynchronous link, with data_bytes bytes of data
 * where the format carries no length of its own (zero where it does). Set
 * between bbcodec_init() and bbcodec_encode_header(). */
static inline void bbcodec_set_tx_format(bbcodec_t *codec,
		const bbcodec_types_t *t, unsigned data_bytes)
{
	codec->t = t;
	codec->data_offset = t->voice_bytes;
	codec->fixed_length = data_bytes;
}

/* How many air bits nbytes host bytes occupy under the given correction. */
static inline unsigned bbcodec_fec_air_bits(unsigned fec, unsigned nbytes)
{
	switch (fec)
	{
	case BB_FEC_13:
		return nbytes*8*3;
	case BB_FEC_23:
		return BBCODEC_FEC23_AIR_BITS(nbytes);
	default:
		return nbytes*8;
	}
}

#define BBCODEC_DONE  1
#define BBCODEC_SHORT 2

/* The air layout of one of the formats the host can ask to transmit, and the
 * TYPE code that format goes out with. Returns NULL for a format number this
 * firmware does not know. */
const bbcodec_types_t *bbcodec_tx_format(unsigned format, uint8_t *air_type);

/* The most data bytes the given format carries after its payload header, and
 * the fewest. A format whose length is fixed reports the same for both. */
void bbcodec_tx_format_limits(const bbcodec_types_t *t,
		unsigned *min_data, unsigned *max_data);

/* Codec Interface */
void bbcodec_encode_header(bbcodec_t *codec, uint8_t *air_data, bbhdr_t *in_hdr, uint8_t trailer, uint8_t *in_data);
void bbcodec_encode_chunk(bbcodec_t *codec, uint8_t *air_data, bbhdr_t *in_hdr, uint8_t *in_data);

int bbcodec_decode_header(bbcodec_t *codec, bbhdr_t *out_hdr, uint8_t *in_data);
int bbcodec_decode_chunk(bbcodec_t *codec, uint8_t *out, bbhdr_t *in_hdr, uint8_t *in_data, unsigned in_length);
uint8_t bbcodec_decode_finalize(bbcodec_t *codec, uint8_t *out, uint16_t *out_size);
#endif
