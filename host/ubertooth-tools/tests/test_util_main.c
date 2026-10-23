/*
 * Hardware-independent tests of ubertooth-util's main(): option parsing, the
 * order and content of its output, and its exit status.
 *
 * ubertooth-util.c is compiled into this test with main() renamed, and the
 * libubertooth device functions it calls are replaced by the fakes below, so
 * the real command-line handling runs against a scripted "device".
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../../firmware/tests/minunit.h"

/* ubertooth-util waits two seconds after a reset; the fake device does not need it. */
static unsigned fake_sleep(unsigned seconds)
{
	(void)seconds;
	return 0;
}
#define sleep(seconds) fake_sleep(seconds)
#define main ubertooth_util_main
#include "ubertooth-util.c"
#undef sleep
#undef main

/* ---- scripted device -------------------------------------------------- */

static struct {
	int board_id, channel, palevel, squelch, usrled, rxled, txled, partnum;
	int serial_rc, api_rc;
	uint16_t api_version;
	const char *revision, *compile_info;
	/* results of commands that set state or start a mode */
	int stop_rc, reset_rc, set_channel_rc, set_usrled_rc, set_rxled_rc;
	int set_txled_rc, set_palevel_rc, set_squelch_rc, flash_rc, isp_rc;
	int led_specan_rc, range_test_rc, repeater_rc, tx_test_rc, xmas_rc;
	int rangeresult_rc;
	rangetest_result range;
	/* bookkeeping */
	int device_count;
	int start_calls;
	int fail_start_from; /* 0: never; n: ubertooth_start_select fails from call n */
	char selector[64];
	int stop_calls, flash_calls, isp_calls, led_specan_calls, range_test_calls;
	int repeater_calls, tx_test_calls, xmas_calls, reset_calls;
	int last_channel_set, last_squelch_set, last_led_specan_arg;
} fake;

static ubertooth_t fake_ut;

static void reset_fake(void)
{
	memset(&fake, 0, sizeof(fake));
	fake.board_id = 1;
	fake.channel = 2441;
	fake.palevel = 7;
	fake.squelch = 136; /* unsigned byte 0x88, printed as -120 */
	fake.partnum = 0x25011723;
	fake.api_version = 0x0107;
	fake.revision = "git-test";
	fake.compile_info = "compile-test";
	fake.device_count = 1;
	fake.range.valid = 1;
	fake.range.request_pa = 1;
	fake.range.request_num = 2;
	fake.range.reply_pa = 3;
	fake.range.reply_num = 4;
	memset(&fake_ut, 0, sizeof(fake_ut));
	fake_ut.devh = (struct libusb_device_handle *)&fake;
}

int ubertooth_count(void) { return fake.device_count; }

ubertooth_t *ubertooth_start(int ubertooth_device)
{
	(void)ubertooth_device;
	return &fake_ut;
}

ubertooth_t *ubertooth_start_select(const char *u_arg)
{
	fake.start_calls++;
	snprintf(fake.selector, sizeof(fake.selector), "%s", u_arg ? u_arg : "");
	if (fake.fail_start_from && fake.start_calls >= fake.fail_start_from)
		return NULL;
	return &fake_ut;
}

void ubertooth_stop(ubertooth_t *ut) { (void)ut; }

int ubertooth_get_api(ubertooth_t *ut, uint16_t *version)
{
	(void)ut;
	*version = fake.api_version;
	return fake.api_rc;
}

void print_version(void) { printf("libubertooth fake\n"); }

int cmd_get_board_id(struct libusb_device_handle *h) { (void)h; return fake.board_id; }
int cmd_get_channel(struct libusb_device_handle *h) { (void)h; return fake.channel; }
int cmd_get_palevel(struct libusb_device_handle *h) { (void)h; return fake.palevel; }
int cmd_get_squelch(struct libusb_device_handle *h) { (void)h; return fake.squelch; }
int cmd_get_usrled(struct libusb_device_handle *h) { (void)h; return fake.usrled; }
int cmd_get_rxled(struct libusb_device_handle *h) { (void)h; return fake.rxled; }
int cmd_get_txled(struct libusb_device_handle *h) { (void)h; return fake.txled; }
int cmd_get_partnum(struct libusb_device_handle *h) { (void)h; return fake.partnum; }

int cmd_get_serial(struct libusb_device_handle *h, u8 *serial)
{
	int i;

	(void)h;
	serial[0] = 0;
	for (i = 1; i < 17; i++)
		serial[i] = (u8)(0xa0 + i);
	return fake.serial_rc;
}

void cmd_get_rev_num(struct libusb_device_handle *h, char *version, u8 len)
{
	(void)h;
	snprintf(version, len, "%s", fake.revision);
}

void cmd_get_compile_info(struct libusb_device_handle *h, char *info, u8 len)
{
	(void)h;
	snprintf(info, len, "%s", fake.compile_info);
}

int cmd_get_rangeresult(struct libusb_device_handle *h, rangetest_result *rr)
{
	(void)h;
	*rr = fake.range;
	return fake.rangeresult_rc;
}

int cmd_reset(struct libusb_device_handle *h) { (void)h; fake.reset_calls++; return fake.reset_rc; }
int cmd_stop(struct libusb_device_handle *h) { (void)h; fake.stop_calls++; return fake.stop_rc; }

int cmd_set_channel(struct libusb_device_handle *h, u16 channel)
{
	(void)h;
	fake.last_channel_set = channel;
	return fake.set_channel_rc;
}

int cmd_set_usrled(struct libusb_device_handle *h, u16 s) { (void)h; (void)s; return fake.set_usrled_rc; }
int cmd_set_rxled(struct libusb_device_handle *h, u16 s) { (void)h; (void)s; return fake.set_rxled_rc; }
int cmd_set_txled(struct libusb_device_handle *h, u16 s) { (void)h; (void)s; return fake.set_txled_rc; }
int cmd_set_palevel(struct libusb_device_handle *h, u16 l) { (void)h; (void)l; return fake.set_palevel_rc; }

int cmd_set_squelch(struct libusb_device_handle *h, u16 level)
{
	(void)h;
	fake.last_squelch_set = level;
	return fake.set_squelch_rc;
}

int cmd_flash(struct libusb_device_handle *h) { (void)h; fake.flash_calls++; return fake.flash_rc; }
int cmd_set_isp(struct libusb_device_handle *h) { (void)h; fake.isp_calls++; return fake.isp_rc; }
int cmd_range_test(struct libusb_device_handle *h) { (void)h; fake.range_test_calls++; return fake.range_test_rc; }
int cmd_repeater(struct libusb_device_handle *h) { (void)h; fake.repeater_calls++; return fake.repeater_rc; }
int cmd_tx_test(struct libusb_device_handle *h) { (void)h; fake.tx_test_calls++; return fake.tx_test_rc; }
int cmd_xmas(struct libusb_device_handle *h) { (void)h; fake.xmas_calls++; return fake.xmas_rc; }

int cmd_led_specan(struct libusb_device_handle *h, u16 rssi)
{
	(void)h;
	fake.led_specan_calls++;
	fake.last_led_specan_arg = rssi;
	return fake.led_specan_rc;
}

int cmd_do_something(struct libusb_device_handle *h, unsigned char *d, int n)
{
	(void)h; (void)d; (void)n;
	return 0;
}

int cmd_do_something_reply(struct libusb_device_handle *h, unsigned char *d, int n)
{
	(void)h; (void)d; (void)n;
	return 0;
}

/* ---- running main() and capturing what it prints ---------------------- */

struct run_result {
	int rc;
	char out[4096];
	char err[2048];
};

static void read_back(FILE *file, char *buffer, size_t size)
{
	size_t length;

	fflush(file);
	rewind(file);
	length = fread(buffer, 1, size - 1, file);
	buffer[length] = '\0';
	/* A capture that did not fit must fail loudly instead of comparing a prefix. */
	mu_assert(fgetc(file) == EOF, "captured output was truncated");
}

static struct run_result run_util(const char *const *args)
{
	struct run_result result;
	char *argv[32];
	int argc = 0;
	int saved_out, saved_err;
	FILE *out = tmpfile();
	FILE *err = tmpfile();

	memset(&result, 0, sizeof(result));
	result.rc = -1; /* a failed capture must not look like a successful run */
	mu_assert(out != NULL && err != NULL, "tmpfile failed");
	if (out == NULL || err == NULL)
		return result;
	while (args[argc] != NULL && argc < 31) {
		argv[argc] = (char *)args[argc];
		argc++;
	}
	argv[argc] = NULL;

	fflush(stdout);
	fflush(stderr);
	saved_out = dup(STDOUT_FILENO);
	saved_err = dup(STDERR_FILENO);
	dup2(fileno(out), STDOUT_FILENO);
	dup2(fileno(err), STDERR_FILENO);
	optind = 0; /* restart option parsing for each run */
	result.rc = ubertooth_util_main(argc, argv);
	fflush(stdout);
	fflush(stderr);
	dup2(saved_out, STDOUT_FILENO);
	dup2(saved_err, STDERR_FILENO);
	close(saved_out);
	close(saved_err);

	read_back(out, result.out, sizeof(result.out));
	read_back(err, result.err, sizeof(result.err));
	fclose(out);
	fclose(err);
	return result;
}

#define RUN(...) run_util((const char *const[]){ "ubertooth-util", __VA_ARGS__, NULL })

/* Exact expected report lines, in the order ubertooth-util prints them. */
#define BOARD_LINE "Board ID Number: 1 (Ubertooth One)\n"
#define CHANNEL_LINE "Current frequency: 2441 MHz (Bluetooth channel 39)\n"
#define FIRMWARE_LINE "Firmware version: git-test (API:1.07)\n"
#define COMPILE_LINE "compile-test\n"
#define USRLED_LINE "USR LED status: 0\n"
#define PALEVEL_LINE "PA Level: 7\n"
#define PART_LINE "Part ID: 25011723\n"
#define SERIAL_LINE "Serial No: a1a2a3a4a5a6a7a8a9aaabacadaeafb0\n"

/* ---- successful queries exit 0 even when the value is nonzero --------- */

MU_TEST(nonzero_query_values_are_not_exit_statuses)
{
	struct run_result r;

	reset_fake();
	r = RUN("-b");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, BOARD_LINE) == 0, "board line");

	r = RUN("-c");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, CHANNEL_LINE) == 0, "channel line");

	r = RUN("-a");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, PALEVEL_LINE) == 0, "PA level line");

	r = RUN("-z");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, "Squelch set to -120\n") == 0, "squelch line");

	fake.usrled = 1;
	r = RUN("-l");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, "USR LED status: 1\n") == 0, "LED line");
}

MU_TEST(every_known_board_id_exits_zero)
{
	static const char *const names[] = {
		"Ubertooth Zero", "Ubertooth One", "ToorCon 13 Badge",
	};
	int id;

	for (id = 0; id < 3; id++) {
		struct run_result r;
		char expected[64];

		reset_fake();
		fake.board_id = id;
		r = RUN("-b");
		snprintf(expected, sizeof(expected), "Board ID Number: %d (%s)\n", id, names[id]);
		mu_assert_int_eq(0, r.rc);
		mu_assert(strcmp(r.out, expected) == 0, "known board ID line");
	}
}

MU_TEST(unknown_board_id_fails_without_indexing_the_name_table)
{
	int unknown[] = { 3, 255, 100000 };
	size_t i;

	for (i = 0; i < sizeof(unknown) / sizeof(unknown[0]); i++) {
		struct run_result r;

		reset_fake();
		fake.board_id = unknown[i];
		r = RUN("-b");
		mu_assert_int_eq(1, r.rc);
		mu_assert_int_eq(0, (long)strlen(r.out));
		mu_assert(strstr(r.err, "unknown board ID") != NULL, "diagnostic");
	}
}

MU_TEST(device_access_applies_the_selector_and_fails_cleanly_without_a_device)
{
	struct run_result r;

	reset_fake();
	r = RUN("-U", "abc123", "-b");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(fake.selector, "abc123") == 0, "selector passed to the library");

	reset_fake();
	fake.fail_start_from = 1;
	r = RUN("-b");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(0, (long)strlen(r.out));
}

/* ---- composed options: output order and status ------------------------ */

MU_TEST(composed_queries_print_in_the_documented_order_and_exit_zero)
{
	struct run_result r;

	reset_fake();
	r = RUN("-b", "-c", "-v", "-V", "-l", "-a", "-p", "-s");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out,
	                 BOARD_LINE CHANNEL_LINE FIRMWARE_LINE COMPILE_LINE
	                 USRLED_LINE PALEVEL_LINE PART_LINE SERIAL_LINE) == 0,
	          "composed output order");

	/* The identity readback used by the hardware checks. */
	r = RUN("-vVspb");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out,
	                 BOARD_LINE FIRMWARE_LINE COMPILE_LINE PART_LINE SERIAL_LINE) == 0,
	          "identity readback order");
}

MU_TEST(an_earlier_failure_is_not_masked_by_later_successes)
{
	struct run_result r;

	reset_fake();
	fake.channel = -1; /* the channel read fails */
	r = RUN("-c", "-a", "-b");
	mu_assert_int_eq(1, r.rc);
	/* later reports still ran */
	mu_assert(strstr(r.out, PALEVEL_LINE) != NULL, "PA level still reported");
	mu_assert(strstr(r.out, BOARD_LINE) != NULL, "board ID still reported");

	reset_fake();
	fake.board_id = 255; /* the first report fails */
	r = RUN("-b", "-v", "-V", "-p", "-s");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.out, FIRMWARE_LINE) != NULL, "firmware still reported");
	mu_assert(strstr(r.out, SERIAL_LINE) != NULL, "serial still reported");
}

MU_TEST(each_failing_report_makes_the_command_fail)
{
	struct run_result r;

	reset_fake();
	fake.partnum = -1;
	r = RUN("-p", "-s");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.out, "Part ID") == NULL, "no part ID line on error");
	mu_assert(strstr(r.out, SERIAL_LINE) != NULL, "serial still reported");

	reset_fake();
	fake.serial_rc = 1; /* nonzero status byte */
	r = RUN("-s");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.out, "Serial No") == NULL, "no serial line on error");

	reset_fake();
	fake.api_rc = -1;
	r = RUN("-v");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.out, "Firmware version: git-test\n") == 0, "firmware line without API");

	reset_fake();
	fake.palevel = -1;
	r = RUN("-a");
	mu_assert_int_eq(1, r.rc);

	reset_fake();
	fake.squelch = -1;
	r = RUN("-z");
	mu_assert_int_eq(1, r.rc);

	reset_fake();
	fake.usrled = -1;
	r = RUN("-l");
	mu_assert_int_eq(1, r.rc);
}

MU_TEST(all_leds_query_reports_each_led_and_fails_if_any_read_fails)
{
	struct run_result r;

	reset_fake();
	fake.txled = 1;
	r = RUN("-d");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out,
	                 "USR LED status: 0\nRX LED status : 0\nTX LED status : 1\n") == 0,
	          "LED lines");

	reset_fake();
	fake.rxled = -1;
	r = RUN("-d");
	mu_assert_int_eq(1, r.rc);
}

MU_TEST(range_result_is_reported_and_a_failed_read_fails)
{
	struct run_result r;

	reset_fake();
	r = RUN("-m");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strstr(r.out, "request PA level : 1\n") != NULL, "range output");
	mu_assert(strstr(r.out, "reply number     : 4\n") != NULL, "range output");

	reset_fake();
	fake.rangeresult_rc = -1;
	r = RUN("-m");
	mu_assert_int_eq(1, r.rc);
}

/* ---- commands that change the device ---------------------------------- */

MU_TEST(setters_pass_their_arguments_and_report_failures)
{
	struct run_result r;

	reset_fake();
	r = RUN("-C39");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(2441, fake.last_channel_set);

	reset_fake();
	r = RUN("-c2450");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(2450, fake.last_channel_set);

	reset_fake();
	fake.set_channel_rc = -1;
	r = RUN("-C39", "-b"); /* a later success must not hide the failed set */
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.out, BOARD_LINE) == 0, "later report still printed");

	reset_fake();
	r = RUN("-z5");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(5, fake.last_squelch_set);
	mu_assert(strcmp(r.out, "Setting squelch to 5\n") == 0, "squelch message");

	reset_fake();
	fake.set_squelch_rc = -1;
	r = RUN("-z5");
	mu_assert_int_eq(1, r.rc);

	reset_fake();
	fake.set_palevel_rc = -1;
	r = RUN("-a3");
	mu_assert_int_eq(1, r.rc);

	reset_fake();
	fake.set_usrled_rc = -1;
	r = RUN("-l1");
	mu_assert_int_eq(1, r.rc);

	reset_fake();
	fake.set_txled_rc = -1;
	r = RUN("-d1");
	mu_assert_int_eq(1, r.rc);
	reset_fake();
	fake.set_rxled_rc = -1;
	r = RUN("-d1");
	mu_assert_int_eq(1, r.rc);
	reset_fake();
	fake.set_usrled_rc = -1;
	r = RUN("-d1");
	mu_assert_int_eq(1, r.rc);

	reset_fake();
	r = RUN("-d1", "-l1", "-a3");
	mu_assert_int_eq(0, r.rc);
}

MU_TEST(stop_and_reset_report_failures)
{
	struct run_result r;

	reset_fake();
	r = RUN("-S");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(1, fake.stop_calls);

	reset_fake();
	fake.stop_rc = -1;
	r = RUN("-S", "-b");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.out, BOARD_LINE) != NULL, "later report still printed");

	reset_fake();
	r = RUN("-r");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(1, fake.reset_calls);
	mu_assert_int_eq(2, fake.start_calls); /* reconnects after the reset */

	reset_fake();
	fake.reset_rc = -1;
	r = RUN("-r");
	mu_assert_int_eq(1, r.rc);

	/* the device cannot be reselected after the reset: fail, do not crash */
	reset_fake();
	fake.fail_start_from = 2;
	r = RUN("-r");
	mu_assert_int_eq(1, r.rc);
}

/* ---- mode commands: success and failure ------------------------------- */

static const char *const mode_options[] = { "-f", "-i", "-q", "-n", "-e", "-t", "-x" };
#define MODE_COUNT (sizeof(mode_options) / sizeof(mode_options[0]))

/* The scripted result and call counter for the i-th mode command. */
static int *mode_result(size_t i)
{
	int *results[] = {
		&fake.flash_rc, &fake.isp_rc, &fake.led_specan_rc, &fake.range_test_rc,
		&fake.repeater_rc, &fake.tx_test_rc, &fake.xmas_rc,
	};
	return results[i];
}

static int mode_calls(size_t i)
{
	int calls[] = {
		fake.flash_calls, fake.isp_calls, fake.led_specan_calls,
		fake.range_test_calls, fake.repeater_calls, fake.tx_test_calls,
		fake.xmas_calls,
	};
	return calls[i];
}

MU_TEST(mode_commands_exit_zero_on_success_and_one_on_failure)
{
	size_t i;

	for (i = 0; i < MODE_COUNT; i++) {
		struct run_result r;
		/* -f and -i start no mode, so they do not stop the device first */
		int stops_first = (i >= 2);

		reset_fake();
		r = run_util((const char *const[]){ "ubertooth-util", mode_options[i], NULL });
		mu_assert_int_eq(0, r.rc);
		mu_assert_int_eq(1, mode_calls(i));
		mu_assert_int_eq(stops_first, fake.stop_calls);

		reset_fake();
		*mode_result(i) = -1;
		r = run_util((const char *const[]){ "ubertooth-util", mode_options[i], NULL });
		mu_assert_int_eq(1, r.rc);
		mu_assert_int_eq(1, mode_calls(i));
	}
}

MU_TEST(a_failed_stop_before_a_mode_command_fails_the_command)
{
	size_t i;

	for (i = 2; i < MODE_COUNT; i++) { /* the modes that stop the device first */
		struct run_result r;

		reset_fake();
		fake.stop_rc = -1;
		r = run_util((const char *const[]){ "ubertooth-util", mode_options[i], NULL });
		mu_assert_int_eq(1, r.rc);
		mu_assert_int_eq(1, mode_calls(i)); /* the mode still ran */
	}
}

MU_TEST(led_specan_uses_the_default_threshold_and_an_explicit_one)
{
	struct run_result r;

	reset_fake();
	r = RUN("-q");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(225, fake.last_led_specan_arg);

	reset_fake();
	r = RUN("-q100");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(100, fake.last_led_specan_arg);
}

/* ---- argument handling that must not touch the device ----------------- */

MU_TEST(invalid_arguments_are_rejected_before_opening_the_device)
{
	struct run_result r;

	reset_fake();
	r = RUN("-C99");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "channel must be 0-78") != NULL, "channel diagnostic");
	mu_assert_int_eq(0, fake.start_calls);

	reset_fake();
	r = RUN("-c2400");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "frequency must be 2402-2480") != NULL, "frequency diagnostic");
	mu_assert_int_eq(0, fake.start_calls);

	reset_fake();
	r = RUN("-Y");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "ubertooth-util - command line utility") != NULL, "usage on stderr");
	mu_assert_int_eq(0, fake.start_calls);
}

MU_TEST(help_version_and_count_exit_zero_without_a_device)
{
	struct run_result r;

	reset_fake();
	r = RUN("-h");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strstr(r.out, "ubertooth-util - command line utility") != NULL, "usage on stdout");

	r = RUN("-H");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, "libubertooth fake\n") == 0, "version");

	r = RUN("--host-version");
	mu_assert_int_eq(0, r.rc);

	fake.device_count = 3;
	r = RUN("-N");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, "3\n") == 0, "device count");
	mu_assert_int_eq(0, fake.start_calls);
}

MU_TEST(firmware_listing_with_several_devices_reports_each_device)
{
	struct run_result r;

	reset_fake();
	fake.device_count = 2;
	r = RUN("-v");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out,
	                 "0: git-test (API:1.07)\n1: git-test (API:1.07)\n") == 0,
	          "per-device firmware lines");
}

MU_TEST(test_command_reads_back_through_the_library_and_exits_zero)
{
	struct run_result r;

	reset_fake();
	r = RUN("-9");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, "55 55 55 55\n") == 0, "reply bytes");
}

MU_TEST(serial_listing_with_several_devices_exits_zero)
{
	struct run_result r;

	reset_fake();
	fake.device_count = 2;
	r = RUN("-s");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strstr(r.out, "0: a1a2a3a4a5a6a7a8a9aaabacadaeafb0\n") != NULL, "first serial");
	mu_assert(strstr(r.out, "1: a1a2a3a4a5a6a7a8a9aaabacadaeafb0\n") != NULL, "second serial");
}

int main(void)
{
	MU_RUN(nonzero_query_values_are_not_exit_statuses);
	MU_RUN(every_known_board_id_exits_zero);
	MU_RUN(unknown_board_id_fails_without_indexing_the_name_table);
	MU_RUN(device_access_applies_the_selector_and_fails_cleanly_without_a_device);
	MU_RUN(composed_queries_print_in_the_documented_order_and_exit_zero);
	MU_RUN(an_earlier_failure_is_not_masked_by_later_successes);
	MU_RUN(each_failing_report_makes_the_command_fail);
	MU_RUN(all_leds_query_reports_each_led_and_fails_if_any_read_fails);
	MU_RUN(range_result_is_reported_and_a_failed_read_fails);
	MU_RUN(setters_pass_their_arguments_and_report_failures);
	MU_RUN(stop_and_reset_report_failures);
	MU_RUN(mode_commands_exit_zero_on_success_and_one_on_failure);
	MU_RUN(a_failed_stop_before_a_mode_command_fails_the_command);
	MU_RUN(led_specan_uses_the_default_threshold_and_an_explicit_one);
	MU_RUN(invalid_arguments_are_rejected_before_opening_the_device);
	MU_RUN(help_version_and_count_exit_zero_without_a_device);
	MU_RUN(firmware_listing_with_several_devices_reports_each_device);
	MU_RUN(test_command_reads_back_through_the_library_and_exits_zero);
	MU_RUN(serial_listing_with_several_devices_exits_zero);
	return mu_report();
}
