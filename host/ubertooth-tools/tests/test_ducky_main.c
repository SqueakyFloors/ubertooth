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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../../firmware/tests/minunit.h"

#define main ubertooth_ducky_main
#include "ubertooth-ducky.c"
#undef main

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
	/* Every device call in order, one letter each: c set channel,
	 * a set advertising data, s start advertising as a slave. */
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

ubertooth_t *ubertooth_init(void)
{
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

/* ---- running the tool --------------------------------------------------- */

struct run_result {
	int rc;
	char out[4096];
	/* The duck, the "Quacking ..." line and any error line land here. */
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

static struct run_result run_ducky(const char *const *args)
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
	optind = 0; /* restart option parsing for each run */
	result.rc = ubertooth_ducky_main(argc, argv);
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
	/* Checked for every case in this file, not just the cases below that
	 * name an exit path: a run must give back whatever it was handed, and
	 * nothing else. The tool used to keep it on every path. */
	mu_assert(fake.frees == fake.inits, "the run did not free its device");
	mu_assert(fake.inits == 0 || fake.last_freed == &fake_ut,
	          "the run freed something other than its device");
	return result;
}

#define RUN(...) run_ducky((const char *const[]){ "ubertooth-ducky", __VA_ARGS__, NULL })

/* ---- tests -------------------------------------------------------------- */

/* A quack the board took: the three requests in order, the advertisement the
 * tool has always sent, and nothing on standard output. */
MU_TEST(a_quack_the_board_took_sends_the_three_requests_and_exits_0)
{
	static const uint8_t UUID_LAST_BYTE_FIRST[] = { 0x0c, 0xc8, 0x2d, 0x7d };
	struct run_result r;

	reset_fake();
	r = RUN("-q", "-a", "11:22:33:44:55:66");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(fake.calls, "cas") == 0, fake.calls);
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
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strstr(r.err, "Quacking ") != NULL, r.err);
	mu_assert(strstr(r.err, "error: ") == NULL, r.err);
}

/* Each refused request is named, in one line on standard error, and ends the
 * command there: it used to be ignored, so a board that refused one still
 * slept for two seconds and reported success. */
MU_TEST(a_refused_request_is_named_and_exits_1)
{
	static const struct {
		const char *calls;   /* the requests made before the refusal */
		const char *message;
	} cases[] = {
		{ "c",   "error: could not set the advertising channel\n" },
		{ "ca",  "error: could not set the advertising data\n" },
		{ "cas", "error: could not start advertising\n" },
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
		else
			fake.slave_rc = -9;

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

/* -b signals the bootloader through the same three requests, so it is checked
 * the same way. */
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
	 * released the board and ended the libusb context, so the tool gives
	 * back the memory and nothing else. */
	reset_fake();
	fake.api_rc = -1;
	fake.api_mismatch = 1;
	r = RUN("-q");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(1, fake.inits);
	mu_assert_int_eq(1, fake.frees);
	mu_assert_int_eq(0, fake.closes);

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
	MU_RUN(a_quack_the_board_took_sends_the_three_requests_and_exits_0);
	MU_RUN(a_refused_request_is_named_and_exits_1);
	MU_RUN(the_bootloader_signal_is_checked_too);
	MU_RUN(a_random_address_quack_is_checked_as_well);
	MU_RUN(every_exit_path_gives_the_device_back);
	return mu_report();
}
