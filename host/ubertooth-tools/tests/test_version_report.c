/*
 * The -V answer every tool gives (ubertooth_print_versions()), against a
 * scripted board: what it prints for no board, one board, several boards and
 * a board that cannot be read, and the exit status that goes with each.
 *
 * libubertooth's ubertooth_version.c is compiled into this test and the
 * device calls it makes are the fakes below, so the real answer runs without
 * a board on the bus. The host line is faked too: it carries the versions the
 * build was configured with, which say nothing about this answer's shape.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../firmware/tests/minunit.h"
#include "run_main.h"
#include "ubertooth.h"

#define HOST_LINE "Host tools version: libubertooth test\n"

#define MAX_BOARDS 3

/* One scripted board: what each of its reads answers. */
struct fake_board {
	const char *image, *revision, *compile_info;
	uint16_t api;
	/* 0, or the negative result the read fails with. LIBUSB_ERROR_PIPE is
	 * how firmware older than the info block answers a read of it. */
	int info_rc, revision_rc, api_rc, compile_rc;
	int opens; /* 0, or the negative result the open fails with */
};

static struct {
	int count;        /* boards on the bus */
	int count_rc;     /* 0, or a negative result from ubertooth_count() */
	int hidden;       /* UBERTOOTH_TEST_NO_DEVICES is set */
	const char *open_reason;
	struct fake_board board[MAX_BOARDS];
	/* bookkeeping: a handle opened and not given back is a leak, and a
	 * STOP would end a mode the user is running */
	int opens, closes, frees, stops;
	int dfu_notes;    /* ubertooth_note_dfu_bootloader() calls */
	int current;      /* the board the open handle belongs to */
	ubertooth_t handle;
} fake;

static void make_fake(int boards)
{
	int i;

	memset(&fake, 0, sizeof(fake));
	fake.count = boards;
	fake.open_reason = "in use by another program";
	for (i = 0; i < MAX_BOARDS; i++) {
		fake.board[i].image = "bluetooth_rxtx";
		fake.board[i].revision = "git-test";
		fake.board[i].compile_info = "compile-test";
		fake.board[i].api = 0x0107;
	}
}

static struct fake_board *current_board(void)
{
	return &fake.board[fake.current];
}

/* ---- the library calls the answer makes -------------------------------- */

void ubertooth_print_host_version(FILE *out)
{
	fputs(HOST_LINE, out);
}

int ubertooth_devices_hidden(void)
{
	return fake.hidden;
}

int ubertooth_count(void)
{
	return fake.count_rc < 0 ? fake.count_rc : fake.count;
}

void ubertooth_note_dfu_bootloader(void)
{
	fake.dfu_notes++;
}

static ubertooth_t *open_board(int index)
{
	if (index < 0 || index >= fake.count)
		return NULL;
	if (fake.board[index].opens < 0)
		return NULL;
	fake.current = index;
	fake.opens++;
	/* A handle with a device on it; the fakes below read fake.current
	 * rather than this pointer. */
	fake.handle.devh = (struct libusb_device_handle *)&fake;
	return &fake.handle;
}

ubertooth_t *ubertooth_start(int ubertooth_device)
{
	return open_board(ubertooth_device);
}

ubertooth_t *ubertooth_start_select(const char *u_arg)
{
	/* The selector is an index here; a serial is the library's own lookup,
	 * which this answer does not take part in. */
	return open_board(u_arg == NULL ? 0 : atoi(u_arg));
}

void ubertooth_close(ubertooth_t *ut)
{
	if (ut != NULL)
		fake.closes++;
}

void ubertooth_stop(ubertooth_t *ut)
{
	if (ut != NULL)
		fake.stops++;
}

void ubertooth_free(ubertooth_t *ut)
{
	if (ut != NULL)
		fake.frees++;
}

const char *ubertooth_open_failure_reason(void)
{
	return fake.open_reason;
}

int ubertooth_get_api(ubertooth_t *ut, uint16_t *version)
{
	(void)ut;
	if (current_board()->api_rc < 0)
		return current_board()->api_rc;
	*version = current_board()->api;
	return 0;
}

int cmd_get_firmware_info(struct libusb_device_handle *devh,
                          struct ubertooth_fw_info *info)
{
	struct fake_board *board = current_board();

	(void)devh;
	memset(info, 0, sizeof(*info));
	info->format_version = UBERTOOTH_FW_INFO_FORMAT;
	info->api_version = board->api;
	snprintf(info->image, sizeof(info->image), "%s", board->image);
	snprintf(info->revision, sizeof(info->revision), "%s", board->revision);
	return board->info_rc;
}

int cmd_get_firmware_revision(struct libusb_device_handle *devh, char *revision,
                              size_t size)
{
	struct fake_board *board = current_board();

	(void)devh;
	if (board->revision_rc < 0) {
		if (size > 0)
			revision[0] = '\0';
		return board->revision_rc;
	}
	snprintf(revision, size, "%s", board->revision);
	return 0;
}

int cmd_get_firmware_compile_info(struct libusb_device_handle *devh, char *info,
                                  size_t size)
{
	struct fake_board *board = current_board();

	(void)devh;
	if (board->compile_rc < 0) {
		if (size > 0)
			info[0] = '\0';
		return board->compile_rc;
	}
	snprintf(info, size, "%s", board->compile_info);
	return 0;
}

/* ---- running the answer ------------------------------------------------ */

/* A main()-shaped wrapper, so run_main.h captures both streams: argv[1] is
 * the -U argument, and no argument means none was given. */
static int version_main(int argc, char *argv[])
{
	/* Standard output, as every tool passes, so run_main.h captures it. */
	return ubertooth_print_versions(stdout, argc > 1 ? argv[1] : NULL);
}

static struct run_result run_versions(const char *const *args)
{
	struct run_result result = run_tool_main(version_main, args);

	mu_assert_int_eq(fake.opens, fake.closes);
	mu_assert_int_eq(fake.opens, fake.frees);
	/* Asking a board its version must not end the mode it is running. */
	mu_assert_int_eq(0, fake.stops);
	return result;
}

#define RUN(...) \
	run_versions((const char *const[]){ "version", __VA_ARGS__, NULL })
/* The same with no -U argument, which is how a tool is usually run. */
#define RUN_PLAIN() run_versions((const char *const[]){ "version", NULL })

MU_TEST(no_board_is_an_answer_not_a_failure)
{
	struct run_result r;

	make_fake(0);
	r = RUN_PLAIN();
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, HOST_LINE
	                 "Firmware version: no Ubertooth attached\n") == 0,
	          "the no-board answer changed");
	mu_assert_int_eq(0, (long)strlen(r.err));
	mu_assert_int_eq(0, fake.opens);
	/* A board in the bootloader is what "no Ubertooth attached" most often
	 * means, so the library's note is asked for on this path. */
	mu_assert_int_eq(1, fake.dfu_notes);
}

/* The switch the project's tests set must not turn -V's answer into a note on
 * standard error: nothing is attached, and that is what the answer says. */
MU_TEST(hidden_devices_read_as_no_board)
{
	struct run_result r;

	make_fake(2);
	fake.hidden = 1;
	r = RUN_PLAIN();
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, HOST_LINE
	                 "Firmware version: no Ubertooth attached\n") == 0,
	          "the hidden-devices answer is not the no-board one");
	mu_assert_int_eq(0, (long)strlen(r.err));
	mu_assert_int_eq(0, fake.opens);
}

MU_TEST(one_board_prints_the_three_lines)
{
	struct run_result r;

	make_fake(1);
	r = RUN_PLAIN();
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, HOST_LINE
	                 "Firmware version: bluetooth_rxtx git-test (API:1.07)\n"
	                 "Firmware build: compile-test\n") == 0,
	          "the single-board answer changed");
	mu_assert_int_eq(1, fake.opens);
	/* The note is for the paths that found no board at all. */
	mu_assert_int_eq(0, fake.dfu_notes);
}

/* Firmware built before the info block existed stalls the request for it. The
 * revision alone is what such a board answers, and the line carries it. */
MU_TEST(firmware_without_an_info_block_is_still_reported)
{
	struct run_result r;

	make_fake(1);
	fake.board[0].info_rc = LIBUSB_ERROR_PIPE;
	r = RUN_PLAIN();
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, HOST_LINE
	                 "Firmware version: git-test (API:1.07)\n"
	                 "Firmware build: compile-test\n") == 0,
	          "a board with no info block was not reported from what it answers");
}

/* A board that answers nothing: the line says so once, rather than a second
 * line saying the build string could not be read either. */
MU_TEST(a_board_that_cannot_be_read_says_so_and_fails)
{
	struct run_result r;

	make_fake(1);
	fake.board[0].info_rc = -7;
	r = RUN_PLAIN();
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.out, HOST_LINE
	                 "Firmware version: read failed\n") == 0,
	          "an unreadable board's line changed");

	make_fake(1);
	fake.board[0].info_rc = LIBUSB_ERROR_PIPE;
	fake.board[0].revision_rc = -7;
	r = RUN_PLAIN();
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.out, HOST_LINE
	                 "Firmware version: read failed\n") == 0,
	          "a stalled block with an unreadable revision changed");
}

/* Part of the answer missing is still a failure, and what was read is still
 * printed: the API version is a separate read from the version itself, and
 * the build string a third. */
MU_TEST(a_partial_answer_prints_what_it_read_and_fails)
{
	struct run_result r;

	make_fake(1);
	fake.board[0].api_rc = -1;
	r = RUN_PLAIN();
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.out, HOST_LINE
	                 "Firmware version: bluetooth_rxtx git-test\n"
	                 "Firmware build: compile-test\n") == 0,
	          "the answer without an API version changed");

	make_fake(1);
	fake.board[0].compile_rc = -1;
	r = RUN_PLAIN();
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.out, HOST_LINE
	                 "Firmware version: bluetooth_rxtx git-test (API:1.07)\n"
	                 "Firmware build: read failed\n") == 0,
	          "the answer without a build string changed");
}

MU_TEST(several_boards_are_indexed)
{
	struct run_result r;

	make_fake(2);
	fake.board[1].revision = "git-other";
	r = RUN_PLAIN();
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, HOST_LINE
	                 "Firmware version (0): bluetooth_rxtx git-test (API:1.07)\n"
	                 "Firmware build (0): compile-test\n"
	                 "Firmware version (1): bluetooth_rxtx git-other (API:1.07)\n"
	                 "Firmware build (1): compile-test\n") == 0,
	          "the several-boards answer changed");
	mu_assert_int_eq(2, fake.opens);
}

/* One board of several that cannot be opened gets its own line, and the
 * others are still reported. */
MU_TEST(a_board_that_will_not_open_is_named_and_the_rest_reported)
{
	struct run_result r;

	make_fake(2);
	fake.board[0].opens = -6;
	r = RUN_PLAIN();
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.out, HOST_LINE
	                 "Firmware version (0): unavailable, in use by another "
	                 "program\n"
	                 "Firmware version (1): bluetooth_rxtx git-test (API:1.07)\n"
	                 "Firmware build (1): compile-test\n") == 0,
	          "a board in use was not reported beside the others");
}

/* The board the user named with -U is the only one reported, in the form
 * that carries no index. */
MU_TEST(a_selected_board_is_reported_without_an_index)
{
	struct run_result r;

	make_fake(2);
	fake.board[1].revision = "git-other";
	r = RUN("1");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, HOST_LINE
	                 "Firmware version: bluetooth_rxtx git-other (API:1.07)\n"
	                 "Firmware build: compile-test\n") == 0,
	          "the selected-board answer changed");
	mu_assert_int_eq(1, fake.opens);

	make_fake(1);
	fake.board[0].opens = -6;
	r = RUN("0");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.out, HOST_LINE
	                 "Firmware version: unavailable, in use by another "
	                 "program\n") == 0,
	          "a selected board that will not open changed");
}

/* An empty -U value names no board, as it does in every other selection, so
 * the answer is the whole bus rather than a refusal to pick one. */
MU_TEST(an_empty_selector_reports_every_board)
{
	struct run_result r;

	make_fake(2);
	fake.board[1].revision = "git-other";
	r = RUN("");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, HOST_LINE
	                 "Firmware version (0): bluetooth_rxtx git-test (API:1.07)\n"
	                 "Firmware build (0): compile-test\n"
	                 "Firmware version (1): bluetooth_rxtx git-other (API:1.07)\n"
	                 "Firmware build (1): compile-test\n") == 0,
	          "an empty -U did not report every board");
	mu_assert_int_eq(2, fake.opens);

	/* And with one board, the single-board form, as no -U gives. */
	make_fake(1);
	r = RUN("");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, HOST_LINE
	                 "Firmware version: bluetooth_rxtx git-test (API:1.07)\n"
	                 "Firmware build: compile-test\n") == 0, r.out);
}

/* A board that answers an empty build string has nothing to show, so the line
 * says the read failed and the status agrees with it. */
MU_TEST(an_empty_build_string_is_a_failed_read)
{
	struct run_result r;

	make_fake(1);
	fake.board[0].compile_info = "";
	r = RUN_PLAIN();
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.out, HOST_LINE
	                 "Firmware version: bluetooth_rxtx git-test (API:1.07)\n"
	                 "Firmware build: read failed\n") == 0, r.out);
}

/* A bus that cannot be listed is a failure, not an empty one: the host line
 * still goes out, because it is the half that needs no board. */
MU_TEST(a_bus_that_cannot_be_listed_fails_after_the_host_line)
{
	struct run_result r;

	make_fake(1);
	fake.count_rc = -1;
	r = RUN_PLAIN();
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.out, HOST_LINE "Firmware version: unavailable, the USB "
	                 "bus could not be listed\n") == 0,
	          "the answer for a bus that cannot be listed changed");
	mu_assert_int_eq(0, fake.opens);
}

int main(void)
{
	MU_RUN(no_board_is_an_answer_not_a_failure);
	MU_RUN(hidden_devices_read_as_no_board);
	MU_RUN(one_board_prints_the_three_lines);
	MU_RUN(firmware_without_an_info_block_is_still_reported);
	MU_RUN(a_board_that_cannot_be_read_says_so_and_fails);
	MU_RUN(a_partial_answer_prints_what_it_read_and_fails);
	MU_RUN(several_boards_are_indexed);
	MU_RUN(a_board_that_will_not_open_is_named_and_the_rest_reported);
	MU_RUN(a_selected_board_is_reported_without_an_index);
	MU_RUN(an_empty_selector_reports_every_board);
	MU_RUN(an_empty_build_string_is_a_failed_read);
	MU_RUN(a_bus_that_cannot_be_listed_fails_after_the_host_line);
	return mu_report();
}
