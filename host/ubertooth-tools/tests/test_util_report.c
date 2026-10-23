#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../../firmware/tests/minunit.h"
#include "util_report.h"

struct fake_device {
	int board_id;
	const char *firmware;
	int api_result;
	uint16_t api_version;
	const char *compile_info;
	int part_id;
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

static void fake_firmware(void *context, char *revision, size_t size)
{
	snprintf(revision, size, "%s", ((struct fake_device *)context)->firmware);
}

static int fake_api(void *context, uint16_t *version)
{
	struct fake_device *fake = context;

	*version = fake->api_version;
	return fake->api_result;
}

static void fake_compile_info(void *context, char *info, size_t size)
{
	snprintf(info, size, "%s", ((struct fake_device *)context)->compile_info);
}

static int fake_part_id(void *context)
{
	return ((struct fake_device *)context)->part_id;
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
	captured.status = util_run_reports(request, &ops, fake, out, err);
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

MU_TEST(part_id_error_fails_and_firmware_api_error_fails)
{
	struct fake_device fake = make_fake();
	struct util_report_request request = all_reports();
	struct captured captured;

	fake.part_id = -3;
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
}

int main(void)
{
	MU_RUN(board_id_one_prints_name_and_succeeds);
	MU_RUN(every_known_board_id_succeeds_with_its_name);
	MU_RUN(unknown_board_id_is_rejected_without_indexing_the_name_table);
	MU_RUN(transfer_error_fails_without_printing_a_board_line);
	MU_RUN(board_id_composes_with_firmware_compile_part_and_serial);
	MU_RUN(each_report_composes_with_board_id_individually);
	MU_RUN(early_failure_is_not_masked_by_later_success);
	MU_RUN(serial_with_nonzero_result_fails_and_is_not_printed);
	MU_RUN(part_id_error_fails_and_firmware_api_error_fails);
	MU_RUN(nonzero_successful_values_do_not_become_status);
	MU_RUN(status_helpers);
	return mu_report();
}
