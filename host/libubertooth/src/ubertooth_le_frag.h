/* Reassembly for long Bluetooth LE packets split across 64-byte USB records. */
#ifndef __UBERTOOTH_LE_FRAG_H__
#define __UBERTOOTH_LE_FRAG_H__

#include <stdint.h>

#include "ubertooth_interface.h"

typedef struct {
	uint8_t data[LE_MAX_USB_PACKET_SIZE];
	uint16_t expected;
	uint16_t received;
	uint8_t next_index;
	uint8_t active;
	usb_pkt_rx metadata;
} le_usb_reassembly;

enum le_usb_reassembly_result {
	LE_REASSEMBLY_ERROR = -1,
	LE_REASSEMBLY_WAIT = 0,
	LE_REASSEMBLY_COMPLETE = 1,
};

/* Legacy LE_PACKET records complete immediately. Fragmented records complete
 * only after the declared total length has arrived in order. On COMPLETE,
 * data/length/metadata remain valid until the next call on this state. */
int le_usb_reassemble(le_usb_reassembly *state, const usb_pkt_rx *frame,
                      const uint8_t **data, uint16_t *length,
                      const usb_pkt_rx **metadata);

#endif /* __UBERTOOTH_LE_FRAG_H__ */
