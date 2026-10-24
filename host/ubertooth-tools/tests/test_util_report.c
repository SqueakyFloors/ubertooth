#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../../firmware/tests/minunit.h"
#include "util_report.h"

struct fake_device {
	int board_id;
	const char *firmware;
	int firmware_result;
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

static int fake_firmware(void *context, char *revision, size_t size)
{
	struct fake_device *fake = context;

	/* On failure, leave text behind to check that it is not printed. */
	snprintf(revision, size, "%s", fake->firmware);
	return fake->firmware_result;
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
	fake_firmware,
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
	                 "Firmware version: git-test (API:1.07)\ncompile-test\n") == 0,
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
		"Firmware version: git-test (API:1.07)\n"
		"compile-test\n"
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
	mu_assert(strstr(captured.out, "Firmware version: git-test") != NULL,
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
	mu_assert(strstr(captured.out, "Firmware version: git-test\n") != NULL,
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
	mu_assert(strstr(captured.out, "compile-test\n") != NULL,
	          "compile info skipped after firmware failure");
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
	mu_assert(strstr(captured.out, "Firmware version: git-test (API:1.07)\n") != NULL,
	          "firmware line missing");
	mu_assert(strstr(captured.out, "Serial No: ") != NULL,
	          "serial skipped after compile info failure");
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

int main(void)
{
	MU_RUN(board_id_one_prints_name_and_succeeds);
	MU_RUN(every_known_board_id_succeeds_with_its_name);
	MU_RUN(unknown_board_id_is_rejected_without_indexing_the_name_table);
	MU_RUN(transfer_error_fails_without_printing_a_board_line);
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
	MU_RUN(nonzero_successful_values_do_not_become_status);
	MU_RUN(status_helpers);
	return mu_report();
}
