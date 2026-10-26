#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../firmware/tests/minunit.h"
#include "util_report.h"
#include "ubertooth_interface.h"

struct fake_device {
	int board_id;
	const char *image;
	const char *firmware;
	int firmware_result;
	int revision_result;
	int api_result;
	uint16_t api_version;
	const char *compile_info;
	int compile_info_result;
	uint32_t part_id;
	int part_id_result;
	int serial_result;
	uint8_t serial[UTIL_SERIAL_BUFFER_LEN];
};

static struct fake_device make_fake(void)
{
	struct fake_device fake;
	int i;

	memset(&fake, 0, sizeof(fake));
	fake.board_id = 1;
	fake.image = "bluetooth_rxtx";
	fake.firmware = "git-test";
	fake.api_version = 0x0107;
	fake.compile_info = "compile-test";
	fake.part_id = 0x25011723;
	for (i = 1; i < UTIL_SERIAL_BUFFER_LEN; i++)
		fake.serial[i] = (uint8_t)(0xa0 + i);
	return fake;
}

static int fake_board_id(void *context)
{
	return ((struct fake_device *)context)->board_id;
}

/* The firmware info block the board carries: the image and the revision -v
 * and -V name come from here. On failure the fields are filled in anyway, to
 * check that nothing is printed from them. */
static int fake_firmware_info(void *context, struct ubertooth_fw_info *info)
{
	struct fake_device *fake = context;

	memset(info, 0, sizeof(*info));
	info->format_version = UBERTOOTH_FW_INFO_FORMAT;
	info->api_version = fake->api_version;
	snprintf(info->image, sizeof(info->image), "%s", fake->image);
	snprintf(info->revision, sizeof(info->revision), "%s", fake->firmware);
	return fake->firmware_result;
}

/* The revision on its own, which every firmware answers: what -v falls back
 * to when the board's firmware carries no info block. */
static int fake_firmware_revision(void *context, char *revision, size_t size)
{
	struct fake_device *fake = context;

	snprintf(revision, size, "%s", fake->firmware);
	return fake->revision_result;
}

static int fake_api(void *context, uint16_t *version)
{
	struct fake_device *fake = context;

	*version = fake->api_version;
	return fake->api_result;
}

static int fake_compile_info(void *context, char *info, size_t size)
{
	struct fake_device *fake = context;

	snprintf(info, size, "%s", fake->compile_info);
	return fake->compile_info_result;
}

static int fake_part_id(void *context, uint32_t *part_id)
{
	struct fake_device *fake = context;

	*part_id = fake->part_id;
	return fake->part_id_result;
}

static int fake_serial(void *context, uint8_t serial[UTIL_SERIAL_BUFFER_LEN])
{
	struct fake_device *fake = context;

	memcpy(serial, fake->serial, UTIL_SERIAL_BUFFER_LEN);
	return fake->serial_result;
}

static const struct util_report_ops ops = {
	fake_board_id,
	fake_firmware_info,
	fake_firmware_revision,
	fake_api,
	fake_compile_info,
	fake_part_id,
	fake_serial,
};

/* Which reports a command line asked for (-b, -v, -V, -p, -s). */
struct util_report_request {
	int board_id;
	int firmware;
	int compile_info;
	int part_id;
	int serial;
};

struct captured {
	char out[1024];
	char err[1024];
	int status;
};

static void read_back(FILE *file, char *buffer, size_t size)
{
	size_t length;

	fflush(file);
	rewind(file);
	length = fread(buffer, 1, size - 1, file);
	buffer[length] = '\0';
}

static struct captured run(const struct util_report_request *request,
                           struct fake_device *fake)
{
	struct captured captured;
	FILE *out = tmpfile();
	FILE *err = tmpfile();

	memset(&captured, 0, sizeof(captured));
	mu_assert(out != NULL && err != NULL, "tmpfile failed");
	if (out == NULL || err == NULL)
		return captured;
	/* Same order and status merging as ubertooth-util main(). */
	if (request->board_id)
		captured.status = util_status_merge(captured.status,
		        util_report_board_id(&ops, fake, out, err));
	if (request->firmware)
		captured.status = util_status_merge(captured.status,
		        util_report_firmware(&ops, fake, out));
	if (request->compile_info)
		captured.status = util_status_merge(captured.status,
		        util_report_compile_info(&ops, fake, out));
	if (request->part_id)
		captured.status = util_status_merge(captured.status,
		        util_report_part_id(&ops, fake, out, err));
	if (request->serial)
		captured.status = util_status_merge(captured.status,
		        util_report_serial(&ops, fake, out, err));
	read_back(out, captured.out, sizeof(captured.out));
	read_back(err, captured.err, sizeof(captured.err));
	fclose(out);
	fclose(err);
	return captured;
}

static struct util_report_request only_board_id(void)
{
	struct util_report_request request;

	memset(&request, 0, sizeof(request));
	request.board_id = 1;
	return request;
}

static struct util_report_request all_reports(void)
{
	struct util_report_request request;

	request.board_id = 1;
	request.firmware = 1;
	request.compile_info = 1;
	request.part_id = 1;
	request.serial = 1;
	return request;
}

MU_TEST(board_id_one_prints_name_and_succeeds)
{
	struct fake_device fake = make_fake();
	struct util_report_request request = only_board_id();
	struct captured captured = run(&request, &fake);

	mu_assert_int_eq(0, captured.status);
	mu_assert(strcmp(captured.out, "Board ID Number: 1 (Ubertooth One)\n") == 0,
	          "board ID line is wrong");
	mu_assert_int_eq(0, (long)strlen(captured.err));
}

MU_TEST(every_known_board_id_succeeds_with_its_name)
{
	static const char *const expected[] = {
		"Ubertooth Zero", "Ubertooth One", "ToorCon 13 Badge",
	};
	size_t id;

	mu_assert_int_eq(sizeof(expected) / sizeof(expected[0]),
	                 (long)util_board_count());
	for (id = 0; id < util_board_count(); id++) {
		struct fake_device fake = make_fake();
		struct util_report_request request = only_board_id();
		struct captured captured;
		char line[128];

		fake.board_id = (int)id;
		captured = run(&request, &fake);
		snprintf(line, sizeof(line), "Board ID Number: %d (%s)\n",
		         (int)id, expected[id]);
		mu_assert_int_eq(0, captured.status);
		mu_assert(strcmp(captured.out, line) == 0, "known board ID line wrong");
	}
}

MU_TEST(unknown_board_id_is_rejected_without_indexing_the_name_table)
{
	int unknown[3];
	size_t i;

	unknown[0] = (int)util_board_count();
	unknown[1] = 255;
	unknown[2] = 1 << 20;
	for (i = 0; i < sizeof(unknown) / sizeof(unknown[0]); i++) {
		struct fake_device fake = make_fake();
		struct util_report_request request = only_board_id();
		struct captured captured;

		fake.board_id = unknown[i];
		mu_assert(util_board_name(unknown[i]) == NULL, "name for unknown ID");
		captured = run(&request, &fake);
		mu_assert_int_eq(1, captured.status);
		mu_assert_int_eq(0, (long)strlen(captured.out));
		mu_assert(strstr(captured.err, "unknown board ID") != NULL,
		          "missing unknown-ID diagnostic");
	}
}

MU_TEST(transfer_error_fails_without_printing_a_board_line)
{
	struct fake_device fake = make_fake();
	struct util_report_request request = only_board_id();
	struct captured captured;

	fake.board_id = -7;
	mu_assert(util_board_name(-7) == NULL, "name for negative ID");
	captured = run(&request, &fake);
	mu_assert_int_eq(1, captured.status);
	mu_assert_int_eq(0, (long)strlen(captured.out));
	mu_assert(strcmp(captured.err, "error: board ID read failed\n") == 0,
	          "failed board ID read not named");
}

/* A short reply's whole "error: <query> read failed: board returned ..." line
 * comes from libubertooth, so the report adds none; a libusb error still gets
 * the report's own line. */
MU_TEST(short_reply_adds_no_second_error_line)
{
	struct fake_device fake = make_fake();
	struct util_report_request request = only_board_id();
	struct captured captured;

	fake.board_id = UTIL_ERROR_SHORT_REPLY;
	captured = run(&request, &fake);
	mu_assert_int_eq(1, captured.status);
	mu_assert_int_eq(0, (long)strlen(captured.out));
	mu_assert_int_eq(0, (long)strlen(captured.err));

	mu_assert_int_eq(1, util_report_read_error(stderr, "channel", UTIL_ERROR_SHORT_REPLY));
}

MU_TEST(failed_identity_reads_name_the_query_once_each)
{
	struct fake_device fake = make_fake();
	struct util_report_request request;
	struct captured captured;

	memset(&request, 0, sizeof(request));
	request.part_id = 1;
	fake.part_id_result = -7;
	captured = run(&request, &fake);
	mu_assert_int_eq(1, captured.status);
	mu_assert_int_eq(0, (long)strlen(captured.out));
	mu_assert(strcmp(captured.err, "error: part ID read failed\n") == 0,
	          "failed part ID read not named");

	memset(&request, 0, sizeof(request));
	request.serial = 1;
	fake = make_fake();
	fake.serial_result = 1; /* nonzero status, not only negative values */
	captured = run(&request, &fake);
	mu_assert_int_eq(1, captured.status);
	mu_assert_int_eq(0, (long)strlen(captured.out));
	mu_assert(strcmp(captured.err, "error: serial read failed\n") == 0,
	          "failed serial read not named");

	/* All three failing in one command: one line each, in report order, and
	 * the reports in between are still printed. */
	request = all_reports();
	fake = make_fake();
	fake.board_id = -1;
	fake.part_id_result = -1;
	fake.serial_result = -1;
	captured = run(&request, &fake);
	mu_assert_int_eq(1, captured.status);
	mu_assert(strcmp(captured.out,
	                 "Firmware version: bluetooth_rxtx git-test (API:1.07)\n"
	                 "bluetooth_rxtx: compile-test\n") == 0,
	          "other reports not printed");
	mu_assert(strcmp(captured.err,
	                 "error: board ID read failed\n"
	                 "error: part ID read failed\n"
	                 "error: serial read failed\n") == 0,
	          "one message per failed read");

	/* Successful reads print nothing on err. */
	fake = make_fake();
	captured = run(&request, &fake);
	mu_assert_int_eq(0, captured.status);
	mu_assert_int_eq(0, (long)strlen(captured.err));
}

MU_TEST(an_unknown_board_id_is_not_reported_as_a_read_failure)
{
	struct fake_device fake = make_fake();
	struct util_report_request request = only_board_id();
	struct captured captured;

	fake.board_id = 7;
	captured = run(&request, &fake);
	mu_assert_int_eq(1, captured.status);
	mu_assert(strcmp(captured.err, "error: unknown board ID 7\n") == 0,
	          "unknown board ID diagnostic changed");
}

MU_TEST(read_failed_message_matches_only_the_exact_line_for_that_query)
{
	FILE *err = tmpfile();
	char line[128];

	mu_assert(err != NULL, "tmpfile failed");
	if (err == NULL)
		return;
	mu_assert_int_eq(1, util_report_read_failed(err, UTIL_QUERY_PART_ID));
	read_back(err, line, sizeof(line));
	fclose(err);
	mu_assert(strcmp(line, "error: part ID read failed\n") == 0, "line format");
	mu_assert(util_is_read_failed_message(line, UTIL_QUERY_PART_ID), "own line");
	mu_assert(!util_is_read_failed_message(line, UTIL_QUERY_BOARD_ID),
	          "other query");
	mu_assert(!util_is_read_failed_message("error: part ID read failed",
	                                       UTIL_QUERY_PART_ID), "no newline");
	mu_assert(!util_is_read_failed_message("error: part ID read failed\nx\n",
	                                       UTIL_QUERY_PART_ID), "trailing text");
	mu_assert(!util_is_read_failed_message("error: unknown board ID 7\n",
	                                       UTIL_QUERY_BOARD_ID), "other error");
	mu_assert(!util_is_read_failed_message("", UTIL_QUERY_SERIAL), "empty");
}

MU_TEST(board_id_composes_with_firmware_compile_part_and_serial)
{
	struct fake_device fake = make_fake();
	struct util_report_request request = all_reports();
	struct captured captured = run(&request, &fake);
	static const char expected[] =
		"Board ID Number: 1 (Ubertooth One)\n"
		"Firmware version: bluetooth_rxtx git-test (API:1.07)\n"
		"bluetooth_rxtx: compile-test\n"
		"Part ID: 25011723\n"
		"Serial No: a1a2a3a4a5a6a7a8a9aaabacadaeafb0\n";

	mu_assert_int_eq(0, captured.status);
	mu_assert(strcmp(captured.out, expected) == 0, "composed output wrong");
}

MU_TEST(each_report_composes_with_board_id_individually)
{
	struct util_report_request requests[4];
	size_t i;

	memset(requests, 0, sizeof(requests));
	requests[0].firmware = 1;
	requests[1].compile_info = 1;
	requests[2].serial = 1;
	requests[3].part_id = 1;
	for (i = 0; i < sizeof(requests) / sizeof(requests[0]); i++) {
		struct fake_device fake = make_fake();
		struct captured captured;

		requests[i].board_id = 1;
		captured = run(&requests[i], &fake);
		mu_assert_int_eq(0, captured.status);
		mu_assert(strncmp(captured.out, "Board ID Number: 1 (Ubertooth One)\n",
		                  35) == 0, "board ID not reported first");
	}
}

MU_TEST(early_failure_is_not_masked_by_later_success)
{
	struct fake_device fake = make_fake();
	struct util_report_request request = all_reports();
	struct captured captured;

	fake.board_id = -1;
	captured = run(&request, &fake);
	mu_assert_int_eq(1, captured.status);
	/* Later reports still run and print. */
	mu_assert(strstr(captured.out, "Firmware version: bluetooth_rxtx git-test") != NULL,
	          "firmware report skipped after failure");
	mu_assert(strstr(captured.out, "Serial No: ") != NULL,
	          "serial report skipped after failure");
}

MU_TEST(serial_with_nonzero_result_fails_and_is_not_printed)
{
	struct fake_device fake = make_fake();
	struct util_report_request request = all_reports();
	struct captured captured;

	fake.serial_result = 1;
	captured = run(&request, &fake);
	mu_assert_int_eq(1, captured.status);
	mu_assert(strstr(captured.out, "Serial No: ") == NULL,
	          "serial printed despite failure");
}

MU_TEST(part_id_with_bit_31_set_prints_unsigned)
{
	struct fake_device fake = make_fake();
	struct util_report_request request;
	struct captured captured;

	memset(&request, 0, sizeof(request));
	request.part_id = 1;
	fake.part_id = 0x8000ABCDu;
	captured = run(&request, &fake);
	mu_assert_int_eq(0, captured.status);
	mu_assert(strcmp(captured.out, "Part ID: 8000ABCD\n") == 0,
	          "part ID with bit 31 set not printed as unsigned hex");
}

MU_TEST(part_id_error_fails_and_firmware_api_error_fails)
{
	struct fake_device fake = make_fake();
	struct util_report_request request = all_reports();
	struct captured captured;

	fake.part_id_result = -3;
	captured = run(&request, &fake);
	mu_assert_int_eq(1, captured.status);
	mu_assert(strstr(captured.out, "Part ID") == NULL, "part ID printed on error");

	fake = make_fake();
	fake.api_result = -1;
	captured = run(&request, &fake);
	mu_assert_int_eq(1, captured.status);
	mu_assert(strstr(captured.out, "Firmware version: bluetooth_rxtx git-test\n") != NULL,
	          "firmware line missing without API");
}

MU_TEST(firmware_revision_error_fails_without_printing_a_line)
{
	struct fake_device fake = make_fake();
	struct util_report_request request;
	struct captured captured;

	memset(&request, 0, sizeof(request));
	request.firmware = 1;
	fake.firmware_result = -7;
	captured = run(&request, &fake);
	mu_assert_int_eq(1, captured.status);
	mu_assert_int_eq(0, (long)strlen(captured.out));

	/* Later reports still run, and the failure is not masked. */
	request = all_reports();
	captured = run(&request, &fake);
	mu_assert_int_eq(1, captured.status);
	mu_assert(strstr(captured.out, "Firmware version") == NULL,
	          "firmware line printed on error");
	/* -V names the image from the same block, so a block that will not read
	 * costs that line too. */
	mu_assert(strstr(captured.out, "compile-test") == NULL,
	          "compile info printed without the block that names its image");
	mu_assert(strstr(captured.out, "Serial No: ") != NULL,
	          "serial report skipped after firmware failure");
}

MU_TEST(compile_info_error_fails_without_printing_a_line)
{
	struct fake_device fake = make_fake();
	struct util_report_request request;
	struct captured captured;

	memset(&request, 0, sizeof(request));
	request.compile_info = 1;
	fake.compile_info_result = -1;
	captured = run(&request, &fake);
	mu_assert_int_eq(1, captured.status);
	mu_assert_int_eq(0, (long)strlen(captured.out));

	request = all_reports();
	captured = run(&request, &fake);
	mu_assert_int_eq(1, captured.status);
	mu_assert(strstr(captured.out, "compile-test") == NULL,
	          "compile info printed on error");
	mu_assert(strstr(captured.out, "Firmware version: bluetooth_rxtx git-test (API:1.07)\n") != NULL,
	          "firmware line missing");
	mu_assert(strstr(captured.out, "Serial No: ") != NULL,
	          "serial skipped after compile info failure");
}

/* -V names the image the compile info came from, and that name is in the
 * firmware info block, so -V on its own fails when the block will not read
 * even though the compile info itself would have. */
MU_TEST(compile_info_fails_when_the_block_read_fails)
{
	struct fake_device fake = make_fake();
	struct util_report_request request;
	struct captured captured;

	memset(&request, 0, sizeof(request));
	request.compile_info = 1;
	fake.firmware_result = -7;
	captured = run(&request, &fake);
	mu_assert_int_eq(1, captured.status);
	mu_assert_int_eq(0, (long)strlen(captured.out));
}

/*
 * Firmware built before the info block existed stalls the request for it.
 * Both reports carry on from what that firmware does answer: -v prints the
 * revision with no image name, the line this tool always printed, and -V
 * prints the build string on its own.
 */
MU_TEST(firmware_without_a_block_is_still_reported)
{
	struct fake_device fake = make_fake();
	struct util_report_request request;
	struct captured captured;

	memset(&request, 0, sizeof(request));
	request.firmware = 1;
	request.compile_info = 1;
	fake.firmware_result = UTIL_ERROR_NO_FW_INFO;
	captured = run(&request, &fake);
	mu_assert_int_eq(0, captured.status);
	mu_assert(strcmp(captured.out,
	                 "Firmware version: git-test (API:1.07)\n"
	                 "compile-test\n") == 0,
	          "a board with no info block was not reported from what it answers");
}

/* A stall with no revision to fall back on is still a failure, and a failed
 * read prints no line. */
MU_TEST(a_stalled_block_with_an_unreadable_revision_prints_no_line)
{
	struct fake_device fake = make_fake();
	struct util_report_request request;
	struct captured captured;

	memset(&request, 0, sizeof(request));
	request.firmware = 1;
	fake.firmware_result = UTIL_ERROR_NO_FW_INFO;
	fake.revision_result = -7;
	captured = run(&request, &fake);
	mu_assert_int_eq(1, captured.status);
	mu_assert_int_eq(0, (long)strlen(captured.out));
}

MU_TEST(nonzero_successful_values_do_not_become_status)
{
	struct fake_device fake = make_fake();
	struct util_report_request request = only_board_id();
	struct captured captured;

	fake.board_id = 2;
	captured = run(&request, &fake);
	mu_assert_int_eq(0, captured.status);

	request = all_reports();
	fake = make_fake();
	captured = run(&request, &fake);
	mu_assert_int_eq(0, captured.status);
}

MU_TEST(status_helpers)
{
	mu_assert_int_eq(0, util_status_merge(0, 0));
	mu_assert_int_eq(1, util_status_merge(0, 1));
	mu_assert_int_eq(1, util_status_merge(1, 0));
	mu_assert_int_eq(1, util_status_merge(1, 1));
	mu_assert_int_eq(0, util_value_status(0));
	mu_assert_int_eq(0, util_value_status(2437));
	mu_assert_int_eq(1, util_value_status(-1));
	mu_assert_int_eq(0, util_status_after(0, 2437));
	mu_assert_int_eq(1, util_status_after(0, -1));
	mu_assert_int_eq(1, util_status_after(1, 0));
}

/* ---- the watchdog report (-w) ------------------------------------------ */

/* The report, from the eight bytes the board sends. */
static char *watchdog_text(const uint8_t *info, size_t len, int *status)
{
	char *text = NULL;
	size_t size = 0;
	FILE *out = open_memstream(&text, &size);

	*status = util_report_watchdog(out, info, len);
	fclose(out);
	return text;
}

/*
 * A board that has reset itself says so, and says where it was. This is the
 * whole point of the breadcrumb: a wedge that used to need a power cycle and
 * left no trace now names the mode and the wait it happened in.
 */
MU_TEST(a_wedged_board_reports_where_it_was_stuck)
{
	uint8_t info[UBERTOOTH_WATCHDOG_INFO_LEN] = {
		WATCHDOG_RESET_WATCHDOG, 1, 2, 5,
		MODE_BT_FOLLOW_LE, WATCHDOG_SITE_XOSC,
		MODE_IDLE, WATCHDOG_SITE_NONE,
	};
	int status = -1;
	char *text = watchdog_text(info, sizeof(info), &status);

	mu_assert(text != NULL, "no report");
	mu_assert_int_eq(0, status);
	mu_assert(strstr(text, "Last boot cause: watchdog\n") != NULL, text);
	mu_assert(strstr(text, "Watchdog: armed;") != NULL, text);
	mu_assert(strstr(text, "resets since power-on: 2 (in 5 boots)") != NULL,
	          text);
	mu_assert(strstr(text, "BLE follow") != NULL, text);
	mu_assert(strstr(text, "16 MHz crystal oscillator") != NULL, text);
	mu_assert(strstr(text, "Now in idle mode") != NULL, text);
	free(text);
}

/* A board that has never reset itself has no last one to describe. */
MU_TEST(a_healthy_board_reports_no_last_reset)
{
	uint8_t info[UBERTOOTH_WATCHDOG_INFO_LEN] = {
		WATCHDOG_RESET_POWER_ON, 1, 0, 1,
		MODE_IDLE, WATCHDOG_SITE_NONE, MODE_SPECAN, WATCHDOG_SITE_USB,
	};
	int status = -1;
	char *text = watchdog_text(info, sizeof(info), &status);

	mu_assert(text != NULL, "no report");
	mu_assert_int_eq(0, status);
	mu_assert(strstr(text, "Last boot cause: power-on\n") != NULL, text);
	mu_assert(strstr(text, "resets since power-on: 0 (in 1 boot)") != NULL,
	          text);
	mu_assert(strstr(text, "the last was") == NULL, text);
	mu_assert(strstr(text, "Now in spectrum analyzer mode, servicing USB")
	          != NULL, text);
	free(text);
}

/* An unarmed watchdog is reported as unarmed, not as a guard the board does
 * not have. */
MU_TEST(an_unarmed_watchdog_is_reported_as_such)
{
	uint8_t info[UBERTOOTH_WATCHDOG_INFO_LEN] = { 0, 0, 0, 1, 0, 0, 0, 0 };
	int status = -1;
	char *text = watchdog_text(info, sizeof(info), &status);

	mu_assert(text != NULL, "no report");
	mu_assert(strstr(text, "Watchdog: not armed\n") != NULL, text);
	mu_assert(strstr(text, "Last boot cause: not recorded\n") != NULL, text);
	free(text);
}

/* Several causes can be set at once, and a reply this version cannot read
 * fails rather than printing from it. */
MU_TEST(causes_combine_and_a_short_reply_fails)
{
	uint8_t info[UBERTOOTH_WATCHDOG_INFO_LEN] = {
		WATCHDOG_RESET_POWER_ON | WATCHDOG_RESET_WATCHDOG,
		1, 0, 1, 0, 0, 0, 0,
	};
	char cause[64];
	int status = -1;
	char *text = watchdog_text(info, sizeof(info), &status);

	mu_assert(text != NULL, "no report");
	mu_assert(strstr(text, "Last boot cause: power-on+watchdog\n") != NULL,
	          text);
	free(text);

	text = watchdog_text(info, sizeof(info) - 1, &status);
	mu_assert_int_eq(1, status);
	free(text);

	/* Every cause has a name, and a buffer too small is still terminated. */
	util_watchdog_cause_text(WATCHDOG_RESET_EXTERNAL, cause, sizeof(cause));
	mu_assert(strcmp(cause, "reset pin") == 0, cause);
	util_watchdog_cause_text(WATCHDOG_RESET_BROWN_OUT, cause, sizeof(cause));
	mu_assert(strcmp(cause, "brown-out") == 0, cause);
	util_watchdog_cause_text(0xFF, cause, 6);
	mu_assert_int_eq(0, cause[5]);
}

/*
 * Every mode and wait site the interface defines has a name, so a breadcrumb
 * never reads as a mode this version does not know when it does; and a value
 * from a newer firmware says so instead of indexing past the table.
 */
MU_TEST(every_mode_and_site_has_a_name)
{
	unsigned i;

	for (i = 0; i <= MODE_IDENTIFY; i++) {
		const char *name = util_watchdog_mode_name(i);

		mu_assert(strcmp(name, "unknown") != 0, name);
	}
	mu_assert(strcmp(util_watchdog_mode_name(MODE_IDENTIFY + 1),
	                 "unknown") == 0, "an unnamed mode is not unknown");
	mu_assert(strcmp(util_watchdog_mode_name(MODE_UNKNOWN),
	                 "not reported") == 0, "btbr's mode is not reported");

	for (i = 0; i < WATCHDOG_SITE_COUNT; i++)
		mu_assert(strstr(util_watchdog_site_text(i), "does not know")
		          == NULL, util_watchdog_site_text(i));
	mu_assert(strstr(util_watchdog_site_text(WATCHDOG_SITE_COUNT),
	                 "does not know") != NULL,
	          "an unknown site is not reported as unknown");
}

int main(void)
{
	MU_RUN(board_id_one_prints_name_and_succeeds);
	MU_RUN(every_known_board_id_succeeds_with_its_name);
	MU_RUN(unknown_board_id_is_rejected_without_indexing_the_name_table);
	MU_RUN(transfer_error_fails_without_printing_a_board_line);
	MU_RUN(short_reply_adds_no_second_error_line);
	MU_RUN(failed_identity_reads_name_the_query_once_each);
	MU_RUN(an_unknown_board_id_is_not_reported_as_a_read_failure);
	MU_RUN(read_failed_message_matches_only_the_exact_line_for_that_query);
	MU_RUN(board_id_composes_with_firmware_compile_part_and_serial);
	MU_RUN(each_report_composes_with_board_id_individually);
	MU_RUN(early_failure_is_not_masked_by_later_success);
	MU_RUN(serial_with_nonzero_result_fails_and_is_not_printed);
	MU_RUN(part_id_with_bit_31_set_prints_unsigned);
	MU_RUN(part_id_error_fails_and_firmware_api_error_fails);
	MU_RUN(firmware_revision_error_fails_without_printing_a_line);
	MU_RUN(compile_info_error_fails_without_printing_a_line);
	MU_RUN(compile_info_fails_when_the_block_read_fails);
	MU_RUN(firmware_without_a_block_is_still_reported);
	MU_RUN(a_stalled_block_with_an_unreadable_revision_prints_no_line);
	MU_RUN(nonzero_successful_values_do_not_become_status);
	MU_RUN(status_helpers);
	MU_RUN(a_wedged_board_reports_where_it_was_stuck);
	MU_RUN(a_healthy_board_reports_no_last_reset);
	MU_RUN(an_unarmed_watchdog_is_reported_as_such);
	MU_RUN(causes_combine_and_a_short_reply_fails);
	MU_RUN(every_mode_and_site_has_a_name);
	return mu_report();
}
