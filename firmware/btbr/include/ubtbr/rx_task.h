#ifndef __RX_TASK_H
#define __RX_TASK_H
#include <ubertooth_interface.h>
#include <ubtbr/bb_msg.h>

#define RX_F_PAYLOAD 	0
#define RX_F_RAW	1

typedef int (*rx_task_cb_t)(msg_t *msg, void *arg, int time_offset);

/* Open a listening slot `delay` frames from now. Returns 0 when it is open and
 * -1 when the scheduler had no room for it, which it has not when one of the
 * two frames a receive needs already holds its four tasks.
 *
 * On -1 nothing is listening and the callback is never reached, because the
 * tasks that would reach it are the ones that were refused. Every caller asks
 * for its next slot from that callback, so a caller that carried on would sit
 * with no task left to run and wait forever. Either go back through the path
 * that opens the next window, where there is one, or report the session over.
 *
 * Nothing is left held on -1, so a caller has only its own state to put back:
 * the packet buffer belongs to the slot task that did not run.
 *
 * Asking again in the same slot does not help. One of the frames asked for is
 * the one that was refused, and a frame is emptied only by running, so a
 * second ask from the same slot clock interrupt is refused too. That is what
 * bounds the refusal: one report, then the caller acts on it. */
int rx_task_schedule(unsigned delay, rx_task_cb_t cb, void*cbarg, unsigned flags);
void rx_task_reset(void);
#endif
