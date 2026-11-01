/* Bluetooth codec
 *
 * Copyright 2020 Etienne Helluy-Lafont, Univ. Lille, CNRS.
 *
 * This file is part of Project Ubertooth.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */
#include <stdlib.h>
#include <string.h>
#include <ubtbr/cfg.h>
#include <ubtbr/defines.h>
#include <ubtbr/bb.h>
#include <ubtbr/codec.h>

#include <ubtbr/btphy.h>

const bbcodec_types_t bbcodec_acl_types[16] = {
/* 				ns   	hlen 	plen 	fec		crc	voice	*/
	[BB_TYPE_NULL]	= {	1,	0,	0,	BB_FEC_NONE,	0,	0},
	[BB_TYPE_POLL]	= {	1,	0,	0,	BB_FEC_NONE,	0,	0},
	[BB_TYPE_FHS]	= {	1,	0,	18,	BB_FEC_23,	1,	0},
	[BB_TYPE_DM1]	= {	1,	1,	17,	BB_FEC_23,	1,	0},
	[BB_TYPE_DH1]	= {	1,	1,	27,	BB_FEC_NONE,	1,	0},
	[BB_TYPE_DM3]	= {	3,	2,	121,	BB_FEC_23,	1,	0},
	[BB_TYPE_DH3]	= {	3,	2,	183,	BB_FEC_NONE,	1,	0},
	[BB_TYPE_DM5]	= {	5,	2,	224,	BB_FEC_23,	1,	0},
	[BB_TYPE_DH5]	= {	5,	2,	339,	BB_FEC_NONE,	1,	0}
};

/* The formats a synchronous or an unprotected logical transport uses, which
 * the asynchronous table above has no entry for: the lengths are Bluetooth
 * Core, Vol 2 Part B, Table 6.5 and Table 6.6.
 *
 * These are transmit-only. A TYPE code does not say which format it is -- a
 * received packet of code 7 is an HV3 on a synchronous link and an EV3 on an
 * enhanced one -- and this firmware has no synchronous link to tell it which,
 * so nothing decodes one. On transmit the caller has already said.
 */
/* 					ns  hlen plen	fec		crc voice */
/* AUX1 is a DH1 with no CRC, so a bit error in it is not detected */
static const bbcodec_types_t bbcodec_type_aux1 =
	{	1,  1,	29,	BB_FEC_NONE,	0,  0 };
/* 10 voice bytes, then a data field of up to 9 bytes built like a DM1's */
static const bbcodec_types_t bbcodec_type_dv =
	{	1,  1,	9,	BB_FEC_23,	1,  10 };
static const bbcodec_types_t bbcodec_type_hv1 =
	{	1,  0,	10,	BB_FEC_13,	0,  0 };
static const bbcodec_types_t bbcodec_type_hv2 =
	{	1,  0,	20,	BB_FEC_23,	0,  0 };
static const bbcodec_types_t bbcodec_type_hv3 =
	{	1,  0,	30,	BB_FEC_NONE,	0,  0 };
/* EV3 shares HV3's TYPE code and EV4 and EV5 have codes of their own. Each
 * carries a CRC and a length agreed when the link was set up, so the caller's
 * length stands rather than the maximum here. */
static const bbcodec_types_t bbcodec_type_ev3 =
	{	1,  0,	30,	BB_FEC_NONE,	1,  0 };
static const bbcodec_types_t bbcodec_type_ev4 =
	{	3,  0,	120,	BB_FEC_23,	1,  0 };
static const bbcodec_types_t bbcodec_type_ev5 =
	{	3,  0,	180,	BB_FEC_NONE,	1,  0 };

/* The formats the host can ask to transmit, in the order of
 * btctl_tx_format_t, each with the TYPE code it goes out with. The
 * asynchronous ones are the entries above rather than copies of them, so the
 * two descriptions of a DM1 cannot drift apart.
 */
static const struct {
	uint8_t air_type;
	const bbcodec_types_t *t;
} bbcodec_tx_formats[BTCTL_FMT_COUNT] = {
	[BTCTL_FMT_DM1]  = { BB_TYPE_DM1,  &bbcodec_acl_types[BB_TYPE_DM1]  },
	[BTCTL_FMT_DH1]  = { BB_TYPE_DH1,  &bbcodec_acl_types[BB_TYPE_DH1]  },
	[BTCTL_FMT_DM3]  = { BB_TYPE_DM3,  &bbcodec_acl_types[BB_TYPE_DM3]  },
	[BTCTL_FMT_DH3]  = { BB_TYPE_DH3,  &bbcodec_acl_types[BB_TYPE_DH3]  },
	[BTCTL_FMT_DM5]  = { BB_TYPE_DM5,  &bbcodec_acl_types[BB_TYPE_DM5]  },
	[BTCTL_FMT_DH5]  = { BB_TYPE_DH5,  &bbcodec_acl_types[BB_TYPE_DH5]  },
	[BTCTL_FMT_AUX1] = { BB_TYPE_AUX1, &bbcodec_type_aux1 },
	[BTCTL_FMT_DV]   = { BB_TYPE_DV,   &bbcodec_type_dv   },
	[BTCTL_FMT_HV1]  = { BB_TYPE_HV1,  &bbcodec_type_hv1  },
	[BTCTL_FMT_HV2]  = { BB_TYPE_HV2,  &bbcodec_type_hv2  },
	[BTCTL_FMT_HV3]  = { BB_TYPE_HV3,  &bbcodec_type_hv3  },
	[BTCTL_FMT_EV3]  = { BB_TYPE_HV3,  &bbcodec_type_ev3  },
	[BTCTL_FMT_EV4]  = { BB_TYPE_EV4,  &bbcodec_type_ev4  },
	[BTCTL_FMT_EV5]  = { BB_TYPE_EV5,  &bbcodec_type_ev5  },
};

const bbcodec_types_t *bbcodec_tx_format(unsigned format, uint8_t *air_type)
{
	if (format >= BTCTL_FMT_COUNT)
		return NULL;
	/* A hole left by a format number this table does not fill */
	if (bbcodec_tx_formats[format].t == NULL)
		return NULL;
	*air_type = bbcodec_tx_formats[format].air_type;

	return bbcodec_tx_formats[format].t;
}

void bbcodec_tx_format_limits(const bbcodec_types_t *t,
		unsigned *min_data, unsigned *max_data)
{
	*max_data = t->payload_bytes;
	if (t->payload_header_bytes != 0)
	{
		/* The length rides in the payload header, so an empty data
		 * field is a packet like any other. */
		*min_data = 0;
	}
	else if (t->has_crc)
	{
		/* An eSCO payload: any length the type carries, down to the
		 * one information byte the CRC needs something to cover. */
		*min_data = 1;
	}
	else
	{
		/* A voice payload is the one length its type always has. */
		*min_data = *max_data;
	}
}

/* Unwhiten n bits from packed input into packed output. 
 * update whiten seed in packet.
 */
static inline void bbcodec_unwhiten(uint8_t *whiten_state, uint8_t *out, uint8_t *in, unsigned bit_count )
{
	whiten(out, in, bit_count, whiten_state);
}

/* Payload must contain at least two decoded bytes */
static void bbcodec_calc_payload_length(bbcodec_t *codec, uint8_t *payload)
{
	const bbcodec_types_t *t = codec->t;
	unsigned payload_bits, hdr_len;
	/* The data field starts past the voice bytes, which carry no length */
	uint8_t *data = payload ? payload + codec->data_offset : NULL;

	if (t->payload_bytes != 0 && payload == NULL)
		DIE("No payload for type %p\n", codec->t);

	hdr_len = t->payload_header_bytes;

	if (codec->rx_raw)
	{
		codec->payload_length = t->payload_bytes;
	}
	else
	{
		switch(hdr_len)
		{
		case 1:
			codec->payload_length = data[0]>>3;
			break;
		case 2:
			codec->payload_length = 0x3ff & ((data[0]|(data[1]<<8))>>3);
			break;
		default: // 0
			/* A length nothing in the packet carries: the one the
			 * caller asked for, or the one the type always has. */
			codec->payload_length = codec->fixed_length ?
				codec->fixed_length : t->payload_bytes;
			break;
		}
		if (codec->payload_length > t->payload_bytes)
		{
			BB_DEBUG("(Bad size %d for type %d)", codec->payload_length,
				t->payload_bytes);
			codec->payload_length = 0;
		}
	}
	codec->coded_total = (hdr_len + codec->payload_length);
	if (t->has_crc)
		codec->coded_total += 2;
	payload_bits = bbcodec_fec_air_bits(t->fec, codec->coded_total)
		+ 8*t->voice_bytes;
	codec->air_bytes = BYTE_ALIGN(58+payload_bits);
}

/* Set codec type according to header type
 * Decode payload length in payload_header if any */
static void bbcodec_set_payload_type(bbcodec_t *codec, unsigned type)
{
	/* FIXME: only acl supported */
	codec->t = &bbcodec_acl_types[type&0xf];

	if (codec->t->nslots == 0)
	{
		cprintf("(Nyi plt %d)", type);
		/* Fallback to null.. */
		codec->t = &bbcodec_acl_types[0];
	}
}

int bbcodec_decode_header(bbcodec_t *codec, bbhdr_t *out_hdr, uint8_t *in_data)
{
	int be;
	uint8_t hdr_bytes[3];
	uint32_t h32, hdr_data;
	uint8_t hec;

	/* Unfec13 header */
	be = unfec13_hdr(hdr_bytes, in_data);
	if (be>3)
	{
		BB_DEBUG("(hdr be: %d)", be);
		return -1;
	}

	/* Unwhiten header */
	if (codec->use_whiten)
		bbcodec_unwhiten(&codec->whiten_state, hdr_bytes, hdr_bytes, 18);

	/* Decode header */
	h32 = hdr_bytes[0]|((uint32_t)hdr_bytes[1]<<8)|((uint32_t)hdr_bytes[2]<<16);
	hdr_data = h32&0x3ff;
	out_hdr->hec =(h32>>10);
	out_hdr->lt_addr = hdr_data & 7;
	out_hdr->type   = (hdr_data>>3) & 0xf;
	out_hdr->flags  = (hdr_data>>7) & 7;

	/* Check the HEC. All eight bits have to match.
	 *
	 * This is the only check between the access code, which matches on the
	 * LAP alone, and the payload being decoded and the packet reported. The
	 * HEC is computed from the piconet's UAP, so for a given header no two
	 * UAPs lead to the same HEC, and comparing fewer bits gives that up:
	 * the HEC is linear in the UAP, so there is one UAP whose HEC for every
	 * header is this one's but for the top bit, and dropping that bit
	 * accepts every header of that piconet which gets past the access code.
	 * Dropping it also doubles the chance that a header read out of noise
	 * passes, from 1 in 256 to 1 in 128.
	 *
	 * Every stage above keeps the eighth bit: the 1/3 FEC decode returns
	 * all 18 header bits, and the unwhitening is given the whole 18, the
	 * HEC included. */
	hec = hec_compute(hdr_data, codec->uap);
	if (hec != out_hdr->hec)
	{
		BB_DEBUG("(heccal %x, pkt %x)", hec, out_hdr->hec);
		return -1;
	}
	codec->air_off_b = 58;

	bbcodec_set_payload_type(codec, out_hdr->type);

	return 0;
}

/* encode pkt's header in given buffer
 * data: packet buffer, must be zeroed by caller
 */
void bbcodec_encode_header(bbcodec_t *codec, uint8_t *air_data, bbhdr_t *in_hdr, uint8_t trailer, uint8_t *in_data)
{
	uint32_t h32;
	uint8_t hdr_bytes[3];
	// Write 4bits sw_trailer + 18*3 bits header in air_data
	
	/* encode the header */
	h32 = 7 & in_hdr->lt_addr;
	h32 |= (uint32_t)(0xf & in_hdr->type) << 3;
	h32 |= (uint32_t)(0x7 & in_hdr->flags) << 7;
	h32 |= (uint32_t)hec_compute(h32, codec->uap) << 10;

	hdr_bytes[0] = 0xff & h32;
	hdr_bytes[1] = 0xff & (h32>>8);
	hdr_bytes[2] = 0xff & (h32>>16);

	/* whiten the header */
	if (codec->use_whiten)
		bbcodec_unwhiten(&codec->whiten_state, hdr_bytes, hdr_bytes, 18);

	/* Write trailer of syncword in the buffer */
	air_data[0] = trailer;

	/* fec13 & write the header at position 4 */
	fec13(air_data, hdr_bytes, 4, 18);

	/* encoder output position */
	codec->air_off_b = 58;

	/* Configure codec type, unless the caller chose the format itself
	 * (bbcodec_set_tx_format): a TYPE code alone does not say which
	 * format a synchronous packet is. */
	if (codec->t == NULL)
		bbcodec_set_payload_type(codec, in_hdr->type);

	/* Set codec type / payload length */
	bbcodec_calc_payload_length(codec, in_data);

	/* A DV packet's voice bytes come before its data field and carry
	 * neither a length nor a CRC. They are whitened like the rest of the
	 * payload, so they go out here and the data field's chunks carry on
	 * from the whitening state and the air position they leave behind. */
	if (codec->t->voice_bytes)
	{
		uint8_t voice[BB_VOICE_BYTES_MAX];

		if (codec->t->voice_bytes > sizeof(voice))
			DIE("voice field of %d bytes\n", codec->t->voice_bytes);
		if (codec->use_whiten)
			bbcodec_unwhiten(&codec->whiten_state, voice, in_data,
				codec->t->voice_bytes*8);
		else
			memcpy(voice, in_data, codec->t->voice_bytes);
		null_encode(air_data+(codec->air_off_b>>3), voice,
			codec->air_off_b&7, codec->t->voice_bytes);
		codec->air_off_b += 8*codec->t->voice_bytes;
	}
}

void bbcodec_encode_chunk(bbcodec_t *codec, uint8_t *air_data, bbhdr_t *in_hdr, uint8_t *in_data)
{
	const bbcodec_types_t *t = codec->t;
	uint8_t tmp[CODEC_TX_CHUNK_SIZE+1];
	unsigned byte_left, byte_num, body_total, body_num, i, out_bitlen;

	/* The data field, the only part a length, a CRC or error correction
	 * applies to, starts past any voice bytes. */
	uint8_t *data = in_data + codec->data_offset;

	/* total encode bytes left including crc */
	byte_left = codec->coded_total - codec->coded_pos;

	/* nothing to do */
	if (byte_left == 0)
		return;

	/* Num bytes we're going to encode */
	byte_num = MIN(byte_left, CODEC_TX_CHUNK_SIZE);

	/* How much of the packet is the caller's bytes rather than the CRC,
	 * and how much of that falls in this chunk. The caller's buffer holds
	 * the payload and no more, so the CRC is gathered here instead of
	 * being written back into it: writing it there put two bytes past the
	 * end of the message the host framed to its payload. */
	body_total = codec->coded_total - (t->has_crc ? 2 : 0);
	body_num = codec->coded_pos < body_total ?
		MIN(byte_num, body_total - codec->coded_pos) : 0;

	memcpy(tmp, data + codec->coded_pos, body_num);
	if (!codec->rx_raw && t->has_crc)
	{
		if (body_num != 0)
			codec->crc_state = crc_compute(tmp, body_num,
				codec->crc_state);
		/* The CRC goes out low byte first, once every byte it covers
		 * has gone through crc_compute() above. */
		for (i = body_num; i < byte_num; i++)
			tmp[i] = 0xff & (codec->crc_state >>
				(8*(codec->coded_pos + i - body_total)));
	}

	/* whiten the encode bytes */
	if (codec->use_whiten)
		bbcodec_unwhiten(&codec->whiten_state, tmp, tmp, byte_num*8);

	/* Encode data in the air buffer */
	if (t->fec == BB_FEC_13)
	{
		/* Rate 1/3 is only the HV1 payload, ten bytes, so one chunk
		 * covers it; fec13() wants an even bit offset, which 58 and
		 * every multiple of 24 above it are. */
		out_bitlen = byte_num*8*3;
		fec13(air_data+(codec->air_off_b>>3), tmp,
			codec->air_off_b&7, byte_num*8);
	}
	else if (t->fec == BB_FEC_23)
	{
		out_bitlen = BBCODEC_FEC23_AIR_BITS(byte_num);

		/* A chunk that does not fill its last 10-bit group is padded
		 * with zeros (Bluetooth Core, Vol 2 Part B 7.4). Only a byte
		 * count that is not a multiple of 5 has such a group, and its
		 * padding reaches at most 8 bits past the chunk -- the one
		 * byte above it. Whitening wrote the chunk itself and no more,
		 * so without this the pad bits are whatever the stack left in
		 * the temporary. The chunk size is a multiple of 5, so a chunk
		 * that needs padding is shorter than that and the byte above
		 * it is inside the temporary. */
		if (byte_num % 5 != 0)
			tmp[byte_num] = 0;

		fec23(air_data+(codec->air_off_b>>3), tmp, codec->air_off_b&7, byte_num*8);
	}
	else
	{
		null_encode(air_data+(codec->air_off_b>>3), tmp, codec->air_off_b&7, byte_num);
		out_bitlen = byte_num*8;
	}

	/* Update input position */
	codec->coded_pos += byte_num;

	/* Update output position */
	codec->air_off_b += out_bitlen;
}

static void bbcodec_decode_update_crc(bbcodec_t *codec, const uint8_t *data)
{
	const uint8_t *crc_data = data+codec->crc_pos;
	unsigned crc_num = MIN(codec->coded_total-2,codec->coded_pos)-codec->crc_pos;

	if (crc_num)
	{
		codec->crc_state = crc_compute(crc_data, crc_num, codec->crc_state);
		codec->crc_pos += crc_num;
	}
}

/* Decode 10 host bytes */
int bbcodec_decode_chunk(bbcodec_t *codec, uint8_t *out, bbhdr_t *in_hdr, uint8_t *in_data, unsigned in_length)
{
	const bbcodec_types_t *t = codec->t;
	int bit_av, in_bitlen, first_chunk = codec->coded_pos == 0;
	uint8_t *outp;
	unsigned byte_num;

	/* Check if there is a payload to decode */
	if (t->payload_bytes == 0)
		return BBCODEC_DONE;

	/* First chunk: we do not know coded position,
	 * Just do one full chunk */
	if (first_chunk)
	{
		
		byte_num = CODEC_RX_CHUNK_SIZE;
		outp = out;
	}
	else{
		/* FIXME: hacky assert that payload length was decoded */
		if(codec->coded_total == 0)
			DIE("dec_nc: unk pl len\n");
		byte_num = MIN(CODEC_RX_CHUNK_SIZE,codec->coded_total - codec->coded_pos);
		outp = out+codec->coded_pos;
	}

	if (byte_num == 0)
		return BBCODEC_DONE;

	bit_av = in_length*8 - codec->air_off_b;
	if (bit_av < 0)
		DIE("dec_nc: error av %d of %d\n", in_length, codec->air_off_b);

	/* Rate 1/3 is not reached: it covers a voice payload, which only the
	 * transmit side has a format for. */
	in_bitlen = bbcodec_fec_air_bits(t->fec, byte_num);
	if (bit_av < in_bitlen)
		return BBCODEC_SHORT;
	if (t->fec == BB_FEC_23)
	{
		/* TODO: return unfec23 error for pkt->flags ?
		 * For now, we're just relying on CRC to check validity of message. */
		unfec23(outp, in_data, codec->air_off_b, 8*byte_num);
	}
	else
	{
		null_decode(outp, in_data, codec->air_off_b, byte_num);
	}
	if (codec->use_whiten)
		bbcodec_unwhiten(&codec->whiten_state, outp, outp, byte_num*8);

	codec->air_off_b += in_bitlen;
	codec->coded_pos += byte_num;

	if (first_chunk)
	{
		/* Read payload length */
		bbcodec_calc_payload_length(codec, out);
	}
	/* update crc */
	if (!codec->rx_raw && t->has_crc)
	{
		bbcodec_decode_update_crc(codec, out);
	}
	if (codec->coded_pos >= codec->coded_total)
	{
		return BBCODEC_DONE;
	}
	return 0;
}

/* Finalize decoding of the packet type,
 * must be called after all chunks are received
 */
uint8_t bbcodec_decode_finalize(bbcodec_t *codec, uint8_t *out, uint16_t *out_size)
{
	uint16_t crc;
	uint8_t *outp;
	uint8_t rc = 0;
	const bbcodec_types_t *t = codec->t;

	/* Check if there is a payload to decode */
	if (codec->coded_pos < codec->coded_total)
	{
		DIE("Pkt not decoded? %d:%d\n",
			codec->coded_pos, codec->coded_total);
	}
	if (!codec->rx_raw && t->has_crc)
	{
		/* Verify crc */
		crc = out[codec->coded_total-2]|(out[codec->coded_total-1]<<8);
		if (crc!=codec->crc_state)
		{
			BB_DEBUG("(bad crc: cal=%x, crc=%x)", codec->crc_state, crc);
		}
		else{
			rc |= 1<<BBPKT_F_GOOD_CRC;
		}
		*out_size = codec->coded_total-2;
	}
	else
	{
		*out_size = codec->coded_total;
	}

	return rc;
}
