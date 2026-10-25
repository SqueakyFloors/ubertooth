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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../../firmware/tests/minunit.h"

#define main ubertooth_ego_main
#include "ubertooth-ego.c"
#undef main

/* ---- scripted device ---------------------------------------------------- */

static struct {
	int channel_rc;      /* returned by cmd_set_channel() */
	int set_aa_rc;       /* returned by cmd_rfcat_subcmd(RFCAT_SET_AA) */
	int cap_len_rc;      /* returned by cmd_rfcat_subcmd(RFCAT_CAP_LEN) */
	int ego_rc;          /* returned by cmd_ego() */
	int poll_rc;         /* returned by cmd_poll() */
	int last_channel;    /* the channel the capture was set to, in MHz */
	uint8_t last_access_code[8];
	int last_access_code_len;
	uint8_t last_cap_len;
	int last_ego_mode;
	int stops;
	/* Every device call in order, one letter each: c set channel, a set
	 * access code, l set capture length, e start the E-GO mode. */
	char calls[16];
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

ubertooth_t *ubertooth_start_select(const char *u_arg)
{
	(void)u_arg;
	return &fake_ut;
}

int ubertooth_check_api(ubertooth_t *ut)
{
	(void)ut;
	return 0;
}

void ubertooth_stop(ubertooth_t *ut)
{
	(void)ut;
	fake.stops++;
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
 * callback's own tests, and nothing here should wait on a board. */
int cmd_poll(struct libusb_device_handle *devh, usb_pkt_rx *p)
{
	(void)devh;
	(void)p;
	running = 0;
	return fake.poll_rc;
}

void fifo_push(fifo_t *fifo, const usb_pkt_rx *packet)
{
	(void)fifo;
	(void)packet;
}

void cb_ego(ubertooth_t *ut, void *args)
{
	(void)ut;
	(void)args;
}

/* ---- running the tool --------------------------------------------------- */

struct run_result {
	int rc;
	char out[4096];
	char err[4096];
};

static void read_back(FILE *file, char *buffer, size_t size)
{
	size_t length;

	fflush(file);
	rewind(file);
	length = fread(buffer, 1, size - 1, file);
	buffer[length] = '\0';
	/* A capture that did not fit must fail loudly instead of comparing a
	 * prefix. */
	mu_assert(fgetc(file) == EOF, "captured output was truncated");
}

static struct run_result run_ego(const char *const *args)
{
	struct run_result result;
	char *argv[16];
	int argc = 0;
	int saved_out, saved_err;
	FILE *out = tmpfile();
	FILE *err = tmpfile();

	memset(&result, 0, sizeof(result));
	result.rc = -1; /* a failed capture must not look like a successful run */
	mu_assert(out != NULL && err != NULL, "tmpfile failed");
	if (out == NULL || err == NULL)
		return result;
	while (args[argc] != NULL && argc < 15) {
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
	optind = 0;  /* restart option parsing for each run */
	running = 1; /* the previous run's faked poll left it at 0 */
	result.rc = ubertooth_ego_main(argc, argv);
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
	mu_assert_int_eq(1, fake.stops);
	mu_assert(strstr(r.err, "error: ") == NULL, r.err);
}

/* Following connections hops on the board, so no channel is set, and without
 * -a the firmware keeps its default access code: the capture length is the
 * only setup request. */
MU_TEST(following_sets_only_the_capture_length)
{
	struct run_result r;

	reset_fake();
	r = RUN("-f");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(fake.calls, "le") == 0, fake.calls);
	mu_assert_int_eq(18, fake.last_cap_len); /* the default */
}

/* Each refused setup request is named, in one line on standard error, and ends
 * the command there: they used to be ignored, so a board that refused one
 * captured on whatever channel, access code or length it was left on and
 * reported success. */
MU_TEST(a_refused_setup_request_is_named_and_exits_1)
{
	static const struct {
		const char *calls;   /* the requests made before the refusal */
		const char *message;
	} cases[] = {
		{ "c",   "error: could not set the channel\n" },
		{ "ca",  "error: could not set the access code\n" },
		{ "cal", "error: could not set the capture length\n" },
	};
	struct run_result r;
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		reset_fake();
		/* LIBUSB_ERROR_PIPE, how a board refuses a request it does not
		 * implement. */
		if (i == 0)
			fake.channel_rc = -9;
		else if (i == 1)
			fake.set_aa_rc = -9;
		else
			fake.cap_len_rc = -9;

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
 * the only setup request it sends: no channel is set even though -c was
 * given. Every device call here is faked, so nothing is transmitted. */
MU_TEST(interfering_sets_only_the_capture_length)
{
	struct run_result r;

	reset_fake();
	r = RUN("-i", "-c", "2408");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(fake.calls, "le") == 0, fake.calls);
	mu_assert_int_eq(2, fake.last_ego_mode);
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
	MU_RUN(a_refused_setup_request_is_named_and_exits_1);
	MU_RUN(the_access_code_request_is_only_sent_for_a);
	MU_RUN(a_firmware_without_ego_is_still_reported);
	MU_RUN(interfering_sets_only_the_capture_length);
	MU_RUN(a_failed_poll_ends_the_capture_with_1);
	return mu_report();
}
