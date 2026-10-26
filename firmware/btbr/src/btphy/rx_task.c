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
#define NUM_DATA_HDR_BYTES BYTE_ALIGN(4+54+12*CODEC_RX_CHUNK_SIZE)

/* The receiver must listen for at least 426usec. (BT Vol B, part 2, fig 2.3)
 * Reception deadline is setup_time + sym_count + 28usec of lag
 * (we wait until 312.5+150 = 462.5usec.)
  (426+28)  */
#define	WAIT_SYNCWORD	(610+100) // perfect_time=61.usec, max_delta=10.usec

/* Maximum transmission size, for debug purpose */
#define	WAIT_RX_MAX	1500 

/* The air carries one symbol per microsecond, and CLKN_OFFSET counts the slot
 * clock in 100ns steps, so one air byte takes 8usec, which is 80 steps. */
#define RX_CLKN_PER_AIR_BYTE	80

/* How much later than the air says a byte may still turn up. The radio's
 * symbol timing is not exact, and the SSP FIFO sits between the radio and the
 * DMA controller, so allow four bytes' worth. */
#define RX_WAIT_SLACK_BYTES	4

/* No wait may run past the end of the clkn tick, because the slot clock
 * restarts there and a deadline beyond it would never be reached. Stop 10usec
 * short of the restart, which is later than the last byte of a packet header
 * is ever due, and leaves the loop time to notice the deadline pass. */
#define RX_WAIT_LATEST		(CLKN_OFFSET_RESET_VAL-100)

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
	if (size > 4)
		return size-4;
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
 * due at a known time: the sync word was detected at pkt_time, and after that
 * the air delivers one byte every 8usec. A poll count would wait for a
 * different length of time on every change to this code or the clock speed.
 */
static unsigned rx_wait_bytes(unsigned want)
{
	/* pkt_time is the slot clock when the sync word was detected, so the
	 * bytes after it are the ones still to come. */
	unsigned due = rx_task.pkt_time
			+ (want+RX_WAIT_SLACK_BYTES)*RX_CLKN_PER_AIR_BYTE;
	unsigned size;

	if (due > RX_WAIT_LATEST)
		due = RX_WAIT_LATEST;

	while ((size=rx_buf_update()) < want && CLKN_OFFSET < due)
		;

	return size;
}

/* Stop RX and call user callback */
static void rx_finalize(void)
{
	int rc;
	uint32_t i, size;
	uint16_t rx_size;
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
		size=rx_buf_update();

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

// -1
static int rx_prepare(uint8_t p1, uint8_t p2, uint16_t p3)
{
	uint8_t chan;
	uint32_t rx_clkn = btphy_cur_clkn()+1;
	btctl_rx_pkt_t *pkt;

	/* In inquiry/paging, hop.x must be incremented before each tx
	 * of the master / RX of the slave
	 * (Actually, rx_task in not used in mode INQUIRY_SCAN)
	 */
	if (btphy.mode == BT_MODE_PAGE_SCAN)
		hop_increment();
	chan = hop_channel(rx_clkn);

	/* Tune to chan & start RX */
	//cprintf("tune rx %d\n", pkt->chan);
	btphy_rf_tune_chan(2402+chan, 0);
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
		 * is handed a part-decoded payload. */
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
	unsigned i, delay;
	btctl_rx_pkt_t *pkt = rx_task.rx_pkt;

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
					tdma_schedule(1, rx_decode, 0, 0, 0, 0);
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
	unsigned i, size, bytes_left;
	int rc, last;
	btctl_rx_pkt_t *pkt = rx_task.rx_pkt;

	/* Check if we're in the last slot according to BT spec table 6.2 */
	last = --rx_task.slot_num == 0;

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
		//cputc('D');
	}
	if (rc == BBCODEC_DONE)
	{
		rx_finalize();
	}
	else
	{
		/* Schedule one more decode */
		tdma_schedule(1, rx_decode, 0, 0, 0, 0);
	}
	return 0;
}

const struct tdma_sched_item rx_sched_set[] = {				// clk1_0		
	SCHED_ITEM(rx_prepare,	0, 0, 0), 	SCHED_END_FRAME(), 	// 1
	SCHED_ITEM(rx_execute,  -3, 0, 0), 	SCHED_END_FRAME(),	// 2
	SCHED_END_SET()
};

void rx_task_reset(void)
{
	if (rx_task.rx_msg)
	{
		msg_free(rx_task.rx_msg);
		rx_task.rx_pkt = NULL;
		rx_task.rx_msg = NULL;
	}
}

void rx_task_schedule(unsigned delay, rx_task_cb_t cb, void*cbarg, unsigned flags)
{
	btctl_rx_pkt_t *pkt;

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

	tdma_schedule_set(delay, rx_sched_set, 0);
}
