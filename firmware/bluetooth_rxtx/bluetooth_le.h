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
