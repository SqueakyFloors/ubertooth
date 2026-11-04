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
 * with no task left to run and wait forever. A caller must therefore either go
 * back through the path that opens its next window, where it has one, or report
 * the session over.
 *
 * Nothing is left held on -1, so a caller has only its own state to put back:
 * the packet buffer belongs to the slot task that did not run.
 *
 * A caller that recovers by opening another window asks for a different frame,
 * so that ask can succeed and usually does. What stops a refusal from going
 * round for ever is that every such path reaches its own deadline or its own
 * refusal, and both of those report the session over.
 *
 * The reason reported is the one that caller's own deadline uses. The host
 * protocol has three reasons -- success, timeout and paged -- and none of them
 * means "the scheduler was full". */
int rx_task_schedule(unsigned delay, rx_task_cb_t cb, void*cbarg, unsigned flags);
void rx_task_reset(void);
#endif
