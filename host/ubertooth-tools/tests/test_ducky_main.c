/*
 * Hardware-independent tests of ubertooth-ducky's main(): what it sends to
 * quack, what it reports and exits with when the board refuses one of the
 * requests, and that every exit path gives the device bookkeeping back.
 *
 * ubertooth-ducky.c is compiled into this test with main() renamed, and the
 * libubertooth functions it calls are replaced by the fakes below, so the real
 * command-line handling and reporting run against a scripted device. sleep()
 * is faked too: it is what a quack that went out waits in, so a case can
 * require that a refused one does not wait at all.
 */
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../../firmware/tests/minunit.h"

#define main ubertooth_ducky_main
#include "ubertooth-ducky.c"
#undef main

#include "run_main.h"

/* ---- scripted device ---------------------------------------------------- */

static struct {
	int connect_rc;      /* returned by ubertooth_connect_select() */
	int api_rc;          /* returned by ubertooth_check_api() */
	/* Whether that failure is a version mismatch, which the library
	 * reports by releasing the board itself, or a version it could not read
	 * at all, which leaves the board open for the caller to close. */
	int api_mismatch;
	int closes;          /* ubertooth_close(): released without a STOP */
	int channel_rc;      /* returned by cmd_set_channel() */
	/* Returned by the channel request made after advertising has started,
	 * which is the one a mode start cannot throw away. */
	int restated_channel_rc;
	int adv_data_rc;     /* returned by cmd_le_set_adv_data() */
	int slave_rc;        /* returned by cmd_btle_slave() */
	int last_channel;    /* the channel the quack was set to, in MHz */
	uint8_t last_adv_data[32];
	unsigned last_adv_len;
	uint8_t last_bd_addr[6];
	int stops;
	int inits;           /* ubertooth_t allocations handed to the tool */
	int frees;           /* and how many of them it gave back */
	ubertooth_t *last_freed;
	int sleeps;
	int cleanup_handlers; /* ubertooth_catch_stop_signals() calls */
	int stop_requested;  /* what ubertooth_stop_requested() answers */
	/* Every device call in order, one letter each: c set channel,
	 * a set advertising data, s start advertising as a slave. */
	char calls[16];
	/* 1 to have the teardown's STOP refused, so the board is left running
	 * the mode the capture started. */
	int stop_refused;
} fake;

static ubertooth_t fake_ut;
static int fake_devh;

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
}

ubertooth_t *ubertooth_init(void)
{
	fake.inits++;
	return &fake_ut;
}

/* The library's handlers, installed before the board is opened, so a Ctrl-C
 * during the opening is not discarded and one during the advertising wait
 * below cuts it short and takes the board out of advertising. Counted rather
 * than put in fake.calls, which is the device's own calls. */
void ubertooth_catch_stop_signals(void)
{
	fake.cleanup_handlers++;
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

/* Asserting here would print to the standard output a run has redirected, so
 * the pointer is recorded and checked in the test body instead. */
void ubertooth_free(ubertooth_t *ut)
{
	fake.frees++;
	fake.last_freed = ut;
}

int ubertooth_connect_select(ubertooth_t *ut, const char *u_arg)
{
	(void)ut;
	(void)u_arg;
	return fake.connect_rc;
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
	ut->devh = NULL;
	fake.closes++;
}

int cmd_set_channel(struct libusb_device_handle *devh, u16 channel)
{
	int after_slave = strchr(fake.calls, 's') != NULL;

	(void)devh;
	record('c');
	fake.last_channel = channel;
	return after_slave ? fake.restated_channel_rc : fake.channel_rc;
}

int cmd_le_set_adv_data(struct libusb_device_handle *devh, uint8_t *data,
                        unsigned data_len)
{
	(void)devh;
	record('a');
	if (data_len <= sizeof(fake.last_adv_data))
		memcpy(fake.last_adv_data, data, data_len);
	fake.last_adv_len = data_len;
	return fake.adv_data_rc;
}

int cmd_btle_slave(struct libusb_device_handle *devh, u8 *mac_address)
{
	(void)devh;
	record('s');
	memcpy(fake.last_bd_addr, mac_address, sizeof(fake.last_bd_addr));
	return fake.slave_rc;
}

/* The two seconds a successful quack advertises for; counted instead of
 * waited, so the suite stays fast and a refused quack can be required not to
 * wait. */
unsigned int sleep(unsigned int seconds)
{
	(void)seconds;
	fake.sleeps++;
	return 0;
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

static struct run_result run_ducky(const char *const *args)
{
	struct run_result result = run_tool_main(ubertooth_ducky_main, args);

	/* Checked for every case in this file, not just the cases below that
	 * name an exit path: a run must give back whatever it was handed, and
	 * nothing else. The tool used to keep it on every path. */
	mu_assert(fake.frees == fake.inits, "the run did not free its device");
	mu_assert(fake.frees == 0 || fake.last_freed == &fake_ut,
	          "the run freed something other than its device");
	return result;
}

#define RUN(...) run_ducky((const char *const[]){ "ubertooth-ducky", __VA_ARGS__, NULL })

/* ---- tests -------------------------------------------------------------- */

/* A quack the board took: the requests in order, the advertisement the tool
 * has always sent, and nothing on standard output.
 *
 * The channel is stated twice. Advertising starts with the mode, so the board
 * has to be on the channel before the mode request -- and a mode start puts
 * the channel back to 2441 MHz, which a board left in another mode by a killed
 * tool does after that request has been answered, so the first statement was
 * thrown away and the quack went out on 2441 MHz whatever -A asked for. The
 * second is the one such a board keeps. */
MU_TEST(a_quack_the_board_took_sends_its_requests_and_exits_0)
{
	static const uint8_t UUID_LAST_BYTE_FIRST[] = { 0x0c, 0xc8, 0x2d, 0x7d };
	struct run_result r;

	reset_fake();
	r = RUN("-q", "-a", "11:22:33:44:55:66");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(fake.calls, "casc") == 0, fake.calls);
	mu_assert_int_eq(2426, fake.last_channel); /* advertising index 38 */
	mu_assert_int_eq(18, fake.last_adv_len);
	mu_assert_int_eq(0x11, fake.last_adv_data[0]);
	mu_assert_int_eq(0x07, fake.last_adv_data[1]);
	/* The UUID goes out in reverse byte order. */
	mu_assert(memcmp(fake.last_adv_data + 2, UUID_LAST_BYTE_FIRST,
	                 sizeof(UUID_LAST_BYTE_FIRST)) == 0, "the UUID's bytes");
	mu_assert_int_eq(0x11, fake.last_bd_addr[0]);
	mu_assert_int_eq(0x66, fake.last_bd_addr[5]);
	mu_assert_int_eq(1, fake.sleeps);
	mu_assert_int_eq(1, fake.stops);
	/* So a Ctrl-C during those two seconds cuts the wait short and the
	 * board is taken out of advertising rather than left in it. */
	mu_assert_int_eq(1, fake.cleanup_handlers);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strstr(r.err, "Quacking ") != NULL, r.err);
	mu_assert(strstr(r.err, "error: ") == NULL, r.err);
}

/* The three advertising channel indexes tune the three frequencies the
 * Bluetooth Core specification gives them: 37 at the bottom of the band, 38 in
 * the middle, 39 at the top. */
/* A stop that arrived while the board was being opened ends the run before
 * anything is transmitted: no channel, no advertising data, no advertising,
 * and none of the two-second wait. */
MU_TEST(a_stop_while_the_board_was_opened_quacks_nothing)
{
	struct run_result r;

	reset_fake();
	fake.stop_requested = 1;
	r = RUN("-q", "-a", "11:22:33:44:55:66");

	mu_assert_int_eq(0, r.rc);
	mu_assert(fake.calls[0] == '\0', fake.calls);
	mu_assert_int_eq(0, fake.sleeps);
	mu_assert_int_eq(1, fake.closes);
	mu_assert_int_eq(1, fake.frees);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strstr(r.err, "error: ") == NULL, r.err);
}

MU_TEST(each_advertising_index_tunes_its_own_frequency)
{
	reset_fake();
	mu_assert_int_eq(0, RUN("-q", "-A", "37").rc);
	mu_assert_int_eq(2402, fake.last_channel);

	reset_fake();
	mu_assert_int_eq(0, RUN("-q", "-A", "38").rc);
	mu_assert_int_eq(2426, fake.last_channel);

	reset_fake();
	mu_assert_int_eq(0, RUN("-q", "-A", "39").rc);
	mu_assert_int_eq(2480, fake.last_channel);
}

/* Each refused request is named, in one line on standard error, and ends the
 * command there: it used to be ignored, so a board that refused one still
 * slept for two seconds and reported success. */
/* A quack the board would not stop leaves that board advertising, and no other
 * tool can open it until it is reset or re-plugged, so the run did not finish.
 * libubertooth has already named the refused request. */
MU_TEST(a_quack_whose_teardown_was_refused_exits_1)
{
	struct run_result r;

	reset_fake();
	fake.stop_refused = 1;
	r = RUN("-q");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(1, fake.stops);
	/* Every request still went out, and the duck still quacked. */
	mu_assert(strcmp(fake.calls, "casc") == 0, fake.calls);
	mu_assert(strstr(r.err, "Quacking ") != NULL, r.err);
	mu_assert(strstr(r.err, "error: ") == NULL, r.err);

	/* The same quack on a board that takes the stop. */
	reset_fake();
	r = RUN("-q");
	mu_assert_int_eq(0, r.rc);
}

MU_TEST(a_refused_request_is_named_and_exits_1)
{
	static const struct {
		const char *calls;   /* every request up to and including it */
		const char *message;
	} cases[] = {
		{ "c",    "error: could not set the advertising channel\n" },
		{ "ca",   "error: could not set the advertising data\n" },
		{ "cas",  "error: could not start advertising\n" },
		/* The channel restated once advertising has started: the board
		 * is advertising on a channel the user did not ask for, so the
		 * command ends there rather than quacking from it. */
		{ "casc", "error: could not set the advertising channel\n" },
	};
	struct run_result r;
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		const char *tail;

		reset_fake();
		/* LIBUSB_ERROR_PIPE, how a board refuses a request it does not
		 * implement. */
		if (i == 0)
			fake.channel_rc = -9;
		else if (i == 1)
			fake.adv_data_rc = -9;
		else if (i == 2)
			fake.slave_rc = -9;
		else
			fake.restated_channel_rc = -9;

		r = RUN("-q", "-a", "11:22:33:44:55:66");
		mu_assert_int_eq(1, r.rc);
		mu_assert(strcmp(fake.calls, cases[i].calls) == 0, fake.calls);
		mu_assert_int_eq(0, fake.sleeps);
		mu_assert_int_eq(1, fake.stops); /* the board is still released */
		mu_assert(r.out[0] == '\0', r.out);
		/* The duck and the "Quacking ..." line come first. These fakes print
		 * nothing else, so the tool's line is the tail here; on a board the
		 * library prints why it refused the request before it, and the stop
		 * that follows may print too. */
		mu_assert_fatal(strlen(r.err) > strlen(cases[i].message), r.err);
		tail = r.err + strlen(r.err) - strlen(cases[i].message);
		mu_assert(strcmp(tail, cases[i].message) == 0, r.err);
	}
}

/* -b signals the bootloader through the same requests, so it is checked the
 * same way. */
MU_TEST(the_bootloader_signal_is_checked_too)
{
	struct run_result r;

	reset_fake();
	fake.slave_rc = -9;
	r = RUN("-b", "-a", "11:22:33:44:55:66");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(fake.calls, "cas") == 0, fake.calls);
	mu_assert_int_eq(0, fake.sleeps);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strstr(r.err, "error: could not start advertising\n") != NULL,
	          r.err);
}

/* Without -a the address is read from /dev/urandom, which is the default way
 * the tool is used; the requests are checked there as well. */
MU_TEST(a_random_address_quack_is_checked_as_well)
{
	struct run_result r;

	reset_fake();
	fake.channel_rc = -9;
	r = RUN("-q");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(fake.calls, "c") == 0, fake.calls);
	mu_assert_int_eq(0, fake.sleeps);
}

/* Every exit path gives the device bookkeeping back, and a command line that
 * never reaches a board asks for none in the first place. run_ducky() checks
 * the balance for each run; this walks the paths so a regression names the one
 * that kept it. */
MU_TEST(every_exit_path_gives_the_device_back)
{
	struct run_result r;

	/* A refused command line: nothing is allocated, so there is nothing to
	 * give back. */
	reset_fake();
	r = RUN("-Z");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(0, fake.inits);

	reset_fake();
	r = RUN("-h");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(0, fake.inits);

	/* A UUID the tool cannot read is refused before a board is opened too. */
	reset_fake();
	r = RUN("-q", "nonsense");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(0, fake.inits);

	/* From here the tool has a device to give back. */
	reset_fake();
	fake.connect_rc = -1;
	r = RUN("-q");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(1, fake.inits);
	mu_assert_int_eq(1, fake.frees);

	/* A firmware version the host will not run with: the library has
	 * released the board and ended the libusb session. The tool closes it
	 * all the same, which does nothing on a board already released, and
	 * gives the memory back. */
	reset_fake();
	fake.api_rc = -1;
	fake.api_mismatch = 1;
	r = RUN("-q");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(1, fake.inits);
	mu_assert_int_eq(1, fake.frees);
	mu_assert_int_eq(1, fake.closes);

	/* A version it could not read at all leaves the board open, so the tool
	 * closes it rather than freeing the memory around a claimed board. No
	 * STOP: this firmware is not one the host will drive. */
	reset_fake();
	fake.api_rc = -1;
	r = RUN("-q");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(1, fake.frees);
	mu_assert_int_eq(1, fake.closes);
	mu_assert_int_eq(0, fake.stops);

	reset_fake();
	fake.adv_data_rc = -9;
	r = RUN("-q");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(1, fake.frees);

	reset_fake();
	r = RUN("-q", "-a", "11:22:33:44:55:66");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(1, fake.frees);
}

int main(void)
{
	MU_RUN(a_quack_the_board_took_sends_its_requests_and_exits_0);
	MU_RUN(a_stop_while_the_board_was_opened_quacks_nothing);
	MU_RUN(each_advertising_index_tunes_its_own_frequency);
	MU_RUN(a_quack_whose_teardown_was_refused_exits_1);
	MU_RUN(a_refused_request_is_named_and_exits_1);
	MU_RUN(the_bootloader_signal_is_checked_too);
	MU_RUN(a_random_address_quack_is_checked_as_well);
	MU_RUN(every_exit_path_gives_the_device_back);
	return mu_report();
}
