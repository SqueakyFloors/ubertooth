/*
 * Hardware-independent tests of ubertooth-afh's main(): what it reports and
 * what it exits with after the initial AFH detection phase.
 *
 * ubertooth-afh.c is compiled into this test with main() renamed, and the
 * libubertooth and libbtbb functions it calls are replaced by the fakes below,
 * so the real command-line handling and reporting run against a scripted
 * detection phase. The faked piconet calls also count what a run allocates and
 * frees, so every path can be required to give the piconet back.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../../firmware/tests/minunit.h"

#define main ubertooth_afh_main
#include "ubertooth-afh.c"
#undef main

#include "run_main.h"

/* ---- scripted library --------------------------------------------------- */

static struct {
	int rx_afh_rc;    /* returned by rx_afh() */
	int rx_afh_r_rc;  /* returned by rx_afh_r() */
	int rx_afh_calls;
	int rx_afh_r_calls;
	int last_timeout; /* timeout passed to the detection phase */
	int stops;
	int closes;       /* ubertooth_close() calls */
	int frees;        /* ubertooth_free() calls */
	int no_device;    /* no board matches -U */
	int check_api_rc; /* returned by ubertooth_check_api() */
	int stream_failed;/* the receive stream died part-way */
	int fails_at_stop;/* ... and only the teardown finds out, which is what a
	                   * transfer the board never gave back looks like */
	int no_memory;    /* btbb_piconet_new() cannot allocate */
	int piconet_news; /* btbb_piconet_new() calls */
	int piconet_frees;/* btbb_piconet_unref() calls */
	/* 1 to have the teardown's STOP refused, so the board is left running
	 * the mode the capture started. */
	int stop_refused;
	/* The library's stop handlers, which go in before the board is
	 * opened, so a Ctrl-C during the opening is not discarded. */
	int catch_handlers;
	/* The stop a signal recorded while the board was being opened, which
	 * the tool reads before it asks anything of the board. */
	int stop_requested;
} fake;

static ubertooth_t fake_ut;
static int fake_devh;

static void reset_fake(void)
{
	memset(&fake, 0, sizeof(fake));
	memset(&fake_ut, 0, sizeof(fake_ut));
	fake_ut.devh = (struct libusb_device_handle *)&fake_devh;
}

/* libubertooth defines these; the tool reads and sets them. */
int max_ac_errors = 2;
unsigned int packet_counter_max;

ubertooth_t *ubertooth_start_select(const char *u_arg)
{
	(void)u_arg;
	return fake.no_device ? NULL : &fake_ut;
}

int ubertooth_check_api(ubertooth_t *ut)
{
	(void)ut;
	return fake.check_api_rc;
}

/* The piconet is opaque to the tool, so a plain allocation stands in for
 * libbtbb's. It is allocated for real, so a leak checker run over this binary
 * still reports an unfreed piconet if the tool stops giving it back. */
btbb_piconet *btbb_piconet_new(void)
{
	if (fake.no_memory)
		return NULL;
	fake.piconet_news++;
	return (btbb_piconet *)calloc(1, sizeof(int));
}

void btbb_piconet_unref(btbb_piconet *pn)
{
	/* The real one dereferences its argument at once, to drop a reference, so
	 * an unref of NULL must be reported here rather than pass and crash in a
	 * real build. */
	mu_assert(pn != NULL, "the piconet given back was NULL");
	if (pn == NULL)
		return;
	fake.piconet_frees++;
	free(pn);
}

void btbb_init_piconet(btbb_piconet *pn, uint32_t lap)
{
	(void)pn;
	(void)lap;
}

void btbb_piconet_set_uap(btbb_piconet *pn, uint8_t uap)
{
	(void)pn;
	(void)uap;
}

void btbb_piconet_set_flag(btbb_piconet *pn, int flag, int val)
{
	(void)pn;
	(void)flag;
	(void)val;
}

int btbb_init_survey(void)
{
	return 0;
}

void ubertooth_catch_stop_signals(void)
{
	fake.catch_handlers++;
}

int ubertooth_stop_requested(void)
{
	return fake.stop_requested;
}

/* The fake open always finds the board, so it never gives up on a stop. */
int ubertooth_open_stopped(void)
{
	return 0;
}

int rx_afh(ubertooth_t *ut, btbb_piconet *pn, int timeout)
{
	(void)ut;
	(void)pn;
	fake.rx_afh_calls++;
	fake.last_timeout = timeout;
	return fake.rx_afh_rc;
}

int rx_afh_r(ubertooth_t *ut, btbb_piconet *pn, int timeout)
{
	(void)ut;
	(void)pn;
	fake.rx_afh_r_calls++;
	fake.last_timeout = timeout;
	return fake.rx_afh_r_rc;
}


/* The end-of-capture gap report, which the tool calls and no case here reads:
 * a capture whose records never carried FIFO_OVERFLOW has nothing to say. */
void ubertooth_report_capture_losses(ubertooth_t *ut)
{
	(void)ut;
}

int ubertooth_stream_failed(ubertooth_t *ut)
{
	(void)ut;
	return fake.stream_failed;
}

void ubertooth_stop(ubertooth_t *ut)
{
	(void)ut;
	fake.stops++;
	if (fake.fails_at_stop)
		fake.stream_failed = 1;
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
	(void)ut;
	fake.closes++;
}

void ubertooth_free(ubertooth_t *ut)
{
	(void)ut;
	fake.frees++;
}

/* The -V answer, which libubertooth gives in every tool: faked here, so this
 * test stays about the tool's own command-line handling. The answer's own
 * cases are in test_version_report.c. */
int ubertooth_print_versions(FILE *out, const char *selector)
{
	(void)selector;
	fprintf(out, "version (test)\n");
	return 0;
}

/* ---- running the tool --------------------------------------------------- */

static struct run_result run_afh(const char *const *args)
{
	struct run_result result = run_tool_main(ubertooth_afh_main, args);

	/* The tool owns the piconet for the whole run, so by the time main()
	 * returns it has given back whatever it allocated, on every path. */
	mu_assert_int_eq(fake.piconet_news, fake.piconet_frees);
	return result;
}

#define RUN(...) run_afh((const char *const[]){ "ubertooth-afh", __VA_ARGS__, NULL })

/* ---- tests -------------------------------------------------------------- */

/* An all-zero map used to be printed, and the tool kept running. */
MU_TEST(no_packets_in_the_detection_phase_is_reported_and_exits_2)
{
	struct run_result r;

	reset_fake();
	fake.rx_afh_rc = UBERTOOTH_AFH_NO_PACKETS;
	r = RUN("-l", "446688", "-u", "22", "-t", "3");
	mu_assert_int_eq(2, r.rc);
	mu_assert(strcmp(r.err,
	    "error: no packets from ??:??:22:44:66:88 within 3 s;"
	    " AFH map unknown, not monitoring\n") == 0, r.err);
	mu_assert_int_eq(1, fake.rx_afh_calls);
	mu_assert_int_eq(1, fake.stops); /* the device is still released */
}

/* The address and the number of seconds in the message are the ones that were
 * asked for, not hard-coded. */
MU_TEST(the_message_names_the_piconet_and_the_timeout)
{
	struct run_result r;

	reset_fake();
	fake.rx_afh_rc = UBERTOOTH_AFH_NO_PACKETS;
	r = RUN("-l", "0a0b0c", "-u", "ff", "-t", "30");
	mu_assert_int_eq(2, r.rc);
	mu_assert(strcmp(r.err,
	    "error: no packets from ??:??:ff:0a:0b:0c within 30 s;"
	    " AFH map unknown, not monitoring\n") == 0, r.err);
	mu_assert_int_eq(30, fake.last_timeout);
}

/* With packets received, nothing changes: the map is printed by the library
 * and the tool exits 0 once monitoring is interrupted. */
/* A stop that arrived while the board was being opened ends the run before
 * any capture starts: exit 0, nothing printed, the board and the piconet given
 * back. */
MU_TEST(a_stop_while_the_board_was_opened_captures_nothing)
{
	struct run_result r;

	reset_fake();
	fake.stop_requested = 1;
	r = RUN("-l", "446688", "-u", "22", "-t", "3");
	mu_assert_int_eq(0, r.rc);
	mu_assert(r.err[0] == '\0', r.err);
	mu_assert_int_eq(0, fake.rx_afh_calls);
	mu_assert_int_eq(0, fake.stops);
}

MU_TEST(a_detected_map_still_exits_0_with_no_message)
{
	struct run_result r;

	reset_fake();
	r = RUN("-l", "446688", "-u", "22", "-t", "3");
	mu_assert_int_eq(0, r.rc);
	mu_assert(r.err[0] == '\0', r.err);
	mu_assert_int_eq(3, fake.last_timeout);
	mu_assert_int_eq(1, fake.stops);
	/* And the stop handlers were in before the board was opened, so a
	 * Ctrl-C during the opening would have been acted on. */
	mu_assert_int_eq(1, fake.catch_handlers);
}

/* Without -t there is no detection phase to fail. */
MU_TEST(no_timeout_runs_the_monitor_as_before)
{
	struct run_result r;

	reset_fake();
	r = RUN("-l", "446688", "-u", "22");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(1, fake.rx_afh_calls);
	mu_assert_int_eq(0, fake.last_timeout);
	mu_assert(r.err[0] == '\0', r.err);
}

/* -r keeps its own loop, which ignores the timeout. */
MU_TEST(the_r_format_is_unchanged)
{
	struct run_result r;

	reset_fake();
	r = RUN("-r", "-l", "446688", "-u", "22", "-t", "3");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(0, fake.rx_afh_calls);
	mu_assert_int_eq(1, fake.rx_afh_r_calls);
	mu_assert(r.err[0] == '\0', r.err);
}

/* A stream that died part-way is read from the handle rather than from the
 * return value, because the teardown is where a transfer the board never gave
 * back comes out: it used to exit 0, which told a script the monitoring it
 * never got had worked. */
MU_TEST(the_r_format_exits_1_when_its_stream_died)
{
	struct run_result r;

	reset_fake();
	fake.stream_failed = 1;
	r = RUN("-r", "-l", "446688", "-u", "22");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(1, fake.rx_afh_r_calls);
	/* the library reported the failure; the tool adds nothing */
	mu_assert(r.err[0] == '\0', r.err);
	mu_assert_int_eq(1, fake.stops);
}

/* A receive -r could not start at all -- a refused band sweep, a transfer
 * that could not be set up -- comes back as the error, and the run exits 1
 * rather than 0 for monitoring that never happened. The library has already
 * said what failed. */
MU_TEST(the_r_format_exits_1_when_its_receive_could_not_start)
{
	static const int errors[] = {
		-1, -9 /* LIBUSB_ERROR_PIPE: the board stalled the request */,
		-4 /* LIBUSB_ERROR_NO_DEVICE */
	};
	struct run_result r;
	size_t i;

	for (i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
		reset_fake();
		fake.rx_afh_r_rc = errors[i];
		r = RUN("-r", "-l", "446688", "-u", "22");
		mu_assert_int_eq(1, r.rc);
		mu_assert_int_eq(1, fake.rx_afh_r_calls);
		mu_assert(r.out[0] == '\0', r.out);
		mu_assert(r.err[0] == '\0', r.err);
		mu_assert_int_eq(1, fake.stops);
	}
}

/* A run whose board never gave the receive transfer back: the phases above
 * finished as usual, and the teardown is where that comes out. It exits 1
 * rather than reporting the monitoring, or the quiet piconet, that a lost
 * stream cannot have established. */
MU_TEST(a_run_whose_stream_is_only_found_lost_at_the_teardown_exits_1)
{
	struct run_result r;

	reset_fake();
	fake.fails_at_stop = 1;
	r = RUN("-l", "446688", "-u", "22", "-t", "3");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(1, fake.stops);

	/* the same for a detection phase that found nothing, where the exit
	 * status would otherwise be the one for a quiet piconet */
	reset_fake();
	fake.fails_at_stop = 1;
	fake.rx_afh_rc = UBERTOOTH_AFH_NO_PACKETS;
	r = RUN("-l", "446688", "-u", "22", "-t", "3");
	mu_assert_int_eq(1, r.rc);

	/* and for -r, which has no return value of its own */
	reset_fake();
	fake.fails_at_stop = 1;
	r = RUN("-r", "-l", "446688", "-u", "22");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(1, fake.rx_afh_r_calls);
}

/* A teardown the board refused leaves that board running the mode this tool
 * started, and no other tool can open it until it is reset or re-plugged, so
 * the run did not do what was asked. The AFH map it found is still reported,
 * and libubertooth has already named the refused request. */
MU_TEST(a_run_whose_teardown_was_refused_exits_1)
{
	struct run_result r;

	reset_fake();
	fake.stop_refused = 1;
	r = RUN("-l", "446688", "-u", "22", "-t", "3");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(1, fake.stops);
	/* The detection and monitoring phases both ran: what the run found is
	 * kept, and only the status changed. */
	mu_assert_int_eq(1, fake.rx_afh_calls);

	/* A detection phase that found nothing would otherwise exit 2, the
	 * status that says the piconet was quiet. A board left running is a
	 * failure, which is 1. */
	reset_fake();
	fake.stop_refused = 1;
	fake.rx_afh_rc = UBERTOOTH_AFH_NO_PACKETS;
	r = RUN("-l", "446688", "-u", "22", "-t", "3");
	mu_assert_int_eq(1, r.rc);

	/* The same board on a run that stopped cleanly. */
	reset_fake();
	r = RUN("-l", "446688", "-u", "22", "-t", "3");
	mu_assert_int_eq(0, r.rc);
}

/* A library error - libbtbb, or a receive that could not be started - is not
 * reported as "no packets", and it is not reported as success either. The
 * library has already said what failed. */
MU_TEST(a_library_error_is_not_mistaken_for_no_packets)
{
	static const int errors[] = {
		-1, -4 /* LIBUSB_ERROR_NO_DEVICE */,
		UBERTOOTH_ERROR_STREAM_FAILED
	};
	struct run_result r;
	size_t i;

	for (i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
		reset_fake();
		fake.rx_afh_rc = errors[i];
		r = RUN("-l", "446688", "-u", "22", "-t", "3");
		mu_assert_int_eq(1, r.rc);
		mu_assert(r.err[0] == '\0', r.err);
		mu_assert_int_eq(1, fake.stops);
	}
}

/* The piconet is allocated before a board is opened, so the paths that end
 * the run there have to give it back as well: no board matching -U, and a
 * board whose firmware does not match. */
MU_TEST(a_run_that_never_reaches_the_receive_phase_frees_the_piconet)
{
	struct run_result r;

	reset_fake();
	fake.no_device = 1;
	r = RUN("-l", "446688", "-u", "22");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(1, fake.piconet_news);
	mu_assert_int_eq(1, fake.piconet_frees);
	mu_assert_int_eq(0, fake.rx_afh_calls);

	reset_fake();
	fake.check_api_rc = -1;
	r = RUN("-l", "446688", "-u", "22");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(1, fake.piconet_news);
	mu_assert_int_eq(1, fake.piconet_frees);
	mu_assert_int_eq(0, fake.rx_afh_calls);
}

/* A piconet that cannot be allocated is reported rather than used: the tool
 * sets its fields straight after, so a NULL would be dereferenced. */
MU_TEST(a_piconet_that_cannot_be_allocated_is_refused)
{
	struct run_result r;

	reset_fake();
	fake.no_memory = 1;
	r = RUN("-l", "446688", "-u", "22");
	mu_assert_int_eq(1, r.rc);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strcmp(r.err, "error: out of memory\n") == 0, r.err);
	mu_assert_int_eq(0, fake.rx_afh_calls);
}

/* The usage text says that the tool keeps monitoring until it is interrupted
 * and what -t does at the end of its phase. -h is what the user asked for, so
 * it goes to standard output, leaves standard error empty and exits 0. */
MU_TEST(the_usage_text_documents_the_monitoring_and_the_exit_status)
{
	struct run_result r;

	reset_fake();
	r = RUN("-h");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strstr(r.out, "keeps monitoring") != NULL, r.out);
	mu_assert(strstr(r.out, "exit status 2") != NULL, r.out);
	mu_assert(strstr(r.out, "-U <index|serial>") != NULL, r.out);
	mu_assert(r.err[0] == '\0', r.err);
	mu_assert_int_eq(0, fake.rx_afh_calls);
}

/* A command line with no piconet address does nothing, so it is refused: one
 * error line on standard error, nothing on standard output, exit 1, and no
 * device opened. The error line says what was wrong, so the usage text does
 * not follow it. */
MU_TEST(a_missing_piconet_address_is_refused_on_standard_error)
{
	static const char REFUSAL[] =
		"error: -l <LAP> and -u <UAP> are both required";
	struct run_result r;

	reset_fake();
	r = RUN("-u", "22");
	mu_assert_int_eq(1, r.rc);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strncmp(r.err, REFUSAL, strlen(REFUSAL)) == 0, r.err);
	/* "keeps monitoring" is a line of the usage text. */
	mu_assert(strstr(r.err, "keeps monitoring") == NULL, r.err);
	mu_assert_int_eq(0, fake.rx_afh_calls);
}

/* A bare command line is the one refusal the usage text follows: the user
 * asked for nothing, so there is nothing else to say. */
MU_TEST(a_bare_command_line_is_refused_with_the_usage_text)
{
	struct run_result r;

	reset_fake();
	r = run_afh((const char *const[]){ "ubertooth-afh", NULL });
	mu_assert_int_eq(1, r.rc);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strncmp(r.err, "error: -l <LAP> and -u <UAP> are both required",
	                  strlen("error: -l <LAP> and -u <UAP> are both required")) == 0,
	          r.err);
	mu_assert(strstr(r.err, "keeps monitoring") != NULL, r.err);
	mu_assert_int_eq(0, fake.rx_afh_calls);
}

/* The handle and its 72 MB fifo go back on every exit once the board has been
 * opened: the tool used to leave them to process exit. */
MU_TEST(every_exit_path_gives_the_handle_back)
{
	struct run_result r;

	reset_fake();
	r = RUN("-l", "446688", "-u", "22");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(1, fake.frees);

	/* A firmware version the host will not drive: the library has released
	 * a board whose version it refused, and closing a board already
	 * released does nothing, so the tool closes either way. */
	reset_fake();
	fake.check_api_rc = -1;
	r = RUN("-l", "446688", "-u", "22");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(1, fake.closes);
	mu_assert_int_eq(1, fake.frees);

	/* A receive that could not be started. */
	reset_fake();
	fake.rx_afh_rc = -1;
	r = RUN("-l", "446688", "-u", "22");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(1, fake.frees);

	/* No board matches -U: there is no handle to give back. */
	reset_fake();
	fake.no_device = 1;
	r = RUN("-l", "446688", "-u", "22");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(0, fake.frees);
}

/* The removal threshold counts packets, not seconds, so it has to be a good
 * few times the number of channels in use or the monitor prunes channels the
 * piconet is still using: a piconet on the whole band puts about one packet
 * in 79 on any given channel. */
MU_TEST(the_removal_threshold_defaults_to_800_packets)
{
	struct run_result r;

	reset_fake();
	packet_counter_max = 0;
	r = RUN("-l", "446688", "-u", "22");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(800, (int)packet_counter_max);

	/* -m still has the last word. */
	reset_fake();
	r = RUN("-l", "446688", "-u", "22", "-m", "5");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(5, (int)packet_counter_max);
}

int main(void)
{
	MU_RUN(no_packets_in_the_detection_phase_is_reported_and_exits_2);
	MU_RUN(every_exit_path_gives_the_handle_back);
	MU_RUN(the_message_names_the_piconet_and_the_timeout);
	MU_RUN(a_stop_while_the_board_was_opened_captures_nothing);
	MU_RUN(a_detected_map_still_exits_0_with_no_message);
	MU_RUN(no_timeout_runs_the_monitor_as_before);
	MU_RUN(the_r_format_is_unchanged);
	MU_RUN(the_r_format_exits_1_when_its_stream_died);
	MU_RUN(the_r_format_exits_1_when_its_receive_could_not_start);
	MU_RUN(a_run_whose_stream_is_only_found_lost_at_the_teardown_exits_1);
	MU_RUN(a_run_whose_teardown_was_refused_exits_1);
	MU_RUN(a_library_error_is_not_mistaken_for_no_packets);
	MU_RUN(a_run_that_never_reaches_the_receive_phase_frees_the_piconet);
	MU_RUN(a_piconet_that_cannot_be_allocated_is_refused);
	MU_RUN(the_usage_text_documents_the_monitoring_and_the_exit_status);
	MU_RUN(a_missing_piconet_address_is_refused_on_standard_error);
	MU_RUN(a_bare_command_line_is_refused_with_the_usage_text);
	MU_RUN(the_removal_threshold_defaults_to_800_packets);
	return mu_report();
}
