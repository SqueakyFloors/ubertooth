/*
 * Monotonic clock and sleep for ubertooth-util; see util_clock.h.
 */
#define _POSIX_C_SOURCE 200809L

#include "util_clock.h"

#include <errno.h>
#include <time.h>

uint64_t util_clock_now_ms(void)
{
	struct timespec now;

	/* Fall back to the wall clock rather than a constant, so a wait with a
	 * deadline still ends. */
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return (uint64_t)time(NULL) * 1000u;
	return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

void util_clock_sleep_ms(unsigned ms)
{
	struct timespec request, remaining;

	request.tv_sec = ms / 1000u;
	request.tv_nsec = (long)(ms % 1000u) * 1000000L;
	while (nanosleep(&request, &remaining) != 0 && errno == EINTR)
		request = remaining;
}
