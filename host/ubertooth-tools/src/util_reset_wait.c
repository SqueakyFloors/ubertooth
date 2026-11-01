/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Waiting for an Ubertooth to come back after a reset; see util_reset_wait.h.
 */
#include "util_reset_wait.h"

#include <stddef.h>
#include <string.h>

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

/* True if a -U argument selects by serial number rather than by index, the
 * same split libubertooth's classify_selector() makes (an all-digits argument
 * is an index). This module stays free of the library, so it only decides
 * which of the two a string is, never which device it selects. */
static int selects_by_serial(const char *u_arg)
{
	if (u_arg == NULL || u_arg[0] == '\0')
		return 0;
	return u_arg[strspn(u_arg, "0123456789")] != '\0';
}

void util_reset_follow_init(struct util_reset_follow *follow,
                            const struct util_reset_follow_ops *ops,
                            void *context, const char *u_arg, int attached)
{
	follow->serial[0] = '\0';
	/* A string of digits would be taken for an index, so only a serial that
	 * selects as one is followed. Nothing is lost: the fallback below is what
	 * the command did before. */
	if (ops->get_serial(context, follow->serial, sizeof(follow->serial)) == 0 &&
	    selects_by_serial(follow->serial)) {
		/* A serial matches one board wherever it enumerates, so the wait does
		 * not need the device count to open the right one. */
		follow->selector = follow->serial;
		follow->select_needs_count = 0;
		return;
	}
	follow->serial[0] = '\0';
	follow->selector = u_arg;
	follow->select_needs_count = !selects_by_serial(u_arg);
	/* Only a selection that can come back as another board is worth a warning:
	 * an index or the default device, with another board there to be picked. */
	if (follow->select_needs_count && attached != 1)
		ops->warn(context, u_arg);
}
