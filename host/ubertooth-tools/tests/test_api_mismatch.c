/*
 * What the host does about firmware of another API version, and how it
 * reports a request that firmware has not got.
 *
 * The real ubertooth_api_version.c is compiled into this test, twice: once
 * with UBERTOOTH_RELEASE_BUILD=1 and once with 0, because the whole point of
 * the decision is that it differs between a release build and a build from a
 * working checkout. Each build runs the same cases and asserts what that
 * build should do. Nothing here opens a device: the source only formats lines
 * and reads an environment variable.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../../firmware/tests/minunit.h"
#include "ubertooth.h"

/* Which build this copy of the test is. The source under test defaults to a
 * release build when the build system says nothing, and so does this. */
#ifndef UBERTOOTH_RELEASE_BUILD
#define UBERTOOTH_RELEASE_BUILD 1
#endif

/* A version either side of the one these tools require, so both directions of
 * mismatch are covered: an older board is reflashed, a newer one means the
 * host tools are the old half. */
#define OLDER (UBERTOOTH_API_VERSION - 1)
#define NEWER (UBERTOOTH_API_VERSION + 1)

/* ---- helpers ------------------------------------------------------------ */

static char err[1024];
static char out[1024];

/* Runs call() with both streams captured. Standard output is captured too,
 * because a library may never write to it. */
static void captured(void (*call)(void))
{
	FILE *cap_err = tmpfile();
	FILE *cap_out = tmpfile();
	int saved_err = dup(fileno(stderr));
	int saved_out = dup(fileno(stdout));
	size_t n;

	err[0] = '\0';
	out[0] = '\0';
	mu_assert_fatal(cap_err != NULL && cap_out != NULL, "capture files");
	mu_assert_fatal(saved_err >= 0 && saved_out >= 0, "saved streams");

	fflush(stderr);
	fflush(stdout);
	dup2(fileno(cap_err), fileno(stderr));
	dup2(fileno(cap_out), fileno(stdout));

	call();

	fflush(stderr);
	fflush(stdout);
	dup2(saved_err, fileno(stderr));
	dup2(saved_out, fileno(stdout));
	close(saved_err);
	close(saved_out);

	rewind(cap_err);
	n = fread(err, 1, sizeof(err) - 1, cap_err);
	err[n] = '\0';
	rewind(cap_out);
	n = fread(out, 1, sizeof(out) - 1, cap_out);
	out[n] = '\0';
	fclose(cap_err);
	fclose(cap_out);
}

/* Stand-ins for two boards' USB handles: the report keeps a version against
 * the board it was read from. */
static int board_a;
static int board_b;

/* Whether the board behind the handle above has left the USB bus. The real
 * check lists the bus through libusb; these handles are not real ones, so the
 * answer is scripted here. Zero by default, which is the bench a board is
 * normally on. */
static int board_has_left_the_bus;

int ubertooth_device_gone(const void* board)
{
	mu_assert(board == &board_a || board == &board_b,
	          "asked about a board the report was given");
	return board_has_left_the_bus;
}

/* The handful of one-call wrappers captured() needs, with what they report
 * passed through a file-scope variable. */
static uint16_t call_version;
static const char *call_selector;
static int call_result;

static void report_mismatch(void)
{
	call_result = ubertooth_report_api_mismatch(call_version, call_selector);
}

static void report_unsupported(void)
{
	ubertooth_report_unsupported_request("set the channel", -9);
}

static void report_unsupported_without_a_code(void)
{
	ubertooth_report_unsupported_request("receive the capture", 0);
}

/* The mismatch line as the library builds it, so a reworded message fails
 * rather than matching a shorter prefix. */
static const char *mismatch_line(const char *level, uint16_t version)
{
	static char line[256];

	snprintf(line, sizeof(line),
	         "%s: Ubertooth firmware API version %x.%02x found, "
	         "libubertooth %s requires %x.%02x.\n",
	         level, (version >> 8) & 0xFF, version & 0xFF, VERSION,
	         (UBERTOOTH_API_VERSION >> 8) & 0xFF,
	         UBERTOOTH_API_VERSION & 0xFF);
	return line;
}

static void run_mismatch(uint16_t version, const char *selector)
{
	call_version = version;
	call_selector = selector;
	captured(report_mismatch);
}

/* ---- the decision ------------------------------------------------------- */

/*
 * With nothing in the environment, the build decides: a release build refuses
 * a mismatch as an error, a development build warns and lets the command go
 * on. This is the whole change, so both halves are asserted here rather than
 * only the one this build happens to be.
 */
MU_TEST(the_build_decides_when_the_environment_says_nothing)
{
	unsetenv(UBERTOOTH_ALLOW_API_MISMATCH_VAR);
	run_mismatch(OLDER, NULL);

	if (UBERTOOTH_RELEASE_BUILD) {
		mu_assert_int_eq(-1, call_result);
		mu_assert(strstr(err, mismatch_line("error", OLDER)) != NULL, err);
		mu_assert(strstr(err, "runs on") == NULL,
		          "a release build does not run on");
	} else {
		mu_assert_int_eq(0, call_result);
		mu_assert(strstr(err, mismatch_line("warning", OLDER)) != NULL, err);
		mu_assert(strstr(err, "error:") == NULL,
		          "a development build refuses nothing");
		/* The whole note, so a reworded one fails here and in the check
		 * that the man page shows the same text
		 * (test_doc_example_lines.py). */
		mu_assert(strstr(err, "note: this is a development build, so the "
		                 "command runs on; what it reports may be wrong, "
		                 "and a request this firmware has not got will "
		                 "fail\n") != NULL, err);
	}
	/* Whatever the build, the report is a diagnostic. */
	mu_assert(out[0] == '\0', out);
}

/*
 * The override decides whatever the build. "1" is the escape hatch a release
 * build needs for debugging; "0" is the strictness CI and the test harness
 * want from a build made out of a checkout. Both name themselves, so the
 * reader of a warning knows why nothing was refused.
 */
MU_TEST(the_override_decides_whatever_the_build)
{
	setenv(UBERTOOTH_ALLOW_API_MISMATCH_VAR, "1", 1);
	run_mismatch(OLDER, NULL);
	mu_assert_int_eq(0, call_result);
	mu_assert(strstr(err, mismatch_line("warning", OLDER)) != NULL, err);
	mu_assert(strstr(err, "note: " UBERTOOTH_ALLOW_API_MISMATCH_VAR
	                 " is set, so the command runs on") != NULL, err);

	setenv(UBERTOOTH_ALLOW_API_MISMATCH_VAR, "0", 1);
	run_mismatch(OLDER, NULL);
	mu_assert_int_eq(-1, call_result);
	mu_assert(strstr(err, mismatch_line("error", OLDER)) != NULL, err);
	mu_assert(strstr(err, "runs on") == NULL, err);

	unsetenv(UBERTOOTH_ALLOW_API_MISMATCH_VAR);
}

/*
 * Only the two exact values decide. An empty or unexpected value is not a
 * third answer: it leaves the build's own, so a typo cannot quietly turn the
 * guard off in a release build.
 */
MU_TEST(any_other_value_leaves_the_builds_own_answer)
{
	static const char *values[] = { "", "2", "yes", "01", "1 ", "true" };
	size_t i;

	for (i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
		setenv(UBERTOOTH_ALLOW_API_MISMATCH_VAR, values[i], 1);
		run_mismatch(OLDER, NULL);
		if (UBERTOOTH_RELEASE_BUILD)
			mu_assert_int_eq(-1, call_result);
		else
			mu_assert_int_eq(0, call_result);
	}
	unsetenv(UBERTOOTH_ALLOW_API_MISMATCH_VAR);
}

/* ---- the message -------------------------------------------------------- */

/*
 * Whichever side is behind is the one the note asks the user to replace, and
 * the example reflashes the board the message is about. Neither depends on
 * the decision, so this runs with the override set each way.
 */
MU_TEST(the_note_names_the_side_to_replace_and_the_board)
{
	static const char *settings[] = { "0", "1" };
	size_t i;

	for (i = 0; i < sizeof(settings) / sizeof(settings[0]); i++) {
		setenv(UBERTOOTH_ALLOW_API_MISMATCH_VAR, settings[i], 1);

		run_mismatch(OLDER, NULL);
		mu_assert(strstr(err, "note: flash the firmware built from the "
		                 "same source tree, for example: ./install.sh "
		                 "--flash\n") != NULL, err);

		run_mismatch(OLDER, "1cc0");
		mu_assert(strstr(err, "note: flash the firmware built from the "
		                 "same source tree, for example: ./install.sh "
		                 "--flash-device 1cc0\n") != NULL, err);
		mu_assert(strstr(err, "--flash-device all") == NULL,
		          "and reflashes no other board");

		run_mismatch(NEWER, "1cc0");
		mu_assert(strstr(err, "note: install the host tools built from "
		                 "the same source tree as the firmware\n") != NULL,
		          err);
		mu_assert(strstr(err, "flash the firmware") == NULL,
		          "and no backwards flash");
	}
	unsetenv(UBERTOOTH_ALLOW_API_MISMATCH_VAR);
}

/*
 * A request the board's firmware has not got is reported in one wording: the
 * request named, the stall said plainly, and the libusb code kept for a bug
 * report. Where the stall arrived as a transfer status there is no libusb
 * code, and the line says so by leaving it out rather than printing a number
 * from the wrong enumeration.
 */
MU_TEST(an_unsupported_request_is_named_with_the_stall)
{
	captured(report_unsupported);
	mu_assert(strstr(err, "error: could not set the channel: the board's "
	                 "firmware does not support it (stalled, libusb -9)\n")
	          != NULL, err);
	mu_assert(out[0] == '\0', out);

	captured(report_unsupported_without_a_code);
	mu_assert(strstr(err, "error: could not receive the capture: the board's "
	                 "firmware does not support it (stalled)\n") != NULL, err);
}

/*
 * Once a board's version has been read, the report puts it next to the one
 * these tools need: different versions say which reflash fixes the request,
 * equal ones say the request is simply not in this firmware at all. Before
 * any board has been read there is no second half to give, and the line is
 * left without it rather than carrying a made-up version.
 */
MU_TEST(an_unsupported_request_names_both_api_versions)
{
	char expected[256];

	/* Nothing recorded, whatever has run before this. */
	ubertooth_forget_api_version(&board_a);
	ubertooth_forget_api_version(&board_b);
	captured(report_unsupported);
	mu_assert(strstr(err, "firmware API version") == NULL,
	          "no version has been read, so none is quoted");

	ubertooth_note_api_version(&board_a, OLDER);
	snprintf(expected, sizeof(expected),
	         "note: the board reports firmware API version %x.%02x, "
	         "libubertooth %s requires %x.%02x\n",
	         (OLDER >> 8) & 0xFF, OLDER & 0xFF, VERSION,
	         (UBERTOOTH_API_VERSION >> 8) & 0xFF,
	         UBERTOOTH_API_VERSION & 0xFF);
	captured(report_unsupported);
	mu_assert(strstr(err, expected) != NULL, err);

	/* Firmware that does match still gets both versions, which is how the
	 * reader learns the version is not the problem. */
	ubertooth_note_api_version(&board_a, UBERTOOTH_API_VERSION);
	snprintf(expected, sizeof(expected),
	         "note: the board reports firmware API version %x.%02x, "
	         "libubertooth %s requires %x.%02x\n",
	         (UBERTOOTH_API_VERSION >> 8) & 0xFF,
	         UBERTOOTH_API_VERSION & 0xFF, VERSION,
	         (UBERTOOTH_API_VERSION >> 8) & 0xFF,
	         UBERTOOTH_API_VERSION & 0xFF);
	captured(report_unsupported);
	mu_assert(strstr(err, expected) != NULL, err);

	ubertooth_forget_api_version(&board_a);
}

/* A version belongs to the board it was read from, and goes when that board
 * is released. A process that works through several boards in turn -- which
 * is what ubertooth-util's listing does -- must not quote the board before
 * for a request the board in hand refused. */
MU_TEST(a_released_boards_version_is_not_quoted_for_the_next_board)
{
	ubertooth_note_api_version(&board_a, OLDER);
	ubertooth_forget_api_version(&board_a);

	captured(report_unsupported);
	mu_assert(strstr(err, "firmware API version") == NULL,
	          "the released board's version is still being quoted");

	/* Releasing another board leaves this one's alone. */
	ubertooth_note_api_version(&board_a, OLDER);
	ubertooth_forget_api_version(&board_b);
	captured(report_unsupported);
	mu_assert(strstr(err, "firmware API version") != NULL, err);
	ubertooth_forget_api_version(&board_a);
}

/* A board that has left the USB bus stalls a request on its way out on some
 * hosts, and its firmware is not the reason: the board is simply not there.
 * Blaming the firmware sent the user to reflash a board that was never the
 * problem, with a note quoting two API versions that were the same. Such a
 * board gets one line saying what happened, and no word about firmware.
 */
MU_TEST(a_board_that_has_left_the_bus_is_not_blamed_on_its_firmware)
{
	ubertooth_note_api_version(&board_a, UBERTOOTH_API_VERSION);
	board_has_left_the_bus = 1;

	captured(report_unsupported);
	mu_assert(strstr(err, "error: could not set the channel: the board is "
	                 "no longer attached\n") != NULL, err);
	mu_assert(strstr(err, "firmware") == NULL,
	          "a board that has gone is not a firmware problem");
	mu_assert(strstr(err, "stalled") == NULL,
	          "the stall is how it was seen, not what happened");
	mu_assert(strstr(err, "note:") == NULL, "one line, no note");
	mu_assert(out[0] == '\0', out);

	/* The board back on the bus: the stall is the firmware refusing the
	 * request again, which is what the reader needs then. */
	board_has_left_the_bus = 0;
	captured(report_unsupported);
	mu_assert(strstr(err, "the board's firmware does not support it") != NULL,
	          err);

	ubertooth_forget_api_version(&board_a);
}

/* With no board's version read, there is no handle to ask about either: the
 * bootloader ubertooth-dfu talks to has no API version, so that report keeps
 * the stall wording rather than guessing at a board that has gone. */
MU_TEST(a_report_with_no_board_behind_it_keeps_the_stall_wording)
{
	ubertooth_forget_api_version(&board_a);
	ubertooth_forget_api_version(&board_b);
	board_has_left_the_bus = 1;

	captured(report_unsupported);
	mu_assert(strstr(err, "the board's firmware does not support it") != NULL,
	          err);
	mu_assert(strstr(err, "firmware API version") == NULL,
	          "no version has been read, so none is quoted");

	board_has_left_the_bus = 0;
}

int main(void)
{
	MU_RUN(an_unsupported_request_names_both_api_versions);
	MU_RUN(the_build_decides_when_the_environment_says_nothing);
	MU_RUN(the_override_decides_whatever_the_build);
	MU_RUN(any_other_value_leaves_the_builds_own_answer);
	MU_RUN(the_note_names_the_side_to_replace_and_the_board);
	MU_RUN(an_unsupported_request_is_named_with_the_stall);
	MU_RUN(a_released_boards_version_is_not_quoted_for_the_next_board);
	MU_RUN(a_board_that_has_left_the_bus_is_not_blamed_on_its_firmware);
	MU_RUN(a_report_with_no_board_behind_it_keeps_the_stall_wording);
	return mu_report();
}
