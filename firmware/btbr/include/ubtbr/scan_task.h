#ifndef __SCAN_TASK_H
#define __SCAN_TASK_H

typedef void (*scan_task_cb_t)(int sw_detected, void *arg);

/* Arm the next listening slot, in the slot after this one. Returns 0 when it
 * is armed and -1 when the scheduler had no room for it, which it has not when
 * that slot already holds its four tasks.
 *
 * On -1 nothing is listening and the callback is never reached, because the
 * task that reaches it is the one that was refused. So a caller that carried
 * on would sit in its scan state with no task left to run, and not even its
 * own deadline would end the session: that is read where the next window is
 * opened. Report the session over instead.
 *
 * Asking again does not help within one slot. The slot asked for is the one
 * that was refused, and a slot is emptied only by running, so a second ask
 * from the same slot clock interrupt is refused too. That is what bounds the
 * refusal: one report, then the session ends.
 *
 * The caller's own console line is not needed either way: this says what was
 * refused. */
int scan_task_schedule(scan_task_cb_t cb, void*cbarg);

#endif
