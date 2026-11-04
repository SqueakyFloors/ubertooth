#ifndef __SCAN_TASK_H
#define __SCAN_TASK_H

typedef void (*scan_task_cb_t)(int sw_detected, void *arg);

/* Arm the next listening slot, in the slot after this one. Returns 0 when it
 * is armed and -1 when the scheduler had no room for it.
 *
 * On -1 nothing is armed and the callback is never reached, so a caller that
 * carried on would sit in its scan state with no task left to run. End what
 * the scan was for instead. */
int scan_task_schedule(scan_task_cb_t cb, void*cbarg);

#endif
