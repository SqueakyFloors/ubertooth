#include "ubertooth_le_frag.h"

#include <string.h>

static uint16_t fragment_total(const usb_pkt_rx *frame)
{
	return (uint16_t)frame->reserved[0] |
	       ((uint16_t)(frame->reserved[1] & LE_FRAG_TOTAL_HIGH_MASK) << 8);
}

static uint8_t fragment_index(const usb_pkt_rx *frame)
{
	return frame->reserved[1] >> LE_FRAG_INDEX_SHIFT;
}

static int append_fragment(le_usb_reassembly *state, const usb_pkt_rx *frame)
{
	uint16_t remaining = state->expected - state->received;
	uint16_t chunk = remaining < DMA_SIZE ? remaining : DMA_SIZE;

	memcpy(state->data + state->received, frame->data, chunk);
	state->received += chunk;
	return state->received == state->expected;
}

int le_usb_reassemble(le_usb_reassembly *state, const usb_pkt_rx *frame,
                      const uint8_t **data, uint16_t *length,
                      const usb_pkt_rx **metadata)
{
	uint16_t total;

	if (!state || !frame || !data || !length || !metadata)
		return LE_REASSEMBLY_ERROR;

	*data = NULL;
	*length = 0;
	*metadata = NULL;

	if (frame->pkt_type == LE_PACKET) {
		state->active = 0;
		*data = frame->data;
		*length = DMA_SIZE;
		*metadata = frame;
		return LE_REASSEMBLY_COMPLETE;
	}

	if (frame->pkt_type == LE_PACKET_START) {
		total = fragment_total(frame);
		if (fragment_index(frame) != 0 || total <= DMA_SIZE ||
		    total > LE_MAX_USB_PACKET_SIZE) {
			state->active = 0;
			return LE_REASSEMBLY_ERROR;
		}
		state->metadata = *frame;
		state->metadata.pkt_type = LE_PACKET;
		state->expected = total;
		state->received = 0;
		state->next_index = 1;
		state->active = 1;
	} else if (frame->pkt_type == LE_PACKET_CONT) {
		total = fragment_total(frame);
		if (!state->active || total != state->expected ||
		    fragment_index(frame) != state->next_index) {
			state->active = 0;
			return LE_REASSEMBLY_ERROR;
		}
		++state->next_index;
	} else {
		return LE_REASSEMBLY_ERROR;
	}

	if (!append_fragment(state, frame))
		return LE_REASSEMBLY_WAIT;

	state->active = 0;
	*data = state->data;
	*length = state->expected;
	*metadata = &state->metadata;
	return LE_REASSEMBLY_COMPLETE;
}
