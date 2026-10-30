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

#include "run_main.h"

/* ---- scripted device -------------------------------------------------- */

static struct {
	int board_id, channel, palevel, squelch, usrled, rxled, txled;
	uint32_t partnum;
	int partnum_rc, serial_rc, api_rc, revision_rc, compile_info_rc;
	/* the info block read on its own: LIBUSB_ERROR_PIPE is how firmware
	 * older than the block answers it */
	int fw_info_rc;
	uint16_t api_version;
	const char *image, *revision, *compile_info;
	/* results of commands that set state or start a mode */
	int stop_rc, reset_rc, set_channel_rc, set_usrled_rc, set_rxled_rc;
	int set_txled_rc, set_palevel_rc, set_squelch_rc, isp_rc;
	int led_specan_rc, range_test_rc, repeater_rc, tx_test_rc, xmas_rc;
	int identify_rc;
	int rangeresult_rc;
	rangetest_result range;
	int rangeresult_calls;
	/* the scripted range test -n follows; see fake_range_reply_arrived() */
	uint64_t range_started_ms, range_reply_after_ms, range_leaves_after_ms;
	int range_reply_at_call;
	int range_stall_calls, watchdog_stall_calls;
	int range_stale_reads;        /* reads answered with the test before */
	rangetest_result range_stale; /* ...and what that one said */
	/* A packet the board discarded, reported from this poll on: the board
	 * records one while it listens, before it has a result. */
	int range_discard_from_call;
	uint8_t range_discard;
	watchdog_info watchdog;
	int watchdog_rc;
	int watchdog_calls;
	radio_timeouts timeouts;
	int timeouts_rc;
	int timeouts_calls;
	/* bookkeeping */
	int device_count;
	int count_calls; /* ubertooth_count() calls */
	int start_calls;
	int fail_start_from; /* 0: never; n: ubertooth_start_select fails from call n */
	char selector[64];
	int stop_calls, isp_calls, led_specan_calls, range_test_calls;
	int repeater_calls, tx_test_calls, xmas_calls, reset_calls;
	int identify_calls;
	int serial_reads;
	/* the quiet serial read -r makes before the reset */
	int follow_serial_reads, resets_at_first_serial_read;
	int follow_serial_rc, follow_serial_timeout_ms;
	/* -r on a board that answers nothing: the USB connection reset */
	int usb_resets, usb_reset_rc;
	struct libusb_device_handle *usb_reset_handle;
	/* the board goes on answering nothing after the reset, instead of
	 * answering again as it does on the bench */
	int usb_reset_leaves_it_silent;
	/* ...or answers only once this many polls have passed */
	int usb_reset_answers_after_polls;
	uint64_t reset_at_ms; /* the clock when the reset request was sent */
	int last_channel_set, last_squelch_set, last_led_specan_arg;
	int last_palevel_set, last_usrled_set;
	/* LED writes: -I must not blink from the host */
	int usrled_writes, rxled_writes, txled_writes;
	int last_usrled_level, last_rxled_level, last_txled_level;
	/* multi-device listing */
	int current_index;     /* device opened by ubertooth_start(), or -1 */
	int unavailable_index; /* ubertooth_start() fails for this index */
	/* why that open failed, as the library would say it (NULL: the
	 * library's own fallback phrase) */
	const char *open_failure_reason;
	int board_fail_index;  /* the board ID read fails on this device */
	int board_fail_rc;     /* ...with this code (0: -1) */
	int revision_fail_index;     /* the firmware revision read fails here */
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
	char first_quiet_selector[64]; /* what the first wait looked for */
	int quiet_null_selector; /* it was called with NULL (no -U) */
	int loud_starts_after_reset;
	/* libusb default-context references taken by ubertooth-util itself */
	int libusb_refs, libusb_refs_min_during_wait, libusb_init_rc;
	/* 1 to have the teardown's STOP refused, so the board is left running
	 * the mode the capture started. */
	int stop_refused;
	/* the signal handler -n installs, and the signal itself: the wait is
	 * stopped once it has slept this many times (0: never), which is where
	 * a Ctrl-C lands */
	int cleanup_handlers;
	int stop_after_sleeps;
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
	fake.image = "bluetooth_rxtx";
	fake.revision = "git-test";
	fake.compile_info = "compile-test";
	fake.device_count = 1;
	fake.range.valid = 1;
	fake.range.request_pa = 1;
	fake.range.request_num = 2;
	fake.range.reply_pa = 3;
	fake.range.reply_num = 4;
	/* a repeater that answers quickly, so a plain -n finishes */
	fake.range_reply_after_ms = 300;
	fake.current_index = -1;
	fake.unavailable_index = -1;
	fake.board_fail_index = -1;
	fake.revision_fail_index = -1;
	fake.now_ms = 1000;
	fake.leaves_at_ms = 1050;  /* soon after the reset... */
	fake.returns_at_ms = 1600; /* ...and back after about half a second */
	memset(&fake_ut, 0, sizeof(fake_ut));
	fake_ut.devh = FIRST_DEVH;
	memset(&fake_ut_reset, 0, sizeof(fake_ut_reset));
	fake_ut_reset.devh = (struct libusb_device_handle *)&fake_devh_reset;
}

/* Whether the USB connection reset reported the device gone, which is how
 * libusb says the board re-enumerated. */
static int fake_usb_reset_took_the_device(void)
{
	return fake.usb_reset_rc == LIBUSB_ERROR_NOT_FOUND ||
	       fake.usb_reset_rc == LIBUSB_ERROR_NO_DEVICE;
}

/* Whether a reset that re-enumerates the board has been made: the request it
 * answered, or a USB connection reset that took the device with it. */
static int fake_board_was_reset(void)
{
	return (fake.reset_calls > 0 && fake.reset_rc >= 0) ||
	       (fake.usb_resets > 0 && fake_usb_reset_took_the_device());
}

/* The reset device is missing from the count while it re-enumerates. */
static int fake_reset_device_absent(void)
{
	return fake_board_was_reset() &&
	       fake.now_ms >= fake.leaves_at_ms && fake.now_ms < fake.returns_at_ms;
}

void ubertooth_note_dfu_bootloader(void)
{
	/* The library's note for a bus with no Ubertooth on it but a board in the
	 * bootloader; this test scripts no bootloader board. */
}

int ubertooth_devices_hidden(void)
{
	/* The switch the project's tests set is not what this test scripts:
	 * fake.device_count below says how many boards are attached. */
	return 0;
}

int ubertooth_count(void)
{
	fake.count_calls++;
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

/* The USB connection reset -r falls back to on a board that answers nothing.
 * The board answers from the call after it, which is what the bench showed,
 * unless the case says otherwise. */
int libusb_reset_device(struct libusb_device_handle *h)
{
	fake_use(h);
	fake.usb_resets++;
	fake.usb_reset_handle = h;
	if (fake.usb_reset_rc == 0 && !fake.usb_reset_leaves_it_silent &&
	    fake.usb_reset_answers_after_polls == 0)
		fake.follow_serial_rc = 0;
	return fake.usb_reset_rc;
}

/* The library's signal handling, as the tool uses it: the handler is
 * installed, and a flag it would set is read on the tool's own thread. A case
 * raises the flag by the sleep count rather than by a real signal, so the
 * moment it arrives is the same on every run. */
void register_cleanup_handler(ubertooth_t *ut, int do_exit)
{
	(void)ut;
	(void)do_exit;
	fake.cleanup_handlers++;
}

int ubertooth_stopping(ubertooth_t *ut)
{
	(void)ut;
	return fake.stop_after_sleeps != 0
	       && fake.sleeps >= fake.stop_after_sleeps;
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
	if (fake.quiet_starts == 1)
		snprintf(fake.first_quiet_selector,
		         sizeof(fake.first_quiet_selector), "%s", u_arg ? u_arg : "");
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

/* The library's phrase for the open that just failed. The real one classifies
 * the libusb result; the listing only has to put it on the line. */
const char *ubertooth_open_failure_reason(void)
{
	return fake.open_failure_reason != NULL ? fake.open_failure_reason
	                                        : "it could not be opened";
}

ubertooth_t *ubertooth_start_select(const char *u_arg)
{
	fake.start_calls++;
	snprintf(fake.selector, sizeof(fake.selector), "%s", u_arg ? u_arg : "");
	if (fake.reset_calls > 0)
		fake.loud_starts_after_reset++; /* would print errors and send STOP */
	if (fake.fail_start_from && fake.start_calls >= fake.fail_start_from)
		return NULL;
	/* Like the library: the handle remembers the board the user named, so
	 * a message about this one board can say which it is. */
	fake_ut.selector = u_arg;
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

/* The report about a refused request asks whether the board has left the bus
 * before it blames the board's firmware. Nothing here unplugs a board, so it
 * is always still there. */
int ubertooth_device_gone(const void* board)
{
	(void)board;
	return 0;
}

/* Whether the teardown's STOP was refused, which leaves the board running the
 * mode the capture started. Scripted, because the fake board above always
 * takes it. */
int ubertooth_stop_failed(ubertooth_t *ut)
{
	(void)ut;
	return fake.stop_refused;
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

#ifndef UTIL_TEST_REAL_API_MESSAGE
/* Built against an installed libubertooth, so there is no library source to
 * compile in: the message is a stub here, and the case below then checks only
 * that -I refuses the board. */
int ubertooth_report_api_mismatch(uint16_t version, const char *selector)
{
	(void)version;
	(void)selector;
	return -1;
}
#endif

/* The host line of the version answer: faked, so the cases pin the shape of
 * the answer rather than the versions this build was configured with. */
void ubertooth_print_host_version(FILE *out) { fprintf(out, "libubertooth fake\n"); }

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
	fake.serial_reads++;
	serial[0] = 0;
	for (i = 1; i < 17; i++)
		serial[i] = (u8)(0xa0 + i);
	return fake.serial_rc;
}

/* The library's quiet serial read, which prints nothing of its own. -r uses
 * it before the reset; cmd_get_serial() above would print a transfer error
 * and wait out its own timeout. */
int ubertooth_read_serial_string(struct libusb_device_handle *h, char *out,
                                 size_t size, unsigned timeout_ms)
{
	int i;

	fake_use(h);
	/* Like the library: too small a buffer is refused before any transfer. */
	if (size < UBERTOOTH_SERIAL_STR_LEN)
		return -1;
	/* -r must read the serial while the device is still there, before the
	 * reset is sent: how many resets had been sent at the first read. */
	if (fake.follow_serial_reads++ == 0)
		fake.resets_at_first_serial_read = fake.reset_calls;
	fake.follow_serial_timeout_ms = (int)timeout_ms;
	/* A board that takes a few polls after the USB reset to answer. */
	if (fake.usb_reset_answers_after_polls > 0 && fake.usb_resets > 0 &&
	    fake.sleeps >= fake.usb_reset_answers_after_polls)
		fake.follow_serial_rc = 0;
	/* A board the USB reset re-enumerated answers again once it is back on
	 * the bus, as any board that has just reset does. */
	if (fake.usb_resets > 0 && fake_usb_reset_took_the_device() &&
	    fake.now_ms >= fake.returns_at_ms)
		fake.follow_serial_rc = 0;
	if (fake.follow_serial_rc != 0)
		return fake.follow_serial_rc;
	for (i = 1; i < UBERTOOTH_SERIAL_LEN; i++)
		snprintf(out + (i - 1) * 2, 3, "%02x", 0xa0 + i);
	return 0;
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

/* The firmware info block -v and -V take the image and the revision from. It
 * fails where the revision read used to, so the tests that script a failed
 * firmware read still script this one. */
int cmd_get_firmware_info(struct libusb_device_handle *h,
                          struct ubertooth_fw_info *info)
{
	fake_use(h);
	memset(info, 0, sizeof(*info));
	if (fake.fw_info_rc < 0)
		return fake.fw_info_rc;
	if (fake.current_index >= 0 &&
	    fake.current_index == fake.revision_fail_index)
		return LIBUSB_ERROR_TIMEOUT;
	if (fake.revision_rc < 0)
		return fake.revision_rc;
	info->format_version = UBERTOOTH_FW_INFO_FORMAT;
	info->api_version = fake.api_version;
	snprintf(info->image, sizeof(info->image), "%s", fake.image);
	snprintf(info->revision, sizeof(info->revision), "%s", fake.revision);
	return 0;
}

/* The revision on its own, which every firmware answers: what -v reads when
 * the board carries no info block. */
int cmd_get_firmware_revision(struct libusb_device_handle *h, char *revision,
                              size_t size)
{
	fake_use(h);
	if (revision == NULL || size == 0)
		return LIBUSB_ERROR_INVALID_PARAM;
	revision[0] = '\0';
	if (fake.current_index >= 0 &&
	    fake.current_index == fake.revision_fail_index)
		return LIBUSB_ERROR_TIMEOUT;
	if (fake.revision_rc < 0)
		return fake.revision_rc;
	snprintf(revision, size, "%s", fake.revision);
	return 0;
}

int cmd_get_firmware_compile_info(struct libusb_device_handle *h, char *info,
                                  size_t size)
{
	return fake_text_read(h, info, size, fake.compile_info,
	                      fake.compile_info_rc, -1);
}

/*
 * The board as -n sees it, on the fake clock: a test that has been started
 * clears the previous result, reports one once the reply arrives, and leaves
 * MODE_RANGE_TEST when the test is over. range_reply_after_ms of 0 is a run
 * no repeater answers, and range_leaves_after_ms says when something else
 * (a -S from another command) ended the mode.
 */
static int fake_range_reply_arrived(void)
{
	if (fake.range_test_calls == 0)
		return 0;
	/* range_reply_at_call puts the reply between two reads that the fake
	 * clock cannot separate, which is where the board leaves the mode with
	 * a result the host has not read yet. */
	if (fake.range_reply_at_call != 0 &&
	    fake.rangeresult_calls >= fake.range_reply_at_call)
		return 1;
	return fake.range_reply_after_ms != 0 &&
	       fake.now_ms >= fake.range_started_ms + fake.range_reply_after_ms;
}

static int fake_range_mode_running(void)
{
	uint64_t leaves = fake.range_leaves_after_ms;

	if (fake.range_test_calls == 0)
		return 0;
	if (leaves == 0) {
		if (fake.range_reply_after_ms == 0)
			return 1; /* listening forever */
		leaves = fake.range_reply_after_ms;
	}
	return fake.now_ms < fake.range_started_ms + leaves;
}

int cmd_get_rangeresult(struct libusb_device_handle *h, rangetest_result *rr)
{
	fake_use(h);
	fake.rangeresult_calls++;
	/* range_stale_reads is the window before the board has entered the mode
	 * and cleared the previous test: a read that lands there still answers
	 * with that test's levels. */
	if (fake.range_test_calls > 0 &&
	    fake.rangeresult_calls <= fake.range_stale_reads)
		*rr = fake.range_stale;
	else if (fake.range_test_calls > 0 && !fake_range_reply_arrived())
		memset(rr, 0, sizeof(*rr)); /* cleared while the test runs */
	else
		*rr = fake.range;
	/* A packet that was not this board's reply, from this poll on. Left out
	 * of the stale window above: that read is answering with the previous
	 * test, which this run has nothing to do with. */
	if (fake.range_discard_from_call != 0 &&
	    fake.rangeresult_calls >= fake.range_discard_from_call &&
	    fake.rangeresult_calls > fake.range_stale_reads)
		rr->discarded = fake.range_discard;
	return fake.rangeresult_rc;
}

int cmd_get_watchdog(struct libusb_device_handle *h, watchdog_info *info)
{
	fake_use(h);
	fake.watchdog_calls++;
	*info = fake.watchdog;
	if (fake.range_test_calls > 0)
		info->mode = fake_range_mode_running() ? MODE_RANGE_TEST : MODE_IDLE;
	return fake.watchdog_rc;
}

/*
 * The quiet forms -n polls with. range_stall_calls and watchdog_stall_calls
 * are the stall a board part-way through a mode answers one poll in twenty
 * with: the first that many reads fail, and the rest answer.
 */
int cmd_get_rangeresult_quiet(struct libusb_device_handle *h,
                              rangetest_result *rr)
{
	int r = cmd_get_rangeresult(h, rr);

	if (r == 0 && fake.rangeresult_calls <= fake.range_stall_calls)
		return -9;
	return r;
}

int cmd_get_watchdog_quiet(struct libusb_device_handle *h, watchdog_info *info)
{
	int r = cmd_get_watchdog(h, info);

	if (r == 0 && fake.watchdog_calls <= fake.watchdog_stall_calls)
		return -9;
	return r;
}

int cmd_get_radio_timeouts(struct libusb_device_handle *h,
                           radio_timeouts *timeouts)
{
	fake_use(h);
	fake.timeouts_calls++;
	*timeouts = fake.timeouts;
	return fake.timeouts_rc;
}

int cmd_reset(struct libusb_device_handle *h)
{
	fake_use(h);
	fake.reset_calls++;
	/* When it was sent, so a case can pin that the board was back by then. */
	fake.reset_at_ms = fake.now_ms;
	return fake.reset_rc;
}
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

int cmd_set_isp(struct libusb_device_handle *h) { fake_use(h); fake.isp_calls++; return fake.isp_rc; }
int cmd_range_test(struct libusb_device_handle *h)
{
	fake_use(h);
	fake.range_test_calls++;
	fake.range_started_ms = fake.now_ms;
	return fake.range_test_rc;
}
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

/* Nothing of this tool's own has to be set up or checked around a run, so
 * this is the shared harness and nothing else. The cases that build their own
 * argument list call it rather than the macro below. */
static struct run_result run_util(const char *const *args)
{
	return run_tool_main(ubertooth_util_main, args);
}

#define RUN(...) run_util((const char *const[]){ "ubertooth-util", __VA_ARGS__, NULL })

/* Exact expected report lines, in the order ubertooth-util prints them. */
#define BOARD_LINE "Board ID Number: 1 (Ubertooth One)\n"
#define CHANNEL_LINE "Current frequency: 2441 MHz (Bluetooth channel 39)\n"
#define FIRMWARE_LINE "Firmware version: bluetooth_rxtx git-test (API:1.57)\n"
#define BUILD_LINE "Firmware build: compile-test\n"
/* The host line is faked above, so it is this. */
#define HOST_LINE "libubertooth fake\n"
/* The whole -V answer for the scripted board, which is libubertooth's and the
 * same in every tool (test_version_report.c has its own cases). */
#define VERSION_ANSWER HOST_LINE FIRMWARE_LINE BUILD_LINE
#define USRLED_LINE "USR LED status: 0\n"
#define PALEVEL_LINE "PA Level: 7\n"
#define PART_LINE "Part ID: 25011723\n"
/* The serial the scripted device reports, as -s prints it and as -r follows
 * it across a reset. */
#define FAKE_SERIAL "a1a2a3a4a5a6a7a8a9aaabacadaeafb0"
#define SERIAL_LINE "Serial No: " FAKE_SERIAL "\n"

/* The same lines as the multi-device listing prints them: the report's own
 * label with the board's index in it. i is the index, as a string. */
#define LISTED_BOARD(i)    "Board ID Number (" i "): 1 (Ubertooth One)\n"
#define LISTED_FIRMWARE(i) "Firmware version (" i "): bluetooth_rxtx git-test (API:1.57)\n"
#define LISTED_PART(i)     "Part ID (" i "): 25011723\n"
#define LISTED_SERIAL(i)   "Serial No (" i "): " FAKE_SERIAL "\n"

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
	r = RUN("-b", "-c", "-v", "-l", "-a", "-p", "-s");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out,
	                 BOARD_LINE CHANNEL_LINE FIRMWARE_LINE
	                 USRLED_LINE PALEVEL_LINE PART_LINE SERIAL_LINE) == 0,
	          "composed output order");

	/* The identity readback used by the hardware checks. */
	r = RUN("-vspb");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out,
	                 BOARD_LINE FIRMWARE_LINE PART_LINE SERIAL_LINE) == 0,
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
	r = RUN("-b", "-v", "-p", "-s");
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

/*
 * Firmware built before the info block stalls the request for it, and this is
 * the one tool that has to name a board whatever it is running. -v then
 * prints the revision alone, the line it always printed.
 */
MU_TEST(a_board_without_an_info_block_is_still_identified)
{
	struct run_result r;

	reset_fake();
	fake.fw_info_rc = LIBUSB_ERROR_PIPE;
	r = RUN("-v");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, "Firmware version: git-test (API:1.57)\n") == 0,
	          r.out);
	mu_assert_int_eq(0, (long)strlen(r.err));

	/* With nothing to fall back on it is still a failure, with no line. */
	reset_fake();
	fake.fw_info_rc = LIBUSB_ERROR_PIPE;
	fake.revision_rc = LIBUSB_ERROR_TIMEOUT;
	r = RUN("-v");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(0, (long)strlen(r.out));
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
	mu_assert(strcmp(r.out, "Firmware version: bluetooth_rxtx git-test\n") == 0,
	          "firmware line without API");

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

	/* -V is the version answer, so a build string it could not read is a
	 * line of its own saying so, and the command fails. */
	reset_fake();
	fake.compile_info_rc = LIBUSB_ERROR_IO;
	r = RUN("-V");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.out, HOST_LINE FIRMWARE_LINE
	                 "Firmware build: read failed\n") == 0, r.out);

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

/*
 * -w reads the watchdog breadcrumb. A board that reset itself says which mode
 * and which wait it was in, on standard output because it is a read; a failed
 * read names the query and fails the command.
 */
MU_TEST(the_watchdog_state_is_reported_and_a_failed_read_fails)
{
	struct run_result r;

	reset_fake();
	fake.watchdog.reset_cause = WATCHDOG_RESET_WATCHDOG;
	fake.watchdog.armed = 1;
	fake.watchdog.resets = 2;
	fake.watchdog.boots = 4;
	fake.watchdog.reset_mode = MODE_BT_FOLLOW_LE;
	fake.watchdog.reset_site = WATCHDOG_SITE_XOSC;
	fake.watchdog.mode = MODE_IDLE;
	fake.watchdog.site = WATCHDOG_SITE_NONE;
	fake.watchdog.transfer_errors = 5;
	fake.watchdog.fault_status = 0x82;
	fake.watchdog.fault_addr = 0x4000c008;
	fake.timeouts.fs_lock = 3;
	fake.timeouts.fsm = 65535;
	r = RUN("-w");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(1, fake.watchdog_calls);
	mu_assert(strstr(r.out, "Last boot cause: watchdog\n") != NULL, r.out);
	mu_assert(strstr(r.out, "BLE follow") != NULL, r.out);
	/* What the last processor fault was, and the packets a capture lost to
	 * failed transfers. */
	mu_assert(strstr(r.out, "Last processor fault: status 0x00000082, "
	                 "the access was at 0x4000c008\n") != NULL, r.out);
	mu_assert(strstr(r.out, "Capture transfer errors since power-on: 5\n")
	          != NULL, r.out);
	/* The radio wait counts come with it, in the reply's order. */
	mu_assert_int_eq(1, fake.timeouts_calls);
	mu_assert(strstr(r.out, "Radio waits that gave up since boot: "
	                 "synthesizer lock 3, unlock 0, oscillator 0, "
	                 "state machine 65535, packet bytes 0\n") != NULL,
	          r.out);
	mu_assert(strcmp(r.err, "") == 0, r.err);

	/* btbr records no mode, and the line says so in words. */
	reset_fake();
	fake.watchdog.armed = 1;
	fake.watchdog.boots = 1;
	fake.watchdog.mode = MODE_UNKNOWN;
	fake.watchdog.site = WATCHDOG_SITE_USB;
	r = RUN("-w");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strstr(r.out, "Now in a mode the board does not report, "
	                 "servicing USB\n") != NULL, r.out);
	/* A board that has never faulted says nothing about faults, rather
	 * than reporting a status of zero as if one had happened. */
	mu_assert(strstr(r.out, "processor fault") == NULL, r.out);
	mu_assert(strstr(r.out, "Capture transfer errors since power-on: 0\n")
	          != NULL, r.out);
	mu_assert(strcmp(r.err, "") == 0, r.err);

	/* A fault the part keeps no access address for, where the board
	 * recorded where the processor was instead. 0x01 is an instruction
	 * fetch from a region that cannot be executed -- a branch through a
	 * corrupted pointer -- and the address is the one it refused to run. */
	reset_fake();
	fake.watchdog.armed = 1;
	fake.watchdog.boots = 1;
	fake.watchdog.fault_status = 0x01;
	fake.watchdog.fault_addr = 0x4001f0a4;
	r = RUN("-w");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strstr(r.out, "Last processor fault: status 0x00000001, "
	                 "the instruction was at 0x4001f0a4\n") != NULL, r.out);
	mu_assert(strcmp(r.err, "") == 0, r.err);

	/* A fault with nothing to name: 0x1000 is a bus fault on stacking, so
	 * the exception frame was never written and the board records no
	 * address. Zero is not printed as an address here, because address zero
	 * is the start of flash and no fault names it. */
	reset_fake();
	fake.watchdog.armed = 1;
	fake.watchdog.boots = 1;
	fake.watchdog.fault_status = 0x1000;
	r = RUN("-w");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strstr(r.out, "Last processor fault: status 0x00001000, "
	                 "no address recorded\n") != NULL, r.out);
	mu_assert(strcmp(r.err, "") == 0, r.err);

	/* A record with an address and no status bit at all, which a memory
	 * management fault can leave: the address is the whole of what
	 * happened, so the line is printed rather than the fault going
	 * unreported for want of a status. */
	reset_fake();
	fake.watchdog.armed = 1;
	fake.watchdog.boots = 1;
	fake.watchdog.fault_status = 0;
	fake.watchdog.fault_addr = 0x00002f18;
	r = RUN("-w");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strstr(r.out, "Last processor fault: status 0x00000000, "
	                 "the instruction was at 0x00002f18\n") != NULL, r.out);
	mu_assert(strcmp(r.err, "") == 0, r.err);

	/* The other address-valid bit, which a bus fault sets: both halves of
	 * the host's discriminator are read, not just the memory management
	 * one. */
	reset_fake();
	fake.watchdog.armed = 1;
	fake.watchdog.boots = 1;
	fake.watchdog.fault_status = 0x8400;
	fake.watchdog.fault_addr = 0x2007c0d0;
	r = RUN("-w");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strstr(r.out, "Last processor fault: status 0x00008400, "
	                 "the access was at 0x2007c0d0\n") != NULL, r.out);
	mu_assert(strcmp(r.err, "") == 0, r.err);

	reset_fake();
	fake.watchdog_rc = -7;
	r = RUN("-w");
	mu_assert_int_eq(1, r.rc);
	/* The watchdog read failed and the counts still arrived: the two are
	 * read separately so that one board that cannot answer the first still
	 * reports the second. */
	mu_assert(strstr(r.out, "Radio waits that gave up") != NULL, r.out);
	mu_assert(strstr(r.err, "watchdog state read failed") != NULL, r.err);

	/* And the other way round. */
	reset_fake();
	fake.timeouts_rc = -7;
	r = RUN("-w");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.out, "Last boot cause") != NULL, r.out);
	mu_assert(strstr(r.out, "Radio waits that gave up") == NULL, r.out);
	mu_assert(strstr(r.err, "radio wait timeouts read failed") != NULL, r.err);
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

	/* A test that ran but whose data did not match: a diagnostic, so it is
	 * on standard error and the command fails. Standard output stays empty,
	 * so a script reading a result is not handed this instead. */
	reset_fake();
	fake.range.valid = 2 + 5;
	r = RUN("-m");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(0, (long)strlen(r.out));
	mu_assert(strcmp(r.err, "error: invalid range test: mismatch on byte 5\n") == 0,
	          r.err);

	reset_fake();
	fake.rangeresult_rc = -1;
	r = RUN("-m");
	mu_assert_int_eq(1, r.rc);
}

/* ---- -n: following the range test it starts --------------------------- */

/* The number of lines in text, where the run printed a stream of them. */
static int lines_in(const char *text)
{
	int lines = 0;

	for (; *text != '\0'; text++)
		if (*text == '\n')
			lines++;
	return lines;
}

MU_TEST(range_test_reports_progress_and_then_the_result)
{
	struct run_result r;

	/* A repeater that takes two and a half seconds to answer: a progress
	 * line a second while the test runs, then the result block -m prints,
	 * both on standard output. */
	reset_fake();
	fake.range_reply_after_ms = 2500;
	r = RUN("-n");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out,
	                 "waiting for the repeater... (1 s)\n"
	                 "waiting for the repeater... (2 s)\n"
	                 "request PA level : 1\n"
	                 "request number   : 2\n"
	                 "reply PA level   : 3\n"
	                 "reply number     : 4\n") == 0, r.out);
	/* the one STOP that leaves a prior mode, and nothing else: the test
	 * ended by itself */
	mu_assert_int_eq(1, fake.stop_calls);
	mu_assert(strcmp(r.err, "Starting range test\n") == 0, r.err);

	/* An exchange that finishes inside the first second has no progress to
	 * report, only its result. */
	reset_fake();
	fake.range_reply_after_ms = 300;
	r = RUN("-n");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, "request PA level : 1\n"
	                        "request number   : 2\n"
	                        "reply PA level   : 3\n"
	                        "reply number     : 4\n") == 0, r.out);

	/* The result of this test, not of the one before it. A poll that lands
	 * before the board has entered the mode still reads the previous test's
	 * levels, so -n reports a result only once it has seen them cleared. */
	reset_fake();
	fake.range_reply_after_ms = 1000;
	fake.range_stale_reads = 3;
	fake.range_stale.valid = 1;
	fake.range_stale.request_pa = 7;
	fake.range_stale.request_num = 9;
	fake.range_stale.reply_pa = 7;
	fake.range_stale.reply_num = 9;
	r = RUN("-n");
	mu_assert_int_eq(0, r.rc);
	mu_assert(fake.rangeresult_calls > 1, "polled while the test ran");
	mu_assert(strstr(r.out, "request number   : 9\n") == NULL,
	          "reported the previous test's levels");
	mu_assert(strstr(r.out, "request number   : 2\n") != NULL, r.out);

	/* A reply whose payload did not match is a diagnostic, as it is for -m:
	 * standard error, nothing on standard output, exit 1. */
	reset_fake();
	fake.range.valid = 2 + 5;
	r = RUN("-n");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(0, (long)strlen(r.out));
	mu_assert(strstr(r.err, "error: invalid range test: mismatch on byte 5\n")
	          != NULL, r.err);
}

/*
 * A packet the board heard that was not its own reply is named once, and the
 * run goes on: it is somebody else's traffic, not a fault in this board. The
 * warning is on standard error, and the result still arrives on standard
 * output with exit 0.
 */
MU_TEST(a_discarded_packet_is_named_once_and_the_run_goes_on)
{
	struct run_result r;

	reset_fake();
	fake.range_reply_after_ms = 2500;
	fake.range_discard_from_call = 2;
	fake.range_discard = 2 + 2;  /* the serial number's first byte */
	r = RUN("-n");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strstr(r.out, "request PA level : 1\n") != NULL, r.out);
	mu_assert(strcmp(r.err, "Starting range test\n"
	          "warning: range test: a packet that is not this board's reply "
	          "was ignored (first difference at byte 2)\n") == 0, r.err);

	/* A run that discarded nothing says nothing. */
	reset_fake();
	fake.range_reply_after_ms = 1000;
	r = RUN("-n");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strstr(r.err, "warning:") == NULL, r.err);

	/* And a run whose result only turns up on the read that settles whether
	 * it finished or was stopped -- the board writes the result just before
	 * it leaves the mode -- is reported from there, discard included. The
	 * mode ends on the fifth poll and that read is the sixth. */
	reset_fake();
	fake.range_reply_after_ms = 0;
	fake.range_leaves_after_ms = 500;
	fake.range_reply_at_call = 6;
	fake.range_discard_from_call = 6;
	fake.range_discard = 2 + 5;
	r = RUN("-n");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strstr(r.out, "reply number     : 4\n") != NULL, r.out);
	mu_assert(strstr(r.err, "warning: range test: a packet that is not this "
	          "board's reply was ignored (first difference at byte 5)\n")
	          != NULL, r.err);

	/* And a discard the previous run left behind is not this run's: a poll
	 * that lands before the board has cleared that result reads the run
	 * before, and is not reported. What says the board has cleared it is an
	 * empty result, so the firmware clears the two together -- a run that
	 * ended without a reply leaves no discard record either, which
	 * test_rangetest_radio.c pins. */
	reset_fake();
	fake.range_reply_after_ms = 1000;
	fake.range_stale_reads = 3;
	fake.range_stale.valid = 1;
	fake.range_stale.discarded = 2 + 4;
	r = RUN("-n");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strstr(r.err, "warning:") == NULL, r.err);
}

MU_TEST(range_test_with_no_repeater_times_out_and_stops_the_board)
{
	struct run_result r;

	/* Nothing answers. The board would listen forever, so -n gives up at
	 * its deadline, stops the mode and says what is likely missing. */
	reset_fake();
	fake.range_reply_after_ms = 0;
	r = RUN("-n");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.err,
	                 "Starting range test\n"
	                 "error: no reply from the repeater within 10 s: start "
	                 "one in range with ubertooth-util -e, or wait longer "
	                 "with -n <seconds>; the range test has been stopped\n")
	          == 0, r.err);
	/* the STOP before the test, and the one that leaves the board idle */
	mu_assert_int_eq(2, fake.stop_calls);
	/* the deadline is a deadline, not a poll count */
	mu_assert_int_eq(1000 + RANGE_TEST_TIMEOUT_S * 1000, (int)fake.now_ms);
	/* the wait is reported as it goes, a line a second, and nothing else:
	 * there is no result to print */
	mu_assert(strstr(r.out, "waiting for the repeater... (1 s)\n") != NULL,
	          r.out);
	mu_assert(strstr(r.out, "waiting for the repeater... (9 s)\n") != NULL,
	          r.out);
	mu_assert_int_eq(RANGE_TEST_TIMEOUT_S - 1, lines_in(r.out));

	/* -n <seconds> chooses the deadline, and is named in the message. */
	reset_fake();
	fake.range_reply_after_ms = 0;
	r = RUN("-n", "3");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "within 3 s:") != NULL, r.err);
	mu_assert_int_eq(1000 + 3000, (int)fake.now_ms);

	/* -n 0 waits forever: no deadline, so a repeater that takes half a
	 * minute is still reported rather than given up on. */
	reset_fake();
	fake.range_reply_after_ms = 30000;
	r = RUN("-n0");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strstr(r.out, "request PA level : 1\n") != NULL, r.out);
	mu_assert(strstr(r.out, "waiting for the repeater... (29 s)\n") != NULL,
	          "the whole wait is reported");

	/* A read that keeps failing mid-wait ends it, naming the read and
	 * saying what became of the test. */
	reset_fake();
	fake.range_reply_after_ms = 0;
	fake.rangeresult_rc = -7;
	r = RUN("-n");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "error: range test result read failed; the range "
	                 "test has been stopped\n") != NULL, r.err);
	reset_fake();
	fake.range_reply_after_ms = 0;
	fake.watchdog_rc = -7;
	r = RUN("-n");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "error: board mode read failed; the range test "
	                 "has been stopped\n") != NULL, r.err);

	/* A board that refuses to start the test is not waited for. */
	reset_fake();
	fake.range_test_rc = -1;
	r = RUN("-n");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(0, fake.rangeresult_calls);

	/* A board part-way through a mode stalls the odd poll. Two in a row are
	 * polls that learned nothing, and the test still finishes; the third
	 * ends the wait, naming the read, and leaves the board stopped since
	 * nobody is watching the test now. */
	reset_fake();
	fake.range_reply_after_ms = 1000;
	fake.range_stall_calls = RANGE_TEST_READ_FAILURES - 1;
	r = RUN("-n");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strstr(r.out, "reply number     : 4\n") != NULL, r.out);
	mu_assert(strcmp(r.err, "Starting range test\n") == 0, r.err);

	reset_fake();
	fake.range_reply_after_ms = 1000;
	fake.range_stall_calls = RANGE_TEST_READ_FAILURES;
	r = RUN("-n");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "error: range test result read failed; the range "
	                 "test has been stopped\n") != NULL, r.err);
	mu_assert_int_eq(2, fake.stop_calls);

	/* and when the stop is refused too, it says so rather than claiming the
	 * board is idle */
	reset_fake();
	fake.range_reply_after_ms = 1000;
	fake.range_stall_calls = RANGE_TEST_READ_FAILURES;
	fake.stop_rc = -1;
	r = RUN("-n");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "error: range test result read failed; the range "
	                 "test has not been stopped; -S stops it\n") != NULL,
	          r.err);

	reset_fake();
	fake.range_reply_after_ms = 1000;
	fake.watchdog_stall_calls = RANGE_TEST_READ_FAILURES - 1;
	r = RUN("-n");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strstr(r.out, "reply number     : 4\n") != NULL, r.out);

	reset_fake();
	fake.range_reply_after_ms = 0;
	fake.watchdog_stall_calls = RANGE_TEST_READ_FAILURES;
	r = RUN("-n");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "error: board mode read failed; the range test "
	                 "has been stopped\n") != NULL, r.err);
	mu_assert_int_eq(2, fake.stop_calls);

	/* A stop the board refuses leaves the test running, and the message
	 * says so rather than claiming otherwise. */
	reset_fake();
	fake.range_reply_after_ms = 0;
	fake.stop_rc = -1;
	r = RUN("-n");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "the range test has not been stopped; -S stops "
	                 "it\n") != NULL, r.err);

	/* A board that never entered the mode is not the repeater's fault, and
	 * the deadline says so rather than naming it. */
	reset_fake();
	fake.range_reply_after_ms = 0;
	fake.watchdog.mode = MODE_IDLE;
	fake.range_leaves_after_ms = 1; /* never seen in range test mode */
	r = RUN("-n");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "error: the board did not start the range test "
	                 "within 10 s; the range test has been stopped\n")
	          != NULL, r.err);
}

MU_TEST(an_interrupted_range_test_stops_the_board)
{
	struct run_result r;

	/* Ctrl-C during the wait. The board would otherwise be left listening
	 * for a reply nobody is waiting for, until someone ran -S, so the
	 * signal ends the wait like every other ending: the mode is stopped,
	 * the line says so, and the status is 1 because no result was read. */
	reset_fake();
	fake.range_reply_after_ms = 0; /* nothing would ever answer */
	fake.stop_after_sleeps = 3;
	r = RUN("-n", "0");           /* a wait with no deadline to end it */
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(1, fake.cleanup_handlers);
	mu_assert(strstr(r.err, "error: interrupted before the range test "
	                 "finished; the range test has been stopped\n") != NULL,
	          r.err);
	/* the STOP before the test, and the one that leaves the board idle */
	mu_assert_int_eq(2, fake.stop_calls);
	/* the wait ends on the signal, not a poll later */
	mu_assert_int_eq(3, fake.sleeps);

	/* and when the board refuses that stop, the line says what is left
	 * running instead of claiming the board is idle */
	reset_fake();
	fake.range_reply_after_ms = 0;
	fake.stop_after_sleeps = 1;
	fake.stop_rc = -1;
	r = RUN("-n", "0");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "error: interrupted before the range test "
	                 "finished; the range test has not been stopped; -S "
	                 "stops it\n") != NULL, r.err);

	/* A test that finishes is not reported as interrupted, and a wait
	 * nothing interrupts still ends on its deadline. */
	reset_fake();
	fake.range_reply_after_ms = 1000;
	r = RUN("-n");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strstr(r.err, "interrupted") == NULL, r.err);
}

MU_TEST(a_range_test_stopped_from_elsewhere_ends_the_wait)
{
	struct run_result r;

	/* Something takes the board over half a second in -- a mode request
	 * from another program, not a -S from another ubertooth-util, which
	 * cannot reach a board this command holds. The board leaves the mode
	 * with no result, which -n says rather than waiting out its deadline. */
	reset_fake();
	fake.range_reply_after_ms = 0;
	fake.range_leaves_after_ms = 500;
	r = RUN("-n");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(0, (long)strlen(r.out));
	mu_assert(strcmp(r.err, "Starting range test\n"
	                 "error: the range test was stopped before the repeater "
	                 "replied\n") == 0, r.err);
	/* ended well inside the deadline, and the board was already stopped */
	mu_assert(fake.now_ms < 1000 + RANGE_TEST_TIMEOUT_S * 1000, "ended early");
	mu_assert_int_eq(1, fake.stop_calls);

	/* The board writes the result just before it leaves the mode, so a
	 * reply that lands after the result read of a poll but before its mode
	 * read is still reported, rather than read as a test that was stopped.
	 * The mode ends on the fifth poll, and the reply with it. */
	reset_fake();
	fake.range_reply_after_ms = 0;
	fake.range_leaves_after_ms = 500;
	fake.range_reply_at_call = 6;
	r = RUN("-n");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strstr(r.out, "reply number     : 4\n") != NULL, r.out);
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

	/* Two values for one setting are refused, before any board is
	 * touched: the last one used to win and the other was dropped
	 * without a word. */
	reset_fake();
	r = RUN("-c2426", "-c2440", "-c");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.out, "") == 0, r.out);
	mu_assert(strcmp(r.err, "error: option '-c' given more than once\n") == 0,
	          r.err);
	mu_assert_int_eq(0, fake.start_calls);

	reset_fake();
	r = RUN("-a3", "-a5", "-a");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.err, "error: option '-a' given more than once\n") == 0,
	          r.err);
	mu_assert_int_eq(0, fake.start_calls);

	/* The same setting written both ways: -c is MHz and -C a channel
	 * number, so two values clash even spelled differently. */
	reset_fake();
	r = RUN("-C24", "-c2440");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.err, "error: -c cannot be combined with -C; they are "
	                        "the same channel setting written two ways\n") == 0,
	          r.err);
	mu_assert_int_eq(0, fake.start_calls);

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

static const char *const mode_options[] = { "-i", "-q", "-n", "-e", "-t", "-x", "-I" };
#define MODE_COUNT (sizeof(mode_options) / sizeof(mode_options[0]))

/* The LED specan default is a number in a header, so it is spelled into the
 * expected line rather than repeated here. */
#define MODE_STR_(x) #x
#define MODE_STR(x) MODE_STR_(x)

/* Everything each mode writes to standard error, in order, for a run with no
 * -U that succeeds. Pinned by text, not just by "stdout is empty", so a line
 * cannot be dropped or reworded without a test saying so. */
static const char *const mode_progress[MODE_COUNT] = {
	/* -i */ "Entering flash programming (ISP) mode\n"
	         "note: the board hands itself to its on-chip "
	         "programmer and answers nothing else until it is power-cycled\n",
	/* -q */ "Entering LED specan mode (threshold -"
	         MODE_STR(UBERTOOTH_LED_SPECAN_N_DEFAULT) " dBm)\n",
	/* -n */ "Starting range test\n",
	/* -e */ "Starting repeater\n",
	/* -t */ "Starting TX test\n",
	/* -x */ "",
	/* -I */ "note: the board is blinking; stop it with ubertooth-util -S "
	         "or any mode option\n",
};

/* What each mode writes to standard output. Only -n reports anything: it
 * follows the test it starts and prints its result, so the result block is
 * what the command was for. The others leave the board running and have
 * nothing to report. */
static const char *const mode_stdout[MODE_COUNT] = {
	/* -i */ "",
	/* -q */ "",
	/* -n */ "request PA level : 1\n"
	         "request number   : 2\n"
	         "reply PA level   : 3\n"
	         "reply number     : 4\n",
	/* -e */ "",
	/* -t */ "",
	/* -x */ "",
	/* -I */ "",
};

/* The scripted result and call counter for the i-th mode command. */
static int *mode_result(size_t i)
{
	int *results[] = {
		&fake.isp_rc, &fake.led_specan_rc, &fake.range_test_rc,
		&fake.repeater_rc, &fake.tx_test_rc, &fake.xmas_rc, &fake.identify_rc,
	};
	return results[i];
}

static int mode_calls(size_t i)
{
	int calls[] = {
		fake.isp_calls, fake.led_specan_calls,
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
		/* -i starts no mode, so it does not stop the device first */
		int stops_first = (i >= 1);

		reset_fake();
		r = run_util((const char *const[]){ "ubertooth-util", mode_options[i], NULL });
		mu_assert_int_eq(0, r.rc);
		mu_assert_int_eq(1, mode_calls(i));
		mu_assert_int_eq(stops_first, fake.stop_calls);
		/* starting a mode reports nothing: its progress line is on stderr */
		mu_assert(strcmp(r.out, mode_stdout[i]) == 0, r.out);
		/* and that line is pinned, so dropping one would be noticed. -x has
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

	for (i = 1; i < MODE_COUNT; i++) { /* the modes that stop the device first */
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
 *
 * Whether a mismatch is refused or only warned about depends on the build, so
 * main() pins it to refusing for every case here; the one case that wants the
 * other answer asks for it and puts this one back.
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
#ifdef UTIL_TEST_REAL_API_MESSAGE
		mu_assert(strstr(r.err, "error: Ubertooth firmware API version") != NULL,
		          "says why, in the library's words");
#endif
	}

	reset_fake();
	fake.api_rc = -1;
	r = RUN("-I");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(0, fake.identify_calls);
	mu_assert_int_eq(0, fake.usrled_writes);
}

#ifdef UTIL_TEST_REAL_API_MESSAGE
/*
 * -I prints the library's mismatch message, from the library's own code: both
 * versions, and the side to replace. Older firmware is the board's to fix,
 * newer firmware means the host tools are the old ones.
 */
MU_TEST(identify_prints_the_librarys_mismatch_message)
{
	/* The whole first line, built here the way the library builds it, so a
	 * reworded message fails rather than matching a shorter prefix. */
	char expected[256];
	struct run_result r;
	int written;

	written = snprintf(expected, sizeof(expected),
	         "error: Ubertooth firmware API version %x.%02x found, libubertooth %s requires %x.%02x.\n",
	         ((UBERTOOTH_API_VERSION - 1) >> 8) & 0xFF, (UBERTOOTH_API_VERSION - 1) & 0xFF,
	         VERSION,
	         (UBERTOOTH_API_VERSION >> 8) & 0xFF, UBERTOOTH_API_VERSION & 0xFF);
	mu_assert(written > 0 && (size_t)written < sizeof(expected),
	          "the expected line fits");

	reset_fake();
	fake.api_version = UBERTOOTH_API_VERSION - 1;
	r = RUN("-I");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, expected) != NULL, "names both versions");
	mu_assert(strstr(r.err, "note: flash the firmware built from the same "
	                        "source tree") != NULL,
	          "an older board is reflashed");
	mu_assert(strstr(r.err, "install the host tools") == NULL,
	          "and not told to replace the tools");
	/* The option it was given is not what needs a version. */
	mu_assert(strstr(r.err, "-I needs firmware") == NULL,
	          "does not blame -I");
	mu_assert_int_eq(0, fake.identify_calls); /* said, not done */

	written = snprintf(expected, sizeof(expected),
	         "error: Ubertooth firmware API version %x.%02x found, libubertooth %s requires %x.%02x.\n",
	         ((UBERTOOTH_API_VERSION + 1) >> 8) & 0xFF, (UBERTOOTH_API_VERSION + 1) & 0xFF,
	         VERSION,
	         (UBERTOOTH_API_VERSION >> 8) & 0xFF, UBERTOOTH_API_VERSION & 0xFF);
	mu_assert(written > 0 && (size_t)written < sizeof(expected),
	          "the expected line fits");

	reset_fake();
	fake.api_version = UBERTOOTH_API_VERSION + 1;
	r = RUN("-I");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, expected) != NULL, "names both versions");
	mu_assert(strstr(r.err, "note: install the host tools built from the "
	                        "same source tree as the firmware") != NULL,
	          "a newer board needs newer tools");
	mu_assert(strstr(r.err, "flash the firmware") == NULL,
	          "and no backwards flash");
	mu_assert_int_eq(0, fake.identify_calls);
}

/*
 * A mismatch that is allowed -- a development build, or the override set --
 * warns instead of refusing, and the mode runs: this is what makes host tools
 * and firmware from different commits usable together. The warning says what
 * allowed it, so a capture made this way is not read as trustworthy by
 * accident.
 */
MU_TEST(identify_runs_on_when_a_mismatch_is_allowed)
{
	struct run_result r;

	setenv(UBERTOOTH_ALLOW_API_MISMATCH_VAR, "1", 1);
	reset_fake();
	fake.api_version = UBERTOOTH_API_VERSION - 1;
	r = RUN("-I");
	setenv(UBERTOOTH_ALLOW_API_MISMATCH_VAR, "0", 1);

	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(1, fake.identify_calls); /* the mode did run */
	mu_assert(strstr(r.err, "warning: Ubertooth firmware API version")
	          != NULL, r.err);
	mu_assert(strstr(r.err, "error:") == NULL, "nothing is refused");
	mu_assert(strstr(r.err, "so the command runs on") != NULL, r.err);
}

/*
 * The example command in that note reflashes the board the message is about.
 * A user who named one board with -U and found only that one wrong was told
 * "--flash-device all", which reflashes every board attached.
 */
MU_TEST(the_mismatch_note_names_the_board_it_is_about)
{
	const char *serial = "1cc001038088cb956c43ad61c52000f5";
	struct run_result r;

	reset_fake();
	fake.api_version = UBERTOOTH_API_VERSION - 1;
	r = RUN("-I", "-U", serial);
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "./install.sh --flash-device "
	                        "1cc001038088cb956c43ad61c52000f5\n") != NULL,
	          r.err);
	mu_assert(strstr(r.err, "--flash-device all") == NULL,
	          "and reflashes no other board");

	/* An index is named the same way: install.sh takes one after
	 * --flash-device just as the tool takes one after -U. */
	reset_fake();
	fake.api_version = UBERTOOTH_API_VERSION - 1;
	r = RUN("-I", "-U", "1");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "./install.sh --flash-device 1\n") != NULL, r.err);

	/* No board named, so there is none to name: --flash takes the one that
	 * is attached. */
	reset_fake();
	fake.api_version = UBERTOOTH_API_VERSION - 1;
	r = RUN("-I");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "./install.sh --flash\n") != NULL, r.err);
	mu_assert(strstr(r.err, "--flash-device") == NULL,
	          "and asks for no board in particular");
}
#endif

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

	/* -h and -V are checked too, not exempted by returning early */
	reset_fake();
	r = RUN("-V", "stray");
	assert_refused_untouched(r, "error: unexpected argument 'stray'\n");
	reset_fake();
	r = RUN("-h", "stray");
	assert_refused_untouched(r, "error: unexpected argument 'stray'\n");
	mu_assert(strstr(r.out, "command line utility") == NULL, "no usage printed");

	/* -h still wins over other options, and -V ends the command too */
	reset_fake();
	r = RUN("-x", "-h");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strstr(r.out, "command line utility") != NULL, "usage");
	mu_assert_int_eq(0, fake.start_calls);
	mu_assert_int_eq(0, fake.xmas_calls);
	reset_fake();
	r = RUN("-b", "-V");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, VERSION_ANSWER) == 0, "the version answer only");
	mu_assert(strstr(r.out, BOARD_LINE) == NULL, "and nothing -b asked for");

	/* -h beside -V prints both, the version first and the usage text last,
	 * whichever order they were written in. The first one used to win and
	 * the other was dropped. */
	reset_fake();
	r = RUN("-V", "-h");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strncmp(r.out, VERSION_ANSWER, strlen(VERSION_ANSWER)) == 0,
	          "-V -h: the version first");
	mu_assert(strstr(r.out, "command line utility") != NULL,
	          "-V -h: the usage text after it");
	reset_fake();
	r = RUN("-h", "-V");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strncmp(r.out, VERSION_ANSWER, strlen(VERSION_ANSWER)) == 0,
	          "-h -V: the version first here too");
	mu_assert(strstr(r.out, "command line utility") != NULL,
	          "-h -V: the usage text after it");

	/* after "--", a long option is an operand, reported as typed */
	reset_fake();
	r = RUN("--", "--version");
	assert_refused_untouched(r, "error: unexpected argument '--version'\n");

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
		"-q", "-n", "-e", "-t", "-x", "-I", "-i",
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
	mu_assert(strcmp(r.out, "Board ID Number (0): read failed\n" LISTED_BOARD("1")) == 0,
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
		"-q", "-n", "-e", "-t", "-x", "-I", "-i",
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
	mu_assert(strstr(r.err, "error: unrecognized option '-Y'") != NULL,
	          "bad option named on stderr");
	/* The line above says what was wrong, so no usage text follows it. */
	mu_assert(strstr(r.err, "ubertooth-util - command line utility") == NULL,
	          "no usage text after an error");
	mu_assert_int_eq(0, fake.start_calls);
}

MU_TEST(help_version_and_count_exit_zero_without_a_device)
{
	struct run_result r;

	reset_fake();
	r = RUN("-h");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strstr(r.out, "ubertooth-util - command line utility") != NULL, "usage on stdout");

	r = RUN("-V");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, VERSION_ANSWER) == 0, "the version answer");

	reset_fake();
	fake.device_count = 3;
	r = RUN("-N");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, "3\n") == 0, "device count");
	mu_assert_int_eq(0, fake.start_calls);

	/* the same option twice is still one count */
	reset_fake();
	fake.device_count = 3;
	r = RUN("-N", "-N");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, "3\n") == 0, "device count, once");
	mu_assert_int_eq(0, fake.start_calls);

	/* -h and -V end the command before the count */
	reset_fake();
	fake.device_count = 3;
	r = RUN("-N", "-h");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strstr(r.out, "command line utility") != NULL, "usage on stdout");
	mu_assert(strstr(r.out, "3\n") == NULL, "no count");
	mu_assert_int_eq(0, fake.count_calls);

	/* -V counts the boards itself, to report each one, so it prints its own
	 * answer and no count line. */
	reset_fake();
	fake.device_count = 1;
	r = RUN("-N", "-V");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, VERSION_ANSWER) == 0, r.out);
}

/* ---- -N combines with the rest of the command line -------------------- */

/* One board attached, so the count is "1". */
MU_TEST(number_prints_the_count_first_and_the_rest_still_runs)
{
	struct run_result r;

	/* a read */
	reset_fake();
	r = RUN("-N", "-b");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, "1\n" BOARD_LINE) == 0, r.out);
	mu_assert_int_eq(1, fake.start_calls);

	reset_fake();
	r = RUN("-b", "-N");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, "1\n" BOARD_LINE) == 0, r.out);

	/* several reads in one command: the count, then every value line */
	reset_fake();
	r = RUN("-N", "-v", "-b", "-s");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, "1\n" BOARD_LINE FIRMWARE_LINE SERIAL_LINE) == 0, r.out);
	mu_assert(strcmp(r.err, "") == 0, "nothing on stderr");

	reset_fake();
	r = RUN("-v", "-b", "-s", "-N");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, "1\n" BOARD_LINE FIRMWARE_LINE SERIAL_LINE) == 0, r.out);

	/* a setter, which prints nothing of its own */
	reset_fake();
	r = RUN("-N", "-c2450");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, "1\n") == 0, r.out);
	mu_assert_int_eq(2450, fake.last_channel_set);

	reset_fake();
	r = RUN("-c2450", "-N");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, "1\n") == 0, r.out);
	mu_assert_int_eq(2450, fake.last_channel_set);

	/* a mode */
	reset_fake();
	r = RUN("-N", "-x");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, "1\n") == 0, r.out);
	mu_assert_int_eq(1, fake.xmas_calls);

	reset_fake();
	r = RUN("-x", "-N");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, "1\n") == 0, r.out);
	mu_assert_int_eq(1, fake.xmas_calls);

	/* -i, the remaining programming mode */
	reset_fake();
	r = RUN("-N", "-i");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, "1\n") == 0, r.out);
	mu_assert_int_eq(1, fake.isp_calls);

	/* -r, which counts the devices itself to follow the board back */
	reset_fake();
	r = RUN("-N", "-r");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, "1\n") == 0, r.out);
	mu_assert_int_eq(1, fake.reset_calls);

	reset_fake();
	r = RUN("-r", "-N");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, "1\n") == 0, r.out);
	mu_assert_int_eq(1, fake.reset_calls);

	/* -S */
	reset_fake();
	r = RUN("-N", "-S");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, "1\n") == 0, r.out);
	mu_assert_int_eq(1, fake.stop_calls);

	reset_fake();
	r = RUN("-S", "-N");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, "1\n") == 0, r.out);
	mu_assert_int_eq(1, fake.stop_calls);

	/* a selector, which the count ignores: it counts every attached board */
	reset_fake();
	fake.device_count = 2;
	r = RUN("-U", "1", "-N", "-b");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, "2\n" BOARD_LINE) == 0, r.out);
	mu_assert(strcmp(fake.selector, "1") == 0, "-U still applies");

	reset_fake();
	fake.device_count = 2;
	r = RUN("-N", "-U", "1", "-b");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, "2\n" BOARD_LINE) == 0, r.out);
	mu_assert(strcmp(fake.selector, "1") == 0, "-U still applies");
}

/* The listings are per-device and -N is not, so they run alongside it. */
MU_TEST(number_combines_with_the_multi_device_listings)
{
	struct run_result r;

	/* -U all: the count, then the listing */
	reset_fake();
	fake.device_count = 2;
	r = RUN("-U", "all", "-N", "-s");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, "2\n" LISTED_SERIAL("0") LISTED_SERIAL("1")) == 0, r.out);
	mu_assert_int_eq(2, fake.indexed_starts);
	mu_assert_int_eq(0, fake.start_calls); /* no single-device selection */

	reset_fake();
	fake.device_count = 2;
	r = RUN("-N", "-s", "-U", "all");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, "2\n" LISTED_SERIAL("0") LISTED_SERIAL("1")) == 0, r.out);

	/* and the same listing without -U, which several boards and a read-only
	 * query reach on their own */
	reset_fake();
	fake.device_count = 2;
	r = RUN("-N", "-s");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, "2\n" LISTED_SERIAL("0") LISTED_SERIAL("1")) == 0, r.out);
	mu_assert_int_eq(0, fake.start_calls);

	/* a device the listing cannot open still fails the command, with the
	 * count already printed */
	reset_fake();
	fake.device_count = 2;
	fake.unavailable_index = 1;
	r = RUN("-N", "-U", "all", "-b");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.out, "2\n" LISTED_BOARD("0")
	          "Board ID Number (1): unavailable, it could not be opened\n") == 0, r.out);

	/* no board attached: the count is 0 on standard output, then -U all
	 * says what it could not list, and the command fails */
	reset_fake();
	fake.device_count = 0;
	r = RUN("-N", "-U", "all", "-s");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.out, "0\n") == 0, r.out);
	mu_assert(strstr(r.err, "error: no Ubertooth devices found") != NULL, r.err);
	mu_assert_int_eq(0, fake.indexed_starts);

	/* one enumeration serves both the count and the listing */
	reset_fake();
	fake.device_count = 2;
	r = RUN("-N", "-U", "all", "-s");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(1, fake.count_calls);
}

/* The exit status is the rest of the command's: -N never makes a failing
 * command succeed, and a count that fails never costs the rest its run. */
MU_TEST(number_leaves_the_exit_status_to_the_rest_of_the_command)
{
	struct run_result r;

	/* a failed read still fails the command, after the count */
	reset_fake();
	fake.channel = -7;
	r = RUN("-N", "-c");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.out, "1\n") == 0, r.out);
	mu_assert(strstr(r.err, "error: channel read failed") != NULL, r.err);

	/* so does a failed mode */
	reset_fake();
	fake.xmas_rc = -1;
	r = RUN("-N", "-x");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.out, "1\n") == 0, r.out);

	/* and a board that cannot be selected */
	reset_fake();
	fake.fail_start_from = 1;
	r = RUN("-N", "-b");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.out, "1\n") == 0, r.out);

	/* a count that fails prints no number, fails the command, and leaves
	 * the rest of it to run */
	reset_fake();
	fake.device_count = -1;
	r = RUN("-N", "-b");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.out, BOARD_LINE) == 0, r.out);
	mu_assert_int_eq(1, fake.start_calls);
}

/* ubertooth-dfu reflashes a board; -f is not an option this tool has. */
MU_TEST(the_flash_option_is_gone)
{
	struct run_result r;

	reset_fake();
	r = RUN("-f");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "error: unrecognized option '-f'") != NULL, r.err);
	mu_assert(strstr(r.err, "ubertooth-util - command line utility") == NULL,
	          "no usage text follows it");
	mu_assert_int_eq(0, (long)strlen(r.out));
	mu_assert_int_eq(0, fake.start_calls);
	mu_assert_int_eq(0, fake.count_calls);
	mu_assert(strstr(r.err, "ubertooth-dfu") == NULL, "no deprecation warning");
	mu_assert(strstr(r.err, "(DFU) mode") == NULL, "no DFU progress line");

	/* nor is it offered by the usage text */
	reset_fake();
	r = RUN("-h");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strstr(r.out, "\t-f ") == NULL, "no -f line in the usage text");
	mu_assert(strstr(r.out, "(DFU)") == NULL, "no DFU mode in the usage text");
}

/*
 * A command line that asks for nothing is a mistake, not a no-op: it used to
 * open the selected board, do nothing with it and exit 0.
 */
MU_TEST(a_command_line_with_nothing_to_do_is_refused)
{
	struct run_result r;

	/* no options at all */
	reset_fake();
	r = run_util((const char *const[]){ "ubertooth-util", NULL });
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(0, (long)strlen(r.out));
	mu_assert(strstr(r.err, "error: nothing to do: give an option that reads "
	          "or sets something, or -N to count the attached boards\n") == r.err,
	          r.err);
	/* the usage text follows it, on standard error */
	mu_assert(strstr(r.err, "ubertooth-util - command line utility") != NULL,
	          r.err);
	/* and nothing was opened, counted or sent */
	mu_assert_int_eq(0, fake.start_calls);
	mu_assert_int_eq(0, fake.indexed_starts);
	mu_assert_int_eq(0, fake.count_calls);
	mu_assert_int_eq(0, fake.stop_calls);

	/* a selector on its own says which board and asks nothing of it; the
	 * value is quoted as the user wrote it */
	reset_fake();
	r = RUN("-U", "0");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(0, (long)strlen(r.out));
	mu_assert(strstr(r.err, "error: nothing to do: -U '0' selects a device "
	          "but no option asks anything of it\n") == r.err, r.err);
	/* The command line said something, so that line is the whole answer. */
	mu_assert(strstr(r.err, "ubertooth-util - command line utility") == NULL,
	          r.err);
	mu_assert_int_eq(0, fake.start_calls);

	reset_fake();
	fake.device_count = 2;
	r = RUN("-U", "a9aa");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "error: nothing to do: -U 'a9aa' selects a device")
	          == r.err, r.err);
	mu_assert_int_eq(0, fake.start_calls);

	/* An empty selector is quoted too, so the line does not read as though
	 * -U had been given no value at all. */
	reset_fake();
	r = RUN("-U", "");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "error: nothing to do: -U '' selects a device")
	          == r.err, r.err);
	mu_assert_int_eq(0, fake.start_calls);

	/* -N counts the boards on the bus and asks nothing of any one of them,
	 * so a selector with only -N beside it selects a device that nothing
	 * acts on: refused in the same words -U alone gets, and no count is
	 * printed. */
	reset_fake();
	fake.device_count = 2;
	r = RUN("-U", "0", "-N");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.err, "error: nothing to do: -U '0' selects a device "
	                 "but no option asks anything of it\n") == 0, r.err);
	mu_assert_int_eq(0, (long)strlen(r.out));
	mu_assert_int_eq(0, fake.count_calls);
	mu_assert_int_eq(0, fake.start_calls);
	mu_assert_int_eq(0, fake.indexed_starts);

	/* The order the two were written in makes no difference. */
	reset_fake();
	fake.device_count = 2;
	r = RUN("-N", "-U", "0");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.err, "error: nothing to do: -U '0' selects a device "
	                 "but no option asks anything of it\n") == 0, r.err);
	mu_assert_int_eq(0, (long)strlen(r.out));
	mu_assert_int_eq(0, fake.start_calls);

	/* A selector that asks something of the board it names still runs, with
	 * the count first. */
	reset_fake();
	fake.device_count = 2;
	r = RUN("-U", "0", "-N", "-p");
	mu_assert(strncmp(r.out, "2\n", 2) == 0, r.out);
	mu_assert_int_eq(1, fake.start_calls);
	mu_assert(strcmp(fake.selector, "0") == 0, fake.selector);

	/* so are -h and -V, alongside a selector or not */
	reset_fake();
	r = RUN("-U", "0", "-h");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strstr(r.out, "ubertooth-util - command line utility") != NULL,
	          r.out);
	mu_assert_int_eq(0, (long)strlen(r.err));

	reset_fake();
	r = RUN("-U", "0", "-V");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, VERSION_ANSWER) == 0, r.out);

	/* -U all with nothing to list keeps its own, more specific refusal */
	reset_fake();
	fake.device_count = 2;
	r = RUN("-U", "all");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "-U all needs one of the read-only queries") != NULL,
	          r.err);
	mu_assert(strstr(r.err, "nothing to do") == NULL, r.err);
}

MU_TEST(firmware_listing_with_several_devices_reports_each_device)
{
	struct run_result r;

	reset_fake();
	fake.device_count = 2;
	r = RUN("-v");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, LISTED_FIRMWARE("0") LISTED_FIRMWARE("1")) == 0,
	          "per-device firmware lines");
}

MU_TEST(serial_listing_with_several_devices_exits_zero)
{
	struct run_result r;

	reset_fake();
	fake.device_count = 2;
	r = RUN("-s");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strstr(r.out, LISTED_SERIAL("0")) != NULL, "first serial");
	mu_assert(strstr(r.out, LISTED_SERIAL("1")) != NULL, "second serial");
}

#define SERIAL_VALUE "a1a2a3a4a5a6a7a8a9aaabacadaeafb0"

MU_TEST(each_identity_query_alone_lists_every_device_compactly)
{
	struct { const char *option; const char *expected; } cases[] = {
		{ "-b", LISTED_BOARD("0") LISTED_BOARD("1") },
		{ "-v", LISTED_FIRMWARE("0") LISTED_FIRMWARE("1") },
		{ "-p", LISTED_PART("0") LISTED_PART("1") },
		{ "-s", LISTED_SERIAL("0") LISTED_SERIAL("1") },
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
	r = RUN("-vspb");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out,
	                 LISTED_BOARD("0") LISTED_FIRMWARE("0")
	                 LISTED_PART("0") LISTED_SERIAL("0")
	                 LISTED_BOARD("1") LISTED_FIRMWARE("1")
	                 LISTED_PART("1") LISTED_SERIAL("1")) == 0,
	          "per-device report lines in report order");
}

MU_TEST(an_unavailable_device_is_reported_and_fails_the_listing)
{
	struct run_result r;

	reset_fake();
	fake.device_count = 3;
	fake.unavailable_index = 1;
	fake.open_failure_reason = "in use by another program";
	r = RUN("-b");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.out,
	                 LISTED_BOARD("0")
	                 "Board ID Number (1): unavailable, in use by another program\n"
	                 LISTED_BOARD("2")) == 0,
	          "other devices still listed");
	mu_assert_int_eq(2, fake.closes);
	mu_assert_int_eq(0, fake.stop_calls);
}

/* One board or several, the firmware version is labelled the same way, and a
 * board that cannot be read says so where its value would be. */
MU_TEST(the_firmware_version_is_labelled_whether_one_board_or_several)
{
	struct run_result r;

	/* one board: the label, no index -- there is nothing to tell apart */
	reset_fake();
	fake.device_count = 1;
	r = RUN("-v");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, FIRMWARE_LINE) == 0, r.out);

	/* the same, with the board named by -U */
	reset_fake();
	fake.device_count = 2;
	r = RUN("-U", FAKE_SERIAL, "-v");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, FIRMWARE_LINE) == 0, r.out);

	/* several boards, one of them held by another program: the same label
	 * with the index in it, and the reason where the value would be */
	reset_fake();
	fake.device_count = 2;
	fake.unavailable_index = 1;
	fake.open_failure_reason = "in use by another program";
	r = RUN("-v");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.out,
	                 LISTED_FIRMWARE("0")
	                 "Firmware version (1): unavailable, in use by another program\n")
	          == 0, r.out);

	/* Every requested query gets its own line for that board, so a reader
	 * filtering for one of them still sees it. */
	reset_fake();
	fake.device_count = 2;
	fake.unavailable_index = 0;
	fake.open_failure_reason = "in use by another program";
	r = RUN("-vs");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.out,
	                 "Firmware version (0): unavailable, in use by another program\n"
	                 "Serial No (0): unavailable, in use by another program\n"
	                 LISTED_FIRMWARE("1") LISTED_SERIAL("1")) == 0, r.out);
}

MU_TEST(a_failed_read_on_one_device_is_reported_and_fails_the_listing)
{
	struct run_result r;

	reset_fake();
	fake.device_count = 2;
	fake.board_fail_index = 0;
	r = RUN("-b");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.out, "Board ID Number (0): read failed\n" LISTED_BOARD("1")) == 0,
	          "failed read line");
	/* The report's stderr line is captured into the listing, not repeated. */
	mu_assert_int_eq(0, (long)strlen(r.err));

	reset_fake();
	fake.device_count = 2;
	fake.board_fail_index = 1;
	r = RUN("-b", "-s");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.out, "Board ID Number (1): read failed\n") != NULL,
	          "failed read line");
	mu_assert(strstr(r.out, LISTED_SERIAL("1")) != NULL, "later query still reported");

	reset_fake();
	fake.device_count = 2;
	fake.serial_rc = 1;
	r = RUN("-s");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.out, "Serial No (0): read failed\nSerial No (1): read failed\n") == 0,
	          "serial read failure named by its label");
	mu_assert_int_eq(0, (long)strlen(r.err));

	reset_fake();
	fake.device_count = 2;
	fake.partnum_rc = -1;
	r = RUN("-p");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.out, "Part ID (0): read failed\nPart ID (1): read failed\n") == 0,
	          "part ID read failure shown per device");
	mu_assert_int_eq(0, (long)strlen(r.err));

	reset_fake();
	fake.device_count = 2;
	fake.board_fail_index = 0;
	fake.partnum_rc = -1;
	r = RUN("-b", "-p", "-s");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.out,
	                 "Board ID Number (0): read failed\nPart ID (0): read failed\n"
	                 LISTED_SERIAL("0")
	                 LISTED_BOARD("1") "Part ID (1): read failed\n"
	                 LISTED_SERIAL("1")) == 0,
	          "combined listing with failed reads");
	mu_assert_int_eq(0, (long)strlen(r.err));

	reset_fake();
	fake.device_count = 2;
	fake.board_id = 7; /* read succeeds but the ID is not a known board */
	r = RUN("-b");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.out,
	                 "Board ID Number (0): unknown board ID 7\n"
	                 "Board ID Number (1): unknown board ID 7\n") == 0,
	          "unknown board ID shown per device");
	mu_assert_int_eq(0, (long)strlen(r.err));

	reset_fake();
	fake.device_count = 2;
	fake.api_rc = -1;
	r = RUN("-v");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.out,
	                 "Firmware version (0): bluetooth_rxtx git-test\n"
	                 "Firmware version (1): bluetooth_rxtx git-test\n") == 0,
	          "revision without API");

	reset_fake();
	fake.device_count = 2;
	fake.revision_fail_index = 0;
	r = RUN("-v");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.out,
	                 "Firmware version (0): read failed\n"
	                 LISTED_FIRMWARE("1")) == 0,
	          "failed revision read shown per device");

	reset_fake();
	fake.device_count = 2;
	fake.revision_fail_index = 1;
	r = RUN("-U", "all", "-v");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.out,
	                 LISTED_FIRMWARE("0")
	                 "Firmware version (1): read failed\n") == 0,
	          "-U all shows the failed read and keeps the good device");
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
	mu_assert(strcmp(r.out, LISTED_SERIAL("0")) == 0, "single device listed");
	mu_assert_int_eq(1, fake.indexed_starts);
	mu_assert_int_eq(1, fake.closes);
	mu_assert_int_eq(0, fake.start_calls); /* no single-device selection */
	mu_assert_int_eq(0, fake.stop_calls);

	reset_fake();
	fake.device_count = 2;
	r = RUN("-Uall", "-vspb");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out,
	                 LISTED_BOARD("0") LISTED_FIRMWARE("0")
	                 LISTED_PART("0") LISTED_SERIAL("0")
	                 LISTED_BOARD("1") LISTED_FIRMWARE("1")
	                 LISTED_PART("1") LISTED_SERIAL("1")) == 0,
	          "same listing as the implicit multi-device form");
	mu_assert_int_eq(0, fake.start_calls);
	mu_assert_int_eq(0, fake.stop_calls);

	/* the option may follow the queries */
	reset_fake();
	fake.device_count = 2;
	r = RUN("-b", "-U", "all");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, LISTED_BOARD("0") LISTED_BOARD("1")) == 0,
	          "listing with -U all last");

	/* a failure on one device still fails the command */
	reset_fake();
	fake.device_count = 2;
	fake.unavailable_index = 0;
	r = RUN("-U", "all", "-b");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.out,
	                 "Board ID Number (0): unavailable, it could not be opened\n"
	                 LISTED_BOARD("1")) == 0,
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
		"-z", "-z5", "-m", "-S", "-r", "-I",
		"-i", "-q", "-n", "-e", "-t", "-x",
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
		mu_assert_int_eq(0, fake.count_calls);
		mu_assert_int_eq(0, fake.start_calls);
		mu_assert_int_eq(0, fake.indexed_starts);
		mu_assert_int_eq(0, fake.stop_calls);
		mu_assert_int_eq(0, fake.reset_calls);
		mu_assert_int_eq(0, fake.isp_calls + fake.led_specan_calls +
		                    fake.range_test_calls + fake.repeater_calls +
		                    fake.tx_test_calls + fake.xmas_calls + fake.identify_calls);
	}

	/* -U all with no query at all is refused too, but for the other reason:
	 * the message names the queries the listing needs, not the options the
	 * user did not give */
	{
		struct run_result r;

		reset_fake();
		fake.device_count = 2;
		r = RUN("-U", "all");
		mu_assert_int_eq(1, r.rc);
		mu_assert(strcmp(r.err, "error: -U all needs one of the read-only "
		                 "queries -b, -v, -p or -s to list\n") == 0, r.err);
		mu_assert_int_eq(0, (long)strlen(r.out));
		mu_assert_int_eq(0, fake.count_calls);
		mu_assert_int_eq(0, fake.start_calls);
		mu_assert_int_eq(0, fake.indexed_starts);
	}
}

MU_TEST(u_all_with_only_the_count_beside_it_has_nothing_to_list)
{
	struct run_result r;

	/* A count is not one of the queries the listing can show, so -U all
	 * with nothing but -N beside it has nothing to list and is refused in
	 * the same words as -U all on its own. */
	reset_fake();
	fake.device_count = 2;
	r = RUN("-U", "all", "-N");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.err, "error: -U all needs one of the read-only "
	                 "queries -b, -v, -p or -s to list\n") == 0, r.err);
	mu_assert_int_eq(0, (long)strlen(r.out));
	mu_assert_int_eq(0, fake.count_calls);
	mu_assert_int_eq(0, fake.start_calls);
	mu_assert_int_eq(0, fake.indexed_starts);

	/* An option -U all does not support is still refused, count and all. */
	reset_fake();
	fake.device_count = 2;
	r = RUN("-U", "all", "-N", "-S");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "-U all only supports the read-only queries") != NULL,
	          r.err);
	mu_assert_int_eq(0, (long)strlen(r.out));
	mu_assert_int_eq(0, fake.count_calls);
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

	/* Two selectors name two different boards for one command, so the
	 * command line is refused: the last -U used to win silently. */
	reset_fake();
	fake.device_count = 2;
	r = RUN("-U", "all", "-U", "1", "-c");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.err, "error: option '-U' given more than once\n") == 0,
	          r.err);
	mu_assert_int_eq(0, fake.start_calls);
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
			/* only the STOP that leaves a prior mode (-i sends none);
			 * releasing the handle sends no STOP of its own */
			mu_assert(fake.stop_calls == (i >= 1), mode_options[i]);
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
	/* read before the reset was sent, while the device was still there,
	 * through the library's quiet read and not cmd_get_serial(). Twice: the
	 * first read is how -r tells a board that answers from one that needs
	 * its USB connection reset, the second is the follow's. */
	mu_assert_int_eq(2, fake.follow_serial_reads);
	mu_assert_int_eq(0, fake.resets_at_first_serial_read);
	mu_assert_int_eq(0, fake.serial_reads);
	mu_assert_int_eq((int)UBERTOOTH_SERIAL_READ_QUIET_MS,
	                 fake.follow_serial_timeout_ms);
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
	 * is told later options may not reach the same board. The serial is
	 * unreadable here because the USB reset took the handle with it, which
	 * is the one way -r reaches the wait with nothing to read. */
	reset_fake();
	fake.device_count = 2;
	fake.follow_serial_rc = -1;
	fake.usb_reset_rc = LIBUSB_ERROR_NOT_FOUND;
	fake.returns_at_ms = 1000 + 3000;
	r = RUN("-U", "1", "-r", "-b");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(fake.first_quiet_selector, "1") == 0,
	          "the index is kept for the wait that has no serial to read");
	/* and once the board is back its serial is readable, so the wait after
	 * the reset request follows that instead */
	mu_assert(strcmp(fake.quiet_selector, FAKE_SERIAL) == 0,
	          "the board that came back is followed by its serial");
	mu_assert(strcmp(r.err, "Ubertooth device 1 is not answering; resetting "
	                 "its USB connection\n"
	                 "warning: serial read failed on Ubertooth device 1 "
	                 "before the reset; later options may reach a different "
	                 "board if the enumeration order changes\n"
	                 "Resetting ubertooth device 1\n") == 0, r.err);
	mu_assert(strstr(r.out, BOARD_LINE) != NULL, "board read after the reset");

	/* A serial selector already follows the board, so nothing is lost and
	 * nothing is warned about. */
	reset_fake();
	fake.device_count = 2;
	fake.follow_serial_rc = -1;
	fake.usb_reset_rc = LIBUSB_ERROR_NOT_FOUND;
	r = RUN("-U", "a9aaabacadaeafb0", "-r");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(fake.first_quiet_selector, "a9aaabacadaeafb0") == 0,
	          "the serial selector is kept while there is nothing to read");
	/* the board that came back is followed by its whole serial, which the
	 * prefix the user gave selects too */
	mu_assert(strcmp(fake.quiet_selector, FAKE_SERIAL) == 0,
	          "and by its whole serial once it can be read");
	mu_assert(strcmp(r.err, "Ubertooth device a9aaabacadaeafb0 is not "
	                 "answering; resetting its USB connection\n"
	                 "Resetting ubertooth device a9aaabacadaeafb0\n") == 0,
	          "no warning for a serial selector");
}

/*
 * A board that answers nothing refuses the reset request as it refuses every
 * other request, so -r resets its USB connection first: the re-plug done in
 * software, which is what recovers such a board. The request follows on the
 * board that answers again, so -r still leaves it idle.
 */
MU_TEST(a_board_that_answers_nothing_has_its_usb_connection_reset)
{
	struct run_result r;

	reset_fake();
	fake.follow_serial_rc = -1;
	r = RUN("-U", "1", "-r", "-b");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(1, fake.usb_resets);
	mu_assert(fake.usb_reset_handle == FIRST_DEVH,
	          "the open board's handle is the one reset");
	mu_assert_int_eq(1, fake.reset_calls); /* the full reset still happens */
	/* Nothing is sent to the board before the USB reset, and the board that
	 * answers again is followed by its serial, as any other is. */
	mu_assert_int_eq(0, fake.resets_at_first_serial_read);
	mu_assert(strcmp(fake.quiet_selector, FAKE_SERIAL) == 0,
	          "the recovered board is followed by its serial");
	mu_assert(strcmp(r.err, "Ubertooth device 1 is not answering; resetting "
	                 "its USB connection\n"
	                 "Resetting ubertooth device 1\n") == 0, r.err);
	mu_assert(strstr(r.out, BOARD_LINE) != NULL, "board read after the reset");
	mu_assert_int_eq(0, fake.stop_calls);
}

/* A board that takes a few polls to answer after the reset is still
 * recovered, and nothing is sent to it before it answers. */
MU_TEST(a_board_that_answers_a_few_polls_after_the_usb_reset_is_recovered)
{
	struct run_result r;

	reset_fake();
	fake.follow_serial_rc = -1;
	fake.usb_reset_answers_after_polls = 3;
	r = RUN("-U", "1", "-r", "-b");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(1, fake.usb_resets);
	mu_assert_int_eq(1, fake.reset_calls);
	mu_assert(strcmp(r.err, "Ubertooth device 1 is not answering; resetting "
	                 "its USB connection\n"
	                 "Resetting ubertooth device 1\n") == 0, r.err);
	mu_assert(strstr(r.out, BOARD_LINE) != NULL, "board read after the reset");
}

/* A board that answers is reset by the request alone, as it always was. */
MU_TEST(a_board_that_answers_is_not_usb_reset)
{
	reset_fake();
	mu_assert_int_eq(0, RUN("-r", "-b").rc);
	mu_assert_int_eq(0, fake.usb_resets);
	mu_assert_int_eq(1, fake.reset_calls);
}

/*
 * A USB reset that re-enumerated the board leaves no handle to ask, so the
 * board is waited for first and the reset request goes to the board that
 * comes back: -r is a full reset on this path too. The same holds for a
 * device-gone code from the same call.
 */
MU_TEST(a_usb_reset_that_re_enumerated_the_board_waits_then_resets_it)
{
	struct run_result r;
	const int gone[] = { LIBUSB_ERROR_NOT_FOUND, LIBUSB_ERROR_NO_DEVICE };
	size_t i;

	for (i = 0; i < sizeof(gone) / sizeof(gone[0]); ++i) {
		reset_fake();
		fake.follow_serial_rc = -1;
		fake.usb_reset_rc = gone[i];
		r = RUN("-U", "1", "-r", "-b");
		mu_assert_int_eq(0, r.rc);
		mu_assert_int_eq(1, fake.usb_resets);
		/* the request is sent, on the handle the wait reopened */
		mu_assert_int_eq(1, fake.reset_calls);
		mu_assert_int_eq(0, fake.first_used_after_release);
		mu_assert(fake.quiet_starts >= 2, "waited for, reset, waited again");
		/* The board really was off the bus for that first wait -- an index
		 * selection waits for the count rather than polling while it is
		 * gone -- and the request went out only once it was back. */
		mu_assert(fake.now_ms >= fake.returns_at_ms, "waited for it to return");
		mu_assert(fake.reset_at_ms >= fake.returns_at_ms,
		          "the request waited for the board to come back");
		/* The board that came back can be read, so the wait after the reset
		 * follows its serial rather than the index it was opened by. */
		mu_assert(strcmp(fake.quiet_selector, FAKE_SERIAL) == 0,
		          "the reopened board is followed by its serial");
		mu_assert(strcmp(r.err, "Ubertooth device 1 is not answering; "
		                 "resetting its USB connection\n"
		                 "Resetting ubertooth device 1\n") == 0, r.err);
		mu_assert(strstr(r.out, BOARD_LINE) != NULL, "board read at the end");
	}
}

/* A reset the host could not make is one line and a failure: the board is
 * still there, so nothing is waited for. */
MU_TEST(a_failed_usb_connection_reset_is_reported)
{
	struct run_result r;

	reset_fake();
	fake.follow_serial_rc = -1;
	fake.usb_reset_rc = LIBUSB_ERROR_IO;
	r = RUN("-U", "1", "-r", "-b");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.err, "Ubertooth device 1 is not answering; resetting "
	                 "its USB connection\n"
	                 "error: resetting the USB connection of Ubertooth device "
	                 "1 failed (libusb -1)\n") == 0, r.err);
	mu_assert(r.out[0] == '\0', "no report is printed");
	mu_assert_int_eq(0, fake.reset_calls);
	mu_assert_int_eq(0, fake.quiet_starts);
	mu_assert_int_eq(0, fake.stop_calls); /* it would be refused too */
	mu_assert_int_eq(1, fake.closes);
	mu_assert_int_eq(1, fake.frees);
}

/* A board still silent after the reset is as far as the host can take it. */
MU_TEST(a_board_still_silent_after_the_usb_reset_is_reported)
{
	struct run_result r;

	reset_fake();
	fake.follow_serial_rc = -1;
	fake.usb_reset_leaves_it_silent = 1;
	r = RUN("-U", "1", "-r", "-b");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(1, fake.usb_resets);
	mu_assert(strcmp(r.err, "Ubertooth device 1 is not answering; resetting "
	                 "its USB connection\n"
	                 "error: Ubertooth device 1 is still not answering after "
	                 "its USB connection was reset; power-cycle the board\n")
	          == 0, r.err);
	mu_assert(r.out[0] == '\0', "no report is printed");
	mu_assert_int_eq(0, fake.reset_calls);
	mu_assert_int_eq(0, fake.quiet_starts);
	/* it is given the whole window to answer, and no longer: the two reads
	 * that chose this path, then two on each poll, with a sleep between the
	 * polls and none after the last */
	mu_assert_int_eq((int)UTIL_USB_RESET_POLLS - 1, fake.sleeps);
	mu_assert_int_eq(2 + 2 * (int)UTIL_USB_RESET_POLLS,
	                 fake.follow_serial_reads);
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
#ifdef UTIL_TEST_REAL_API_MESSAGE
	/* A development build warns about a mismatch and runs on, a release
	 * build refuses it. The cases below want one answer whichever build
	 * this is, so they ask for the refusal. */
	setenv(UBERTOOTH_ALLOW_API_MISMATCH_VAR, "0", 1);
#endif

	MU_RUN(nonzero_query_values_are_not_exit_statuses);
	MU_RUN(every_known_board_id_exits_zero);
	MU_RUN(unknown_board_id_fails_without_indexing_the_name_table);
	MU_RUN(device_access_applies_the_selector_and_fails_cleanly_without_a_device);
	MU_RUN(composed_queries_print_in_the_documented_order_and_exit_zero);
	MU_RUN(an_earlier_failure_is_not_masked_by_later_successes);
	MU_RUN(part_id_with_bit_31_set_is_reported_in_full);
	MU_RUN(a_board_without_an_info_block_is_still_identified);
	MU_RUN(each_failing_report_makes_the_command_fail);
	MU_RUN(all_leds_query_reports_each_led_and_fails_if_any_read_fails);
	MU_RUN(range_result_is_reported_and_a_failed_read_fails);
	MU_RUN(range_test_reports_progress_and_then_the_result);
	MU_RUN(a_discarded_packet_is_named_once_and_the_run_goes_on);
	MU_RUN(range_test_with_no_repeater_times_out_and_stops_the_board);
	MU_RUN(an_interrupted_range_test_stops_the_board);
	MU_RUN(a_range_test_stopped_from_elsewhere_ends_the_wait);
	MU_RUN(the_watchdog_state_is_reported_and_a_failed_read_fails);
	MU_RUN(setters_pass_their_arguments_and_report_failures);
	MU_RUN(a_set_and_a_read_of_the_same_value_both_run_set_first);
	MU_RUN(stop_and_reset_report_failures);
	MU_RUN(mode_commands_exit_zero_on_success_and_one_on_failure);
	MU_RUN(a_failed_stop_before_a_mode_command_fails_the_command);
	MU_RUN(identify_starts_the_blink_on_the_board_and_returns);
	MU_RUN(identify_refuses_firmware_of_another_api_version);
#ifdef UTIL_TEST_REAL_API_MESSAGE
	MU_RUN(identify_prints_the_librarys_mismatch_message);
	MU_RUN(identify_runs_on_when_a_mismatch_is_allowed);
	MU_RUN(the_mismatch_note_names_the_board_it_is_about);
#endif
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
	MU_RUN(number_prints_the_count_first_and_the_rest_still_runs);
	MU_RUN(number_combines_with_the_multi_device_listings);
	MU_RUN(number_leaves_the_exit_status_to_the_rest_of_the_command);
	MU_RUN(the_flash_option_is_gone);
	MU_RUN(a_command_line_with_nothing_to_do_is_refused);
	MU_RUN(firmware_listing_with_several_devices_reports_each_device);
	MU_RUN(serial_listing_with_several_devices_exits_zero);
	MU_RUN(each_identity_query_alone_lists_every_device_compactly);
	MU_RUN(combined_identity_queries_list_every_line_for_every_device);
	MU_RUN(an_unavailable_device_is_reported_and_fails_the_listing);
	MU_RUN(the_firmware_version_is_labelled_whether_one_board_or_several);
	MU_RUN(a_failed_read_on_one_device_is_reported_and_fails_the_listing);
	MU_RUN(listing_is_not_used_with_a_selector_one_device_or_other_operations);
	MU_RUN(u_all_lists_every_device_even_when_only_one_is_attached);
	MU_RUN(u_all_fails_cleanly_with_no_devices_or_a_count_error);
	MU_RUN(u_all_is_rejected_before_opening_a_device_for_any_other_option);
	MU_RUN(u_all_with_only_the_count_beside_it_has_nothing_to_list);
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
	MU_RUN(a_board_that_answers_nothing_has_its_usb_connection_reset);
	MU_RUN(a_board_that_answers_a_few_polls_after_the_usb_reset_is_recovered);
	MU_RUN(a_board_that_answers_is_not_usb_reset);
	MU_RUN(a_usb_reset_that_re_enumerated_the_board_waits_then_resets_it);
	MU_RUN(a_failed_usb_connection_reset_is_reported);
	MU_RUN(a_board_still_silent_after_the_usb_reset_is_reported);
	MU_RUN(a_failed_reset_still_reselects_once_the_leave_window_passes);
	MU_RUN(reset_by_index_with_one_board_reopens_it);
	MU_RUN(the_handle_reopened_after_a_reset_is_released);
	return mu_report();
}
