/*
 * Waiting for an Ubertooth to come back after a reset; see util_reset_wait.h.
 */
#include "util_reset_wait.h"

#include <stddef.h>

/* Sleeps for the poll interval, but not past the deadline. */
static void sleep_until_next(const struct util_reset_wait_ops *ops, void *context,
                             uint64_t elapsed, unsigned limit_ms, unsigned poll_ms)
{
	uint64_t left = limit_ms > elapsed ? limit_ms - elapsed : 0;
	unsigned ms = poll_ms;

	if (left < ms)
		ms = (unsigned)left;
	if (ms == 0)
		ms = 1; /* always make progress on a coarse clock */
	ops->sleep_ms(context, ms);
}

void *util_reset_wait(const struct util_reset_wait_ops *ops, void *context,
                      const struct util_reset_wait_params *params)
{
	uint64_t start = ops->now_ms(context);
	uint64_t elapsed;
	int gate = params->select_needs_count && params->count_before > 0;

	if (params->initial_delay_ms > 0)
		ops->sleep_ms(context, params->initial_delay_ms);

	/* Wait for the old device to leave, so it is not opened just before it
	 * drops off the bus. If it is never seen leaving (it came back between
	 * two polls, or never left), carry on after leave_ms. */
	if (params->count_before > 0) {
		for (;;) {
			int count = ops->count(context);

			if (count >= 0 && count < params->count_before)
				break;
			elapsed = ops->now_ms(context) - start;
			if (elapsed >= params->leave_ms || elapsed >= params->deadline_ms)
				break;
			sleep_until_next(ops, context, elapsed, params->leave_ms,
			                 params->poll_ms);
		}
	}

	for (;;) {
		void *device = NULL;

		int count = gate ? ops->count(context) : 0;

		/* An unknown count (an error) does not block the selection. */
		if (!gate || count < 0 || count >= params->count_before)
			device = ops->try_select(context);
		if (device != NULL)
			return device;
		elapsed = ops->now_ms(context) - start;
		if (elapsed >= params->deadline_ms)
			return NULL;
		sleep_until_next(ops, context, elapsed, params->deadline_ms,
		                 params->poll_ms);
	}
}
