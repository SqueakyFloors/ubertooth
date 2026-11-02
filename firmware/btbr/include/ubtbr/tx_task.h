#ifndef __TX_TASK_H
#define __TX_TASK_H
#include <ubtbr/codec.h>

typedef void (*tx_task_cb_t)(void *arg);
void tx_task_schedule(unsigned delay, tx_task_cb_t cb, void*cbarg, bbhdr_t *hdr, uint8_t *data);
/* As tx_task_schedule(), for a packet whose air format the caller chose
 * rather than leaving to the TYPE code in its header: fmt is the format and
 * data_bytes the number of data bytes where the format carries no length of
 * its own. See bbcodec_set_tx_format(). */
void tx_task_schedule_fmt(unsigned delay, tx_task_cb_t cb, void*cbarg,
		bbhdr_t *hdr, uint8_t *data,
		const bbcodec_types_t *fmt, unsigned data_bytes);
void tx_task_reset(void);

#endif
