/*
 * Hardware-independent tests of ubertooth-tx's main(): which exits allocate a
 * handle at all, and whether a handle that was allocated is given back.
 *
 * ubertooth-tx.c is compiled into this test with main() renamed, and the
 * libubertooth functions it calls are replaced by the fakes below. Nothing
 * here reaches the transmit path: the scripted device refuses the connection,
 * so main() never gets as far as its wait loop.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../../firmware/tests/minunit.h"

#define main ubertooth_tx_main
#include "ubertooth-tx.c"
#undef main

/* ---- scripted device -------------------------------------------------- */

static struct {
	int fail_init;      /* ubertooth_init() returns NULL */
	int connect_rc;     /* what ubertooth_connect_select() returns */
	int api_rc;         /* what ubertooth_check_api() returns */
	int set_bdaddr_rc;  /* what cmd_set_bdaddr() returns */
	/* a refused version is closed inside ubertooth_check_api(); a version
	 * that could not be read is not */
	int api_version_was_read;
	int inits, frees, closes, connects, api_checks, bdaddr_sets;
	int library_closes; /* closes done inside the library, not by the tool */
	int stops, registrations, tx_syms, timeout_set;
	int tx_syms_rc;      /* what cmd_tx_syms() returns */
	/* 1 for the cases that are meant to run a whole transmit; the fakes
	 * below refuse to be reached without it. */
	int reach_transmit;
	char selector[64];
	/* 1 to have the teardown's STOP refused, so the board is left running
	 * the mode the capture started. */
	int stop_refused;
} fake;

static ubertooth_t fake_ut;

ubertooth_t* ubertooth_init(void)
{
	fake.inits++;
	if (fake.fail_init)
		return NULL;
	memset(&fake_ut, 0, sizeof(fake_ut));
	return &fake_ut;
}

void ubertooth_free(ubertooth_t* ut)
{
	mu_assert(ut == &fake_ut, "frees the handle it was given");
	fake.frees++;
}

int ubertooth_connect_select(ubertooth_t* ut, const char* u_arg)
{
	mu_assert(ut == &fake_ut, "connects the handle it was given");
	fake.connects++;
	snprintf(fake.selector, sizeof(fake.selector), "%s", u_arg ? u_arg : "");
	if (fake.connect_rc == 0)
		/* As the real one leaves it. Not a real handle: nothing here
		 * dereferences devh, it is only passed through to the faked
		 * requests and tested for NULL. */
		ut->devh = (struct libusb_device_handle*)&fake_ut;
	return fake.connect_rc;
}

/* The real one closes the device itself when it refuses the version it read,
 * and does not when it could not read a version at all. That difference is
 * what the tool's cleanup turns on, so the fake keeps it. */
int ubertooth_check_api(ubertooth_t* ut)
{
	mu_assert(ut == &fake_ut, "checks the handle it was given");
	fake.api_checks++;
	if (fake.api_rc < 0 && fake.api_version_was_read) {
		ut->devh = NULL; /* as ubertooth_stop() leaves it */
		fake.library_closes++;
	}
	return fake.api_rc;
}

/* Reached only on the transmit path. Only a case that sets
 * fake.reach_transmit may take it; the rest fail loudly if they get that far,
 * which is how a reordering that opens the device too early shows up. */
void ubertooth_stop(ubertooth_t* ut)
{
	(void)ut;
	fake.stops++;
	mu_assert(fake.reach_transmit, "no test should reach the transmit path");
}

/* Whether the teardown's STOP was refused, which leaves the board running the
 * mode the capture started. Scripted, because the fake board above always
 * takes it. */
int ubertooth_stop_failed(ubertooth_t* ut)
{
	(void)ut;
	return fake.stop_refused;
}

/* The real one folds the signal handler's own flag into the handle; the
 * fake handler below sets the handle directly, so reading it is enough here. */
int ubertooth_stopping(ubertooth_t* ut)
{
	return ut->stop_ubertooth;
}

void register_cleanup_handler(ubertooth_t* ut, int do_exit)
{
	(void)do_exit;
	fake.registrations++;
	mu_assert(fake.reach_transmit, "no test should reach the transmit path");
	/* The real handler ends the wait on a signal; here it ends it at once,
	 * so a case can run the whole transmit without waiting a second. */
	ut->stop_ubertooth = 1;
}

void ubertooth_set_timeout(ubertooth_t* ut, int seconds)
{
	(void)ut;
	fake.timeout_set = seconds;
	mu_assert(fake.reach_transmit, "no test should reach the transmit path");
}

void ubertooth_close(ubertooth_t* ut)
{
	mu_assert(ut == &fake_ut, "closes the handle it was given");
	fake.closes++;
	ut->devh = NULL; /* as the real one leaves it */
}

int cmd_set_bdaddr(struct libusb_device_handle* devh, uint64_t address)
{
	(void)devh;
	(void)address;
	fake.bdaddr_sets++;
	return fake.set_bdaddr_rc;
}

int cmd_tx_syms(struct libusb_device_handle* devh)
{
	(void)devh;
	fake.tx_syms++;
	mu_assert(fake.reach_transmit, "nothing in this test transmits");
	return fake.tx_syms_rc;
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

/* ---- running main() --------------------------------------------------- */

struct run {
	int rc;
	char out[4096];
	char err[4096];
};

static void run_main(struct run* result, int argc, char* argv[])
{
	FILE* out = tmpfile();
	FILE* err = tmpfile();
	int saved_out = out != NULL ? dup(fileno(stdout)) : -1;
	int saved_err = err != NULL ? dup(fileno(stderr)) : -1;
	size_t n;

	memset(result, 0, sizeof(*result));
	mu_assert_fatal(out != NULL && err != NULL && saved_out >= 0
	                && saved_err >= 0, "capture the streams");

	fflush(stdout);
	fflush(stderr);
	dup2(fileno(out), fileno(stdout));
	dup2(fileno(err), fileno(stderr));

	/* restart option parsing for each run */
	optind = 0;
	result->rc = ubertooth_tx_main(argc, argv);

	fflush(stdout);
	fflush(stderr);
	dup2(saved_out, fileno(stdout));
	dup2(saved_err, fileno(stderr));
	close(saved_out);
	close(saved_err);

	rewind(out);
	n = fread(result->out, 1, sizeof(result->out) - 1, out);
	result->out[n] = '\0';
	rewind(err);
	n = fread(result->err, 1, sizeof(result->err) - 1, err);
	result->err[n] = '\0';
	fclose(out);
	fclose(err);
}

static void reset(void)
{
	memset(&fake, 0, sizeof(fake));
	fake.connect_rc = -1;    /* no board, so nothing transmits */
	fake.set_bdaddr_rc = -1; /* and if one is reached, it gets no address */
}

/* ---- tests ------------------------------------------------------------ */

/* The handle used to be allocated before the command line was read, so each of
 * these refusals left a handle and its 72 MB fifo outstanding. One case per
 * kind of refusal the tool has: an unknown option, an option whose value is
 * missing, an option whose value is refused, a stray word, and a required
 * option that was not given. */
MU_TEST(the_refusals_allocate_nothing)
{
	struct run r;
	char* bad_option[] = { "ubertooth-tx", "-Z", NULL };
	char* lap_without_value[] = { "ubertooth-tx", "-l", NULL };
	char* uap_without_value[] = { "ubertooth-tx", "-u", NULL };
	char* device_without_value[] = { "ubertooth-tx", "-U", NULL };
	char* timeout_without_value[] = { "ubertooth-tx", "-t", NULL };
	char* bad_lap[] = { "ubertooth-tx", "-l", "zzz", NULL };
	char* bad_uap[] = { "ubertooth-tx", "-l", "123456", "-u", "zz", NULL };
	char* bad_timeout[] = { "ubertooth-tx", "-t", "-5", NULL };
	char* stray_word[] = { "ubertooth-tx", "-l", "123456", "-u", "11", "spare", NULL };
	char* no_address[] = { "ubertooth-tx", NULL };
	char* lap_only[] = { "ubertooth-tx", "-l", "123456", NULL };
	char* uap_only[] = { "ubertooth-tx", "-u", "11", NULL };

	reset();
	run_main(&r, 2, bad_option);
	mu_assert_int_eq(1, r.rc);
	run_main(&r, 2, lap_without_value);
	mu_assert_int_eq(1, r.rc);
	run_main(&r, 2, uap_without_value);
	mu_assert_int_eq(1, r.rc);
	run_main(&r, 2, device_without_value);
	mu_assert_int_eq(1, r.rc);
	run_main(&r, 2, timeout_without_value);
	mu_assert_int_eq(1, r.rc);
	run_main(&r, 3, bad_lap);
	mu_assert_int_eq(1, r.rc);
	/* A refused command line writes nothing to standard output. */
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(r.err[0] != '\0', "and says why on stderr");
	run_main(&r, 5, bad_uap);
	mu_assert_int_eq(1, r.rc);
	run_main(&r, 3, bad_timeout);
	mu_assert_int_eq(1, r.rc);
	run_main(&r, 6, stray_word);
	mu_assert_int_eq(1, r.rc);
	run_main(&r, 1, no_address);
	mu_assert_int_eq(1, r.rc);
	run_main(&r, 3, lap_only);
	mu_assert_int_eq(1, r.rc);
	run_main(&r, 3, uap_only);
	mu_assert_int_eq(1, r.rc);

	mu_assert_int_eq(0, fake.inits);
	mu_assert_int_eq(0, fake.connects);
	mu_assert_int_eq(0, fake.frees);
}

/* -h and -V are the same: they end the command before a handle exists. */
MU_TEST(help_and_version_allocate_nothing)
{
	struct run r;
	char* help[] = { "ubertooth-tx", "-h", NULL };
	char* version[] = { "ubertooth-tx", "-V", NULL };

	reset();
	run_main(&r, 2, help);
	mu_assert_int_eq(0, r.rc);
	mu_assert(strstr(r.out, "Usage:") != NULL, r.out);
	run_main(&r, 2, version);
	mu_assert_int_eq(0, r.rc);

	mu_assert_int_eq(0, fake.inits);
	mu_assert_int_eq(0, fake.frees);
}

/* An accepted command line allocates one handle, and a board that cannot be
 * connected gives it straight back. */
MU_TEST(a_failed_connection_gives_the_handle_back)
{
	char* argv[] = { "ubertooth-tx", "-l", "123456", "-u", "11", NULL };
	struct run r;

	reset();
	run_main(&r, 5, argv);

	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(1, fake.inits);
	mu_assert_int_eq(1, fake.connects);
	mu_assert_int_eq(1, fake.frees);
	mu_assert_int_eq(0, fake.api_checks);
}

/* The -U value reaches the library as the user wrote it, and no device is
 * selected at all when the option is absent. */
MU_TEST(the_device_selector_is_passed_through)
{
	char* with_u[] = { "ubertooth-tx", "-l", "123456", "-u", "11",
	                   "-U", "0b70", NULL };
	char* without_u[] = { "ubertooth-tx", "-l", "123456", "-u", "11", NULL };
	struct run r;

	reset();
	run_main(&r, 7, with_u);
	mu_assert(strcmp(fake.selector, "0b70") == 0, fake.selector);

	reset();
	run_main(&r, 5, without_u);
	mu_assert(strcmp(fake.selector, "") == 0, fake.selector);
}

/* A handle that cannot be allocated is reported, not dereferenced. */
MU_TEST(a_handle_that_cannot_be_allocated_stops_the_command)
{
	char* argv[] = { "ubertooth-tx", "-l", "123456", "-u", "11", NULL };
	struct run r;

	reset();
	fake.fail_init = 1;
	run_main(&r, 5, argv);

	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(1, fake.inits);
	mu_assert_int_eq(0, fake.connects);
	mu_assert_int_eq(0, fake.frees);
}

/* A refused firmware version: the library has already closed the device. The
 * tool closes and frees the handle on both of the check's failure paths,
 * because closing a device that is already closed does nothing -- it no
 * longer exits libusb a second time. */
MU_TEST(a_refused_firmware_version_is_closed_and_freed)
{
	char* argv[] = { "ubertooth-tx", "-l", "123456", "-u", "11", NULL };
	struct run r;

	reset();
	fake.connect_rc = 0;
	fake.api_rc = -1;
	fake.api_version_was_read = 1;
	run_main(&r, 5, argv);

	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(1, fake.api_checks);
	mu_assert_int_eq(1, fake.library_closes);
	mu_assert_int_eq(1, fake.closes); /* harmless: already released */
	mu_assert_int_eq(1, fake.frees);
}

/* A version that could not be read at all: the library left the device open,
 * so this is the path where the tool closes it, without STOP. */
MU_TEST(a_version_that_cannot_be_read_closes_and_frees)
{
	char* argv[] = { "ubertooth-tx", "-l", "123456", "-u", "11", NULL };
	struct run r;

	reset();
	fake.connect_rc = 0;
	fake.api_rc = -1;
	fake.api_version_was_read = 0;
	run_main(&r, 5, argv);

	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(0, fake.library_closes);
	mu_assert_int_eq(1, fake.closes);
	mu_assert_int_eq(1, fake.frees);
}

/* A transmit the board would not stop leaves that board sending, and no other
 * tool can open it until it is reset or re-plugged, so the run did not do what
 * was asked. libubertooth has already named the refused request. */
MU_TEST(a_transmit_whose_teardown_was_refused_exits_one)
{
	char* argv[] = { "ubertooth-tx", "-l", "123456", "-u", "11", NULL };
	struct run r;

	reset();
	fake.connect_rc = 0;
	fake.set_bdaddr_rc = 0;
	fake.reach_transmit = 1;
	fake.stop_refused = 1;
	run_main(&r, 5, argv);
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(1, fake.tx_syms);
	mu_assert_int_eq(1, fake.stops);
	mu_assert_int_eq(1, fake.frees);

	/* The same transmit on a board that takes the stop. */
	reset();
	fake.connect_rc = 0;
	fake.set_bdaddr_rc = 0;
	fake.reach_transmit = 1;
	run_main(&r, 5, argv);
	mu_assert_int_eq(0, r.rc);
}

/* The last exit before the tool starts transmitting: the board is open and
 * compatible, but it would not take the address. No signal handler holds the
 * handle yet, so this one does release the board as well as the handle. */
MU_TEST(an_address_the_board_refuses_releases_it)
{
	char* argv[] = { "ubertooth-tx", "-l", "123456", "-u", "11", NULL };
	struct run r;

	reset();
	fake.connect_rc = 0;
	fake.api_rc = 0;
	run_main(&r, 5, argv);

	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(1, fake.bdaddr_sets);
	mu_assert_int_eq(1, fake.closes);
	mu_assert_int_eq(1, fake.frees);
}

int main(void)
{
	/* Neither binary links libubertooth: the fakes above are the whole
	 * device, and any new library call the tool grows becomes a link
	 * error rather than a silently real one. */
	MU_RUN(the_refusals_allocate_nothing);
	MU_RUN(help_and_version_allocate_nothing);
	MU_RUN(a_failed_connection_gives_the_handle_back);
	MU_RUN(the_device_selector_is_passed_through);
	MU_RUN(a_handle_that_cannot_be_allocated_stops_the_command);
	MU_RUN(a_refused_firmware_version_is_closed_and_freed);
	MU_RUN(a_version_that_cannot_be_read_closes_and_frees);
	MU_RUN(a_transmit_whose_teardown_was_refused_exits_one);
	MU_RUN(an_address_the_board_refuses_releases_it);
	return mu_report();
}
