/*
 * Hardware-independent tests of ubertooth-ego's main(): the requests that set
 * a capture up, and what the tool reports and exits with when the board
 * refuses one of them.
 *
 * ubertooth-ego.c is compiled into this test with main() renamed, and the
 * libubertooth functions it calls are replaced by the fakes below, so the real
 * command-line handling and reporting run against a scripted device. The
 * receive loop is ended by the faked poll, so no case waits on a board and no
 * case can transmit.
 */
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../../firmware/tests/minunit.h"

#define main ubertooth_ego_main
#include "ubertooth-ego.c"
#undef main

#include "run_main.h"

/* ---- scripted device ---------------------------------------------------- */

static struct {
	int select_fails;    /* no board answers the -U selection */
	int api_rc;          /* returned by ubertooth_check_api() */
	/* Whether that failure is a version mismatch, which the library
	 * reports by releasing the board itself, or a version it could not read
	 * at all, which leaves the board open for the caller to close. */
	int api_mismatch;
	int channel_rc;      /* returned by cmd_set_channel() */
	int set_aa_rc;       /* returned by cmd_rfcat_subcmd(RFCAT_SET_AA) */
	int cap_len_rc;      /* returned by cmd_rfcat_subcmd(RFCAT_CAP_LEN) */
	int ego_rc;          /* returned by cmd_ego() */
	int poll_rc;         /* returned by cmd_poll() */
	int interrupt_the_capture; /* cmd_poll() raises SIGINT instead of
	                            * writing the handle */
	int last_channel;    /* the channel the capture was set to, in MHz */
	uint8_t last_access_code[8];
	int last_access_code_len;
	uint8_t last_cap_len;
	int last_ego_mode;
	int stops;
	int closes;          /* ubertooth_close(): released without a STOP */
	int inits;           /* ubertooth_t allocations handed to the tool */
	int frees;           /* and how many of them it gave back */
	ubertooth_t *last_freed;
	/* Every device call in order, one letter each: c set channel, a set
	 * access code, l set capture length, e start the E-GO mode, ? a
	 * subcommand this fake does not know. */
	char calls[16];
} fake;

static ubertooth_t fake_ut;
static int fake_devh;

/* The library's stop route, in miniature, so a case can raise a real signal
 * and see what the capture loop makes of it. fake_cleanup() sets this flag and
 * nothing else, and ubertooth_stopping() is the only place it reaches the
 * handle: a loop that read the handle directly would not notice an interrupt
 * here either. */
static volatile sig_atomic_t stop_requested;

static void fake_cleanup(int sig)
{
	(void)sig;
	stop_requested = 1;
}

int ubertooth_stopping(ubertooth_t *ut)
{
	if (stop_requested)
		ut->stop_ubertooth = 1;
	return ut->stop_ubertooth;
}

void register_cleanup_handler(ubertooth_t *ut, int do_exit)
{
	(void)ut;
	(void)do_exit;
	signal(SIGINT, fake_cleanup);
}

static void record(char call)
{
	size_t length = strlen(fake.calls);

	if (length + 1 < sizeof(fake.calls)) {
		fake.calls[length] = call;
		fake.calls[length + 1] = '\0';
	}
}

static void reset_fake(void)
{
	memset(&fake, 0, sizeof(fake));
	memset(&fake_ut, 0, sizeof(fake_ut));
	fake_ut.devh = (struct libusb_device_handle *)&fake_devh;
	stop_requested = 0;
	signal(SIGINT, SIG_DFL);
}

ubertooth_t *ubertooth_start_select(const char *u_arg)
{
	(void)u_arg;
	if (fake.select_fails)
		return NULL; /* the library has reported it already */
	fake.inits++;
	return &fake_ut;
}

/* Asserting here would print to the standard output a run has redirected, so
 * the pointer is recorded and checked in the test body instead. */
void ubertooth_free(ubertooth_t *ut)
{
	fake.frees++;
	fake.last_freed = ut;
}

int ubertooth_check_api(ubertooth_t *ut)
{
	if (fake.api_rc < 0 && fake.api_mismatch)
		ubertooth_stop(ut);
	return fake.api_rc;
}

void ubertooth_stop(ubertooth_t *ut)
{
	ut->devh = NULL; /* as the library's cleanup leaves it */
	fake.stops++;
}

void ubertooth_close(ubertooth_t *ut)
{
	ut->devh = NULL;
	fake.closes++;
}

int cmd_set_channel(struct libusb_device_handle *devh, u16 channel)
{
	(void)devh;
	record('c');
	fake.last_channel = channel;
	return fake.channel_rc;
}

int cmd_rfcat_subcmd(struct libusb_device_handle *devh, int cmd, uint8_t *body,
                     size_t body_len)
{
	(void)devh;
	if (cmd == RFCAT_SET_AA) {
		record('a');
		if (body_len <= sizeof(fake.last_access_code))
			memcpy(fake.last_access_code, body, body_len);
		fake.last_access_code_len = (int)body_len;
		return fake.set_aa_rc;
	}
	if (cmd != RFCAT_CAP_LEN) {
		/* A subcommand added later must not be read as a capture length
		 * and quietly pass; the cases below compare the call letters. */
		record('?');
		return 0;
	}
	record('l');
	fake.last_cap_len = body[0];
	return fake.cap_len_rc;
}

int cmd_ego(struct libusb_device_handle *devh, int mode)
{
	(void)devh;
	record('e');
	fake.last_ego_mode = mode;
	return fake.ego_rc;
}

/* Ends the receive loop at once: a capture that read packets is covered by the
 * callback's own tests, and nothing here should wait on a board. A case that
 * wants the signal path instead raises a real interrupt here, which only
 * reaches the loop through the handler's flag. */
int cmd_poll(struct libusb_device_handle *devh, usb_pkt_rx *p)
{
	(void)devh;
	(void)p;
	if (fake.interrupt_the_capture)
		raise(SIGINT);
	else
		fake_ut.stop_ubertooth = 1;
	return fake.poll_rc;
}

void fifo_push(fifo_t *fifo, const usb_pkt_rx *packet, uint64_t arrival_ns)
{
	(void)fifo;
	(void)packet;
	(void)arrival_ns;
}

uint64_t ubertooth_packet_arrival_ns(void)
{
	return 0;
}

void cb_ego(ubertooth_t *ut, void *args)
{
	(void)ut;
	(void)args;
}

/* ---- running the tool --------------------------------------------------- */

static struct run_result run_ego(const char *const *args)
{
	struct run_result result = run_tool_main(ubertooth_ego_main, args);

	/* Checked for every case in this file, not just the cases that name an
	 * exit path: a run must give back whatever it was handed, and nothing
	 * else. The tool used to keep it on every path. */
	mu_assert(fake.frees == fake.inits, "the run did not free its device");
	mu_assert(fake.frees == 0 || fake.last_freed == &fake_ut,
	          "the run freed something other than its device");
	return result;
}

#define RUN(...) run_ego((const char *const[]){ "ubertooth-ego", __VA_ARGS__, NULL })

/* ---- tests -------------------------------------------------------------- */

/* A capture the board took: the requests in order, carrying the values the
 * command line asked for. */
MU_TEST(a_capture_the_board_took_sends_the_setup_requests_and_exits_0)
{
	struct run_result r;

	reset_fake();
	r = RUN("-r", "-c", "2408", "-a", "11223344", "-l", "24");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(fake.calls, "cale") == 0, fake.calls);
	mu_assert_int_eq(2408, fake.last_channel);
	mu_assert_int_eq(4, fake.last_access_code_len);
	mu_assert_int_eq(0x11, fake.last_access_code[0]);
	mu_assert_int_eq(0x44, fake.last_access_code[3]);
	mu_assert_int_eq(24, fake.last_cap_len);
	mu_assert_int_eq(1, fake.last_ego_mode); /* continuous receive */
	mu_assert_int_eq(1, fake.stops);
	mu_assert(strstr(r.err, "error: ") == NULL, r.err);
}

/* Following connections hops on the board, so no channel is set, and the tool
 * sends no access code of its own: the board keeps whichever one it was last
 * set to, which is what the man page's ACCESS CODE section describes. The
 * capture length is the only setup request. */
MU_TEST(following_sets_only_the_capture_length)
{
	struct run_result r;

	reset_fake();
	r = RUN("-f");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(fake.calls, "le") == 0, fake.calls);
	mu_assert_int_eq(18, fake.last_cap_len); /* the default */
	mu_assert_int_eq(0, fake.last_ego_mode); /* follow */
}

/* Continuous receive always sends a channel, so a run without -c is on a
 * known one rather than on whatever the board was left on. The literal is the
 * pin: 2418 is the value the usage text and the man page name. */
MU_TEST(continuous_receive_without_c_sends_the_default_channel)
{
	struct run_result r;

	reset_fake();
	r = RUN("-r");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(fake.calls, "cle") == 0, fake.calls);
	mu_assert_int_eq(2418, fake.last_channel);
	mu_assert(strstr(r.err, "error: ") == NULL, r.err);
}

/* A board that cannot be opened, and a firmware the host will not drive, each
 * end the command before any setup request. */
MU_TEST(a_board_or_firmware_the_tool_cannot_use_exits_1)
{
	struct run_result r;

	reset_fake();
	fake.select_fails = 1;
	r = RUN("-f");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(0, fake.inits);
	mu_assert(fake.calls[0] == '\0', fake.calls);

	/* A version mismatch: the library has released the board and ended the
	 * libusb session. The tool closes it all the same, which does nothing on
	 * a board already released, and gives the memory back. */
	reset_fake();
	fake.api_rc = -1;
	fake.api_mismatch = 1;
	r = RUN("-f");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(1, fake.frees);
	mu_assert_int_eq(1, fake.closes);
	mu_assert(fake.calls[0] == '\0', fake.calls);

	/* A version it could not read at all leaves the board open, so the tool
	 * closes it rather than freeing the memory around a claimed board. No
	 * STOP: this firmware is not one the host will drive. */
	reset_fake();
	fake.api_rc = -1;
	r = RUN("-f");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(1, fake.frees);
	mu_assert_int_eq(1, fake.closes);
	mu_assert_int_eq(0, fake.stops);
}

/* Each refused setup request is named, in one line on standard error, and ends
 * the command there: they used to be ignored, so a board that refused one
 * captured on whatever channel, access code or length it was left on and
 * reported success. */
MU_TEST(a_refused_setup_request_is_named_and_exits_1)
{
	static const struct {
		int *result;         /* the request to have the board refuse */
		const char *calls;   /* the requests made before the refusal */
		const char *message;
	} cases[] = {
		{ &fake.channel_rc, "c",   "error: could not set the channel\n" },
		{ &fake.set_aa_rc,  "ca",  "error: could not set the access code\n" },
		{ &fake.cap_len_rc, "cal", "error: could not set the capture length\n" },
	};
	struct run_result r;
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		reset_fake();
		/* LIBUSB_ERROR_PIPE, how a board refuses a request it does not
		 * implement. reset_fake() zeroes the struct rather than moving
		 * it, so these pointers stay good. */
		*cases[i].result = -9;

		r = RUN("-r", "-c", "2408", "-a", "11223344");
		mu_assert_int_eq(1, r.rc);
		mu_assert(strcmp(fake.calls, cases[i].calls) == 0, fake.calls);
		/* The E-GO mode is never started on top of a refused setting. */
		mu_assert(strchr(fake.calls, 'e') == NULL, fake.calls);
		mu_assert_int_eq(1, fake.stops); /* the board is still released */
		mu_assert(r.out[0] == '\0', r.out);
		mu_assert(strcmp(r.err, cases[i].message) == 0, r.err);
	}
}

/* The access code is only sent when -a asked for one, so a board that refuses
 * it cannot fail a command that never set one. */
MU_TEST(the_access_code_request_is_only_sent_for_a)
{
	struct run_result r;

	reset_fake();
	fake.set_aa_rc = -9;
	r = RUN("-r", "-c", "2408");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(fake.calls, "cle") == 0, fake.calls);
}

/* The firmware refusing the mode itself is reported as it always was, after
 * the setup requests the board did take, and leaves the board released like a
 * refused setup request does. Interfering needs transmit enabled as well, so
 * it says so. */
MU_TEST(a_firmware_without_ego_is_still_reported)
{
	struct run_result r;

	reset_fake();
	fake.ego_rc = -9;
	r = RUN("-f");
	mu_assert_int_eq(1, r.rc);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strcmp(r.err, "error: E-GO not supported by this firmware\n") == 0,
	          r.err);
	mu_assert_int_eq(1, fake.stops);

	reset_fake();
	fake.ego_rc = -9;
	r = RUN("-i");
	mu_assert_int_eq(1, r.rc);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strcmp(r.err, "error: E-GO not supported by this firmware "
	                        "(or TX not enabled)\n") == 0, r.err);
	mu_assert_int_eq(1, fake.stops);
}

/* Interfering hops on the board and does not read, so the capture length is
 * the only setup request it sends. Every device call here is faked, so nothing
 * is transmitted. */
MU_TEST(interfering_sets_only_the_capture_length)
{
	struct run_result r;

	reset_fake();
	r = RUN("-i");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(fake.calls, "le") == 0, fake.calls);
	mu_assert_int_eq(2, fake.last_ego_mode);
}

/* A channel the chosen mode never sends is refused, naming the mode option as
 * the user spelled it, before any board is opened: -c used to be parsed,
 * range-checked and then dropped without a word. */
MU_TEST(a_channel_the_mode_cannot_use_is_refused)
{
	static const struct {
		const char *mode;
		const char *message;
	} cases[] = {
		{ "-f", "error: -c cannot be combined with -f; only continuous "
		        "receive (-r) uses a channel\n" },
		{ "-i", "error: -c cannot be combined with -i; only continuous "
		        "receive (-r) uses a channel\n" },
		{ "-j", "error: -c cannot be combined with -j; only continuous "
		        "receive (-r) uses a channel\n" },
	};
	struct run_result r;
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		reset_fake();
		r = RUN(cases[i].mode, "-c", "2408");
		mu_assert_int_eq(1, r.rc);
		mu_assert(r.out[0] == '\0', r.out);
		mu_assert(strcmp(r.err, cases[i].message) == 0, r.err);
		mu_assert_int_eq(0, fake.inits); /* no board was opened */
		mu_assert(fake.calls[0] == '\0', fake.calls);
	}

	/* The mode that does use a channel still takes it, and a bad value is
	 * still reported as a bad value rather than as the wrong mode. */
	reset_fake();
	r = RUN("-r", "-c", "2408");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(2408, fake.last_channel);

	reset_fake();
	r = RUN("-f", "-c", "2481");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.err, "error: channel must be 2402-2480 MHz (got 2481)\n")
	          == 0, r.err);

	/* The last mode on the command line is the one that runs, so it decides
	 * whether the channel is used. */
	reset_fake();
	r = RUN("-f", "-c", "2408", "-r");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(2408, fake.last_channel);

	reset_fake();
	r = RUN("-r", "-c", "2408", "-f");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.err, cases[0].message) == 0, r.err);

	/* With no mode at all there is nothing to refuse the channel for, and
	 * the refusal has no option to name: the earlier "nothing to do" check
	 * answers. The command line said something, so that line is all of it. */
	reset_fake();
	r = RUN("-c", "2408");
	mu_assert_int_eq(1, r.rc);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strncmp(r.err, "error: nothing to do: ",
	                  strlen("error: nothing to do: ")) == 0, r.err);
	mu_assert(strstr(r.err, "cannot be combined") == NULL, r.err);
	mu_assert(strstr(r.err, "Usage:") == NULL, r.err);
}

/* A bare command line is the one refusal the usage text follows: the user
 * asked for nothing, so there is nothing else to say. */
MU_TEST(a_bare_command_line_is_refused_with_the_usage_text)
{
	struct run_result r;

	reset_fake();
	r = run_ego((const char *const[]){ "ubertooth-ego", NULL });
	mu_assert_int_eq(1, r.rc);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strncmp(r.err, "error: nothing to do: ",
	                  strlen("error: nothing to do: ")) == 0, r.err);
	mu_assert(strstr(r.err, "Usage:") != NULL, r.err);
	mu_assert_int_eq(0, fake.inits);
}

/* -h is what the user asked for, so it is answered even when the rest of the
 * command line is a combination the tool would refuse. */
MU_TEST(help_wins_over_a_refused_combination)
{
	struct run_result r;

	reset_fake();
	r = RUN("-f", "-c", "2408", "-h");
	mu_assert_int_eq(0, r.rc);
	mu_assert(r.err[0] == '\0', r.err);
	mu_assert(strstr(r.out, "continuous rx only") != NULL, r.out);
	mu_assert_int_eq(0, fake.inits);
}

/* A Ctrl-C ends a capture, with the board taken out of its mode and exit 0:
 * the user stopped a run that was working. The loop used to read a plain int
 * the handler wrote, which a compiler may read once and keep, so nothing
 * guaranteed it would see the interrupt at all. */
MU_TEST(an_interrupted_capture_stops_and_exits_zero)
{
	struct run_result r;

	reset_fake();
	fake.interrupt_the_capture = 1;
	r = RUN("-f");
	mu_assert_int_eq(0, r.rc);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strstr(r.err, "error: ") == NULL, r.err);
	mu_assert_int_eq(1, fake.stops);
}

/* A capture that stops short because a poll failed did not do what was asked,
 * so it says so and exits 1, which the man page's EXIT STATUS promises. */
MU_TEST(a_failed_poll_ends_the_capture_with_1)
{
	struct run_result r;

	reset_fake();
	fake.poll_rc = -9;
	r = RUN("-f");
	mu_assert_int_eq(1, r.rc);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strcmp(r.err, "error: USB poll failed\n") == 0, r.err);
	mu_assert_int_eq(1, fake.stops);
}

int main(void)
{
	MU_RUN(a_capture_the_board_took_sends_the_setup_requests_and_exits_0);
	MU_RUN(following_sets_only_the_capture_length);
	MU_RUN(continuous_receive_without_c_sends_the_default_channel);
	MU_RUN(a_board_or_firmware_the_tool_cannot_use_exits_1);
	MU_RUN(a_refused_setup_request_is_named_and_exits_1);
	MU_RUN(the_access_code_request_is_only_sent_for_a);
	MU_RUN(a_firmware_without_ego_is_still_reported);
	MU_RUN(interfering_sets_only_the_capture_length);
	MU_RUN(a_channel_the_mode_cannot_use_is_refused);
	MU_RUN(a_bare_command_line_is_refused_with_the_usage_text);
	MU_RUN(help_wins_over_a_refused_combination);
	MU_RUN(an_interrupted_capture_stops_and_exits_zero);
	MU_RUN(a_failed_poll_ends_the_capture_with_1);
	return mu_report();
}
