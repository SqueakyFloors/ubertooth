/*
 * Hardware-independent tests of the wait for an Ubertooth to come back after a
 * reset, with a scripted clock, device count and selection.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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

/* ---- following the board that was reset ------------------------------- */

/*
 * Two boards whose enumeration order swaps across the reset: the board that
 * is reset sits at index 0 before it and at index 1 after it, the other board
 * the other way round. Selection resolves whatever selector the wait was
 * given, as the library does: an index against the order of the moment, a
 * serial (or a prefix of one) against the board itself.
 */
#define RESET_SERIAL "aa11aa22aa33aa44aa55aa66aa77aa88"
#define OTHER_SERIAL "bb11bb22bb33bb44bb55bb66bb77bb88"
#define BOARD_RESET 0 /* indices into fake_swap.boards[] */
#define BOARD_OTHER 1

struct fake_swap {
	uint64_t now;
	uint64_t leaves_at_ms, returns_at_ms;
	const char *selector;  /* what the wait is following */
	const char *serial_read; /* what the read reports (NULL: RESET_SERIAL) */
	int serial_read_fails; /* the read before the reset fails */
	int warnings;
	int opened;            /* the board that was opened, or -1 */
	int boards[2];         /* what try_select() returns for each board */
};

static struct fake_swap make_swap(void)
{
	struct fake_swap bus;

	memset(&bus, 0, sizeof(bus));
	bus.now = 5000;
	bus.leaves_at_ms = bus.now + 100;
	bus.returns_at_ms = bus.now + 2000;
	bus.opened = -1;
	return bus;
}

/* The board at index now, or -1 if there is none. */
static int swap_board_at(const struct fake_swap *bus, int index)
{
	int back = bus->now >= bus->returns_at_ms;
	int gone = bus->now >= bus->leaves_at_ms && !back;

	if (index < 0 || index > 1)
		return -1;
	if (gone) /* only the other board is attached */
		return index == 0 ? BOARD_OTHER : -1;
	if (back) /* the reset board came back behind the other one */
		return index == 0 ? BOARD_OTHER : BOARD_RESET;
	return index == 0 ? BOARD_RESET : BOARD_OTHER;
}

static int swap_present(const struct fake_swap *bus, int board)
{
	return swap_board_at(bus, 0) == board || swap_board_at(bus, 1) == board;
}

static uint64_t swap_now(void *context)
{
	return ((struct fake_swap *)context)->now;
}

static void swap_sleep(void *context, unsigned ms)
{
	((struct fake_swap *)context)->now += ms;
}

static int swap_count(void *context)
{
	struct fake_swap *bus = context;

	return (swap_board_at(bus, 0) >= 0) + (swap_board_at(bus, 1) >= 0);
}

static void *swap_try_select(void *context)
{
	struct fake_swap *bus = context;
	const char *selector = bus->selector;
	int board = -1;

	if (selector == NULL || selector[0] == '\0')
		board = swap_count(bus) == 1 ? swap_board_at(bus, 0) : -1;
	else if (selector[strspn(selector, "0123456789")] == '\0')
		board = swap_board_at(bus, atoi(selector));
	else if (swap_present(bus, BOARD_RESET) &&
	         strncmp(selector, RESET_SERIAL, strlen(selector)) == 0)
		board = BOARD_RESET;
	else if (swap_present(bus, BOARD_OTHER) &&
	         strncmp(selector, OTHER_SERIAL, strlen(selector)) == 0)
		board = BOARD_OTHER;
	if (board < 0)
		return NULL;
	bus->opened = board;
	return &bus->boards[board];
}

static const struct util_reset_wait_ops swap_ops = {
	swap_now, swap_sleep, swap_count, swap_try_select,
};

static int swap_get_serial(void *context, char *serial, size_t size)
{
	struct fake_swap *bus = context;

	if (bus->serial_read_fails)
		return -1;
	snprintf(serial, size, "%s", bus->serial_read ? bus->serial_read
	                                              : RESET_SERIAL);
	return 0;
}

static void swap_warn(void *context, const char *u_arg)
{
	(void)u_arg;
	((struct fake_swap *)context)->warnings++;
}

static const struct util_reset_follow_ops follow_ops = {
	swap_get_serial, swap_warn,
};

/* What ubertooth-util -r does: choose what to follow with the device still
 * open, then wait for it with both boards counted before the reset. */
static void *swap_run(struct fake_swap *bus, const char *u_arg,
                      struct util_reset_follow *follow)
{
	struct util_reset_wait_params params = defaults(2, 0);

	util_reset_follow_init(follow, &follow_ops, bus, u_arg, swap_count(bus));
	params.select_needs_count = follow->select_needs_count;
	bus->selector = follow->selector;
	return util_reset_wait(&swap_ops, bus, &params);
}

MU_TEST(an_index_follows_the_board_that_was_reset_across_a_swap)
{
	struct fake_swap bus = make_swap();
	struct util_reset_follow follow;
	void *device = swap_run(&bus, "0", &follow);

	mu_assert(strcmp(follow.selector, RESET_SERIAL) == 0, "follows the serial");
	mu_assert_int_eq(0, follow.select_needs_count);
	mu_assert_int_eq(0, bus.warnings);
	mu_assert(device == &bus.boards[BOARD_RESET], "the board that was reset");
	mu_assert(bus.now >= bus.returns_at_ms, "waited for it to come back");
}

MU_TEST(a_serial_that_cannot_be_read_keeps_the_index_and_warns)
{
	struct fake_swap bus = make_swap();
	struct util_reset_follow follow;
	void *device;

	bus.serial_read_fails = 1;
	device = swap_run(&bus, "0", &follow);
	mu_assert(strcmp(follow.selector, "0") == 0, "the index is kept");
	mu_assert_int_eq(1, follow.select_needs_count);
	mu_assert_int_eq(1, bus.warnings);
	/* what the warning is about: index 0 is the other board now */
	mu_assert(device == &bus.boards[BOARD_OTHER], "the other board");
	mu_assert(bus.now >= bus.returns_at_ms, "waited for both boards");
}

MU_TEST(a_serial_selector_follows_the_same_board)
{
	struct fake_swap bus = make_swap();
	struct util_reset_follow follow;
	void *device = swap_run(&bus, "aa11", &follow);

	/* the prefix the user gave becomes the whole serial of that board */
	mu_assert(strcmp(follow.selector, RESET_SERIAL) == 0, "follows the serial");
	mu_assert_int_eq(0, follow.select_needs_count);
	mu_assert_int_eq(0, bus.warnings);
	mu_assert(device == &bus.boards[BOARD_RESET], "the board that was reset");

	/* and with no serial to read, the prefix is kept: it is already the
	 * board, so nothing is lost and nothing is warned about */
	bus = make_swap();
	bus.serial_read_fails = 1;
	device = swap_run(&bus, "aa11", &follow);
	mu_assert(strcmp(follow.selector, "aa11") == 0, "the prefix is kept");
	mu_assert_int_eq(0, follow.select_needs_count);
	mu_assert_int_eq(0, bus.warnings);
	mu_assert(device == &bus.boards[BOARD_RESET], "the board that was reset");
}

MU_TEST(the_default_selection_is_kept_when_the_serial_cannot_be_read)
{
	struct fake_swap bus = make_swap();
	struct util_reset_follow follow;

	/* No -U: the index gate and the warning apply as they do to an index. */
	bus.serial_read_fails = 1;
	mu_assert(swap_run(&bus, NULL, &follow) == NULL, "two boards, no selection");
	mu_assert(follow.selector == NULL, "no selector to follow");
	mu_assert_int_eq(1, follow.select_needs_count);
	mu_assert_int_eq(1, bus.warnings);
}

MU_TEST(a_serial_that_would_read_as_an_index_is_not_followed)
{
	struct fake_swap bus = make_swap();
	struct util_reset_follow follow;

	/* A selection is a serial or an index, so a serial of nothing but digits
	 * cannot be followed: it would select index 11223344... instead, which no
	 * board has. The selector is kept, as for a read that failed. */
	bus.serial_read = "11223344112233441122334411223344";
	mu_assert(swap_run(&bus, "0", &follow) == &bus.boards[BOARD_OTHER],
	          "the index, as before this fix");
	mu_assert(strcmp(follow.selector, "0") == 0, "the index is kept");
	mu_assert_int_eq(1, follow.select_needs_count);
	mu_assert_int_eq(1, bus.warnings);
}

MU_TEST(nothing_is_warned_about_with_one_board_attached)
{
	struct fake_swap bus = make_swap();
	struct util_reset_follow follow;

	/* No other board can be opened instead, so a serial that cannot be read
	 * costs nothing. */
	bus.serial_read_fails = 1;
	util_reset_follow_init(&follow, &follow_ops, &bus, "0", 1);
	mu_assert(strcmp(follow.selector, "0") == 0, "the index is kept");
	mu_assert_int_eq(1, follow.select_needs_count);
	mu_assert_int_eq(0, bus.warnings);

	/* A count that failed rules nothing out, so it does warn. */
	bus.warnings = 0;
	util_reset_follow_init(&follow, &follow_ops, &bus, "0", -1);
	mu_assert_int_eq(1, bus.warnings);
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
	MU_RUN(an_index_follows_the_board_that_was_reset_across_a_swap);
	MU_RUN(a_serial_that_cannot_be_read_keeps_the_index_and_warns);
	MU_RUN(a_serial_selector_follows_the_same_board);
	MU_RUN(the_default_selection_is_kept_when_the_serial_cannot_be_read);
	MU_RUN(a_serial_that_would_read_as_an_index_is_not_followed);
	MU_RUN(nothing_is_warned_about_with_one_board_attached);
	return mu_report();
}
