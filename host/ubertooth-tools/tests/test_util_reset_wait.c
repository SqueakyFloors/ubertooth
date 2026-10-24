/*
 * Hardware-independent tests of the wait for an Ubertooth to come back after a
 * reset, with a scripted clock, device count and selection.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../../firmware/tests/minunit.h"
#include "util_reset_wait.h"

/* A scripted bus: the reset device is present until leaves_at_ms, absent until
 * returns_at_ms, and present again after that. Other boards stay attached. */
struct fake_bus {
	uint64_t now;
	uint64_t leaves_at_ms;  /* UINT64_MAX: never leaves */
	uint64_t returns_at_ms; /* UINT64_MAX: never returns */
	int other_boards;
	int count_error;        /* count() fails instead of counting */
	int select_fails;       /* the device is back but cannot be opened */
	/* bookkeeping */
	int sleeps, counts, selects;
	unsigned longest_sleep;
	uint64_t first_select_at;
	int selects_while_absent;
	int device; /* what try_select() returns */
};

static int present(const struct fake_bus *bus)
{
	return bus->now < bus->leaves_at_ms || bus->now >= bus->returns_at_ms;
}

static uint64_t fake_now(void *context)
{
	return ((struct fake_bus *)context)->now;
}

static void fake_sleep(void *context, unsigned ms)
{
	struct fake_bus *bus = context;

	bus->sleeps++;
	if (ms > bus->longest_sleep)
		bus->longest_sleep = ms;
	bus->now += ms;
}

static int fake_count(void *context)
{
	struct fake_bus *bus = context;

	bus->counts++;
	if (bus->count_error)
		return -1;
	return bus->other_boards + (present(bus) ? 1 : 0);
}

static void *fake_try_select(void *context)
{
	struct fake_bus *bus = context;

	if (bus->selects++ == 0)
		bus->first_select_at = bus->now;
	if (!present(bus)) {
		bus->selects_while_absent++;
		return NULL;
	}
	if (bus->now < bus->leaves_at_ms && bus->leaves_at_ms != UINT64_MAX)
		bus->selects_while_absent++; /* the old device, about to leave */
	if (bus->select_fails)
		return NULL;
	return &bus->device;
}

static const struct util_reset_wait_ops ops = {
	fake_now, fake_sleep, fake_count, fake_try_select,
};

static struct fake_bus make_bus(uint64_t leaves, uint64_t returns)
{
	struct fake_bus bus;

	memset(&bus, 0, sizeof(bus));
	bus.now = 5000; /* an arbitrary clock origin */
	bus.leaves_at_ms = bus.now + leaves;
	bus.returns_at_ms = bus.now + returns;
	return bus;
}

static struct util_reset_wait_params defaults(int count_before, int needs_count)
{
	struct util_reset_wait_params params = {
		UTIL_RESET_WAIT_INITIAL_DELAY_MS, UTIL_RESET_WAIT_POLL_MS,
		UTIL_RESET_WAIT_LEAVE_MS, UTIL_RESET_WAIT_DEADLINE_MS,
		count_before, needs_count
	};
	return params;
}

MU_TEST(defaults_are_short_polls_with_a_ten_second_deadline)
{
	mu_assert_int_eq(100, UTIL_RESET_WAIT_INITIAL_DELAY_MS);
	mu_assert_int_eq(100, UTIL_RESET_WAIT_POLL_MS);
	mu_assert_int_eq(3000, UTIL_RESET_WAIT_LEAVE_MS);
	mu_assert_int_eq(10000, UTIL_RESET_WAIT_DEADLINE_MS);
}

MU_TEST(a_quick_reenumeration_is_selected_soon_after_it_returns)
{
	struct fake_bus bus = make_bus(50, 700);
	struct util_reset_wait_params params = defaults(1, 1);
	void *device = util_reset_wait(&ops, &bus, &params);

	mu_assert(device == &bus.device, "device selected");
	mu_assert_int_eq(0, bus.selects_while_absent);
	/* selected on the first poll at or after its return */
	mu_assert(bus.now >= bus.returns_at_ms && bus.now < bus.returns_at_ms + 100,
	          "selected within one poll of returning");
	mu_assert_int_eq(100, bus.longest_sleep);
}

MU_TEST(a_slow_reenumeration_is_waited_for_beyond_the_old_two_seconds)
{
	struct fake_bus bus = make_bus(1000, 6500);
	struct util_reset_wait_params params = defaults(1, 1);
	void *device = util_reset_wait(&ops, &bus, &params);

	mu_assert(device == &bus.device, "slow device still selected");
	mu_assert_int_eq(0, bus.selects_while_absent);
	mu_assert(bus.now - 5000 >= 6500 && bus.now - 5000 < 6600, "selected on return");
}

MU_TEST(the_old_device_is_not_opened_before_it_leaves)
{
	/* the firmware keeps the old device on the bus for a while */
	struct fake_bus bus = make_bus(1200, 2000);
	struct util_reset_wait_params params = defaults(1, 1);

	mu_assert(util_reset_wait(&ops, &bus, &params) == &bus.device, "selected");
	mu_assert(bus.first_select_at >= bus.leaves_at_ms, "no select before it left");
	mu_assert_int_eq(0, bus.selects_while_absent);
}

MU_TEST(a_device_never_seen_leaving_is_selected_after_the_leave_window)
{
	struct fake_bus bus = make_bus(0, 0);
	struct util_reset_wait_params params = defaults(1, 1);

	bus.leaves_at_ms = UINT64_MAX; /* for example, the reset did not happen */
	bus.returns_at_ms = UINT64_MAX;
	mu_assert(util_reset_wait(&ops, &bus, &params) == &bus.device, "selected");
	mu_assert_int_eq((int)(5000 + UTIL_RESET_WAIT_LEAVE_MS), (int)bus.first_select_at);
}

MU_TEST(no_leave_wait_when_the_count_before_is_unknown)
{
	struct fake_bus bus = make_bus(0, 0);
	struct util_reset_wait_params params = defaults(0, 1);

	bus.leaves_at_ms = UINT64_MAX;
	mu_assert(util_reset_wait(&ops, &bus, &params) == &bus.device, "selected");
	mu_assert_int_eq(0, bus.counts);
	mu_assert_int_eq((int)(5000 + UTIL_RESET_WAIT_INITIAL_DELAY_MS),
	                 (int)bus.first_select_at);
}

MU_TEST(selection_by_index_waits_for_every_board_to_be_back)
{
	/* With another board attached, opening index 0 while the reset board
	 * is gone would open the other board. */
	struct fake_bus bus = make_bus(100, 3000);
	struct util_reset_wait_params params = defaults(2, 1);

	bus.other_boards = 1;
	mu_assert(util_reset_wait(&ops, &bus, &params) == &bus.device, "selected");
	mu_assert_int_eq(0, bus.selects_while_absent);
	mu_assert(bus.first_select_at >= bus.returns_at_ms, "no select while gone");
}

MU_TEST(selection_by_serial_polls_without_waiting_for_the_count)
{
	struct fake_bus bus = make_bus(100, 3000);
	struct util_reset_wait_params params = defaults(2, 0);

	bus.other_boards = 1;
	mu_assert(util_reset_wait(&ops, &bus, &params) == &bus.device, "selected");
	/* polled (and missed) while gone: a serial cannot match another board */
	mu_assert(bus.selects_while_absent > 0, "polled while absent");
	mu_assert(bus.now >= bus.returns_at_ms && bus.now < bus.returns_at_ms + 100,
	          "selected within one poll of returning");
}

MU_TEST(a_count_error_is_not_taken_as_the_device_leaving)
{
	struct fake_bus bus = make_bus(0, 0);
	struct util_reset_wait_params params = defaults(1, 0);

	bus.count_error = 1;
	mu_assert(util_reset_wait(&ops, &bus, &params) == &bus.device, "selected");
	mu_assert_int_eq((int)(5000 + UTIL_RESET_WAIT_LEAVE_MS), (int)bus.first_select_at);
}

MU_TEST(a_count_error_does_not_block_selection_by_index)
{
	struct fake_bus bus = make_bus(0, 0);
	struct util_reset_wait_params params = defaults(1, 1);

	bus.count_error = 1;
	mu_assert(util_reset_wait(&ops, &bus, &params) == &bus.device, "selected");
	mu_assert_int_eq((int)(5000 + UTIL_RESET_WAIT_LEAVE_MS), (int)bus.first_select_at);
}

MU_TEST(a_device_that_never_returns_times_out_at_the_deadline)
{
	struct fake_bus bus = make_bus(100, 0);
	struct util_reset_wait_params params = defaults(1, 0);

	bus.returns_at_ms = UINT64_MAX;
	mu_assert(util_reset_wait(&ops, &bus, &params) == NULL, "timed out");
	/* the last attempt is made exactly at the deadline, not after it */
	mu_assert_int_eq((int)(5000 + UTIL_RESET_WAIT_DEADLINE_MS), (int)bus.now);
	mu_assert_int_eq(100, bus.longest_sleep);
	/* about one attempt per poll, not a busy loop */
	mu_assert(bus.selects >= 95 && bus.selects <= 100, "one attempt per poll");
}

MU_TEST(a_device_that_returns_but_cannot_be_opened_times_out)
{
	struct fake_bus bus = make_bus(100, 500);
	struct util_reset_wait_params params = defaults(1, 1);

	bus.select_fails = 1;
	mu_assert(util_reset_wait(&ops, &bus, &params) == NULL, "timed out");
	mu_assert_int_eq((int)(5000 + UTIL_RESET_WAIT_DEADLINE_MS), (int)bus.now);
}

MU_TEST(polls_never_sleep_past_an_uneven_deadline)
{
	struct fake_bus bus = make_bus(10, 0);
	struct util_reset_wait_params params = { 0, 300, 100, 1000, 1, 0 };

	bus.returns_at_ms = UINT64_MAX;
	mu_assert(util_reset_wait(&ops, &bus, &params) == NULL, "timed out");
	mu_assert_int_eq(5000 + 1000, (int)bus.now);
}

int main(void)
{
	MU_RUN(defaults_are_short_polls_with_a_ten_second_deadline);
	MU_RUN(a_quick_reenumeration_is_selected_soon_after_it_returns);
	MU_RUN(a_slow_reenumeration_is_waited_for_beyond_the_old_two_seconds);
	MU_RUN(the_old_device_is_not_opened_before_it_leaves);
	MU_RUN(a_device_never_seen_leaving_is_selected_after_the_leave_window);
	MU_RUN(no_leave_wait_when_the_count_before_is_unknown);
	MU_RUN(selection_by_index_waits_for_every_board_to_be_back);
	MU_RUN(selection_by_serial_polls_without_waiting_for_the_count);
	MU_RUN(a_count_error_is_not_taken_as_the_device_leaving);
	MU_RUN(a_count_error_does_not_block_selection_by_index);
	MU_RUN(a_device_that_never_returns_times_out_at_the_deadline);
	MU_RUN(a_device_that_returns_but_cannot_be_opened_times_out);
	MU_RUN(polls_never_sleep_past_an_uneven_deadline);
	return mu_report();
}
