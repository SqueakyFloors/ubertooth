#ifndef __TX_TASK_H
#define __TX_TASK_H
#include <ubtbr/codec.h>

typedef void (*tx_task_cb_t)(void *arg);
void tx_task_schedule(unsigned delay, tx_task_cb_t cb, void*cbarg, bbhdr_t *hdr, uint8_t *data);
/* As tx_task_schedule(), for a packet whose air format the caller chose
 * rather than leaving to the TYPE code in its header: fmt is the format and
 * data_bytes the number of data bytes a BB_LEN_NEGOTIATED format needs. See
 * bbcodec_set_tx_format().
 *
 * Both of these report the completion from this call, rather than from a slot,
 * when the scheduler has no room for the transmit at all: that is the one case
 * where no task of the transmit runs to report it. So a callback that schedules
 * another transmit may be reached before this call returns, and one that keeps
 * asking for a slot that stays full recurses. */
void tx_task_schedule_fmt(unsigned delay, tx_task_cb_t cb, void*cbarg,
		bbhdr_t *hdr, uint8_t *data,
		const bbcodec_types_t *fmt, unsigned data_bytes);
void tx_task_reset(void);

#endif
