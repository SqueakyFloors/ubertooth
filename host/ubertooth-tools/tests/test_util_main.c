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

#define main ubertooth_util_main
#include "ubertooth-util.c"
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
	int identify_rc;
	int rangeresult_rc;
	rangetest_result range;
	/* bookkeeping */
	int device_count;
	int start_calls;
	int fail_start_from; /* 0: never; n: ubertooth_start_select fails from call n */
	char selector[64];
	int stop_calls, flash_calls, isp_calls, led_specan_calls, range_test_calls;
	int repeater_calls, tx_test_calls, xmas_calls, reset_calls;
	int identify_calls;
	int serial_reads, resets_at_first_serial_read;
	int last_channel_set, last_squelch_set, last_led_specan_arg;
	int last_palevel_set, last_usrled_set;
	/* LED writes: -I must not blink from the host */
	int usrled_writes, rxled_writes, txled_writes;
	int last_usrled_level, last_rxled_level, last_txled_level;
	/* multi-device listing */
	int current_index;     /* device opened by ubertooth_start(), or -1 */
	int unavailable_index; /* ubertooth_start() fails for this index */
	int board_fail_index;  /* the board ID read fails on this device */
	int board_fail_rc;     /* ...with this code (0: -1) */
	int revision_fail_index;     /* the firmware revision read fails here */
	int compile_info_fail_index; /* the compile info read fails here */
	int indexed_starts, closes, frees;
	/* -r: the handle opened before the reset (the first select) */
	int first_closes, first_frees, first_stops; /* STOP sent to that handle */
	int first_used_after_release; /* any command on it after it was released */
	int first_closed_before_reselect;
	int first_released;
	/* the wait after a reset: a fake clock, and the device leaving and
	 * coming back on it */
	uint64_t now_ms;
	uint64_t leaves_at_ms;  /* the reset device drops off the bus... */
	uint64_t returns_at_ms; /* ...and is back from here (UINT64_MAX: never) */
	int sleeps;
	int quiet_starts;        /* ubertooth_start_select_quiet() calls */
	int quiet_while_absent;  /* of which while the device was gone */
	char quiet_selector[64];
	int quiet_null_selector; /* it was called with NULL (no -U) */
	int loud_starts_after_reset;
	/* libusb default-context references taken by ubertooth-util itself */
	int libusb_refs, libusb_refs_min_during_wait, libusb_init_rc;
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
	fake.api_version = UBERTOOTH_API_VERSION; /* firmware from this tree */
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
	fake.now_ms = 1000;
	fake.leaves_at_ms = 1050;  /* soon after the reset... */
	fake.returns_at_ms = 1600; /* ...and back after about half a second */
	memset(&fake_ut, 0, sizeof(fake_ut));
	fake_ut.devh = FIRST_DEVH;
	memset(&fake_ut_reset, 0, sizeof(fake_ut_reset));
	fake_ut_reset.devh = (struct libusb_device_handle *)&fake_devh_reset;
}

/* The reset device is missing from the count while it re-enumerates. */
static int fake_reset_device_absent(void)
{
	return fake.reset_calls > 0 && fake.reset_rc >= 0 &&
	       fake.now_ms >= fake.leaves_at_ms && fake.now_ms < fake.returns_at_ms;
}

int ubertooth_count(void)
{
	if (fake.device_count > 0 && fake_reset_device_absent())
		return fake.device_count - 1;
	return fake.device_count;
}

/* ubertooth-util holds a libusb reference across the wait after a reset. The
 * fake library functions do not take references, so only that one is counted. */
int libusb_init(libusb_context **context)
{
	(void)context;
	if (fake.libusb_init_rc < 0)
		return fake.libusb_init_rc;
	fake.libusb_refs++;
	return 0;
}

void libusb_exit(libusb_context *context)
{
	(void)context;
	fake.libusb_refs--;
}

uint64_t util_clock_now_ms(void) { return fake.now_ms; }

void util_clock_sleep_ms(unsigned ms)
{
	fake.sleeps++;
	if (fake.sleeps == 1 || fake.libusb_refs < fake.libusb_refs_min_during_wait)
		fake.libusb_refs_min_during_wait = fake.libusb_refs;
	fake.now_ms += ms;
}

/* Used only for the wait after a reset: no output, no STOP. */
ubertooth_t *ubertooth_start_select_quiet(const char *u_arg)
{
	fake.quiet_starts++;
	fake.quiet_null_selector = (u_arg == NULL);
	snprintf(fake.quiet_selector, sizeof(fake.quiet_selector), "%s",
	         u_arg ? u_arg : "");
	fake.first_closed_before_reselect = fake.first_released;
	if (fake_reset_device_absent()) {
		fake.quiet_while_absent++;
		return NULL;
	}
	/* Like the library: one device is opened whatever the index; with
	 * several, an index past the end opens nothing. */
	if (u_arg != NULL && u_arg[0] != '\0' &&
	    u_arg[strspn(u_arg, "0123456789")] == '\0' &&
	    fake.device_count != 1 && atoi(u_arg) >= fake.device_count)
		return NULL;
	return &fake_ut_reset;
}

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
		fake.loud_starts_after_reset++; /* would print errors and send STOP */
	if (fake.fail_start_from && fake.start_calls >= fake.fail_start_from)
		return NULL;
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
		return fake.board_fail_rc ? fake.board_fail_rc : -1;
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
	/* -r must read the serial while the device is still there, before the
	 * reset is sent: how many resets had been sent at the first read. */
	if (fake.serial_reads++ == 0)
		fake.resets_at_first_serial_read = fake.reset_calls;
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

/* The setters below also change what the matching read returns, so a command
 * that sets and reads the same value shows whether the read saw the set. */
int cmd_set_channel(struct libusb_device_handle *h, u16 channel)
{
	fake_use(h);
	fake.last_channel_set = channel;
	if (fake.set_channel_rc >= 0)
		fake.channel = channel;
	return fake.set_channel_rc;
}

int cmd_set_usrled(struct libusb_device_handle *h, u16 s)
{
	fake_use(h);
	fake.last_usrled_set = s + 1;
	fake.usrled_writes++;
	fake.last_usrled_level = s;
	if (fake.set_usrled_rc >= 0)
		fake.usrled = s;
	return fake.set_usrled_rc;
}

int cmd_set_rxled(struct libusb_device_handle *h, u16 s)
{
	fake_use(h);
	fake.rxled_writes++;
	fake.last_rxled_level = s;
	if (fake.set_rxled_rc >= 0)
		fake.rxled = s;
	return fake.set_rxled_rc;
}

int cmd_set_txled(struct libusb_device_handle *h, u16 s)
{
	fake_use(h);
	fake.txled_writes++;
	fake.last_txled_level = s;
	if (fake.set_txled_rc >= 0)
		fake.txled = s;
	return fake.set_txled_rc;
}

int cmd_set_palevel(struct libusb_device_handle *h, u16 l)
{
	fake_use(h);
	fake.last_palevel_set = l;
	if (fake.set_palevel_rc >= 0)
		fake.palevel = l;
	return fake.set_palevel_rc;
}

int cmd_set_squelch(struct libusb_device_handle *h, u16 level)
{
	fake_use(h);
	fake.last_squelch_set = level;
	if (fake.set_squelch_rc >= 0)
		fake.squelch = level & 0xff;
	return fake.set_squelch_rc;
}

int cmd_flash(struct libusb_device_handle *h) { fake_use(h); fake.flash_calls++; return fake.flash_rc; }
int cmd_set_isp(struct libusb_device_handle *h) { fake_use(h); fake.isp_calls++; return fake.isp_rc; }
int cmd_range_test(struct libusb_device_handle *h) { fake_use(h); fake.range_test_calls++; return fake.range_test_rc; }
int cmd_repeater(struct libusb_device_handle *h) { fake_use(h); fake.repeater_calls++; return fake.repeater_rc; }
int cmd_tx_test(struct libusb_device_handle *h) { fake_use(h); fake.tx_test_calls++; return fake.tx_test_rc; }
int cmd_xmas(struct libusb_device_handle *h) { fake_use(h); fake.xmas_calls++; return fake.xmas_rc; }
int cmd_identify(struct libusb_device_handle *h) { fake_use(h); fake.identify_calls++; return fake.identify_rc; }

int cmd_led_specan(struct libusb_device_handle *h, u16 rssi)
{
	fake_use(h);
	fake.led_specan_calls++;
	fake.last_led_specan_arg = rssi;
	return fake.led_specan_rc;
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
#define FIRMWARE_LINE "Firmware version: git-test (API:1.14)\n"
#define COMPILE_LINE "compile-test\n"
#define USRLED_LINE "USR LED status: 0\n"
#define PALEVEL_LINE "PA Level: 7\n"
#define PART_LINE "Part ID: 25011723\n"
/* The serial the scripted device reports, as -s prints it and as -r follows
 * it across a reset. */
#define FAKE_SERIAL "a1a2a3a4a5a6a7a8a9aaabacadaeafb0"
#define SERIAL_LINE "Serial No: " FAKE_SERIAL "\n"

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

	/* no range test since the board started: a plain "no result", exit 0 */
	reset_fake();
	fake.range.valid = 0;
	r = RUN("-m");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, "no range test result yet\n") == 0,
	          "no-result output");

	/* a test that ran but whose data did not match */
	reset_fake();
	fake.range.valid = 2 + 5;
	r = RUN("-m");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, "Invalid range test: mismatch on byte 5\n") == 0,
	          "mismatch output");

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
	mu_assert(strcmp(r.out, "") == 0, "a set writes no result");
	mu_assert(strcmp(r.err, "") == 0, "and says nothing: a set is silent");

	reset_fake();
	fake.set_squelch_rc = -1;
	r = RUN("-z5"); /* a failed set exits 1 and still prints no value line; the
	                 * error line itself comes from libubertooth, which the
	                 * fake replaces */
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.out, "") == 0, "a failed set writes no result either");

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

/* A set and a read of the same value in one command both happen, the set
 * first, so the read reports the new value. The set used to be dropped
 * silently with exit 0 because the two shared one variable. */
MU_TEST(a_set_and_a_read_of_the_same_value_both_run_set_first)
{
	struct run_result r;

	reset_fake();
	r = RUN("-c2426", "-c");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(2426, fake.last_channel_set);
	mu_assert(strcmp(r.out, "Current frequency: 2426 MHz (Bluetooth channel 24)\n") == 0,
	          "-c2426 -c sets then reports the new frequency");

	/* the read written first is the same command */
	reset_fake();
	r = RUN("-c", "-c2426");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(2426, fake.last_channel_set);
	mu_assert(strcmp(r.out, "Current frequency: 2426 MHz (Bluetooth channel 24)\n") == 0,
	          "-c -c2426 sets then reports the new frequency");

	/* separate-word values too: -c 2426 -c */
	reset_fake();
	r = RUN("-c", "2426", "-c");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(2426, fake.last_channel_set);
	mu_assert(strcmp(r.out, "Current frequency: 2426 MHz (Bluetooth channel 24)\n") == 0,
	          "-c 2426 -c sets then reports the new frequency");

	/* -C sets by channel number and -c reads, and -C both ways */
	reset_fake();
	r = RUN("-C24", "-c");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(2426, fake.last_channel_set);
	mu_assert(strcmp(r.out, "Current frequency: 2426 MHz (Bluetooth channel 24)\n") == 0,
	          "-C24 -c sets then reports the new frequency");

	reset_fake();
	r = RUN("-C24", "-C");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(2426, fake.last_channel_set);
	mu_assert(strcmp(r.out, "Current frequency: 2426 MHz (Bluetooth channel 24)\n") == 0,
	          "-C24 -C sets then reports the new frequency");

	reset_fake();
	r = RUN("-a5", "-a");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(5, fake.last_palevel_set);
	mu_assert(strcmp(r.out, "PA Level: 5\n") == 0, "-a5 -a reports the new level");

	/* level 0 is a set, not a missing value */
	reset_fake();
	r = RUN("-a0", "-a");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(0, fake.last_palevel_set);
	mu_assert(strcmp(r.out, "PA Level: 0\n") == 0, "-a0 -a reports the new level");

	reset_fake();
	r = RUN("-l1", "-l");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(2, fake.last_usrled_set); /* recorded as value + 1 */
	mu_assert(strcmp(r.out, "USR LED status: 1\n") == 0, "-l1 -l reports the new state");

	reset_fake();
	fake.usrled = fake.rxled = fake.txled = 1;
	r = RUN("-d0", "-d");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, "USR LED status: 0\nRX LED status : 0\n"
	                        "TX LED status : 0\n") == 0,
	          "-d0 -d reports every new LED state");

	/* the reported command line: the squelch set, the LED set and the
	 * LED read all happen */
	reset_fake();
	r = RUN("-z-120", "-l1", "-l");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq((u16)-120, fake.last_squelch_set);
	mu_assert_int_eq(2, fake.last_usrled_set);
	mu_assert(strcmp(r.out, "USR LED status: 1\n") == 0,
	          "-z-120 -l1 -l reports the LED read on stdout");
	mu_assert(strcmp(r.err, "") == 0, "and nothing on stderr: both sets are silent");

	/* the squelch pair, which always worked this way */
	reset_fake();
	r = RUN("-z-60", "-z");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq((u16)-60, fake.last_squelch_set);
	mu_assert(strcmp(r.out, "Squelch set to -60\n") == 0,
	          "-z-60 -z reports the new squelch on stdout");
	mu_assert(strcmp(r.err, "") == 0,
	          "once: the read-back is the result, the set says nothing");

	/* two sets of the same value keep the last one */
	reset_fake();
	r = RUN("-c2426", "-c2440", "-c");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(2440, fake.last_channel_set);
	mu_assert(strcmp(r.out, "Current frequency: 2440 MHz (Bluetooth channel 38)\n") == 0,
	          "the last channel set wins");

	reset_fake();
	r = RUN("-a3", "-a5", "-a");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(5, fake.last_palevel_set);
	mu_assert(strcmp(r.out, "PA Level: 5\n") == 0, "the last PA level set wins");

	/* a failed set is reported, and the read still runs */
	reset_fake();
	fake.set_channel_rc = -1;
	r = RUN("-c2426", "-c");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.out, CHANNEL_LINE) == 0,
	          "the read reports the unchanged frequency after a failed set");
}

MU_TEST(stop_and_reset_report_failures)
{
	struct run_result r;

	reset_fake();
	r = RUN("-S");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(1, fake.stop_calls);
	mu_assert(strcmp(r.out, "") == 0, r.out);
	mu_assert(strcmp(r.err, "Stopping ubertooth device 0\n") == 0, r.err);

	reset_fake();
	fake.stop_rc = -1;
	r = RUN("-S", "-b");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.out, BOARD_LINE) != NULL, "later report still printed");

	reset_fake();
	r = RUN("-r");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(1, fake.reset_calls);
	mu_assert_int_eq(1, fake.start_calls);
	mu_assert(fake.quiet_starts >= 1, "reconnects quietly after the reset");
	mu_assert_int_eq(0, fake.loud_starts_after_reset);
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

	/* the device does not come back after the reset: fail, do not crash */
	reset_fake();
	fake.returns_at_ms = UINT64_MAX;
	r = RUN("-r");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.out, "") == 0, "nothing on stdout");
	mu_assert(strcmp(r.err, "Resetting ubertooth device 0\n"
	                 "error: Ubertooth device 0 did not come back "
	                 "within 10 s after the reset\n") == 0, "timeout message");
	mu_assert_int_eq(0, fake.stop_calls);
	mu_assert_int_eq(1, fake.first_closes);
	mu_assert_int_eq(1, fake.first_frees);
	mu_assert_int_eq(0, fake.first_stops);
	/* the serial it follows never comes back: polled every time, opened never */
	mu_assert(fake.quiet_starts > 0, "polled for the serial");
	mu_assert_int_eq(fake.quiet_starts, fake.quiet_while_absent);
	mu_assert_int_eq(0, fake.first_used_after_release);
	mu_assert_int_eq(1000 + (int)UTIL_RESET_WAIT_DEADLINE_MS, (int)fake.now_ms);
}

/* ---- mode commands: success and failure ------------------------------- */

static const char *const mode_options[] = { "-f", "-i", "-q", "-n", "-e", "-t", "-x", "-I" };
#define MODE_COUNT (sizeof(mode_options) / sizeof(mode_options[0]))

/* The LED specan default is a number in a header, so it is spelled into the
 * expected line rather than repeated here. */
#define MODE_STR_(x) #x
#define MODE_STR(x) MODE_STR_(x)

/* Everything each mode writes to standard error, in order, for a run with no
 * -U that succeeds. Pinned by text, not just by "stdout is empty", so a line
 * cannot be dropped or reworded without a test saying so. */
static const char *const mode_progress[MODE_COUNT] = {
	/* -f */ "warning: ubertooth-util -f is no longer required "
	         "- use ubertooth-dfu instead\n"
	         "Entering flash programming (DFU) mode\n",
	/* -i */ "Entering flash programming (ISP) mode\n",
	/* -q */ "Entering LED specan mode (threshold -"
	         MODE_STR(UBERTOOTH_LED_SPECAN_N_DEFAULT) " dBm)\n",
	/* -n */ "Starting range test\n",
	/* -e */ "Starting repeater\n",
	/* -t */ "Starting TX test\n",
	/* -x */ "",
	/* -I */ "note: the board is blinking; stop it with ubertooth-util -S "
	         "or any mode option\n",
};

/* The scripted result and call counter for the i-th mode command. */
static int *mode_result(size_t i)
{
	int *results[] = {
		&fake.flash_rc, &fake.isp_rc, &fake.led_specan_rc, &fake.range_test_rc,
		&fake.repeater_rc, &fake.tx_test_rc, &fake.xmas_rc, &fake.identify_rc,
	};
	return results[i];
}

static int mode_calls(size_t i)
{
	int calls[] = {
		fake.flash_calls, fake.isp_calls, fake.led_specan_calls,
		fake.range_test_calls, fake.repeater_calls, fake.tx_test_calls,
		fake.xmas_calls, fake.identify_calls,
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
		/* starting a mode reports nothing: its progress line is on stderr */
		mu_assert(strcmp(r.out, "") == 0, r.out);
		/* and that line is pinned, so dropping one would be noticed. -f
		 * prints its deprecation warning before the progress line; -x has
		 * never had one. */
		mu_assert(strcmp(r.err, mode_progress[i]) == 0, r.err);

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

/*
 * -I starts the blink on the board and returns, like every other mode: no host
 * loop, no LED writes over USB, and nothing to interrupt. The CTest timeout on
 * this test is the backstop if that ever regresses.
 */
MU_TEST(identify_starts_the_blink_on_the_board_and_returns)
{
	struct run_result r;

	reset_fake();
	r = RUN("-I");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(1, fake.identify_calls);
	mu_assert_int_eq(1, fake.stop_calls); /* leaves any prior mode first */
	mu_assert_int_eq(0, fake.usrled_writes); /* the board does the blinking */
	mu_assert_int_eq(0, fake.rxled_writes);
	mu_assert_int_eq(0, fake.txled_writes);
	mu_assert_int_eq(0, fake.sleeps);
	mu_assert(strcmp(r.out, "") == 0, "the blink writes no result");
	/* one line, not a "flashing the LEDs" line and then this one */
	mu_assert(strcmp(r.err, "note: the board is blinking; stop it with "
	                 "ubertooth-util -S or any mode option\n") == 0,
	          "the blink is reported once, on stderr, with how to stop it");

	/* the selector goes into the command the note tells the user to run,
	 * since a bare -S would reach whichever board is index 0 */
	reset_fake();
	r = RUN("-U", "abc123", "-I");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, "") == 0, "the blink writes no result");
	mu_assert(strcmp(r.err, "note: the board is blinking; stop it with "
	                 "ubertooth-util -U abc123 -S or any mode option\n") == 0,
	          "the stop command names the selected device");

	/* a failed request fails the command */
	reset_fake();
	fake.identify_rc = LIBUSB_ERROR_PIPE;
	r = RUN("-I");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(1, fake.identify_calls);
	mu_assert_int_eq(0, fake.usrled_writes);
	mu_assert_int_eq(0, fake.sleeps);
}

/*
 * Firmware that is not this tree's API version, older or newer, is refused:
 * no request is sent and nothing is blinked from the host. So is firmware
 * whose version cannot be read.
 */
MU_TEST(identify_refuses_firmware_of_another_api_version)
{
	const uint16_t versions[] = { UBERTOOTH_API_VERSION - 1, UBERTOOTH_API_VERSION + 1 };
	struct run_result r;
	size_t i;

	for (i = 0; i < sizeof(versions) / sizeof(versions[0]); i++) {
		reset_fake();
		fake.api_version = versions[i];
		r = RUN("-I");
		mu_assert_int_eq(1, r.rc);
		mu_assert_int_eq(0, fake.identify_calls);
		mu_assert_int_eq(0, fake.stop_calls); /* refused before anything is sent */
		mu_assert(strcmp(r.out, "") == 0, "prints nothing on stdout");
		mu_assert(strstr(r.err, "is blinking") == NULL, "and claims no blink");
		mu_assert_int_eq(0, fake.usrled_writes);
		mu_assert_int_eq(0, fake.sleeps);
		mu_assert(strstr(r.err, "error: -I needs firmware API version") != NULL,
		          "says why");
		mu_assert(strstr(r.err, "Flash the firmware built from the same source tree") != NULL,
		          "says what to do");
	}

	reset_fake();
	fake.api_rc = -1;
	r = RUN("-I");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(0, fake.identify_calls);
	mu_assert_int_eq(0, fake.usrled_writes);
}

MU_TEST(led_specan_uses_the_default_threshold_and_an_explicit_one)
{
	struct run_result r;

	reset_fake();
	r = RUN("-q");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(UBERTOOTH_LED_SPECAN_N_DEFAULT, fake.last_led_specan_arg);
	mu_assert(strcmp(r.out, "") == 0, "starting a mode writes no result");
	mu_assert(strstr(r.err, "Entering LED specan mode (threshold -85 dBm)\n") != NULL,
	          "names the threshold, on stderr");

	reset_fake();
	r = RUN("-q100");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(100, fake.last_led_specan_arg);
}

/* ---- argument handling that must not touch the device ----------------- */

/* A word that is not an option is refused before any device action, so a
 * typo cannot drop part of the command while the rest runs. */
static void assert_refused_untouched(struct run_result r, const char *diagnostic)
{
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, diagnostic) != NULL, diagnostic);
	mu_assert_int_eq(0, (long)strlen(r.out));
	mu_assert_int_eq(0, fake.start_calls);
	mu_assert_int_eq(0, fake.reset_calls);
	mu_assert_int_eq(0, fake.stop_calls);
	mu_assert_int_eq(0, fake.last_channel_set);
}

MU_TEST(stray_arguments_are_rejected_before_any_device_action)
{
	struct run_result r;

	/* "-V" typed with a space: the reset must not run either */
	reset_fake();
	r = RUN("-", "V", "-r", "-U1");
	assert_refused_untouched(r, "error: unexpected argument '-'\n");

	reset_fake();
	r = RUN("-v", "extra");
	assert_refused_untouched(r, "error: unexpected argument 'extra'\n");

	reset_fake();
	r = RUN("extra", "-b");
	assert_refused_untouched(r, "error: unexpected argument 'extra'\n");

	/* a word after an optional-value option that is not a number is not
	 * taken as its value */
	reset_fake();
	r = RUN("-c", "24x");
	assert_refused_untouched(r, "error: unexpected argument '24x'\n");

	/* checked before the mode conflict, -U all and -N */
	reset_fake();
	r = RUN("-x", "-t", "oops");
	assert_refused_untouched(r, "error: unexpected argument 'oops'\n");
	mu_assert(strstr(r.err, "cannot be combined") == NULL, "only the stray word is reported");

	reset_fake();
	r = RUN("-N", "oops");
	assert_refused_untouched(r, "error: unexpected argument 'oops'\n");

	/* -h, -H and --host-version are checked too, not exempted by returning
	 * early */
	reset_fake();
	r = RUN("-H", "stray");
	assert_refused_untouched(r, "error: unexpected argument 'stray'\n");
	reset_fake();
	r = RUN("--host-version", "stray");
	assert_refused_untouched(r, "error: unexpected argument 'stray'\n");
	reset_fake();
	r = RUN("-h", "stray");
	assert_refused_untouched(r, "error: unexpected argument 'stray'\n");
	mu_assert(strstr(r.out, "command line utility") == NULL, "no usage printed");

	/* -h still wins over other options, and -H still needs no device */
	reset_fake();
	r = RUN("-x", "-h");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strstr(r.out, "command line utility") != NULL, "usage");
	mu_assert_int_eq(0, fake.start_calls);
	mu_assert_int_eq(0, fake.xmas_calls);
	reset_fake();
	r = RUN("-b", "--host-version");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, "libubertooth fake\n") == 0, "version only");
	mu_assert_int_eq(0, fake.start_calls);

	/* the first of -h and -H wins, and --host-version wins anywhere, as
	 * before */
	reset_fake();
	r = RUN("-H", "-h");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, "libubertooth fake\n") == 0, "-H first: version");
	reset_fake();
	r = RUN("-h", "-H");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strstr(r.out, "command line utility") != NULL, "-h first: usage");
	reset_fake();
	r = RUN("-h", "--host-version");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, "libubertooth fake\n") == 0, "--host-version wins");

	/* after "--", --host-version is an operand, reported as typed */
	reset_fake();
	r = RUN("--", "--host-version");
	assert_refused_untouched(r, "error: unexpected argument '--host-version'\n");

	/* -U takes its value as the next word; that is not a stray argument */
	reset_fake();
	r = RUN("-U", "0", "-b");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strstr(r.err, "unexpected") == NULL, "-U <value> accepted");

	/* an attached optional value still works */
	reset_fake();
	r = RUN("-c2440");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(2440, fake.last_channel_set);
}

/* Every numeric option value is parsed strictly (the whole string, in range)
 * and a bad one is refused before the device is opened. atoi() used to turn
 * "abc" into 0, so -Cabc set 2402 MHz and -zabc set squelch 0; -l2 read the
 * LED instead of setting it and -d5 did nothing; -q300 reached the firmware;
 * -z-200 wrapped to 56. */
MU_TEST(numeric_option_values_are_parsed_strictly)
{
	static const struct { const char *arg, *diag; } bad[] = {
		{ "-zabc", "error: squelch level must be -128 to 127 (got abc)\n" },
		{ "-z-200", "error: squelch level must be -128 to 127 (got -200)\n" },
		{ "-z128", "error: squelch level must be -128 to 127 (got 128)\n" },
		{ "-q\t5", "error: RSSI threshold must be 20-120 (got \t5)\n" },
		{ "-c\n2440", "error: frequency must be 2402-2480 MHz (got \n2440)\n" },
		{ "-l2", "error: USR LED state must be 0-1 (got 2)\n" },
		{ "-lx", "error: USR LED state must be 0-1 (got x)\n" },
		{ "-d5", "error: all-LED state must be 0-1 (got 5)\n" },
		{ "-Cabc", "error: channel must be 0-78 (got abc)\n" },
		{ "-C79", "error: channel must be 0-78 (got 79)\n" },
		{ "-C3x", "error: channel must be 0-78 (got 3x)\n" },
		{ "-cabc", "error: frequency must be 2402-2480 MHz (got abc)\n" },
		{ "-c2441.5", "error: frequency must be 2402-2480 MHz (got 2441.5)\n" },
		{ "-q300", "error: RSSI threshold must be 20-120 (got 300)\n" },
		{ "-q0", "error: RSSI threshold must be 20-120 (got 0)\n" },
		{ "-q+5", "error: RSSI threshold must be 20-120 (got +5)\n" },
		{ "-q19", "error: RSSI threshold must be 20-120 (got 19)\n" },
		{ "-q121", "error: RSSI threshold must be 20-120 (got 121)\n" },
		{ "-q225", "error: RSSI threshold must be 20-120 (got 225)\n" },
	};
	size_t i;
	struct run_result r;

	for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		reset_fake();
		r = run_util((const char *const[]){ "ubertooth-util", bad[i].arg, NULL });
		mu_assert_int_eq(1, r.rc);
		mu_assert(strcmp(r.err, bad[i].diag) == 0, bad[i].arg);
		mu_assert_int_eq(0, (long)strlen(r.out));
		mu_assert_int_eq(0, fake.start_calls);
		mu_assert_int_eq(0, fake.last_channel_set);
		mu_assert_int_eq(0, fake.last_squelch_set);
	}

	/* the edges and the next-word form are still accepted */
	reset_fake();
	r = RUN("-z-128");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq((u16)-128, fake.last_squelch_set);
	reset_fake();
	r = RUN("-z", "127");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(127, fake.last_squelch_set);
	reset_fake();
	r = RUN("-C0");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(2402, fake.last_channel_set);
	reset_fake();
	r = RUN("-C", "78");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(2480, fake.last_channel_set);
	reset_fake();
	r = RUN("-c2480");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(2480, fake.last_channel_set);
	reset_fake();
	r = RUN("-l1");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(2, fake.last_usrled_set); /* recorded as value + 1 */
	reset_fake();
	r = RUN("-q120");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(120, fake.last_led_specan_arg);
	reset_fake();
	r = RUN("-q", "20");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(20, fake.last_led_specan_arg);
}

/* -a0 sets PA level 0 (it used to read the level, because 0 was the "read"
 * marker), and a value outside 0-7 is refused before the device is opened. */
MU_TEST(pa_level_zero_is_set_and_bad_levels_are_refused)
{
	struct run_result r;
	static const char *const bad[] = { "-a8", "-a9", "-afoo", "-a-1", "-a+3", "-a 3", "-a3x", "-a10", "-a07", "-a00" };
	size_t i;

	reset_fake();
	fake.last_palevel_set = 99;
	r = RUN("-a0");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(0, fake.last_palevel_set);
	mu_assert(strstr(r.out, "PA Level") == NULL, "-a0 sets, does not read");

	reset_fake();
	fake.last_palevel_set = 99;
	r = RUN("-a", "0");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(0, fake.last_palevel_set);

	reset_fake();
	fake.last_palevel_set = 99;
	r = RUN("-a7");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(7, fake.last_palevel_set);

	/* a bare -a still reads, and sets nothing */
	reset_fake();
	fake.last_palevel_set = 99;
	r = RUN("-a");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, "PA Level: 7\n") == 0, "-a reads");
	mu_assert_int_eq(99, fake.last_palevel_set);

	for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		reset_fake();
		fake.last_palevel_set = 99;
		r = run_util((const char *const[]){ "ubertooth-util", bad[i], NULL });
		mu_assert_int_eq(1, r.rc);
		mu_assert(strstr(r.err, "error: PA level must be a single digit 0-7 (got ") != NULL, bad[i]);
		mu_assert_int_eq(0, (long)strlen(r.out));
		mu_assert_int_eq(0, fake.start_calls);
		mu_assert_int_eq(99, fake.last_palevel_set);
	}

	/* the next-word form is validated the same way */
	reset_fake();
	r = RUN("-a", "9");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "error: PA level must be a single digit 0-7 (got 9)") != NULL, "-a 9");
	mu_assert_int_eq(0, fake.start_calls);
}

/* -z cannot take effect with a mode (the mode's STOP resets it, or the mode
 * does not use it), so the combination is refused before the device is
 * opened instead of -z being dropped silently. */
MU_TEST(squelch_with_a_mode_option_is_refused)
{
	static const char *const modes[] = {
		"-q", "-n", "-e", "-t", "-x", "-I", "-i", "-f",
	};
	size_t i;
	struct run_result r;

	for (i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
		char want[64];
		snprintf(want, sizeof(want), "error: -z cannot be combined with %s\n", modes[i]);

		reset_fake();
		r = run_util((const char *const[]){ "ubertooth-util", "-z-60", modes[i], NULL });
		mu_assert_int_eq(1, r.rc);
		mu_assert(strstr(r.err, want) != NULL, want);
		mu_assert_int_eq(0, (long)strlen(r.out));
		mu_assert_int_eq(0, fake.start_calls);
		mu_assert_int_eq(0, fake.last_squelch_set);

		/* a squelch read is refused the same way */
		reset_fake();
		r = run_util((const char *const[]){ "ubertooth-util", modes[i], "-z", NULL });
		mu_assert_int_eq(1, r.rc);
		mu_assert(strstr(r.err, want) != NULL, want);
		mu_assert_int_eq(0, fake.start_calls);
	}

	/* -z on its own, or with settings and queries, still works */
	reset_fake();
	r = RUN("-z-60", "-c2440", "-a5");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq((u16)-60, fake.last_squelch_set);
	mu_assert_int_eq(2440, fake.last_channel_set);
}

/* An optional value may be the next word as well as attached: -c 2440 is
 * -c2440. Only a number is taken, and only -z takes a negative one. */
/* A short reply's whole error line comes from libubertooth (not in this fake),
 * so ubertooth-util adds no "read failed" line of its own, still exits 1, and
 * the multi-device listing still says the read failed. */
MU_TEST(short_reply_gets_one_error_line_from_the_library_only)
{
	struct run_result r;

	reset_fake();
	fake.channel = UBERTOOTH_ERROR_SHORT_REPLY;
	r = RUN("-c");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(0, (long)strlen(r.out));
	mu_assert(strstr(r.err, "read failed") == NULL, "no second line");

	/* any other error still gets ubertooth-util's own line */
	reset_fake();
	fake.channel = -7;
	r = RUN("-c");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "error: channel read failed\n") != NULL, "own line");

	reset_fake();
	fake.device_count = 2;
	fake.board_fail_index = 0;
	fake.board_fail_rc = UBERTOOTH_ERROR_SHORT_REPLY;
	r = RUN("-b");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.out, "0: <board ID read failed>\n1: 1 (Ubertooth One)\n") == 0,
	          "listing still names the failed read");
}

MU_TEST(optional_values_may_be_the_next_word)
{
	struct run_result r;

	reset_fake();
	r = RUN("-c", "2440");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(2440, fake.last_channel_set);
	mu_assert(strstr(r.out, "Current frequency") == NULL, "set, not read");

	reset_fake();
	r = RUN("-C", "39", "-U0");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(2441, fake.last_channel_set);
	mu_assert(strcmp(fake.selector, "0") == 0, "-U still applies");

	reset_fake();
	r = RUN("-a", "5");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(5, fake.last_palevel_set);

	reset_fake();
	r = RUN("-l", "1");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(2, fake.last_usrled_set); /* recorded as value + 1 */

	reset_fake();
	r = RUN("-z", "-90");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq((u16)-90, fake.last_squelch_set);

	reset_fake();
	r = RUN("-q", "100");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(100, fake.last_led_specan_arg);

	/* attached and separate give the same result */
	reset_fake();
	r = RUN("-z-90");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq((u16)-90, fake.last_squelch_set);

	/* a following option is not a value: -c reads, then -b runs */
	reset_fake();
	r = RUN("-c", "-b");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strstr(r.out, "Current frequency") != NULL, "-c read the channel");
	mu_assert(strstr(r.out, "Board ID Number") != NULL, "-b ran");
	mu_assert_int_eq(0, fake.last_channel_set);

	/* only -z takes a negative value, so -a does not take -9 as its value:
	 * it is read as an option, which the tool does not have */
	reset_fake();
	r = RUN("-a", "-9");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(0, fake.last_palevel_set);
	mu_assert(strstr(r.err, "error: unrecognized option '-9'") != NULL,
	          "-9 was not taken as the value of -a");

	/* a value at the end of a longer command */
	reset_fake();
	r = RUN("-U", "0", "-b", "-c", "2450");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(2450, fake.last_channel_set);
}

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
	                 "0: git-test (API:1.14)\n1: git-test (API:1.14)\n") == 0,
	          "per-device firmware lines");
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
		{ "-v", "0: git-test (API:1.14)\n1: git-test (API:1.14)\n" },
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
	/* The report's stderr line is captured into the listing, not repeated. */
	mu_assert_int_eq(0, (long)strlen(r.err));

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
	mu_assert_int_eq(0, (long)strlen(r.err));

	reset_fake();
	fake.device_count = 2;
	fake.partnum_rc = -1;
	r = RUN("-p");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.out, "0: <part ID read failed>\n1: <part ID read failed>\n") == 0,
	          "part ID read failure shown per device");
	mu_assert_int_eq(0, (long)strlen(r.err));

	reset_fake();
	fake.device_count = 2;
	fake.board_fail_index = 0;
	fake.partnum_rc = -1;
	r = RUN("-b", "-p", "-s");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.out,
	                 "0: <board ID read failed>\n0: <part ID read failed>\n0: " SERIAL_LINE
	                 "1: " BOARD_LINE "1: <part ID read failed>\n1: " SERIAL_LINE) == 0,
	          "combined listing with failed reads");
	mu_assert_int_eq(0, (long)strlen(r.err));

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
	                 "0: <firmware read failed>\n1: git-test (API:1.14)\n") == 0,
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
		const char *const others[] = { "-c", "-a", "-l", "-d", "-z", "-m", "-S" };
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
		"-z", "-z5", "-m", "-S", "-r", "-I", "-N",
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
		                    fake.tx_test_calls + fake.xmas_calls + fake.identify_calls);
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

MU_TEST(a_failed_identity_read_names_the_query_on_stderr)
{
	struct run_result r;

	reset_fake();
	fake.board_id = LIBUSB_ERROR_TIMEOUT;
	r = RUN("-b");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.out, "") == 0, "-b stdout");
	mu_assert(strcmp(r.err, "error: board ID read failed\n") == 0, "-b stderr");

	reset_fake();
	fake.partnum_rc = LIBUSB_ERROR_TIMEOUT;
	r = RUN("-p");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.out, "") == 0, "-p stdout");
	mu_assert(strcmp(r.err, "error: part ID read failed\n") == 0, "-p stderr");

	reset_fake();
	fake.serial_rc = 1; /* nonzero status byte */
	r = RUN("-s");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.out, "") == 0, "-s stdout");
	mu_assert(strcmp(r.err, "error: serial read failed\n") == 0, "-s stderr");

	/* Composed with the other reads, in output order. */
	reset_fake();
	fake.board_id = -1;
	fake.channel = -1;
	fake.serial_rc = -1;
	r = RUN("-b", "-c", "-a", "-p", "-s");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.out, PALEVEL_LINE PART_LINE) == 0, "remaining lines in order");
	mu_assert(strcmp(r.err,
	                 "error: board ID read failed\nerror: channel read failed\n"
	                 "error: serial read failed\n") == 0,
	          "one message per failed read");

	/* An unknown board ID keeps its own message and is not a read failure. */
	reset_fake();
	fake.board_id = 7;
	r = RUN("-b");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.err, "error: unknown board ID 7\n") == 0, "unknown board ID");
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

MU_TEST(reset_waits_for_a_slow_reenumeration_then_runs_later_actions)
{
	struct run_result r;

	/* Slower than the fixed two-second sleep this replaced. */
	reset_fake();
	fake.leaves_at_ms = 1000 + 800;
	fake.returns_at_ms = 1000 + 6000;
	r = RUN("-r", "-b");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, BOARD_LINE) == 0, "board read after the reset");
	mu_assert(strcmp(r.err, "Resetting ubertooth device 0\n") == 0,
	          "the reset message is progress, on stderr, and the polls are quiet");
	mu_assert(fake.now_ms >= fake.returns_at_ms &&
	          fake.now_ms < fake.returns_at_ms + UTIL_RESET_WAIT_POLL_MS,
	          "selected within one poll of returning");
	/* the board is followed by its own serial, not by the default selection */
	mu_assert(strcmp(fake.quiet_selector, FAKE_SERIAL) == 0, "follows the serial");
	mu_assert(!fake.quiet_null_selector, "not the default selection");
	mu_assert_int_eq(0, fake.stop_calls);
	mu_assert_int_eq(0, fake.first_used_after_release);
	/* a libusb reference is held through every poll and released after */
	mu_assert_int_eq(1, fake.libusb_refs_min_during_wait);
	mu_assert_int_eq(0, fake.libusb_refs);
}

MU_TEST(the_wait_still_works_if_libusb_cannot_be_held)
{
	reset_fake();
	fake.libusb_init_rc = -1;
	mu_assert_int_eq(0, RUN("-r", "-b").rc);
	mu_assert_int_eq(0, fake.libusb_refs); /* nothing to release */
}

MU_TEST(reset_by_serial_polls_for_the_same_serial)
{
	struct run_result r;

	reset_fake();
	fake.device_count = 2;
	fake.returns_at_ms = 1000 + 4000;
	r = RUN("-U", "a9aaabacadaeafb0", "-r", "-s");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, SERIAL_LINE) == 0, "serial read after the reset");
	mu_assert(strcmp(r.err, "Resetting ubertooth device a9aaabacadaeafb0\n") == 0,
	          "the reset message is progress, on stderr, and the polls are quiet");
	/* the prefix the user gave selected this board; the wait follows its
	 * whole serial, which is the same board */
	mu_assert(strcmp(fake.quiet_selector, FAKE_SERIAL) == 0,
	          "follows the board's serial");
	/* a serial cannot match another board, so it is polled while gone */
	mu_assert(fake.quiet_while_absent > 0, "polled while gone");
	mu_assert(fake.now_ms >= fake.returns_at_ms, "selected once back");
	mu_assert_int_eq(0, fake.stop_calls);
	mu_assert_int_eq(0, fake.loud_starts_after_reset);
}

MU_TEST(reset_by_index_follows_the_board_by_serial)
{
	struct run_result r;

	/* With two boards, waiting for index 1 could open the other one: the
	 * index order can change across the reset. */
	reset_fake();
	fake.device_count = 2;
	fake.returns_at_ms = 1000 + 3000;
	r = RUN("-U", "1", "-r", "-b");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(fake.quiet_selector, FAKE_SERIAL) == 0,
	          "follows the board's serial, not the index");
	/* read before the reset was sent, while the device was still there */
	mu_assert_int_eq(1, fake.serial_reads);
	mu_assert_int_eq(0, fake.resets_at_first_serial_read);
	/* the "Resetting" line is progress, on stderr, and keeps the selector
	 * the user gave; nothing else is printed about the serial */
	mu_assert(strcmp(r.err, "Resetting ubertooth device 1\n") == 0,
	          "the user's selector is reported, and nothing about the serial");
	mu_assert(strcmp(r.out, BOARD_LINE) == 0, "only the board read on stdout");
	mu_assert(fake.now_ms >= fake.returns_at_ms, "selected once back");
	mu_assert_int_eq(0, fake.stop_calls);
}

MU_TEST(a_serial_that_cannot_be_read_keeps_the_selector_with_a_warning)
{
	struct run_result r;

	/* Falls back to today's behaviour: the index is waited for, and the user
	 * is told later options may not reach the same board. */
	reset_fake();
	fake.device_count = 2;
	fake.serial_rc = 1; /* nonzero status byte */
	fake.returns_at_ms = 1000 + 3000;
	r = RUN("-U", "1", "-r", "-b");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(fake.quiet_selector, "1") == 0, "the index is kept");
	mu_assert(strcmp(r.err, "warning: serial read failed on Ubertooth device 1 "
	                 "before the reset; later options may reach a different "
	                 "board if the enumeration order changes\n"
	                 "Resetting ubertooth device 1\n") == 0,
	          "one warning line, then the reset progress");
	mu_assert_int_eq(0, fake.quiet_while_absent); /* index: waits for the count */
	mu_assert(strstr(r.out, BOARD_LINE) != NULL, "board read after the reset");

	/* A serial selector already follows the board, so nothing is lost and
	 * nothing is warned about. */
	reset_fake();
	fake.device_count = 2;
	fake.serial_rc = 1;
	r = RUN("-U", "a9aaabacadaeafb0", "-r");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(fake.quiet_selector, "a9aaabacadaeafb0") == 0,
	          "the serial selector is kept");
	mu_assert(strcmp(r.err, "Resetting ubertooth device a9aaabacadaeafb0\n") == 0,
	          "the reset progress only: no warning for a serial selector");
}

MU_TEST(a_failed_reset_still_reselects_once_the_leave_window_passes)
{
	struct run_result r;

	/* The device may have reset despite the error, so the wait for it to
	 * leave is kept; it never leaves here, so it is reopened after that. */
	reset_fake();
	fake.reset_rc = -1;
	r = RUN("-r", "-b");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.out, BOARD_LINE) != NULL, "later report still printed");
	mu_assert_int_eq(1000 + (int)UTIL_RESET_WAIT_LEAVE_MS, (int)fake.now_ms);
	mu_assert_int_eq(0, fake.stop_calls);
}

MU_TEST(reset_by_index_with_one_board_reopens_it)
{
	struct run_result r;

	/* With one board any index selects it, before and after the reset. */
	reset_fake();
	r = RUN("-U", "1", "-r", "-b");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strstr(r.out, BOARD_LINE) != NULL, "board read after the reset");
	mu_assert(fake.now_ms < fake.returns_at_ms + UTIL_RESET_WAIT_POLL_MS,
	          "selected once back, without waiting for the deadline");
	mu_assert_int_eq(0, fake.stop_calls);
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
	MU_RUN(a_set_and_a_read_of_the_same_value_both_run_set_first);
	MU_RUN(stop_and_reset_report_failures);
	MU_RUN(mode_commands_exit_zero_on_success_and_one_on_failure);
	MU_RUN(a_failed_stop_before_a_mode_command_fails_the_command);
	MU_RUN(identify_starts_the_blink_on_the_board_and_returns);
	MU_RUN(identify_refuses_firmware_of_another_api_version);
	MU_RUN(led_specan_uses_the_default_threshold_and_an_explicit_one);
	MU_RUN(two_or_more_mode_options_are_rejected_before_opening_the_device);
	MU_RUN(stray_arguments_are_rejected_before_any_device_action);
	MU_RUN(short_reply_gets_one_error_line_from_the_library_only);
	MU_RUN(optional_values_may_be_the_next_word);
	MU_RUN(pa_level_zero_is_set_and_bad_levels_are_refused);
	MU_RUN(numeric_option_values_are_parsed_strictly);
	MU_RUN(squelch_with_a_mode_option_is_refused);
	MU_RUN(invalid_arguments_are_rejected_before_opening_the_device);
	MU_RUN(help_version_and_count_exit_zero_without_a_device);
	MU_RUN(firmware_listing_with_several_devices_reports_each_device);
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
	MU_RUN(a_failed_identity_read_names_the_query_on_stderr);
	MU_RUN(a_failed_led_read_drops_only_that_line);
	MU_RUN(a_failed_read_keeps_the_other_lines_of_a_composed_command);
	MU_RUN(read_only_queries_release_the_handle_without_sending_stop);
	MU_RUN(setters_and_modes_release_the_handle_on_success_and_failure);
	MU_RUN(reset_waits_for_a_slow_reenumeration_then_runs_later_actions);
	MU_RUN(the_wait_still_works_if_libusb_cannot_be_held);
	MU_RUN(reset_by_serial_polls_for_the_same_serial);
	MU_RUN(reset_by_index_follows_the_board_by_serial);
	MU_RUN(a_serial_that_cannot_be_read_keeps_the_selector_with_a_warning);
	MU_RUN(a_failed_reset_still_reselects_once_the_leave_window_passes);
	MU_RUN(reset_by_index_with_one_board_reopens_it);
	MU_RUN(the_handle_reopened_after_a_reset_is_released);
	return mu_report();
}
