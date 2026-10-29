/*
 * Tests of what ubertooth-follow -a makes of an AFH map read from a remote
 * device: the channel count, the mode and map validation, the bounded retry,
 * the resulting action and the message. No BlueZ and no device, so these run
 * everywhere, including where ubertooth-follow itself is not built.
 */
#include <stdio.h>
#include <string.h>

#include "../../../firmware/tests/minunit.h"
#include "follow_afh.h"

/* A map with the first n channels enabled. */
static void map_first(uint8_t *map, int n)
{
	int channel;

	memset(map, 0, FOLLOW_AFH_MAP_BYTES);
	for (channel = 0; channel < n; channel++)
		map[channel / 8] |= (uint8_t)(1 << (channel % 8));
}

/* A scripted reader: one entry per attempt. */
struct scripted_read {
	int result;    /* what the read returns */
	uint8_t mode;
	int channels;  /* channels to enable in the map it hands back */
};

struct script {
	const struct scripted_read *reads;
	int count;
	int calls;
	int waits;
};

static int script_read(void *ctx, uint8_t *mode, uint8_t *map)
{
	struct script *s = ctx;
	const struct scripted_read *r;

	/* mu_assert records and carries on, so this returns rather than reading
	 * past the script. */
	mu_assert(s->calls < s->count, "read called more times than scripted");
	if (s->calls >= s->count)
		return -1;
	r = &s->reads[s->calls++];
	if (r->result < 0)
		return r->result;
	*mode = r->mode;
	map_first(map, r->channels);
	return r->result;
}

static void script_wait(void *ctx)
{
	struct script *s = ctx;

	s->waits++;
}

static void run_script(struct script *s, const struct scripted_read *reads,
                       int count, int attempts, follow_afh_read *out)
{
	follow_afh_reader reader;

	s->reads = reads;
	s->count = count;
	s->calls = 0;
	s->waits = 0;
	reader.read = script_read;
	reader.wait = script_wait;
	reader.ctx = s;
	follow_afh_read_map(&reader, attempts, out);
}

MU_TEST(counts_the_79_real_channels_only)
{
	uint8_t map[FOLLOW_AFH_MAP_BYTES];

	mu_assert_int_eq(0, follow_afh_count_used(NULL));

	memset(map, 0, sizeof(map));
	mu_assert_int_eq(0, follow_afh_count_used(map));

	memset(map, 0xff, sizeof(map));
	/* 80 bits are set, but only 79 are channels. */
	mu_assert_int_eq(FOLLOW_AFH_CHANNELS, follow_afh_count_used(map));

	map_first(map, 20);
	mu_assert_int_eq(20, follow_afh_count_used(map));

	/* One channel in the middle of a byte. */
	memset(map, 0, sizeof(map));
	map[4] = 1 << 3;
	mu_assert_int_eq(1, follow_afh_count_used(map));
}

MU_TEST(a_failed_read_never_looks_at_the_mode_or_the_map)
{
	uint8_t map[FOLLOW_AFH_MAP_BYTES];

	/* A full, perfectly good-looking map alongside a failed read is still
	 * a failed read: before this change the mode decided. */
	memset(map, 0xff, sizeof(map));
	mu_assert_int_eq(FOLLOW_AFH_READ_FAILED,
	                 follow_afh_classify(-1, FOLLOW_AFH_MODE_ON, map));
	mu_assert_int_eq(FOLLOW_AFH_READ_FAILED,
	                 follow_afh_classify(-1, FOLLOW_AFH_MODE_ON, NULL));
	mu_assert_int_eq(FOLLOW_AFH_READ_FAILED,
	                 follow_afh_classify(-99, 0x7f, map));
}

MU_TEST(the_mode_byte_is_validated)
{
	uint8_t map[FOLLOW_AFH_MAP_BYTES];

	memset(map, 0xff, sizeof(map));
	mu_assert_int_eq(FOLLOW_AFH_MODE_DISABLED,
	                 follow_afh_classify(0, FOLLOW_AFH_MODE_OFF, map));
	mu_assert_int_eq(FOLLOW_AFH_OK,
	                 follow_afh_classify(0, FOLLOW_AFH_MODE_ON, map));
	/* Anything else is not an answer we can act on. */
	mu_assert_int_eq(FOLLOW_AFH_MODE_INVALID,
	                 follow_afh_classify(0, 0x02, map));
	mu_assert_int_eq(FOLLOW_AFH_MODE_INVALID,
	                 follow_afh_classify(0, 0xff, map));
}

MU_TEST(an_enabled_map_needs_twenty_channels)
{
	uint8_t map[FOLLOW_AFH_MAP_BYTES];

	map_first(map, 0);
	mu_assert_int_eq(FOLLOW_AFH_MAP_EMPTY,
	                 follow_afh_classify(0, FOLLOW_AFH_MODE_ON, map));
	map_first(map, 1);
	mu_assert_int_eq(FOLLOW_AFH_TOO_FEW_CHANNELS,
	                 follow_afh_classify(0, FOLLOW_AFH_MODE_ON, map));
	map_first(map, FOLLOW_AFH_MIN_USED_CHANNELS - 1);
	mu_assert_int_eq(FOLLOW_AFH_TOO_FEW_CHANNELS,
	                 follow_afh_classify(0, FOLLOW_AFH_MODE_ON, map));
	map_first(map, FOLLOW_AFH_MIN_USED_CHANNELS);
	mu_assert_int_eq(FOLLOW_AFH_OK,
	                 follow_afh_classify(0, FOLLOW_AFH_MODE_ON, map));
	map_first(map, FOLLOW_AFH_CHANNELS);
	mu_assert_int_eq(FOLLOW_AFH_OK,
	                 follow_afh_classify(0, FOLLOW_AFH_MODE_ON, map));
}

MU_TEST(only_a_transient_map_is_retried)
{
	mu_assert_int_eq(0, follow_afh_should_retry(FOLLOW_AFH_OK));
	mu_assert_int_eq(0, follow_afh_should_retry(FOLLOW_AFH_READ_FAILED));
	mu_assert_int_eq(0, follow_afh_should_retry(FOLLOW_AFH_MODE_DISABLED));
	mu_assert_int_eq(0, follow_afh_should_retry(FOLLOW_AFH_MODE_INVALID));
	mu_assert_int_eq(1, follow_afh_should_retry(FOLLOW_AFH_MAP_EMPTY));
	mu_assert_int_eq(1, follow_afh_should_retry(FOLLOW_AFH_TOO_FEW_CHANNELS));
}

MU_TEST(a_usable_map_is_read_once_and_copied_out)
{
	static const struct scripted_read reads[] = {
		{ 0, FOLLOW_AFH_MODE_ON, 40 },
	};
	uint8_t expected[FOLLOW_AFH_MAP_BYTES];
	follow_afh_read out;
	struct script s;

	run_script(&s, reads, 1, FOLLOW_AFH_READ_ATTEMPTS, &out);
	mu_assert_int_eq(FOLLOW_AFH_OK, out.result);
	mu_assert_int_eq(1, out.attempts);
	mu_assert_int_eq(1, s.calls);
	mu_assert_int_eq(0, s.waits);
	mu_assert_int_eq(40, out.used);
	mu_assert_int_eq(FOLLOW_AFH_MODE_ON, out.mode);
	map_first(expected, 40);
	mu_assert(memcmp(expected, out.map, sizeof(expected)) == 0,
	          "the map handed back is the one that was read");
	mu_assert_int_eq(FOLLOW_AFH_ACTION_APPLY,
	                 follow_afh_action_for(out.result));
}

MU_TEST(a_read_failure_stops_at_once_and_leaves_no_map)
{
	static const struct scripted_read reads[] = {
		{ -1, FOLLOW_AFH_MODE_ON, FOLLOW_AFH_CHANNELS },
	};
	uint8_t zero[FOLLOW_AFH_MAP_BYTES] = { 0 };
	follow_afh_read out;
	struct script s;

	run_script(&s, reads, 1, FOLLOW_AFH_READ_ATTEMPTS, &out);
	mu_assert_int_eq(FOLLOW_AFH_READ_FAILED, out.result);
	/* Not retried, and nothing was waited for. */
	mu_assert_int_eq(1, s.calls);
	mu_assert_int_eq(0, s.waits);
	mu_assert_int_eq(1, out.attempts);
	mu_assert_int_eq(0, out.used);
	mu_assert_int_eq(0, out.mode);
	mu_assert(memcmp(zero, out.map, sizeof(zero)) == 0,
	          "a failed read leaves the map zero");
	mu_assert_int_eq(FOLLOW_AFH_ACTION_FAIL,
	                 follow_afh_action_for(out.result));
}

MU_TEST(a_remote_with_afh_off_is_followed_without_it)
{
	static const struct scripted_read reads[] = {
		{ 0, FOLLOW_AFH_MODE_OFF, 0 },
	};
	uint8_t zero[FOLLOW_AFH_MAP_BYTES] = { 0 };
	follow_afh_read out;
	struct script s;

	run_script(&s, reads, 1, FOLLOW_AFH_READ_ATTEMPTS, &out);
	mu_assert_int_eq(FOLLOW_AFH_MODE_DISABLED, out.result);
	mu_assert_int_eq(1, s.calls);
	mu_assert_int_eq(0, s.waits);
	mu_assert(memcmp(zero, out.map, sizeof(zero)) == 0, "no map applied");
	mu_assert_int_eq(FOLLOW_AFH_ACTION_DISABLE,
	                 follow_afh_action_for(out.result));
}

MU_TEST(an_empty_map_is_retried_to_the_bound_then_refused)
{
	static const struct scripted_read reads[] = {
		{ 0, FOLLOW_AFH_MODE_ON, 0 },
		{ 0, FOLLOW_AFH_MODE_ON, 0 },
		{ 0, FOLLOW_AFH_MODE_ON, 0 },
	};
	follow_afh_read out;
	struct script s;

	run_script(&s, reads, 3, FOLLOW_AFH_READ_ATTEMPTS, &out);
	mu_assert_int_eq(FOLLOW_AFH_MAP_EMPTY, out.result);
	/* Bounded: exactly the attempts asked for, and no wait after the last. */
	mu_assert_int_eq(FOLLOW_AFH_READ_ATTEMPTS, s.calls);
	mu_assert_int_eq(FOLLOW_AFH_READ_ATTEMPTS - 1, s.waits);
	mu_assert_int_eq(FOLLOW_AFH_READ_ATTEMPTS, out.attempts);
	mu_assert_int_eq(FOLLOW_AFH_ACTION_FAIL,
	                 follow_afh_action_for(out.result));
}

MU_TEST(nineteen_channels_are_retried_then_refused)
{
	static const struct scripted_read reads[] = {
		{ 0, FOLLOW_AFH_MODE_ON, 19 },
		{ 0, FOLLOW_AFH_MODE_ON, 19 },
		{ 0, FOLLOW_AFH_MODE_ON, 19 },
	};
	follow_afh_read out;
	struct script s;

	run_script(&s, reads, 3, FOLLOW_AFH_READ_ATTEMPTS, &out);
	mu_assert_int_eq(FOLLOW_AFH_TOO_FEW_CHANNELS, out.result);
	mu_assert_int_eq(FOLLOW_AFH_READ_ATTEMPTS, s.calls);
	mu_assert_int_eq(19, out.used);
	mu_assert_int_eq(FOLLOW_AFH_ACTION_FAIL,
	                 follow_afh_action_for(out.result));
}

MU_TEST(a_transient_map_settles_across_a_retry)
{
	static const struct scripted_read reads[] = {
		{ 0, FOLLOW_AFH_MODE_ON, 0 },
		{ 0, FOLLOW_AFH_MODE_ON, 19 },
		{ 0, FOLLOW_AFH_MODE_ON, 37 },
	};
	uint8_t expected[FOLLOW_AFH_MAP_BYTES];
	follow_afh_read out;
	struct script s;

	run_script(&s, reads, 3, FOLLOW_AFH_READ_ATTEMPTS, &out);
	mu_assert_int_eq(FOLLOW_AFH_OK, out.result);
	mu_assert_int_eq(3, s.calls);
	mu_assert_int_eq(2, s.waits);
	mu_assert_int_eq(3, out.attempts);
	mu_assert_int_eq(37, out.used);
	map_first(expected, 37);
	mu_assert(memcmp(expected, out.map, sizeof(expected)) == 0,
	          "the settled map is the one applied");
}

MU_TEST(a_mode_that_turns_off_mid_retry_is_not_retried_further)
{
	static const struct scripted_read reads[] = {
		{ 0, FOLLOW_AFH_MODE_ON, 0 },
		{ 0, FOLLOW_AFH_MODE_OFF, 0 },
	};
	follow_afh_read out;
	struct script s;

	run_script(&s, reads, 2, FOLLOW_AFH_READ_ATTEMPTS, &out);
	mu_assert_int_eq(FOLLOW_AFH_MODE_DISABLED, out.result);
	mu_assert_int_eq(2, s.calls);
	mu_assert_int_eq(1, s.waits);
}

MU_TEST(the_attempt_count_is_clamped_to_at_least_one)
{
	static const struct scripted_read reads[] = {
		{ 0, FOLLOW_AFH_MODE_ON, 0 },
	};
	follow_afh_read out;
	struct script s;

	run_script(&s, reads, 1, 0, &out);
	mu_assert_int_eq(1, s.calls);
	mu_assert_int_eq(0, s.waits);
	mu_assert_int_eq(1, out.attempts);
	run_script(&s, reads, 1, -5, &out);
	mu_assert_int_eq(1, s.calls);
	mu_assert_int_eq(1, out.attempts);
}

MU_TEST(a_reader_without_a_wait_still_retries)
{
	static const struct scripted_read reads[] = {
		{ 0, FOLLOW_AFH_MODE_ON, 0 },
		{ 0, FOLLOW_AFH_MODE_ON, 25 },
	};
	follow_afh_reader reader;
	follow_afh_read out;
	struct script s;

	s.reads = reads;
	s.count = 2;
	s.calls = 0;
	s.waits = 0;
	reader.read = script_read;
	reader.wait = NULL;
	reader.ctx = &s;
	mu_assert_int_eq(0, follow_afh_read_map(&reader, 2, &out));
	mu_assert_int_eq(FOLLOW_AFH_OK, out.result);
	mu_assert_int_eq(2, s.calls);
}

MU_TEST(a_missing_reader_is_a_failed_read_not_a_crash)
{
	follow_afh_reader reader = { NULL, NULL, NULL };
	follow_afh_read out;

	mu_assert_int_eq(-1, follow_afh_read_map(&reader, 3, &out));
	mu_assert_int_eq(FOLLOW_AFH_READ_FAILED, out.result);
	mu_assert_int_eq(-1, follow_afh_read_map(NULL, 3, &out));
	mu_assert_int_eq(FOLLOW_AFH_READ_FAILED, out.result);
	mu_assert_int_eq(-1, follow_afh_read_map(NULL, 3, NULL));
}

MU_TEST(the_return_value_follows_the_result)
{
	static const struct scripted_read good[] = {
		{ 0, FOLLOW_AFH_MODE_ON, 20 },
	};
	static const struct scripted_read off[] = {
		{ 0, FOLLOW_AFH_MODE_OFF, 0 },
	};
	follow_afh_reader reader;
	follow_afh_read out;
	struct script s;

	reader.read = script_read;
	reader.wait = script_wait;
	reader.ctx = &s;
	s.reads = good;
	s.count = 1;
	s.calls = 0;
	s.waits = 0;
	mu_assert_int_eq(0, follow_afh_read_map(&reader, 1, &out));
	s.reads = off;
	s.calls = 0;
	mu_assert_int_eq(-1, follow_afh_read_map(&reader, 1, &out));
}

MU_TEST(each_outcome_has_its_own_plain_line)
{
	follow_afh_read out;
	char line[256];

	memset(&out, 0, sizeof(out));
	out.attempts = 1;

	out.result = FOLLOW_AFH_OK;
	out.used = 40;
	follow_afh_message(&out, NULL, line, sizeof(line));
	mu_assert(strcmp("AFH map read: 40 of 79 channels in use", line) == 0,
	          line);

	out.result = FOLLOW_AFH_READ_FAILED;
	out.used = 0;
	follow_afh_message(&out, "Connection timed out", line, sizeof(line));
	mu_assert(strcmp("AFH map read failed: Connection timed out", line) == 0,
	          line);
	/* No system reason to give: still one complete line. */
	follow_afh_message(&out, NULL, line, sizeof(line));
	mu_assert(strcmp("AFH map read failed: no reason given", line) == 0, line);
	follow_afh_message(&out, "", line, sizeof(line));
	mu_assert(strcmp("AFH map read failed: no reason given", line) == 0, line);

	out.result = FOLLOW_AFH_MODE_DISABLED;
	follow_afh_message(&out, NULL, line, sizeof(line));
	mu_assert(strcmp("the remote device is not using AFH; "
	                 "following without it", line) == 0, line);

	out.result = FOLLOW_AFH_MODE_INVALID;
	out.mode = 0x07;
	follow_afh_message(&out, NULL, line, sizeof(line));
	mu_assert(strcmp("the remote device reported AFH mode 0x07, which is "
	                 "neither off (0x00) nor on (0x01)", line) == 0, line);

	out.result = FOLLOW_AFH_MAP_EMPTY;
	out.attempts = FOLLOW_AFH_READ_ATTEMPTS;
	follow_afh_message(&out, NULL, line, sizeof(line));
	mu_assert(strcmp("the remote device reports AFH on but an empty channel "
	                 "map (0 of 79 channels) after 3 reads", line) == 0, line);

	out.result = FOLLOW_AFH_TOO_FEW_CHANNELS;
	out.used = 19;
	follow_afh_message(&out, NULL, line, sizeof(line));
	mu_assert(strcmp("the remote device reports AFH on but only 19 channels "
	                 "in use after 3 reads, and AFH needs at least 20",
	                 line) == 0, line);

	/* Singulars, so a one-read, one-channel report still reads as English. */
	out.used = 1;
	out.attempts = 1;
	follow_afh_message(&out, NULL, line, sizeof(line));
	mu_assert(strcmp("the remote device reports AFH on but only 1 channel "
	                 "in use after 1 read, and AFH needs at least 20",
	                 line) == 0, line);
}

MU_TEST(a_message_call_with_nowhere_to_write_does_nothing)
{
	char line[8] = "keep";

	follow_afh_message(NULL, NULL, NULL, 0);
	follow_afh_message(NULL, NULL, line, 0);
	mu_assert(strcmp("keep", line) == 0, "zero length leaves the buffer");
	follow_afh_message(NULL, NULL, line, sizeof(line));
	mu_assert(line[0] == '\0', "no outcome writes an empty string");
}

MU_TEST(dash_a_needs_a_target_address_and_a_connection)
{
	const char *refusal;

	/* No -t: the old code read the map with an uninitialised handle. */
	refusal = follow_afh_handle_refusal(0, 0);
	mu_assert(refusal != NULL, "no address must refuse");
	/* The connection needs all six bytes, which only -t gives, so the
	 * refusal names that option and the form it takes. */
	mu_assert(strstr(refusal, "-t xx:xx:xx:xx:xx:xx") != NULL, refusal);
	/* An address with no handle is still no link to read over. */
	mu_assert(follow_afh_handle_refusal(0, 1) != NULL, "no address refuses");
	refusal = follow_afh_handle_refusal(1, 0);
	mu_assert(refusal != NULL, "no connection must refuse");
	mu_assert(strstr(refusal, "connection") != NULL, refusal);
	/* Both in hand: nothing to say. */
	mu_assert(follow_afh_handle_refusal(1, 1) == NULL, "both present");
}

/* The line both BlueZ tools report a map on, in the words libbtbb's
 * btbb_print_afh_map() used, so moving the printing into the tools did not
 * change what a reader or a script sees. */
MU_TEST(a_map_is_reported_as_one_line_of_hex)
{
	uint8_t map[FOLLOW_AFH_MAP_BYTES];
	char line[AFH_MAP_LINE_LEN];

	memset(map, 0xff, sizeof(map));
	afh_map_line(map, line, sizeof(line));
	mu_assert(strcmp("AFH map: 0xffffffffffffffffffff", line) == 0, line);

	memset(map, 0, sizeof(map));
	afh_map_line(map, line, sizeof(line));
	mu_assert(strcmp("AFH map: 0x00000000000000000000", line) == 0, line);

	/* Byte order as read: the first byte of the map is the first pair of
	 * digits, low channels first. */
	memset(map, 0, sizeof(map));
	map[0] = 0x01;
	map[9] = 0x7f;
	afh_map_line(map, line, sizeof(line));
	mu_assert(strcmp("AFH map: 0x0100000000000000007f", line) == 0, line);

	/* The buffer length the tools use is exactly enough: a shorter one
	 * would truncate instead of overrunning, and no map is left behind. */
	mu_assert_int_eq((int)strlen(line) + 1, AFH_MAP_LINE_LEN);
}

MU_TEST(a_map_line_call_with_nothing_to_write_does_nothing)
{
	uint8_t map[FOLLOW_AFH_MAP_BYTES];
	char line[AFH_MAP_LINE_LEN] = "keep";

	memset(map, 0xff, sizeof(map));
	afh_map_line(map, NULL, 0);
	/* Nowhere to write is nowhere to write, whatever the length says. */
	afh_map_line(map, NULL, sizeof(line));
	afh_map_line(map, line, 0);
	mu_assert(strcmp("keep", line) == 0, "zero length leaves the buffer");
	afh_map_line(NULL, line, sizeof(line));
	mu_assert(line[0] == '\0', "no map writes an empty string");
}

int main(void)
{
	MU_RUN(counts_the_79_real_channels_only);
	MU_RUN(a_failed_read_never_looks_at_the_mode_or_the_map);
	MU_RUN(the_mode_byte_is_validated);
	MU_RUN(an_enabled_map_needs_twenty_channels);
	MU_RUN(only_a_transient_map_is_retried);
	MU_RUN(a_usable_map_is_read_once_and_copied_out);
	MU_RUN(a_read_failure_stops_at_once_and_leaves_no_map);
	MU_RUN(a_remote_with_afh_off_is_followed_without_it);
	MU_RUN(an_empty_map_is_retried_to_the_bound_then_refused);
	MU_RUN(nineteen_channels_are_retried_then_refused);
	MU_RUN(a_transient_map_settles_across_a_retry);
	MU_RUN(a_mode_that_turns_off_mid_retry_is_not_retried_further);
	MU_RUN(the_attempt_count_is_clamped_to_at_least_one);
	MU_RUN(a_reader_without_a_wait_still_retries);
	MU_RUN(a_missing_reader_is_a_failed_read_not_a_crash);
	MU_RUN(the_return_value_follows_the_result);
	MU_RUN(each_outcome_has_its_own_plain_line);
	MU_RUN(a_message_call_with_nowhere_to_write_does_nothing);
	MU_RUN(dash_a_needs_a_target_address_and_a_connection);
	MU_RUN(a_map_is_reported_as_one_line_of_hex);
	MU_RUN(a_map_line_call_with_nothing_to_write_does_nothing);
	return mu_report();
}
