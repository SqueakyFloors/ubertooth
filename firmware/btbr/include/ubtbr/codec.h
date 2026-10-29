#ifndef __CODEC_H
#define __CODEC_H
#include <stdint.h>
#include <ubtbr/bb.h>

#define MAX_ACL_SIZE 344

/* FIXME: pkt_type interpretation depends on channel's type.
*/
typedef struct bbcodec_types_s {
	uint8_t nslots;		// Slot occupancy
	uint8_t payload_header_bytes;	// Payload header bytes
	uint16_t payload_bytes;	// Payload bytes len
	uint8_t has_fec23;	// Payload is fec23 encoded
	uint8_t has_crc;	// CRC is applied on payload
} bbcodec_types_t;

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

#define BBCODEC_DONE  1
#define BBCODEC_SHORT 2

/* Codec Interface */
void bbcodec_encode_header(bbcodec_t *codec, uint8_t *air_data, bbhdr_t *in_hdr, uint8_t trailer, uint8_t *in_data);
void bbcodec_encode_chunk(bbcodec_t *codec, uint8_t *air_data, bbhdr_t *in_hdr, uint8_t *in_data);

int bbcodec_decode_header(bbcodec_t *codec, bbhdr_t *out_hdr, uint8_t *in_data);
int bbcodec_decode_chunk(bbcodec_t *codec, uint8_t *out, bbhdr_t *in_hdr, uint8_t *in_data, unsigned in_length);
uint8_t bbcodec_decode_finalize(bbcodec_t *codec, uint8_t *out, uint16_t *out_size);
#endif
