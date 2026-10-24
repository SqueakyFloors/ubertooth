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
	int board_id, channel, palevel, squelch, usrled, rxled, txled;
	uint32_t partnum;
	int partnum_rc, serial_rc, api_rc, revision_rc, compile_info_rc;
	uint16_t api_version;
	const char *revision, *compile_info;
	/* results of commands that set state or start a mode */
	int stop_rc, reset_rc, set_channel_rc, set_usrled_rc, set_rxled_rc;
	int set_txled_rc, set_palevel_rc, set_squelch_rc, flash_rc, isp_rc;
	int led_specan_rc, range_test_rc, repeater_rc, tx_test_rc, xmas_rc;
	int rangeresult_rc, do_something_rc, do_something_reply_rc;
	rangetest_result range;
	/* bookkeeping */
	int device_count;
	int start_calls;
	int fail_start_from; /* 0: never; n: ubertooth_start_select fails from call n */
	char selector[64];
	int stop_calls, flash_calls, isp_calls, led_specan_calls, range_test_calls;
	int repeater_calls, tx_test_calls, xmas_calls, reset_calls;
	int last_channel_set, last_squelch_set, last_led_specan_arg;
	/* multi-device listing */
	int current_index;     /* device opened by ubertooth_start(), or -1 */
	int unavailable_index; /* ubertooth_start() fails for this index */
	int board_fail_index;  /* the board ID read fails on this device */
	int revision_fail_index;     /* the firmware revision read fails here */
	int compile_info_fail_index; /* the compile info read fails here */
	int indexed_starts, closes, frees;
	/* -r: the handle opened before the reset (the first select) */
	int first_closes, first_frees, first_stops; /* STOP sent to that handle */
	int first_used_after_release; /* any command on it after it was released */
	int first_closed_before_reselect;
	int first_released;
} fake;

/* Every select returns fake_ut, except that the select after a reset returns
 * fake_ut_reset, so the pre-reset handle can be told apart from the new one. */
static ubertooth_t fake_ut, fake_ut_reset;
static int fake_devh_reset; /* distinct address for fake_ut_reset.devh */

#define FIRST_DEVH ((struct libusb_device_handle *)&fake)

/* Record a command sent to the device handle h. */
static void fake_use(struct libusb_device_handle *h)
{
	if (h == FIRST_DEVH && fake.first_released)
		fake.first_used_after_release++;
}

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
	fake.current_index = -1;
	fake.unavailable_index = -1;
	fake.board_fail_index = -1;
	fake.revision_fail_index = -1;
	fake.compile_info_fail_index = -1;
	memset(&fake_ut, 0, sizeof(fake_ut));
	fake_ut.devh = FIRST_DEVH;
	memset(&fake_ut_reset, 0, sizeof(fake_ut_reset));
	fake_ut_reset.devh = (struct libusb_device_handle *)&fake_devh_reset;
}

int ubertooth_count(void) { return fake.device_count; }

ubertooth_t *ubertooth_start(int ubertooth_device)
{
	fake.indexed_starts++;
	if (ubertooth_device == fake.unavailable_index)
		return NULL;
	fake.current_index = ubertooth_device;
	return &fake_ut;
}

ubertooth_t *ubertooth_start_select(const char *u_arg)
{
	fake.start_calls++;
	snprintf(fake.selector, sizeof(fake.selector), "%s", u_arg ? u_arg : "");
	if (fake.reset_calls > 0)
		fake.first_closed_before_reselect = fake.first_released;
	if (fake.fail_start_from && fake.start_calls >= fake.fail_start_from)
		return NULL;
	if (fake.reset_calls > 0)
		return &fake_ut_reset;
	return &fake_ut;
}

/* Like the library, stop also sends STOP to the device. */
void ubertooth_stop(ubertooth_t *ut)
{
	if (ut == &fake_ut) {
		fake_use(ut->devh);
		fake.first_stops++;
	}
	fake.stop_calls++;
	fake.current_index = -1;
}

void ubertooth_close(ubertooth_t *ut)
{
	if (ut == &fake_ut) {
		fake.first_closes++;
		fake.first_released = 1;
	}
	fake.closes++;
	fake.current_index = -1;
}

void ubertooth_free(ubertooth_t *ut)
{
	if (ut == &fake_ut) {
		fake.first_frees++;
		fake.first_released = 1;
	}
	fake.frees++;
}

int ubertooth_get_api(ubertooth_t *ut, uint16_t *version)
{
	fake_use(ut->devh);
	*version = fake.api_version;
	return fake.api_rc;
}

void print_version(void) { printf("libubertooth fake\n"); }

int cmd_get_board_id(struct libusb_device_handle *h)
{
	fake_use(h);
	if (fake.current_index >= 0 && fake.current_index == fake.board_fail_index)
		return -1;
	return fake.board_id;
}
int cmd_get_channel(struct libusb_device_handle *h) { fake_use(h); return fake.channel; }
int cmd_get_palevel(struct libusb_device_handle *h) { fake_use(h); return fake.palevel; }
int cmd_get_squelch(struct libusb_device_handle *h) { fake_use(h); return fake.squelch; }
int cmd_get_usrled(struct libusb_device_handle *h) { fake_use(h); return fake.usrled; }
int cmd_get_rxled(struct libusb_device_handle *h) { fake_use(h); return fake.rxled; }
int cmd_get_txled(struct libusb_device_handle *h) { fake_use(h); return fake.txled; }
int cmd_get_part_id(struct libusb_device_handle *h, u32 *part_id)
{
	fake_use(h);
	*part_id = fake.partnum;
	return fake.partnum_rc;
}

int cmd_get_serial(struct libusb_device_handle *h, u8 *serial)
{
	int i;

	fake_use(h);
	serial[0] = 0;
	for (i = 1; i < 17; i++)
		serial[i] = (u8)(0xa0 + i);
	return fake.serial_rc;
}

/* Like the library: on failure the buffer is left empty. The void
 * cmd_get_rev_num()/cmd_get_compile_info() are deliberately not faked, so the
 * test fails to link if ubertooth-util goes back to them. */
static int fake_text_read(struct libusb_device_handle *h, char *text,
                          size_t size, const char *value, int rc,
                          int fail_index)
{
	fake_use(h);
	text[0] = '\0';
	if (fake.current_index >= 0 && fake.current_index == fail_index)
		return LIBUSB_ERROR_TIMEOUT;
	if (rc < 0)
		return rc;
	snprintf(text, size, "%s", value);
	return 0;
}

int cmd_get_firmware_revision(struct libusb_device_handle *h, char *revision,
                              size_t size)
{
	return fake_text_read(h, revision, size, fake.revision, fake.revision_rc,
	                      fake.revision_fail_index);
}

int cmd_get_firmware_compile_info(struct libusb_device_handle *h, char *info,
                                  size_t size)
{
	return fake_text_read(h, info, size, fake.compile_info,
	                      fake.compile_info_rc, fake.compile_info_fail_index);
}

int cmd_get_rangeresult(struct libusb_device_handle *h, rangetest_result *rr)
{
	fake_use(h);
	*rr = fake.range;
	return fake.rangeresult_rc;
}

int cmd_reset(struct libusb_device_handle *h) { fake_use(h); fake.reset_calls++; return fake.reset_rc; }
int cmd_stop(struct libusb_device_handle *h)
{
	fake_use(h);
	if (h == FIRST_DEVH)
		fake.first_stops++;
	fake.stop_calls++;
	return fake.stop_rc;
}

int cmd_set_channel(struct libusb_device_handle *h, u16 channel)
{
	fake_use(h);
	fake.last_channel_set = channel;
	return fake.set_channel_rc;
}

int cmd_set_usrled(struct libusb_device_handle *h, u16 s) { fake_use(h); (void)s; return fake.set_usrled_rc; }
int cmd_set_rxled(struct libusb_device_handle *h, u16 s) { fake_use(h); (void)s; return fake.set_rxled_rc; }
int cmd_set_txled(struct libusb_device_handle *h, u16 s) { fake_use(h); (void)s; return fake.set_txled_rc; }
int cmd_set_palevel(struct libusb_device_handle *h, u16 l) { fake_use(h); (void)l; return fake.set_palevel_rc; }

int cmd_set_squelch(struct libusb_device_handle *h, u16 level)
{
	fake_use(h);
	fake.last_squelch_set = level;
	return fake.set_squelch_rc;
}

int cmd_flash(struct libusb_device_handle *h) { fake_use(h); fake.flash_calls++; return fake.flash_rc; }
int cmd_set_isp(struct libusb_device_handle *h) { fake_use(h); fake.isp_calls++; return fake.isp_rc; }
int cmd_range_test(struct libusb_device_handle *h) { fake_use(h); fake.range_test_calls++; return fake.range_test_rc; }
int cmd_repeater(struct libusb_device_handle *h) { fake_use(h); fake.repeater_calls++; return fake.repeater_rc; }
int cmd_tx_test(struct libusb_device_handle *h) { fake_use(h); fake.tx_test_calls++; return fake.tx_test_rc; }
int cmd_xmas(struct libusb_device_handle *h) { fake_use(h); fake.xmas_calls++; return fake.xmas_rc; }

int cmd_led_specan(struct libusb_device_handle *h, u16 rssi)
{
	fake_use(h);
	fake.led_specan_calls++;
	fake.last_led_specan_arg = rssi;
	return fake.led_specan_rc;
}

int cmd_do_something(struct libusb_device_handle *h, unsigned char *d, int n)
{
	fake_use(h); (void)d; (void)n;
	return fake.do_something_rc;
}

int cmd_do_something_reply(struct libusb_device_handle *h, unsigned char *d, int n)
{
	fake_use(h); (void)d; (void)n;
	return fake.do_something_reply_rc;
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

MU_TEST(part_id_with_bit_31_set_is_reported_in_full)
{
	struct run_result r;

	reset_fake();
	fake.partnum = 0x8000ABCDu;
	r = RUN("-p");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, "Part ID: 8000ABCD\n") == 0,
	          "full 32-bit part ID not printed");
}

MU_TEST(each_failing_report_makes_the_command_fail)
{
	struct run_result r;

	reset_fake();
	fake.partnum_rc = LIBUSB_ERROR_OTHER; /* nonzero status byte */
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
	fake.revision_rc = LIBUSB_ERROR_TIMEOUT;
	r = RUN("-v");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(0, (long)strlen(r.out)); /* no "Firmware version: error" */

	reset_fake();
	fake.revision_rc = LIBUSB_ERROR_IO; /* short reply */
	r = RUN("-v", "-p");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.out, PART_LINE) == 0, "part ID still reported");

	reset_fake();
	fake.compile_info_rc = LIBUSB_ERROR_PIPE;
	r = RUN("-V");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(0, (long)strlen(r.out));

	reset_fake();
	fake.compile_info_rc = LIBUSB_ERROR_IO;
	r = RUN("-v", "-V", "-s");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.out, FIRMWARE_LINE SERIAL_LINE) == 0,
	          "other reports still printed around a failed compile info");

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
	/* the pre-reset handle is dead: released once, before the reselect,
	 * without STOP, and never used again */
	mu_assert_int_eq(1, fake.first_closes);
	mu_assert_int_eq(1, fake.first_frees);
	mu_assert_int_eq(0, fake.first_stops);
	mu_assert_int_eq(1, fake.first_closed_before_reselect);
	mu_assert_int_eq(0, fake.first_used_after_release);

	/* later commands go to the new handle, not the released one */
	reset_fake();
	r = RUN("-r", "-S", "-b");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strstr(r.out, BOARD_LINE) != NULL, "board read after reset");
	mu_assert_int_eq(1, fake.stop_calls); /* -S, on the new handle */
	mu_assert_int_eq(0, fake.first_stops);
	mu_assert_int_eq(1, fake.first_closes);
	mu_assert_int_eq(1, fake.first_frees);
	mu_assert_int_eq(0, fake.first_used_after_release);

	reset_fake();
	fake.reset_rc = -1;
	r = RUN("-r");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(1, fake.first_closes); /* released even if the reset failed */
	mu_assert_int_eq(1, fake.first_frees);
	mu_assert_int_eq(0, fake.first_stops);
	mu_assert_int_eq(1, fake.first_closed_before_reselect);
	mu_assert_int_eq(0, fake.first_used_after_release);

	/* the device cannot be reselected after the reset: fail, do not crash */
	reset_fake();
	fake.fail_start_from = 2;
	r = RUN("-r");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(1, fake.first_closes);
	mu_assert_int_eq(1, fake.first_frees);
	mu_assert_int_eq(0, fake.first_stops);
	mu_assert_int_eq(1, fake.first_closed_before_reselect);
	mu_assert_int_eq(0, fake.first_used_after_release);
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

MU_TEST(two_or_more_mode_options_are_rejected_before_opening_the_device)
{
	/* -I is included: alone it blinks until interrupted, but combined it
	 * must be refused before the loop is ever reached. */
	static const char *const modes[] = {
		"-q", "-n", "-e", "-t", "-x", "-I", "-i", "-f",
	};
	size_t count = sizeof(modes) / sizeof(modes[0]);
	size_t a, b, i;
	struct run_result r;

	for (a = 0; a < count; a++) {
		for (b = 0; b < count; b++) {
			if (a == b)
				continue;
			reset_fake();
			r = run_util((const char *const[]){ "ubertooth-util", "-U", "abc123",
			                                    modes[a], modes[b], NULL });
			mu_assert_int_eq(1, r.rc);
			mu_assert(strstr(r.err, "cannot be combined") != NULL, "conflict diagnostic");
			mu_assert(strstr(r.err, "no longer required") == NULL, "only the conflict is reported");
			mu_assert_int_eq(0, (long)strlen(r.out));
			mu_assert_int_eq(0, fake.start_calls);
			mu_assert_int_eq(0, fake.stop_calls);
			for (i = 0; i < MODE_COUNT; i++)
				mu_assert_int_eq(0, mode_calls(i));
		}
	}

	/* an argument to -q and other options in the command do not matter */
	reset_fake();
	r = RUN("-q100", "-n");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "error: options -q -n cannot be combined") != NULL,
	          "names the given options");
	mu_assert_int_eq(0, fake.start_calls);

	reset_fake();
	r = RUN("-r", "-C39", "-t", "-x", "-I");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "error: options -t -x -I cannot be combined") != NULL,
	          "names every given mode");
	mu_assert_int_eq(0, fake.start_calls);
	mu_assert_int_eq(0, fake.reset_calls);
	mu_assert_int_eq(0, fake.last_channel_set);

	/* refused even where the command would otherwise not open a device */
	reset_fake();
	r = RUN("-N", "-q", "-x");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(0, (long)strlen(r.out));

	/* the same option twice is still one mode */
	reset_fake();
	r = RUN("-x", "-x");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(1, fake.xmas_calls);
}

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

#define SERIAL_VALUE "a1a2a3a4a5a6a7a8a9aaabacadaeafb0"

MU_TEST(each_identity_query_alone_lists_every_device_compactly)
{
	struct { const char *option; const char *expected; } cases[] = {
		{ "-b", "0: 1 (Ubertooth One)\n1: 1 (Ubertooth One)\n" },
		{ "-v", "0: git-test (API:1.07)\n1: git-test (API:1.07)\n" },
		{ "-V", "0: compile-test\n1: compile-test\n" },
		{ "-p", "0: 25011723\n1: 25011723\n" },
		{ "-s", "0: " SERIAL_VALUE "\n1: " SERIAL_VALUE "\n" },
	};
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		struct run_result r;

		reset_fake();
		fake.device_count = 2;
		r = run_util((const char *const[]){ "ubertooth-util", cases[i].option, NULL });
		mu_assert_int_eq(0, r.rc);
		mu_assert(strcmp(r.out, cases[i].expected) == 0, cases[i].option);
		mu_assert_int_eq(2, fake.indexed_starts);
		mu_assert_int_eq(2, fake.closes); /* every opened device is closed */
		mu_assert_int_eq(2, fake.frees);
		mu_assert_int_eq(0, fake.stop_calls); /* a query never sends STOP */
		mu_assert_int_eq(0, fake.start_calls); /* no single-device selection */
	}
}

MU_TEST(combined_identity_queries_list_every_line_for_every_device)
{
	struct run_result r;

	reset_fake();
	fake.device_count = 2;
	r = RUN("-vVspb");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out,
	                 "0: " BOARD_LINE "0: " FIRMWARE_LINE "0: " COMPILE_LINE
	                 "0: " PART_LINE "0: " SERIAL_LINE
	                 "1: " BOARD_LINE "1: " FIRMWARE_LINE "1: " COMPILE_LINE
	                 "1: " PART_LINE "1: " SERIAL_LINE) == 0,
	          "per-device report lines in report order");
}

MU_TEST(an_unavailable_device_is_reported_and_fails_the_listing)
{
	struct run_result r;

	reset_fake();
	fake.device_count = 3;
	fake.unavailable_index = 1;
	r = RUN("-b");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.out,
	                 "0: 1 (Ubertooth One)\n1: <unavailable>\n2: 1 (Ubertooth One)\n") == 0,
	          "other devices still listed");
	mu_assert_int_eq(2, fake.closes);
	mu_assert_int_eq(0, fake.stop_calls);
}

MU_TEST(a_failed_read_on_one_device_is_reported_and_fails_the_listing)
{
	struct run_result r;

	reset_fake();
	fake.device_count = 2;
	fake.board_fail_index = 0;
	r = RUN("-b");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.out, "0: <board ID read failed>\n1: 1 (Ubertooth One)\n") == 0,
	          "failed read line");

	reset_fake();
	fake.device_count = 2;
	fake.board_fail_index = 1;
	r = RUN("-b", "-s");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.out, "1: <board ID read failed>\n") != NULL, "failed read line");
	mu_assert(strstr(r.out, "1: " SERIAL_LINE) != NULL, "later query still reported");

	reset_fake();
	fake.device_count = 2;
	fake.serial_rc = 1;
	r = RUN("-s");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.out, "0: <serial read failed>\n1: <serial read failed>\n") == 0,
	          "serial read failure text is unchanged");

	reset_fake();
	fake.device_count = 2;
	fake.board_id = 7; /* read succeeds but the ID is not a known board */
	r = RUN("-b");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.out,
	                 "0: <error: unknown board ID 7>\n1: <error: unknown board ID 7>\n") == 0,
	          "unknown board ID shown per device");
	mu_assert_int_eq(0, (long)strlen(r.err));

	reset_fake();
	fake.device_count = 2;
	fake.api_rc = -1;
	r = RUN("-v");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.out, "0: git-test\n1: git-test\n") == 0, "revision without API");

	reset_fake();
	fake.device_count = 2;
	fake.revision_fail_index = 0;
	r = RUN("-v");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.out,
	                 "0: <firmware read failed>\n1: git-test (API:1.07)\n") == 0,
	          "failed revision read shown per device");

	reset_fake();
	fake.device_count = 2;
	fake.compile_info_fail_index = 1;
	r = RUN("-V");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.out, "0: compile-test\n1: <compile info read failed>\n") == 0,
	          "failed compile info read shown per device");

	reset_fake();
	fake.device_count = 2;
	fake.revision_fail_index = 1;
	fake.compile_info_fail_index = 1;
	r = RUN("-U", "all", "-vV");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.out,
	                 "0: " FIRMWARE_LINE "0: " COMPILE_LINE
	                 "1: <firmware read failed>\n1: <compile info read failed>\n") == 0,
	          "-U all shows both failed reads and keeps the good device");
}

MU_TEST(listing_is_not_used_with_a_selector_one_device_or_other_operations)
{
	struct run_result r;

	reset_fake();
	fake.device_count = 2;
	r = RUN("-U", "1", "-b");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, BOARD_LINE) == 0, "single device with -U");
	mu_assert_int_eq(0, fake.indexed_starts);
	mu_assert(strcmp(fake.selector, "1") == 0, "selector passed");

	reset_fake();
	fake.device_count = 1;
	r = RUN("-b");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, BOARD_LINE) == 0, "single device without -U");
	mu_assert_int_eq(0, fake.indexed_starts);

	/* A device count error falls back to normal selection. */
	reset_fake();
	fake.device_count = -1;
	r = RUN("-b");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, BOARD_LINE) == 0, "single-device report");
	mu_assert_int_eq(0, fake.indexed_starts);
	mu_assert_int_eq(1, fake.start_calls);

	/* The listing only needs identity queries; -U-less commands with any other
	 * option, including the ones listed here, use normal selection. */
	{
		const char *const others[] = { "-c", "-a", "-l", "-d", "-z", "-m", "-S", "-9" };
		size_t i;

		for (i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
			reset_fake();
			fake.device_count = 2;
			r = run_util((const char *const[]){ "ubertooth-util", "-s", others[i], NULL });
			mu_assert_int_eq(0, fake.indexed_starts);
			mu_assert_int_eq(1, fake.start_calls);
		}
	}

	/* Another operation in the same command needs a single selected device,
	 * so it goes through the normal selection (which refuses on real hardware). */
	reset_fake();
	fake.device_count = 2;
	r = RUN("-s", "-c");
	mu_assert_int_eq(0, fake.indexed_starts);
	mu_assert_int_eq(1, fake.start_calls);
	mu_assert(strstr(r.out, "Serial No: ") != NULL, "single-device report format");
}

MU_TEST(u_all_lists_every_device_even_when_only_one_is_attached)
{
	struct run_result r;

	reset_fake();
	fake.device_count = 1;
	r = RUN("-U", "all", "-s");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, "0: " SERIAL_VALUE "\n") == 0, "single device listed");
	mu_assert_int_eq(1, fake.indexed_starts);
	mu_assert_int_eq(1, fake.closes);
	mu_assert_int_eq(0, fake.start_calls); /* no single-device selection */
	mu_assert_int_eq(0, fake.stop_calls);

	reset_fake();
	fake.device_count = 2;
	r = RUN("-Uall", "-vVspb");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out,
	                 "0: " BOARD_LINE "0: " FIRMWARE_LINE "0: " COMPILE_LINE
	                 "0: " PART_LINE "0: " SERIAL_LINE
	                 "1: " BOARD_LINE "1: " FIRMWARE_LINE "1: " COMPILE_LINE
	                 "1: " PART_LINE "1: " SERIAL_LINE) == 0,
	          "same listing as the implicit multi-device form");
	mu_assert_int_eq(0, fake.start_calls);
	mu_assert_int_eq(0, fake.stop_calls);

	/* the option may follow the queries */
	reset_fake();
	fake.device_count = 2;
	r = RUN("-b", "-U", "all");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, "0: 1 (Ubertooth One)\n1: 1 (Ubertooth One)\n") == 0,
	          "listing with -U all last");

	/* a failure on one device still fails the command */
	reset_fake();
	fake.device_count = 2;
	fake.unavailable_index = 0;
	r = RUN("-U", "all", "-b");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.out, "0: <unavailable>\n1: 1 (Ubertooth One)\n") == 0,
	          "unavailable device line");
}

MU_TEST(u_all_fails_cleanly_with_no_devices_or_a_count_error)
{
	struct run_result r;

	reset_fake();
	fake.device_count = 0;
	r = RUN("-U", "all", "-s");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(0, (long)strlen(r.out));
	mu_assert(strstr(r.err, "no Ubertooth devices found") != NULL, "no-device diagnostic");
	mu_assert_int_eq(0, fake.indexed_starts);
	mu_assert_int_eq(0, fake.start_calls);

	reset_fake();
	fake.device_count = -1;
	r = RUN("-U", "all", "-s");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(0, (long)strlen(r.out));
	mu_assert_int_eq(0, fake.indexed_starts);
	mu_assert_int_eq(0, fake.start_calls);
}

MU_TEST(u_all_is_rejected_before_opening_a_device_for_any_other_option)
{
	static const char *const others[] = {
		"-c", "-c2450", "-C39", "-a", "-a3", "-l", "-l1", "-d", "-d1",
		"-z", "-z5", "-m", "-S", "-r", "-I", "-9", "-N",
		"-f", "-i", "-q", "-n", "-e", "-t", "-x",
	};
	size_t i;

	for (i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
		struct run_result r;

		/* alone, and combined with a valid query */
		reset_fake();
		fake.device_count = 2;
		r = run_util((const char *const[]){ "ubertooth-util", "-U", "all", others[i], NULL });
		mu_assert_int_eq(1, r.rc);
		mu_assert(strstr(r.err, "-U all only supports the read-only queries") != NULL,
		          others[i]);
		mu_assert_int_eq(0, (long)strlen(r.out));

		r = run_util((const char *const[]){ "ubertooth-util", "-s", others[i], "-U", "all", NULL });
		mu_assert_int_eq(1, r.rc);
		mu_assert(strstr(r.err, "-U all only supports the read-only queries") != NULL,
		          others[i]);
		mu_assert_int_eq(0, (long)strlen(r.out));

		/* nothing was opened, counted, sent or started */
		mu_assert_int_eq(0, fake.start_calls);
		mu_assert_int_eq(0, fake.indexed_starts);
		mu_assert_int_eq(0, fake.stop_calls);
		mu_assert_int_eq(0, fake.reset_calls);
		mu_assert_int_eq(0, fake.flash_calls + fake.isp_calls + fake.led_specan_calls +
		                    fake.range_test_calls + fake.repeater_calls +
		                    fake.tx_test_calls + fake.xmas_calls);
	}

	/* -U all with no query at all is rejected the same way */
	{
		struct run_result r;

		reset_fake();
		fake.device_count = 2;
		r = RUN("-U", "all");
		mu_assert_int_eq(1, r.rc);
		mu_assert(strstr(r.err, "-U all only supports the read-only queries") != NULL,
		          "no query");
		mu_assert_int_eq(0, fake.start_calls);
		mu_assert_int_eq(0, fake.indexed_starts);
	}
}

MU_TEST(only_the_exact_word_all_selects_every_device)
{
	struct run_result r;

	/* anything else is still passed to the library as an index or serial */
	reset_fake();
	fake.device_count = 2;
	r = RUN("-U", "ALL", "-b");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, BOARD_LINE) == 0, "single-device report");
	mu_assert(strcmp(fake.selector, "ALL") == 0, "selector passed to the library");
	mu_assert_int_eq(0, fake.indexed_starts);

	/* the last -U wins, as before */
	reset_fake();
	fake.device_count = 2;
	r = RUN("-U", "all", "-U", "1", "-c");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(fake.selector, "1") == 0, "later selector used");
}

/* ---- a failed read prints no value line ------------------------------- */

MU_TEST(a_failed_read_prints_no_value_line_and_names_the_query_on_stderr)
{
	struct {
		const char *option;
		int *value;      /* scripted result that is made to fail */
		const char *error;
	} cases[] = {
		{ "-c", &fake.channel, "error: channel read failed\n" },
		{ "-a", &fake.palevel, "error: PA level read failed\n" },
		{ "-z", &fake.squelch, "error: squelch read failed\n" },
		{ "-l", &fake.usrled, "error: USR LED read failed\n" },
		{ "-m", &fake.rangeresult_rc, "error: range test result read failed\n" },
		{ "-9", &fake.do_something_reply_rc, "error: test command reply read failed\n" },
	};
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		struct run_result r;

		reset_fake();
		*cases[i].value = LIBUSB_ERROR_TIMEOUT;
		r = run_util((const char *const[]){ "ubertooth-util", cases[i].option, NULL });
		mu_assert_int_eq(1, r.rc);
		mu_assert(strcmp(r.out, "") == 0, cases[i].option);
		mu_assert(strcmp(r.err, cases[i].error) == 0, cases[i].option);
	}
}

MU_TEST(a_failed_led_read_drops_only_that_line)
{
	struct run_result r;

	reset_fake();
	fake.usrled = -1;
	r = RUN("-d");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.out, "RX LED status : 0\nTX LED status : 0\n") == 0, "USR failed");
	mu_assert(strcmp(r.err, "error: USR LED read failed\n") == 0, "USR message");

	reset_fake();
	fake.rxled = -1;
	r = RUN("-d");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.out, "USR LED status: 0\nTX LED status : 0\n") == 0, "RX failed");
	mu_assert(strcmp(r.err, "error: RX LED read failed\n") == 0, "RX message");

	reset_fake();
	fake.txled = -1;
	r = RUN("-d");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.out, "USR LED status: 0\nRX LED status : 0\n") == 0, "TX failed");
	mu_assert(strcmp(r.err, "error: TX LED read failed\n") == 0, "TX message");
}

MU_TEST(a_failed_read_keeps_the_other_lines_of_a_composed_command)
{
	struct run_result r;

	reset_fake();
	fake.channel = -1;
	fake.squelch = -1;
	r = RUN("-b", "-c", "-a", "-z");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.out, BOARD_LINE PALEVEL_LINE) == 0, "remaining lines in order");
	mu_assert(strcmp(r.err,
	                 "error: channel read failed\nerror: squelch read failed\n") == 0,
	          "one message per failed read");
}

MU_TEST(a_failed_test_command_send_fails_the_command)
{
	struct run_result r;

	reset_fake();
	fake.do_something_rc = -1;
	r = RUN("-9");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.out, "55 55 55 55\n") == 0, "reply still read and printed");
	mu_assert(strcmp(r.err, "error: test command send failed\n") == 0, "send failure named");
}

/* ---- the opened handle is released on every exit ---------------------- */

MU_TEST(read_only_queries_release_the_handle_without_sending_stop)
{
	static const char *const queries[] = {
		"-b", "-v", "-V", "-p", "-s", "-c", "-a", "-z", "-l", "-d", "-m"
	};
	size_t i;

	for (i = 0; i < sizeof(queries) / sizeof(queries[0]); i++) {
		reset_fake();
		run_util((const char *const[]){ "ubertooth-util", queries[i], NULL });
		mu_assert(fake.first_closes == 1, queries[i]);
		mu_assert(fake.first_frees == 1, queries[i]);
		mu_assert(fake.first_used_after_release == 0, queries[i]);
		mu_assert(fake.stop_calls == 0, queries[i]); /* a query never sends STOP */
	}

	/* a failed read is an error exit, and still releases the handle */
	reset_fake();
	fake.channel = -1;
	mu_assert_int_eq(1, RUN("-b", "-c").rc);
	mu_assert_int_eq(1, fake.first_closes);
	mu_assert_int_eq(1, fake.first_frees);
	mu_assert_int_eq(0, fake.stop_calls);
}

MU_TEST(setters_and_modes_release_the_handle_on_success_and_failure)
{
	size_t i;
	int fail;

	for (fail = 0; fail <= 1; fail++) {
		reset_fake();
		fake.set_channel_rc = fail ? -1 : 0;
		mu_assert_int_eq(fail, RUN("-C39").rc);
		mu_assert_int_eq(1, fake.first_closes);
		mu_assert_int_eq(1, fake.first_frees);
		mu_assert_int_eq(0, fake.stop_calls);

		reset_fake();
		fake.do_something_rc = fail ? -1 : 0;
		mu_assert_int_eq(fail, RUN("-9").rc);
		mu_assert_int_eq(1, fake.first_closes);
		mu_assert_int_eq(1, fake.first_frees);

		for (i = 0; i < MODE_COUNT; i++) {
			reset_fake();
			*mode_result(i) = fail ? -1 : 0;
			mu_assert(run_util((const char *const[]){ "ubertooth-util",
			                   mode_options[i], NULL }).rc == fail, mode_options[i]);
			mu_assert(fake.first_closes == 1, mode_options[i]);
			mu_assert(fake.first_frees == 1, mode_options[i]);
			mu_assert(fake.first_used_after_release == 0, mode_options[i]);
			/* only the STOP that leaves a prior mode (-f and -i send none);
			 * releasing the handle sends no STOP of its own */
			mu_assert(fake.stop_calls == (i >= 2), mode_options[i]);
		}
	}
}

MU_TEST(the_handle_reopened_after_a_reset_is_released)
{
	reset_fake();
	mu_assert_int_eq(0, RUN("-r", "-b").rc);
	mu_assert_int_eq(1, fake.first_closes);
	mu_assert_int_eq(1, fake.first_frees);
	mu_assert_int_eq(2, fake.closes); /* pre-reset handle and the new one */
	mu_assert_int_eq(2, fake.frees);
	mu_assert_int_eq(0, fake.stop_calls);

	reset_fake();
	fake.board_id = -1;
	mu_assert_int_eq(1, RUN("-r", "-b").rc);
	mu_assert_int_eq(2, fake.closes);
	mu_assert_int_eq(2, fake.frees);
	mu_assert_int_eq(0, fake.stop_calls);
}

int main(void)
{
	MU_RUN(nonzero_query_values_are_not_exit_statuses);
	MU_RUN(every_known_board_id_exits_zero);
	MU_RUN(unknown_board_id_fails_without_indexing_the_name_table);
	MU_RUN(device_access_applies_the_selector_and_fails_cleanly_without_a_device);
	MU_RUN(composed_queries_print_in_the_documented_order_and_exit_zero);
	MU_RUN(an_earlier_failure_is_not_masked_by_later_successes);
	MU_RUN(part_id_with_bit_31_set_is_reported_in_full);
	MU_RUN(each_failing_report_makes_the_command_fail);
	MU_RUN(all_leds_query_reports_each_led_and_fails_if_any_read_fails);
	MU_RUN(range_result_is_reported_and_a_failed_read_fails);
	MU_RUN(setters_pass_their_arguments_and_report_failures);
	MU_RUN(stop_and_reset_report_failures);
	MU_RUN(mode_commands_exit_zero_on_success_and_one_on_failure);
	MU_RUN(a_failed_stop_before_a_mode_command_fails_the_command);
	MU_RUN(led_specan_uses_the_default_threshold_and_an_explicit_one);
	MU_RUN(two_or_more_mode_options_are_rejected_before_opening_the_device);
	MU_RUN(invalid_arguments_are_rejected_before_opening_the_device);
	MU_RUN(help_version_and_count_exit_zero_without_a_device);
	MU_RUN(firmware_listing_with_several_devices_reports_each_device);
	MU_RUN(test_command_reads_back_through_the_library_and_exits_zero);
	MU_RUN(serial_listing_with_several_devices_exits_zero);
	MU_RUN(each_identity_query_alone_lists_every_device_compactly);
	MU_RUN(combined_identity_queries_list_every_line_for_every_device);
	MU_RUN(an_unavailable_device_is_reported_and_fails_the_listing);
	MU_RUN(a_failed_read_on_one_device_is_reported_and_fails_the_listing);
	MU_RUN(listing_is_not_used_with_a_selector_one_device_or_other_operations);
	MU_RUN(u_all_lists_every_device_even_when_only_one_is_attached);
	MU_RUN(u_all_fails_cleanly_with_no_devices_or_a_count_error);
	MU_RUN(u_all_is_rejected_before_opening_a_device_for_any_other_option);
	MU_RUN(only_the_exact_word_all_selects_every_device);
	MU_RUN(a_failed_read_prints_no_value_line_and_names_the_query_on_stderr);
	MU_RUN(a_failed_led_read_drops_only_that_line);
	MU_RUN(a_failed_read_keeps_the_other_lines_of_a_composed_command);
	MU_RUN(a_failed_test_command_send_fails_the_command);
	MU_RUN(read_only_queries_release_the_handle_without_sending_stop);
	MU_RUN(setters_and_modes_release_the_handle_on_success_and_failure);
	MU_RUN(the_handle_reopened_after_a_reset_is_released);
	return mu_report();
}
