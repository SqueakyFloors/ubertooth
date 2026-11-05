/* RX task
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
#include <string.h>
#include <ubtbr/defines.h>
#include <ubtbr/cfg.h>
#include <ubtbr/debug.h>
#include <ubtbr/rf.h>
#include <ubtbr/ubertooth_dma.h>
#include <ubtbr/btctl_intf.h>
#include <ubtbr/btphy.h>
#include <ubtbr/tdma_sched.h>
#include <ubtbr/codec.h>
#include <ubtbr/hop.h>
#include <ubtbr/rx_task.h>

/* Exclude sw from size (see rx_buf_update()) */
#define NUM_PKT_HDR_BYTES BYTE_ALIGN(4+54)

/* Data header should fit in a fec23 encoded codec chunk */
#define NUM_DATA_HDR_BYTES BYTE_ALIGN(4+54+BBCODEC_FEC23_AIR_BITS(CODEC_RX_CHUNK_SIZE))

/* The receiver must listen for at least 426usec. (BT Vol B, part 2, fig 2.3)
 * Reception deadline is setup_time + sym_count + 28usec of lag
 * (we wait until 312.5+150 = 462.5usec.)
  (426+28)  */
#define	WAIT_SYNCWORD	(610+100) // perfect_time=61.usec, max_delta=10.usec

/* Maximum transmission size, for debug purpose */
#define	WAIT_RX_MAX	1500 

/* CLKN_OFFSET counts the slot clock in 100ns steps. The air carries one symbol
 * per microsecond, so an air byte takes 8usec. */
#define RX_CLKN_PER_USEC	(CLK100NS_SECOND/1000000)
#define RX_CLKN_PER_AIR_BYTE	(8*RX_CLKN_PER_USEC)

/* The radio's sync word register holds the low half of the 64-bit access code,
 * so the packet strobe goes low with the high half still to come. Those four
 * bytes land in the DMA buffer, where rx_buf_update() skips them, and they
 * arrive after the strobe, not before it. */
#define RX_SYNC_WORD_BYTES	4

/* How much later than the air says a byte may still turn up. The radio's symbol
 * timing is not exact, and the SSP FIFO sits between the radio and the DMA
 * controller, so allow four bytes' worth. */
#define RX_WAIT_SLACK_BYTES	4

/* The wait for a packet's bytes takes its ceiling from rf_wait_latest(), this
 * tick's own restart less a little slack: see rf.h. It is later than the last
 * byte of the longest wait -- the first chunk of a payload -- is ever due. The
 * two fixed deadlines above are inside the shortest tick this image ever asks
 * for, so they need no ceiling. */

/* Maximum size we can RX in WAIT_RX_MAX
 * (minus 2 bytes to avoid crashing in case we're unlucky.)
 */
#define MAX_WAIT_BYTES	(150/8-2)

static struct {
	/* arguments */
	rx_task_cb_t pkt_cb;
	void *cb_arg;

	/* variables */
	//btbr_pkt_t *rx_pkt;
	msg_t *rx_msg;
	btctl_rx_pkt_t *rx_pkt;

	bbcodec_t codec;
	uint8_t do_rx_payload;
	/* Maximum acl packet is DH5: 32+4+54+(2+339+2)*8 */
	uint8_t rx_dma_buf[BYTE_ALIGN(32+4+54+MAX_ACL_SIZE*12)];
	uint16_t pkt_time;
	unsigned rx_offset;
	uint8_t rx_done;
	uint8_t slot_num;
	uint8_t rx_raw;
	/* Which attempt at scheduling a receive the slot tasks belong to.
	 *
	 * The two tasks of a receive go on the scheduler together, so an
	 * attempt that lost the second of them leaves the first standing in
	 * the slot before, and a task cannot be taken back off. The scheduler
	 * copies this number into every item of the set, so rx_prepare() can
	 * compare the number it was given against the one that is armed: a
	 * task left over from a refused attempt carries a number that is never
	 * armed and does nothing.
	 *
	 * A flag would not do. The caller's recovery can schedule its next
	 * receive in the same slot clock interrupt, before the leftover task
	 * runs, and the leftover would then take that receive's turn: tuning
	 * the radio and taking a packet buffer a slot early, and leaving the
	 * real task with nothing to do. */
	uint16_t attempts;
	uint16_t armed_attempt;
} rx_task; 

static int rx_decode(uint8_t p1, uint8_t p2, uint16_t p3);

/* This is the function to call to know what's available in rx_buf */
static unsigned rx_buf_update(void)
{
	unsigned i, size = dma_get_rx_offset();

	/* Reverse bytes to host order */
	for(i=rx_task.rx_offset;i<size;i++)
		rx_task.rx_dma_buf[i] = reverse8(rx_task.rx_dma_buf[i]);
	rx_task.rx_offset = size;

	/* Skip 32-bits of syncword*/
	if (size > RX_SYNC_WORD_BYTES)
		return size-RX_SYNC_WORD_BYTES;
	return 0;
}

/*
 * Wait until the DMA controller has `want` bytes of the packet, or until the
 * slot clock says those bytes are overdue. Returns how many bytes are there,
 * which is fewer than `want` if the deadline passed.
 *
 * Bytes only appear while the radio keeps clocking symbols into the SSP, so a
 * radio that stops part-way through a packet, or a DMA transfer that never
 * finishes, leaves nothing to end the wait. This runs from a slot task in
 * TIMER0_IRQHandler, and USB in this image is served from an interrupt of the
 * same priority, so a wait that never ends means the board answers the host
 * nothing until it is re-plugged.
 *
 * The deadline is the slot clock, not a number of polls, because the bytes are
 * due at a known time: the sync word was detected at pkt_time, and the air
 * delivers one byte every 8 usec after that. A poll count would wait for a
 * different length of time on every change to this code or the clock speed.
 */
static unsigned rx_wait_bytes(unsigned want)
{
	/* pkt_time is the slot clock when the packet strobe went low. `want` is
	 * what rx_buf_update() reports, so the DMA controller has to reach the
	 * high half of the access code as well, and every one of those bytes is
	 * still to come. */
	unsigned due = rx_task.pkt_time
			+ (want+RX_SYNC_WORD_BYTES+RX_WAIT_SLACK_BYTES)
			  *RX_CLKN_PER_AIR_BYTE;
	unsigned size;
	unsigned latest = rf_wait_latest();

	if (due > latest)
		due = latest;

	while ((size=rx_buf_update()) < want && CLKN_OFFSET < due)
		;

	return size;
}

/* Stop RX and call user callback */
static void rx_finalize(void)
{
	msg_t *msg = rx_task.rx_msg;
	btctl_rx_pkt_t *pkt = rx_task.rx_pkt;

	/* Stop RX */
	btphy_rf_idle();
	dio_ssp_stop();

	if (!pkt)
		DIE("rxnd: no rx_pkt");
	if (!rx_task.pkt_cb)
		DIE("rxnd: no pkt_cb");

	if (BBPKT_HAS_HDR(pkt))
	{
		/* Reverse payload to host order */
		rx_buf_update();

		/* Finalize pkt's payload decoding */
		pkt->flags |= bbcodec_decode_finalize(&rx_task.codec, pkt->bt_data, &pkt->data_size);
	}
	else
	{
		pkt->data_size = 0;
	}
	/* Trim the message to its real length */
	msg_set_write(rx_task.rx_msg, &pkt->bt_data[pkt->data_size]);

	/* Reset rx state before calling callback,
	 * because callback can schedule a new RX */
	rx_task.rx_msg = NULL;
	rx_task.rx_pkt = NULL;

	/* Call user callback */
	rx_task.pkt_cb(msg, rx_task.cb_arg, rx_task.pkt_time-RF_EXPECTED_RX_CLKN_OFFSET);
}

/* Put the rest of a multi-slot decode in the next slot, and end the receive
 * where the scheduler has no room for it.
 *
 * A decode that is never continued is the costliest way a receive can be lost:
 * rx_finalize() is what gives the packet buffer back and reaches the caller's
 * callback, so without it the packet is lost in silence, rx_msg stays held,
 * and the next receive the state machine asks for stops the firmware with
 * "rx_schedule: rx_msg!=0".
 *
 * So the receive ends here instead, reported the way a receive that produced
 * nothing is reported: the header flag comes back off the packet, which is
 * what every reader keys on, and which also keeps rx_finalize() out of
 * bbcodec_decode_finalize() -- that stops the firmware when it is handed a
 * part-decoded payload. The decoded header itself is left alone, as
 * rx_data_hdr_callback() leaves it. */
static void rx_schedule_decode(void)
{
	if (tdma_schedule(1, rx_decode, 0, 0, 0, 0) == 0)
		return;

	cprintf("RX: cannot schedule rest of decode, packet lost\n");
	rx_task.rx_pkt->flags &= ~((1<<BBPKT_F_HAS_HDR)|(1<<BBPKT_F_HAS_CRC));
	rx_finalize();
}

// -1
static int rx_prepare(uint8_t p1, uint8_t p2, uint16_t p3)
{
	uint8_t chan;
	uint32_t rx_clkn = btphy_cur_clkn()+1;
	btctl_rx_pkt_t *pkt;

	if (p3 != rx_task.armed_attempt)
	{
		/* This task is left over from an attempt rx_task_schedule()
		 * could not finish placing, and it has already said so and
		 * told its caller. The slot that would have listened is not on
		 * the scheduler, so there is nothing to prepare for: tuning
		 * the radio and taking a packet buffer here would leave both
		 * held with nothing left to run that gives them back. */
		return 0;
	}

	/* A paged device advances its response phase once per central transmit
	 * slot, and every window it waits for the FHS in is one such slot. */
	if (btphy.mode == BT_MODE_PERIPHERAL_PAGE_RESPONSE)
		hop_increment();
	chan = hop_channel(rx_clkn);

	/* Tune to chan & start RX */
	//cprintf("tune rx %d\n", pkt->chan);
	btphy_rf_tune_chan(BT_FREQ_MIN+chan, 0);
	/* Here i must strobe SRX before & after GRMDM cfg. why ? */
	cc2400_strobe(SFSON);
	btphy_rf_cfg_rx();
	btphy_rf_rx();

	/* Alloc & reset RX packet */
	if (!(rx_task.rx_msg = btctl_msg_alloc(BTCTL_RX_PKT)))
		DIE("rx_prepare: no more buf");
	pkt = rx_task.rx_pkt = (btctl_rx_pkt_t*)msg_put(rx_task.rx_msg, sizeof(*rx_task.rx_pkt));

	/* Setup rx packet */
	pkt->chan = chan;
	pkt->data_size = 0;
	pkt->clkn = rx_clkn; // clkn at time of execute
	pkt->flags = 0;

	bbcodec_init(&rx_task.codec, btphy_whiten_seed(rx_clkn), btphy.chan_uap, 1, rx_task.rx_raw);
	rx_task.pkt_time = 0;
	rx_task.rx_done = 0;
	rx_task.rx_offset = 0;
	/* Configure DMA RX */
	dma_init_rx_single(rx_task.rx_dma_buf, sizeof(rx_task.rx_dma_buf));

	/* Start SPI DMA */
	dio_ssp_start_rx();


	return 0; 
}

static int rx_pkt_hdr_callback(void)
{
	btctl_rx_pkt_t *pkt = rx_task.rx_pkt;

	/* Wait for 32 bits sw + 4bits trailer + 54 bits header*/
	if (rx_wait_bytes(NUM_PKT_HDR_BYTES) < NUM_PKT_HDR_BYTES)
	{
		/* The header is overdue, so there is nothing to decode. Report
		 * it the way a header that fails to decode is reported: no
		 * header flag on the packet. */
		return -1;
	}

	/* Decode pkt's header (skipping 32-bits of syncword in the dma_buf) & configure codec */
	if(bbcodec_decode_header(&rx_task.codec, &pkt->bb_hdr, rx_task.rx_dma_buf+4))
	{
		return -1;
	}

	if (rx_task.codec.t->has_crc)
		pkt->flags |= 1<<BBPKT_F_HAS_CRC;

	pkt->flags |= 1<<BBPKT_F_HAS_HDR;

	return 0;
}

static int rx_data_hdr_callback(void)
{
	int rc;
	unsigned size;
	btctl_rx_pkt_t *pkt = rx_task.rx_pkt;

	/* Wait for a full chunk */
	size = rx_wait_bytes(NUM_DATA_HDR_BYTES);
	if (size < NUM_DATA_HDR_BYTES)
	{
		/* Too little arrived to decode a chunk from. Take the header
		 * flag back off the packet, which reports it as a receive that
		 * produced nothing -- the same thing a header that fails to
		 * decode reports. It also keeps rx_finalize() out of
		 * bbcodec_decode_finalize(), which stops the firmware when it
		 * is handed a part-decoded payload. The decoded header itself
		 * is left alone: the flag is what every reader keys on. */
		pkt->flags &= ~((1<<BBPKT_F_HAS_HDR)|(1<<BBPKT_F_HAS_CRC));
		return -1;
	}

	/* Decode first data chunk, for the payload header */
	if((rc=bbcodec_decode_chunk(&rx_task.codec, pkt->bt_data, &pkt->bb_hdr, rx_task.rx_dma_buf+4, size)))
	{
		if (rc == 1)
			rx_task.rx_done = 1;
		else
			DIE("dec hdr %d: rc=%d\n", pkt->bb_hdr.type, rc);

	}

	return 0;
}

// 0
static int rx_execute(uint8_t p1, uint8_t p2, uint16_t p3)
{
	btctl_rx_pkt_t *pkt = rx_task.rx_pkt;

	if (!pkt)
	{
		/* rx_prepare() did not run, so nothing is listening and there
		 * is no packet to report. Both tasks of a receive carry the
		 * same attempt number and only btphy_init() resets the task,
		 * so this cannot happen as the image stands; it is two
		 * instructions against a reset arriving between the two tasks,
		 * which clears the packet and leaves them on the scheduler. */
		return 0;
	}

	if ((cc2400_get(FSMSTATE) & 0x1f) != STATE_STROBE_RX)
	{
		cprintf("rxne: RF not rdy\n");
		rx_finalize();
		return 0;
	}

	while (CLKN_OFFSET < WAIT_SYNCWORD)
	{
		/* PKT strobe goes low if a syncword is detected */
		if (GIO6 == 0)
		{
			rx_task.pkt_time = CLKN_OFFSET;
			pkt->flags |= 1<<BBPKT_F_HAS_PKT;
			break;
		}
	}
	/* If a packet is being received and it has a payload,*/
	if (BBPKT_HAS_PKT(pkt) && rx_task.do_rx_payload)
	{
		/* Decode the pkt header */
		if (rx_pkt_hdr_callback() == 0)
		{
			/* If this packet kind has data in it */
			if (rx_task.codec.t->payload_bytes)
			{
				/* Decode the data header, and if the data is not
				 * fully decoded yet, carry on in the next slot */
				if (rx_data_hdr_callback() == 0 && !rx_task.rx_done)
				{
					/* Maximum decode count */
					rx_task.slot_num = rx_task.codec.t->nslots*2-1;

					/* Schedule next decode */
					rx_schedule_decode();
					return 0;
				}
			}
		}
	}
	/* No packet/ID was received, stop now */
	rx_finalize();

	return 0; 
}

/* 1+n : Rx multislot: decode available data */
static int rx_decode(uint8_t p1, uint8_t p2, uint16_t p3)
{
	unsigned size, bytes_left;
	int rc, last;
	btctl_rx_pkt_t *pkt = rx_task.rx_pkt;

	/* Check if we're in the last slot according to BT spec table 6.2. That
	 * is what decides whether to wait for the rest of the packet's bytes
	 * before decoding; the decode itself ends when the codec says it is
	 * done. The count has a floor, because decrementing a zero would wrap
	 * to 255 and the slots after a decode that never finished would then
	 * each be taken for an early one. */
	if (rx_task.slot_num > 0)
		rx_task.slot_num--;
	last = rx_task.slot_num == 0;

	/* Reverse the dma buf */
	size=rx_buf_update();

	/* Check if we can rx end of packet in this slot*/
	bytes_left = rx_task.codec.air_bytes - size;
	last |= bytes_left < MAX_WAIT_BYTES;

	/* last slot: wait for rx end */
	if (last)
	{
		while(CLKN_OFFSET < WAIT_RX_MAX && size < rx_task.codec.air_bytes)
		{
			size=rx_buf_update();
		}
	}

	/* Decode what we can */
	while((rc=bbcodec_decode_chunk(&rx_task.codec, pkt->bt_data, &pkt->bb_hdr, rx_task.rx_dma_buf+4, size))==0)
	{
	}
	if (rc == BBCODEC_DONE)
	{
		rx_finalize();
	}
	else
	{
		/* Schedule one more decode */
		rx_schedule_decode();
	}
	return 0;
}

const struct tdma_sched_item rx_sched_set[] = {				// clk1_0		
	SCHED_ITEM(rx_prepare,	0, 0, 0), 	SCHED_END_FRAME(), 	// 1
	SCHED_ITEM(rx_execute,  -3, 0, 0), 	SCHED_END_FRAME(),	// 2
	SCHED_END_SET()
};

/* How many slot tasks a receive puts on the scheduler: the two above. Each
 * item of the set is followed by an end-of-frame marker and the set by an
 * end-of-set one, so an item added without changing this stops the build. */
#define RX_SCHED_ITEMS	2
_Static_assert(ARRAY_SIZE(rx_sched_set) == 2 * RX_SCHED_ITEMS + 1,
	"RX_SCHED_ITEMS does not match rx_sched_set");

void rx_task_reset(void)
{
	/* Nothing on the scheduler is armed any more. The slot tasks of a
	 * receive in flight are left standing, and the number they carry is
	 * no longer the armed one, so they do nothing. */
	rx_task.armed_attempt = 0;
	if (rx_task.rx_msg)
	{
		msg_free(rx_task.rx_msg);
		rx_task.rx_pkt = NULL;
		rx_task.rx_msg = NULL;
	}
}

int rx_task_schedule(unsigned delay, rx_task_cb_t cb, void*cbarg, unsigned flags)
{
	/* Sanity check */
	if (rx_task.rx_msg)
		DIE("rx_schedule: rx_msg!=0");

	/* user rx callback */
	rx_task.pkt_cb = cb;
	rx_task.cb_arg = cbarg;
	/* Does the packet have a payload ? (else it's an ID) */
	rx_task.do_rx_payload = flags & (1<<RX_F_PAYLOAD);
	/* Receive raw bursts for monitor state */
	rx_task.rx_raw = flags & (1<<RX_F_RAW);

	/* A receive needs both of its tasks, so anything short of the whole
	 * set is a receive that will not happen: rx_execute() is what listens
	 * and what ends in rx_finalize(), and that is the only path to the
	 * caller's callback. The rx_prepare() a partial placement leaves
	 * standing cannot be taken back off, so it is given a number that is
	 * never armed and does nothing (`attempts` above). Nothing is held
	 * either way: the packet buffer is rx_prepare()'s to take.
	 *
	 * The number advances on every attempt, refused or not, so a refused
	 * attempt's number is not handed out again to a later one. */
	/* Zero is the number nothing carries, so it is skipped on the wrap: a
	 * connection asks for a receive every slot pair, which reaches 65536
	 * in well under a minute. */
	if (++rx_task.attempts == 0)
		rx_task.attempts = 1;
	if (tdma_schedule_set(delay, rx_sched_set, rx_task.attempts)
			< RX_SCHED_ITEMS)
	{
		cprintf("RX: cannot schedule receive\n");
		return -1;
	}
	rx_task.armed_attempt = rx_task.attempts;

	return 0;
}
