#ifndef __BTPHY_LL_H
#define __BTPHY_LL_H
#include <ubtbr/codec.h>

typedef struct {
	uint8_t is_connected;
	uint8_t is_central;

	/* Stats / supervision */
	uint32_t clkn_start;
	uint32_t clkn_last_rx;
	uint32_t rx_count;
	
	// ll flags
	uint8_t lt_addr;
	uint8_t rmt_seqn;
	uint8_t loc_seqn;
	uint8_t loc_arqn;

	/* Current tx message */
	bbhdr_t tx_hdr;
	msg_t *cur_tx_msg;
	bbhdr_t *cur_tx_hdr;
	uint8_t *cur_tx_data;

	/* A packet the host composed in full and asked to put on the air as
	 * it is (BTCTL_TX_PKT_REQ). It goes out in the next transmit slot and
	 * in each of the next pkt_slots_left-1 slots, with no acknowledgment
	 * of its own and no retransmission, so it never holds up the data the
	 * link is carrying for longer than the slots it was given. */
	bbhdr_t pkt_hdr;
	msg_t *cur_pkt_msg;
	uint16_t pkt_slots_left;
} link_layer_t;

/* What the link layer hands the transmit task for one slot. */
typedef struct {
	bbhdr_t *hdr;
	uint8_t *data;
	/* The air format, where it was chosen rather than implied by the
	 * header's TYPE code, with the number of data bytes that format needs
	 * told separately; NULL and 0 otherwise. See bbcodec_set_tx_format(). */
	const bbcodec_types_t *fmt;
	unsigned data_bytes;
} ll_tx_t;

void ll_init(link_layer_t *ll, uint8_t is_central, uint8_t lt_addr);
void ll_reset(link_layer_t *ll);
int ll_process_rx(link_layer_t *ll, msg_t *msg);
void ll_prepare_tx(link_layer_t *ll, ll_tx_t *tx);

#endif
