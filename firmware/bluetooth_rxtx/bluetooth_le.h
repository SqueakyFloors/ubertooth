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

    /* Set when the connection hops with channel selection algorithm 2, which
     * has no hop increment: each event's channel comes from the access
     * address and the event counter below. Promiscuous recovery sets both
     * once it has worked the counter out; a connection followed from its
     * setup is handled by le_phy.c, which keeps its own copy. */
    u8 chan_sel_2;
    u16 event_counter;          // the counter of the next connection event

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
uint8_t le_csa2_channel(uint16_t event_counter, uint32_t access_address,
                        le_channel_remapping_t *remapping);
uint8_t le_csa2_unmapped_channel(uint16_t event_counter,
                                 uint32_t access_address);

/* Divide, rounding to the nearest integer: round up at 0.5. */
#define LE_DIVIDE_ROUND(N, D) (((N) + (D)/2) / (D))

/* 1.25 ms, the unit a connection interval is counted in, in the 100 ns ticks
 * the capture clock counts. The same number as LE_BASECLK in
 * ubertooth_clock.h, written again here so that this file's arithmetic needs
 * none of the clock's hardware; test_promisc_csa2.c fails if the two differ. */
#define LE_CONN_INTERVAL_TICKS 12500

/* The longest gap that can be a trip round the channels: 37 channels at the
 * longest connection interval, 148 s. Anything longer is not the connection
 * coming back. It is also what bounds the arithmetic below, where a gap is
 * multiplied back out of the events it spans. */
#define LE_PROMISC_MAX_GAP \
	((u32)LE_HOP_CHANNELS * LE_MAX_CONN_INTERVAL * LE_CONN_INTERVAL_TICKS)

u32 le_gap_tolerance(u32 span);
int le_same_span(u32 a, u32 b);

/*
 * How late a sighting can be against the start of the connection event it
 * belongs to, in the 100 ns ticks the capture clock counts.
 *
 * What the board times is a packet's arrival, and which packet of an event it
 * hears varies. The central's comes at the event's start; the peripheral's
 * answer follows a 150 microsecond gap and however long the central's packet
 * took, which is up to 2.1 ms for the longest the 1M PHY carries. So the gap
 * between two sightings is not an exact number of connection intervals even
 * with a clock that keeps perfect time, and the search allows for the
 * difference on top of the clock's own.
 *
 * It has to stay under half the shortest interval, 3.75 ms, or the number of
 * events a gap spans would stop being the nearest whole number.
 */
#define LE_CSA2_SIGHTING_SPREAD 25000u

/*
 * Passive recovery of a connection that hops with channel selection
 * algorithm 2 (Core 5.4, Vol 6, Part B, 4.5.8.3), from packets heard on one
 * data channel.
 *
 * Algorithm 1 is a running sum of a hop increment, so a listener can work the
 * increment out by timing one hop. Algorithm 2 has no increment: the channel
 * of a connection event is a hash of the access address and the 16-bit event
 * counter, reduced to one of the channels the map keeps. The access address
 * is known by the time this runs, so what is left to find is the connection
 * interval and where in the counter's range the connection is.
 *
 * Both come from the times the connection was heard on the one channel the
 * mode listens to. Every gap between two sightings is a whole number of
 * connection intervals, which pins the interval; the gaps then say how many
 * events apart the sightings are, and the counter is the value that puts the
 * algorithm's channel on this channel at those events.
 *
 * The counter is scored rather than solved. A sighting the predicted sequence
 * does not account for is a point not scored, not a candidate ruled out: on a
 * connection using fewer than the 37 channels a sighting may be a visit the
 * map moved here from a channel it dropped, and those cannot be predicted
 * without knowing the map. About a map's worth of sightings are still direct
 * visits, so the right counter outscores every other by a wide margin.
 */

/* How many sightings the search works from: the more it has, the further the
 * right counter stands above the best wrong one. */
#define LE_CSA2_SIGHTINGS 24

/* The most connection events two sightings on one channel may be apart and
 * still be used. The channel comes up about once in 37 events, so a gap of
 * several hundred events is the connection having been away rather than a
 * visit nobody heard, and leaving those out keeps the candidate intervals in
 * a range worth searching. */
#define LE_CSA2_MAX_EVENT_GAP 256

/* How many sightings the answer must account for, and by how much it must
 * beat the best other counter. A wrong counter puts a predicted visit on this
 * channel about once in 37 events, so these two leave the chance of adopting
 * one well under a percent. */
#define LE_CSA2_MIN_SCORE 10
#define LE_CSA2_MIN_MARGIN 3

/* How many of the gaps the connection interval may fail to account for. A gap
 * longer than LE_CSA2_MAX_EVENT_GAP events, or one taken across a change of
 * connection parameters, is such a gap. */
#define LE_CSA2_INTERVAL_MISSES 5

/*
 * How many connection events of silence a follow set up by this search is
 * given before the connection is taken to be over.
 *
 * Such a follow has no channel map, so it lands on the right channel only for
 * the events whose channel the map did not move, and a run of events it cannot
 * hear says nothing about whether the connection is still there. The runs are
 * not short: they are a property of the access address and the map rather than
 * chance, and the longest one among the connections this search will adopt an
 * answer for was 58 events when it was counted on the real channel sequences
 * (test_promisc_csa2.c counts them again and fails if a run reaches this
 * figure).
 *
 * So this is what the mode waits on a connection that really has gone: a few
 * seconds at a short interval, where the five-second floor wins anyway, and
 * about two minutes at the longest interval a connection can be recovered at.
 * Waiting is the cheaper mistake -- recovering a connection again from the
 * access address takes longer than that.
 */
#define LE_CSA2_SILENCE_EVENTS 64

typedef struct _le_csa2_recovery_t {
	u16 conn_interval;  /* in units of 1.25 ms, 0 when nothing was found */
	u16 event_counter;  /* the counter of the newest sighting */
	u8  sightings;      /* how many the search had to work from */
	u8  score;          /* how many of them the answer accounts for */
	u8  runner_up;      /* the best score of any other counter */
} le_csa2_recovery_t;

uint16_t le_csa2_interval(const u32 *gap, unsigned gap_count);
int le_csa2_recover(const u32 *gap, unsigned gap_count, uint8_t channel_idx,
                    u32 access_address, le_csa2_recovery_t *out);

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

