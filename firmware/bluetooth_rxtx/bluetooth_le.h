/*
 * Copyright 2012 Dominic Spill
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

#include "ubertooth.h"

#define BTLE_CHANNELS 40
#define ADVERTISING_CHANNELS 3
#define DATA_CHANNELS 37

typedef enum {
    LINK_INACTIVE,
    LINK_LISTENING,
    LINK_CONN_PENDING,
    LINK_CONNECTED,
} link_state_t;

typedef struct _le_state_t {
    u32 access_address;         // Access Address to filter by
    /* Whether the host gave us that access address, as against it being the
     * advertising one every reset puts back. Promiscuous mode discovers an
     * access address unless this says it was handed one. Only
     * UBERTOOTH_SET_ACCESS_ADDRESS sets it, and reset_le() clears it, so it
     * never outlives the address it describes. */
    int access_address_set;
    u16 synch;                  // Access address in CC2400 syncword format
    u16 syncl;                  // lower 16 bits thereof
    u32 crc_init;               // CrcInit: used to calculate CRC
    u32 crc_init_reversed;      // bits-reversed version of the previous
    int crc_verify;             // true to reject packets with bad CRC

    link_state_t link_state;         // current link layer state

    u8 channel_idx;             // current channel index
    u8 channel_increment;       // amount to hop

    u32 conn_epoch;             // reference time for the start of the connection
    u16 volatile interval_timer;// number of intervals remaining before next hop
    u16 conn_interval;          // connection-specific hop interval
    u16 volatile conn_count;    // number of intervals since the start of the connection

    u8 win_size;                // window size (max packets per connection)
    u16 win_offset;             // offset of first window from start of connection

    int update_pending;         // whether a connection update is pending
    u16 update_instant;         // the connection count when the update takes effect
    u16 interval_update;        // the new hop_internal
    u8 win_size_update;         // the new window size
    u16 win_offset_update;      // the new window offset

    int do_follow;              // 1 if follow connections, 0 to only log advertising packets

    u8 target[6];               // target MAC for connection following (byte order reversed)
    u8 target_mask[6];          // mask for MAC connection following (byte order reversed)
    int target_set;             // whether a target has been set (default: false)
    u32 last_packet;            // when was the last packet received
} le_state_t;

// for handling LE channel maps
typedef struct _le_channel_remapping_t {
	int         channel_in_use[37];
	uint8_t     remapping_index[37]; // array of channel indices
	unsigned    total_channels;
} le_channel_remapping_t;

static const u8 whitening[] = {
    1, 1, 1, 1, 0, 1, 0, 1, 0, 1, 0, 0, 0, 0, 1, 0, 1, 1, 0, 1, 1, 1,
    1, 0, 0, 1, 1, 1, 0, 0, 1, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0,
    0, 1, 1, 0, 1, 1, 0, 1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 1, 1, 0, 0, 1,
    0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 1, 1, 0, 1, 0, 0,
    1, 1, 1, 1, 0, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0,
    1, 1, 1, 0, 1, 1, 0, 0, 0, 1, 0, 1, 0, 0, 1, 0, 1
};

static const u8 whitening_index[] = {
    70, 62, 120, 111, 77, 46, 15, 101, 66, 39, 31, 26, 80,
    83, 125, 89, 10, 35, 8, 54, 122, 17, 33, 0, 58, 115, 6,
    94, 86, 49, 52, 20, 40, 27, 84, 90, 63, 112, 47, 102
};

/* The data channels a connection's hop sequence runs over. A connection whose
 * channel map holds all of them reaches any one channel exactly once per trip
 * round them. */
#define LE_HOP_CHANNELS 37

/* The connection interval the specification allows, in its own units of
 * 1.25 ms: 7.5 ms to 4 s. */
#define LE_MIN_CONN_INTERVAL 6
#define LE_MAX_CONN_INTERVAL 3200

/* The hop increment the specification allows. */
#define LE_MIN_HOP_INCREMENT 5
#define LE_MAX_HOP_INCREMENT 16

static const u8 hop_interval_lut[] = {
    0, 1, 19, 25, 28, 15, 31, 16, 14, 33, 26, 27, 34, 20, 8,
    5, 7, 24, 35, 2, 13, 30, 32, 29, 17, 3, 10, 11, 4, 23, 21,
    6, 22, 9, 12, 18, 36,
};

u16 btle_next_hop(le_state_t *le);
uint8_t btle_channel_index(uint16_t channel);
u16 btle_channel_index_to_phys(u8 idx);
u32 btle_calc_crc(u32 crc_init, u8 *data, int len);
u32 btle_reverse_crc(u32 crc, u8 *data, int len);
u32 btle_crcgen_lut(u32 crc_init, u8 *data, int len);
void le_dewhiten(uint8_t *data, unsigned size, unsigned channel);
void le_dewhiten_from(uint8_t *data, unsigned size, unsigned channel,
                      unsigned offset);
void le_parse_channel_map(uint8_t *channel_map, le_channel_remapping_t *remapping);
uint8_t le_map_channel(uint8_t channel_idx, le_channel_remapping_t *remapping);

/* How far into a PDU the table below reaches: 12 words of four bytes. A
 * packet longer than this is dewhitened the rest of the way by
 * le_dewhiten_from(), which needs no table. */
#define LE_WHITENING_WORD_BYTES 48

/* The longest payload a PDU's length field may claim, which is what the
 * capture loops use to tell a length from noise.
 *
 * LE_MAX_PAYLOAD is Bluetooth 4.2 Data Length Extension's 251 bytes, the most
 * any PDU can carry.
 *
 * LE_MAX_PAYLOAD_LEGACY is the most any PDU carried before that: 37, from an
 * advertising PDU's 6-bit length field. A data channel PDU's own legacy
 * maximum is lower, 31 (27 payload bytes and a 4-byte message integrity check
 * when the connection is encrypted), but the looser number is the one to
 * compare against, because a mode following a host-supplied target starts on
 * the advertising access address and sees advertising PDUs. Being 6 bytes
 * loose costs about 48 microseconds of waiting; being tight would drop
 * packets that are really there. */
#define LE_MAX_PAYLOAD 251
#define LE_MAX_PAYLOAD_LEGACY 37

/*
 * Whether a PDU header's length field could belong to a packet at all, rather
 * than being noise the radio synchronized on. A capture loop asks before
 * waiting for the bytes, because waiting is time the next connection event is
 * missed in.
 *
 * `crc_verify` says whether the connection's CRCInit is known. Until it is,
 * there is no CRC to check a packet against, and the empty data PDUs a loop
 * is looking for to get there are short, so a claimed Data Length Extension
 * payload in that window is noise as far as it can tell.
 */
static inline int le_length_is_possible(uint8_t length, int crc_verify)
{
	return length <= (crc_verify ? LE_MAX_PAYLOAD : LE_MAX_PAYLOAD_LEGACY);
}

/*
 * How long a capture loop waits for the rest of a packet it has the header of.
 *
 * The 1M PHY carries one bit per microsecond, so a byte takes 8 microseconds,
 * which is 80 ticks of the 100 ns clock. The bytes a loop is waiting for are
 * therefore due within their own air time, and a wait that outlasts that by
 * the slack below is waiting for bytes the radio has stopped sending.
 *
 * The slack covers the radio and the DMA controller between the last bit on
 * the air and the last byte in memory. It is generous on purpose: giving up
 * too early would drop a packet that was really there, while giving up too
 * late only costs that much more blind time in the rare case where the radio
 * synchronized on noise.
 */
#define LE_1M_BYTE_CLK100NS 80
#define LE_RX_BYTE_WAIT_SLACK_CLK100NS 2000   /* 200 microseconds */

/* The air time of `bytes` on the 1M PHY, plus that slack, in 100 ns ticks. */
static inline uint32_t le_rx_byte_wait_clk100ns(unsigned bytes)
{
	return bytes * LE_1M_BYTE_CLK100NS + LE_RX_BYTE_WAIT_SLACK_CLK100NS;
}

/*
 * One step of a wait timed by timer 0: the 100 ns ticks between two readings
 * of T0TC, which a wait adds up as it polls.
 *
 * It is done this way, rather than by comparing CLK100NS against a deadline,
 * because CLK100NS does not only count up. It is
 * CLK100NS_PER_CLKN * (clkn & 0xfffff) + T0TC, timer 0 resets every
 * CLK100NS_PER_CLKN ticks, and clkn is advanced by the timer's own interrupt
 * handler: between the reset and the handler running, every reading is about
 * one whole clkn tick lower than the one before it. A deadline compared
 * against that would be met in that window, every 312.5 us, and the wait would
 * drop a packet that was arriving normally. rangetest_wait_for_fifo() in
 * cc2400_rangetest.c adds up steps for the same reason.
 *
 * A reading lower than the one before it means the timer reset in between, and
 * how much of that period was left is only known if the period was the usual
 * one. The low-energy modes' clock drift correction makes one period longer
 * (T0MR0 goes to 9373), and a match value set by the last drift fix of one
 * mode can still be in the timer when the next mode starts, so that case is
 * reachable. The reading then says only how far into the new period the wait
 * is, and that is all this counts.
 *
 * Undercounting is the safe direction for a deadline: the wait runs a little
 * longer than its budget rather than ending early on a packet that was still
 * arriving. A gap longer than one period is undercounted for the same reason,
 * because nothing here can tell how many resets it spanned.
 */
static inline uint32_t le_clk100ns_step(uint32_t last, uint32_t now)
{
	if (now >= last)
		return now - last;
	if (last <= CLK100NS_PER_CLKN)
		return CLK100NS_PER_CLKN - last + now;
	return now;
}

static const u32 whitening_word[40][12] = {
	{ 0xc3bcb240, 0x5f4a371f, 0x9a9cf685, 0x44c5d6c1, 0xe1de5920, 0xafa51b8f,
	  0xcd4e7b42, 0x2262eb60, 0xf0ef2c90, 0x57d28dc7, 0x66a73da1, 0x113175b0, },
	{ 0xbcb24089, 0x4a371fc3, 0x9cf6855f, 0xc5d6c19a, 0xde592044, 0xa51b8fe1,
	  0x4e7b42af, 0x62eb60cd, 0xef2c9022, 0xd28dc7f0, 0xa73da157, 0x3175b066, },
	{ 0x3da157d2, 0x75b066a7, 0x96481131, 0x46e3f877, 0x9ed0abe9, 0xbad83353,
	  0xcb240898, 0xa371fc3b, 0xcf6855f4, 0x5d6c19a9, 0xe592044c, 0x51b8fe1d, },
	{ 0x42afa51b, 0x60cd4e7b, 0x902262eb, 0xc7f0ef2c, 0xa157d28d, 0xb066a73d,
	  0x48113175, 0xe3f87796, 0xd0abe946, 0xd833539e, 0x240898ba, 0x71fc3bcb, },
	{ 0x3f877964, 0x0abe946e, 0x833539ed, 0x40898bad, 0x1fc3bcb2, 0x855f4a37,
	  0xc19a9cf6, 0x2044c5d6, 0x8fe1de59, 0x42afa51b, 0x60cd4e7b, 0x902262eb, },
	{ 0x40898bad, 0x1fc3bcb2, 0x855f4a37, 0xc19a9cf6, 0x2044c5d6, 0x8fe1de59,
	  0x42afa51b, 0x60cd4e7b, 0x902262eb, 0xc7f0ef2c, 0xa157d28d, 0xb066a73d, },
	{ 0xc19a9cf6, 0x2044c5d6, 0x8fe1de59, 0x42afa51b, 0x60cd4e7b, 0x902262eb,
	  0xc7f0ef2c, 0xa157d28d, 0xb066a73d, 0x48113175, 0xe3f87796, 0xd0abe946, },
	{ 0xbe946e3f, 0x3539ed0a, 0x898bad83, 0xc3bcb240, 0x5f4a371f, 0x9a9cf685,
	  0x44c5d6c1, 0xe1de5920, 0xafa51b8f, 0xcd4e7b42, 0x2262eb60, 0xf0ef2c90, },
	{ 0x3bcb2408, 0xf4a371fc, 0xa9cf6855, 0x4c5d6c19, 0x1de59204, 0xfa51b8fe,
	  0xd4e7b42a, 0x262eb60c, 0x0ef2c902, 0x7d28dc7f, 0x6a73da15, 0x13175b06, },
	{ 0x44c5d6c1, 0xe1de5920, 0xafa51b8f, 0xcd4e7b42, 0x2262eb60, 0xf0ef2c90,
	  0x57d28dc7, 0x66a73da1, 0x113175b0, 0xf8779648, 0xabe946e3, 0x33539ed0, },
	{ 0xc5d6c19a, 0xde592044, 0xa51b8fe1, 0x4e7b42af, 0x62eb60cd, 0xef2c9022,
	  0xd28dc7f0, 0xa73da157, 0x3175b066, 0x77964811, 0xe946e3f8, 0x539ed0ab, },
	{ 0xbad83353, 0xcb240898, 0xa371fc3b, 0xcf6855f4, 0x5d6c19a9, 0xe592044c,
	  0x51b8fe1d, 0xe7b42afa, 0x2eb60cd4, 0xf2c90226, 0x28dc7f0e, 0x73da157d, },
	{ 0xc7f0ef2c, 0xa157d28d, 0xb066a73d, 0x48113175, 0xe3f87796, 0xd0abe946,
	  0xd833539e, 0x240898ba, 0x71fc3bcb, 0x6855f4a3, 0x6c19a9cf, 0x92044c5d, },
	{ 0xb8fe1de5, 0xb42afa51, 0xb60cd4e7, 0xc902262e, 0xdc7f0ef2, 0xda157d28,
	  0x5b066a73, 0x64811317, 0x6e3f8779, 0xed0abe94, 0xad833539, 0xb240898b, },
	{ 0x39ed0abe, 0x8bad8335, 0xbcb24089, 0x4a371fc3, 0x9cf6855f, 0xc5d6c19a,
	  0xde592044, 0xa51b8fe1, 0x4e7b42af, 0x62eb60cd, 0xef2c9022, 0xd28dc7f0, },
	{ 0x46e3f877, 0x9ed0abe9, 0xbad83353, 0xcb240898, 0xa371fc3b, 0xcf6855f4,
	  0x5d6c19a9, 0xe592044c, 0x51b8fe1d, 0xe7b42afa, 0x2eb60cd4, 0xf2c90226, },
	{ 0x33539ed0, 0x0898bad8, 0xfc3bcb24, 0x55f4a371, 0x19a9cf68, 0x044c5d6c,
	  0xfe1de592, 0x2afa51b8, 0x0cd4e7b4, 0x02262eb6, 0x7f0ef2c9, 0x157d28dc, },
	{ 0x4c5d6c19, 0x1de59204, 0xfa51b8fe, 0xd4e7b42a, 0x262eb60c, 0x0ef2c902,
	  0x7d28dc7f, 0x6a73da15, 0x13175b06, 0x87796481, 0xbe946e3f, 0x3539ed0a, },
	{ 0xcd4e7b42, 0x2262eb60, 0xf0ef2c90, 0x57d28dc7, 0x66a73da1, 0x113175b0,
	  0xf8779648, 0xabe946e3, 0x33539ed0, 0x0898bad8, 0xfc3bcb24, 0x55f4a371, },
	{ 0xb240898b, 0x371fc3bc, 0xf6855f4a, 0xd6c19a9c, 0x592044c5, 0x1b8fe1de,
	  0x7b42afa5, 0xeb60cd4e, 0x2c902262, 0x8dc7f0ef, 0x3da157d2, 0x75b066a7, },
	{ 0xcf6855f4, 0x5d6c19a9, 0xe592044c, 0x51b8fe1d, 0xe7b42afa, 0x2eb60cd4,
	  0xf2c90226, 0x28dc7f0e, 0x73da157d, 0x175b066a, 0x79648113, 0x946e3f87, },
	{ 0xb066a73d, 0x48113175, 0xe3f87796, 0xd0abe946, 0xd833539e, 0x240898ba,
	  0x71fc3bcb, 0x6855f4a3, 0x6c19a9cf, 0x92044c5d, 0xb8fe1de5, 0xb42afa51, },
	{ 0x3175b066, 0x77964811, 0xe946e3f8, 0x539ed0ab, 0x98bad833, 0x3bcb2408,
	  0xf4a371fc, 0xa9cf6855, 0x4c5d6c19, 0x1de59204, 0xfa51b8fe, 0xd4e7b42a, },
	{ 0x4e7b42af, 0x62eb60cd, 0xef2c9022, 0xd28dc7f0, 0xa73da157, 0x3175b066,
	  0x77964811, 0xe946e3f8, 0x539ed0ab, 0x98bad833, 0x3bcb2408, 0xf4a371fc, },
	{ 0xcb240898, 0xa371fc3b, 0xcf6855f4, 0x5d6c19a9, 0xe592044c, 0x51b8fe1d,
	  0xe7b42afa, 0x2eb60cd4, 0xf2c90226, 0x28dc7f0e, 0x73da157d, 0x175b066a, },
	{ 0xb42afa51, 0xb60cd4e7, 0xc902262e, 0xdc7f0ef2, 0xda157d28, 0x5b066a73,
	  0x64811317, 0x6e3f8779, 0xed0abe94, 0xad833539, 0xb240898b, 0x371fc3bc, },
	{ 0x3539ed0a, 0x898bad83, 0xc3bcb240, 0x5f4a371f, 0x9a9cf685, 0x44c5d6c1,
	  0xe1de5920, 0xafa51b8f, 0xcd4e7b42, 0x2262eb60, 0xf0ef2c90, 0x57d28dc7, },
	{ 0x4a371fc3, 0x9cf6855f, 0xc5d6c19a, 0xde592044, 0xa51b8fe1, 0x4e7b42af,
	  0x62eb60cd, 0xef2c9022, 0xd28dc7f0, 0xa73da157, 0x3175b066, 0x77964811, },
	{ 0x371fc3bc, 0xf6855f4a, 0xd6c19a9c, 0x592044c5, 0x1b8fe1de, 0x7b42afa5,
	  0xeb60cd4e, 0x2c902262, 0x8dc7f0ef, 0x3da157d2, 0x75b066a7, 0x96481131, },
	{ 0x48113175, 0xe3f87796, 0xd0abe946, 0xd833539e, 0x240898ba, 0x71fc3bcb,
	  0x6855f4a3, 0x6c19a9cf, 0x92044c5d, 0xb8fe1de5, 0xb42afa51, 0xb60cd4e7, },
	{ 0xc902262e, 0xdc7f0ef2, 0xda157d28, 0x5b066a73, 0x64811317, 0x6e3f8779,
	  0xed0abe94, 0xad833539, 0xb240898b, 0x371fc3bc, 0xf6855f4a, 0xd6c19a9c, },
	{ 0xb60cd4e7, 0xc902262e, 0xdc7f0ef2, 0xda157d28, 0x5b066a73, 0x64811317,
	  0x6e3f8779, 0xed0abe94, 0xad833539, 0xb240898b, 0x371fc3bc, 0xf6855f4a, },
	{ 0x2262eb60, 0xf0ef2c90, 0x57d28dc7, 0x66a73da1, 0x113175b0, 0xf8779648,
	  0xabe946e3, 0x33539ed0, 0x0898bad8, 0xfc3bcb24, 0x55f4a371, 0x19a9cf68, },
	{ 0x5d6c19a9, 0xe592044c, 0x51b8fe1d, 0xe7b42afa, 0x2eb60cd4, 0xf2c90226,
	  0x28dc7f0e, 0x73da157d, 0x175b066a, 0x79648113, 0x946e3f87, 0x39ed0abe, },
	{ 0xdc7f0ef2, 0xda157d28, 0x5b066a73, 0x64811317, 0x6e3f8779, 0xed0abe94,
	  0xad833539, 0xb240898b, 0x371fc3bc, 0xf6855f4a, 0xd6c19a9c, 0x592044c5, },
	{ 0xa371fc3b, 0xcf6855f4, 0x5d6c19a9, 0xe592044c, 0x51b8fe1d, 0xe7b42afa,
	  0x2eb60cd4, 0xf2c90226, 0x28dc7f0e, 0x73da157d, 0x175b066a, 0x79648113, },
	{ 0xde592044, 0xa51b8fe1, 0x4e7b42af, 0x62eb60cd, 0xef2c9022, 0xd28dc7f0,
	  0xa73da157, 0x3175b066, 0x77964811, 0xe946e3f8, 0x539ed0ab, 0x98bad833, },
	{ 0xa157d28d, 0xb066a73d, 0x48113175, 0xe3f87796, 0xd0abe946, 0xd833539e,
	  0x240898ba, 0x71fc3bcb, 0x6855f4a3, 0x6c19a9cf, 0x92044c5d, 0xb8fe1de5, },
	{ 0x2044c5d6, 0x8fe1de59, 0x42afa51b, 0x60cd4e7b, 0x902262eb, 0xc7f0ef2c,
	  0xa157d28d, 0xb066a73d, 0x48113175, 0xe3f87796, 0xd0abe946, 0xd833539e, },
	{ 0x5f4a371f, 0x9a9cf685, 0x44c5d6c1, 0xe1de5920, 0xafa51b8f, 0xcd4e7b42,
	  0x2262eb60, 0xf0ef2c90, 0x57d28dc7, 0x66a73da1, 0x113175b0, 0xf8779648, },
};
