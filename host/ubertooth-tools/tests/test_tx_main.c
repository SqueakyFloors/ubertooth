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
	/* The stop a signal recorded while the board was being opened, which
	 * the tool reads before it asks anything of the board, and whether the
	 * open itself gave up because of it. */
	int stop_requested;
	int open_stopped;
	int library_closes; /* closes done inside the library, not by the tool */
	int stops, registrations, tx_syms, timeout_set;
	int tx_syms_rc;      /* what cmd_tx_syms() returns */
	/* The generic transmit: what the board answered, and what the last
	 * request carried. */
	int palevel_rc;      /* what cmd_get_palevel() returns */
	int palevel_reads;
	int generic_rc;      /* what cmd_tx_generic_packet() returns */
	int generic_packets;
	uint32_t generic_syncword;
	uint16_t generic_channel;
	uint8_t generic_pa_level;
	uint8_t generic_payload[GENERIC_TX_MAX_PAYLOAD];
	size_t generic_len;
	/* 1 to leave the handle's stop flag clear, so a run that transmits
	 * reaches its loop. The fake signal registration below sets the flag
	 * by default, which is what ends the dump retransmit's wait at once. */
	int keep_running;
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

/* Installed before the board is opened, so every case here reaches it. */
void ubertooth_catch_stop_signals(void)
{
	fake.registrations++;
	/* The real handler ends the transmit wait on a signal; here it ends it
	 * at once, so a case that reaches the transmit runs the whole of it
	 * without waiting a second. A generic transmit reads the same flag
	 * between packets, so a case that means to send some clears it. */
	fake_ut.stop_ubertooth = fake.keep_running ? 0 : 1;
}

int cmd_get_palevel(struct libusb_device_handle* devh)
{
	(void)devh;
	fake.palevel_reads++;
	return fake.palevel_rc;
}

int cmd_tx_generic_packet(struct libusb_device_handle* devh, uint32_t syncword,
                          uint16_t channel, uint8_t pa_level,
                          const uint8_t* data, size_t data_len)
{
	(void)devh;
	fake.generic_packets++;
	fake.generic_syncword = syncword;
	fake.generic_channel = channel;
	fake.generic_pa_level = pa_level;
	mu_assert(data_len <= sizeof(fake.generic_payload),
	          "the payload fits what the request can carry");
	if (data_len > sizeof(fake.generic_payload))
		return -1;
	memcpy(fake.generic_payload, data, data_len);
	fake.generic_len = data_len;
	return fake.generic_rc;
}

int ubertooth_stop_requested(void)
{
	return fake.stop_requested;
}

/* Whether the fake open gave up because the run was stopped, which is what
 * decides the exit status of a run that opened no board. */
int ubertooth_open_stopped(void)
{
	return fake.open_stopped;
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
	char* payload_without_value[] = { "ubertooth-tx", "-g", NULL };
	char* odd_payload[] = { "ubertooth-tx", "-g", "010", NULL };
	char* empty_payload[] = { "ubertooth-tx", "-g", "", NULL };
	char* payload_too_long[] = { "ubertooth-tx", "-g",
	                             /* 51 bytes: one past what the request holds */
	                             "000102030405060708090a0b0c0d0e0f"
	                             "101112131415161718191a1b1c1d1e1f"
	                             "202122232425262728292a2b2c2d2e2f"
	                             "3031323334353637", NULL };
	char* short_syncword[] = { "ubertooth-tx", "-g", "0102", "-a", "cb87", NULL };
	char* payload_without_syncword[] = { "ubertooth-tx", "-g", "0102", NULL };
	char* syncword_without_payload[] = { "ubertooth-tx", "-a", "cb876543", NULL };
	char* channel_without_payload[] = { "ubertooth-tx", "-c", "2441", NULL };
	char* count_without_payload[] = { "ubertooth-tx", "-n", "5", NULL };
	char* channel_out_of_band[] = { "ubertooth-tx", "-g", "0102",
	                                "-a", "cb876543", "-c", "2500", NULL };
	char* no_packets[] = { "ubertooth-tx", "-g", "0102",
	                       "-a", "cb876543", "-n", "0", NULL };
	char* generic_with_lap[] = { "ubertooth-tx", "-g", "0102", "-a", "cb876543",
	                             "-l", "123456", NULL };
	char* generic_with_timeout[] = { "ubertooth-tx", "-g", "0102",
	                                 "-a", "cb876543", "-t", "5", NULL };
	char* generic_with_uap[] = { "ubertooth-tx", "-g", "0102", "-a", "cb876543",
	                             "-u", "22", NULL };

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
	run_main(&r, 2, payload_without_value);
	mu_assert_int_eq(1, r.rc);
	run_main(&r, 3, odd_payload);
	mu_assert_int_eq(1, r.rc);
	run_main(&r, 3, empty_payload);
	mu_assert_int_eq(1, r.rc);
	run_main(&r, 3, payload_too_long);
	mu_assert_int_eq(1, r.rc);
	run_main(&r, 5, short_syncword);
	mu_assert_int_eq(1, r.rc);
	run_main(&r, 3, payload_without_syncword);
	mu_assert_int_eq(1, r.rc);
	run_main(&r, 3, syncword_without_payload);
	mu_assert_int_eq(1, r.rc);
	run_main(&r, 3, channel_without_payload);
	mu_assert_int_eq(1, r.rc);
	run_main(&r, 3, count_without_payload);
	mu_assert_int_eq(1, r.rc);
	run_main(&r, 7, channel_out_of_band);
	mu_assert_int_eq(1, r.rc);
	run_main(&r, 7, no_packets);
	mu_assert_int_eq(1, r.rc);
	run_main(&r, 7, generic_with_lap);
	mu_assert_int_eq(1, r.rc);
	run_main(&r, 7, generic_with_timeout);
	mu_assert_int_eq(1, r.rc);
	run_main(&r, 7, generic_with_uap);
	mu_assert_int_eq(1, r.rc);

	mu_assert_int_eq(0, fake.inits);
	mu_assert_int_eq(0, fake.connects);
	mu_assert_int_eq(0, fake.frees);
	mu_assert_int_eq(0, fake.generic_packets);
}

/* The whole of what each refusal says, which ubertooth-tx(1) quotes. A
 * reworded message with the page left alone is what this catches. */
MU_TEST(the_generic_refusals_read_as_the_page_says)
{
	char* nothing[] = { "ubertooth-tx", "-U", "0", NULL };
	char* without_syncword[] = { "ubertooth-tx", "-g", "0102", NULL };
	char* syncword_alone[] = { "ubertooth-tx", "-a", "cb876543", NULL };
	char* with_lap[] = { "ubertooth-tx", "-g", "0102", "-a", "cb876543",
	                     "-l", "123456", NULL };
	char* with_uap[] = { "ubertooth-tx", "-g", "0102", "-a", "cb876543",
	                     "-u", "11", NULL };
	char* with_timeout[] = { "ubertooth-tx", "-g", "0102", "-a", "cb876543",
	                         "-t", "5", NULL };
	char* bad_payload[] = { "ubertooth-tx", "-g", "010", NULL };
	char* bad_syncword[] = { "ubertooth-tx", "-g", "0102", "-a", "cb87", NULL };
	struct run r;

	reset();
	run_main(&r, 3, nothing);
	mu_assert(strcmp(r.err, "error: nothing to do: give -g to transmit a"
	                 " generic packet, or -l and -u to retransmit to a"
	                 " piconet\n") == 0, r.err);
	run_main(&r, 3, without_syncword);
	mu_assert(strcmp(r.err, "error: -g needs -a <syncword>: the board frames"
	                 " the generic packet with it\n") == 0, r.err);
	run_main(&r, 3, syncword_alone);
	mu_assert(strcmp(r.err, "error: -a, -c and -n are only used with -g,"
	                 " which transmits a generic packet\n") == 0, r.err);
	run_main(&r, 7, with_lap);
	mu_assert(strcmp(r.err, "error: -g cannot be combined with -l; a generic"
	                 " packet carries no piconet address\n") == 0, r.err);
	run_main(&r, 7, with_uap);
	mu_assert(strcmp(r.err, "error: -g cannot be combined with -u; a generic"
	                 " packet carries no piconet address\n") == 0, r.err);
	run_main(&r, 7, with_timeout);
	mu_assert(strcmp(r.err, "error: -g cannot be combined with -t; a generic"
	                 " transmit ends when the packets asked for have gone"
	                 " out\n") == 0, r.err);
	run_main(&r, 3, bad_payload);
	mu_assert(strcmp(r.err, "error: a generic payload must be hex digits, two"
	                 " per byte, 1 to 50 bytes (got 010)\n") == 0, r.err);
	run_main(&r, 5, bad_syncword);
	mu_assert(strcmp(r.err, "error: sync word must be 8 hex digits"
	                 " (got cb87)\n") == 0, r.err);
}

/* Half an address is refused the same way whether or not the command line
 * also carries one of the generic transmit's options, so the message a user
 * sees does not depend on which other option they happened to write. */
MU_TEST(half_an_address_reads_the_same_either_way)
{
	char* lap_only[] = { "ubertooth-tx", "-l", "123456", NULL };
	char* lap_with_syncword[] = { "ubertooth-tx", "-l", "123456",
	                              "-a", "cb876543", NULL };
	struct run r;
	char first[sizeof(r.err)];

	reset();
	run_main(&r, 3, lap_only);
	mu_assert_int_eq(1, r.rc);
	snprintf(first, sizeof(first), "%s", r.err);
	run_main(&r, 5, lap_with_syncword);
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(first, r.err) == 0, r.err);
}

/* The generic transmit: one request per packet, each carrying the sync word,
 * the channel, the board's own power level and the payload. Nothing is
 * printed, and no mode is started, so there is nothing to stop. */
MU_TEST(a_generic_transmit_sends_one_request_per_packet)
{
	char* argv[] = { "ubertooth-tx", "-g", "0102030405", "-a", "cb876543",
	                 "-c", "2441", "-n", "3", NULL };
	struct run r;

	reset();
	fake.connect_rc = 0;
	fake.palevel_rc = 2;
	fake.keep_running = 1;
	run_main(&r, 9, argv);

	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(3, fake.generic_packets);
	mu_assert_int_eq(1, fake.palevel_reads);
	mu_assert_int_eq(0xcb876543UL, fake.generic_syncword);
	mu_assert_int_eq(2441, fake.generic_channel);
	mu_assert_int_eq(2, fake.generic_pa_level);
	mu_assert_int_eq(5, (int)fake.generic_len);
	mu_assert(memcmp(fake.generic_payload, "\x01\x02\x03\x04\x05", 5) == 0,
	          "the payload reaches the request as it was written");
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(r.err[0] == '\0', r.err);
	mu_assert_int_eq(0, fake.stops);
	mu_assert_int_eq(0, fake.tx_syms);
	mu_assert_int_eq(0, fake.bdaddr_sets);
	mu_assert_int_eq(1, fake.closes);
	mu_assert_int_eq(1, fake.frees);
}

/* One packet, the bottom of the band, with no -c or -n to say so. */
MU_TEST(a_generic_transmit_defaults_to_one_packet_at_2402)
{
	char* argv[] = { "ubertooth-tx", "-g", "ff", "-a", "00000001", NULL };
	struct run r;

	reset();
	fake.connect_rc = 0;
	fake.keep_running = 1;
	run_main(&r, 5, argv);

	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(1, fake.generic_packets);
	mu_assert_int_eq(BT_FREQ_MIN, fake.generic_channel);
	mu_assert_int_eq(1UL, fake.generic_syncword);
}

/* A board that will not answer either request is a failed run, and nothing is
 * transmitted when the power level could not be read: that level is part of
 * the request. */
MU_TEST(a_refused_generic_request_exits_one)
{
	char* argv[] = { "ubertooth-tx", "-g", "0102", "-a", "cb876543",
	                 "-n", "4", NULL };
	struct run r;

	reset();
	fake.connect_rc = 0;
	fake.palevel_rc = -1;
	fake.keep_running = 1;
	run_main(&r, 7, argv);
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(0, fake.generic_packets);
	mu_assert_int_eq(1, fake.frees);

	/* And a transmit the board refuses part-way stops there, saying how far
	 * it got: the fake board refuses the first packet, so none were taken.
	 * ubertooth-tx(1), EXIT STATUS quotes this line. */
	reset();
	fake.connect_rc = 0;
	fake.generic_rc = -1;
	fake.keep_running = 1;
	run_main(&r, 7, argv);
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(1, fake.generic_packets);
	mu_assert_int_eq(1, fake.frees);
	mu_assert(strcmp(r.err, "error: the board took 0 of 4 packets\n") == 0,
	          r.err);
}

/* Ctrl-C between packets ends the run: the rest are not sent, and an
 * interrupted run is not a failure. */
MU_TEST(a_stopped_generic_transmit_sends_no_more_packets)
{
	char* argv[] = { "ubertooth-tx", "-g", "0102", "-a", "cb876543",
	                 "-n", "100", NULL };
	struct run r;

	reset();
	fake.connect_rc = 0;
	run_main(&r, 7, argv);

	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(0, fake.generic_packets);
	mu_assert_int_eq(1, fake.closes);
	mu_assert_int_eq(1, fake.frees);
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
	                   "-U", "dd44", NULL };
	char* without_u[] = { "ubertooth-tx", "-l", "123456", "-u", "11", NULL };
	struct run r;

	reset();
	run_main(&r, 7, with_u);
	mu_assert(strcmp(fake.selector, "dd44") == 0, fake.selector);

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

/* A stop that arrived while the board was being opened ends the run before
 * anything is transmitted. The handlers go in before the open now, so such a
 * signal is no longer discarded, and it must not be left to the transmit loop
 * to notice after the board is already sending. */
MU_TEST(a_stop_while_the_board_was_opened_transmits_nothing)
{
	char* argv[] = { "ubertooth-tx", "-l", "123456", "-u", "11", NULL };
	struct run r;

	reset();
	fake.connect_rc = 0;
	fake.stop_requested = 1;
	run_main(&r, 5, argv);

	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(1, fake.registrations);
	mu_assert_int_eq(0, fake.bdaddr_sets);
	mu_assert_int_eq(1, fake.closes);
	mu_assert_int_eq(1, fake.frees);
}

/* A board selection the user stopped is not a failure to find a board: exit 0,
 * nothing printed, nothing transmitted. Any other failed selection exits 1. */
MU_TEST(a_selection_the_user_stopped_exits_zero)
{
	char* argv[] = { "ubertooth-tx", "-l", "123456", "-u", "11", NULL };
	struct run r;

	reset();
	fake.open_stopped = 1;
	run_main(&r, 5, argv);
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(0, fake.bdaddr_sets);
	mu_assert_int_eq(1, fake.frees);

	reset();
	run_main(&r, 5, argv);
	mu_assert_int_eq(1, r.rc);
}

int main(void)
{
	/* Neither binary links libubertooth: the fakes above are the whole
	 * device, and any new library call the tool grows becomes a link
	 * error rather than a silently real one. */
	MU_RUN(the_refusals_allocate_nothing);
	MU_RUN(the_generic_refusals_read_as_the_page_says);
	MU_RUN(half_an_address_reads_the_same_either_way);
	MU_RUN(a_generic_transmit_sends_one_request_per_packet);
	MU_RUN(a_generic_transmit_defaults_to_one_packet_at_2402);
	MU_RUN(a_refused_generic_request_exits_one);
	MU_RUN(a_stopped_generic_transmit_sends_no_more_packets);
	MU_RUN(help_and_version_allocate_nothing);
	MU_RUN(a_failed_connection_gives_the_handle_back);
	MU_RUN(the_device_selector_is_passed_through);
	MU_RUN(a_handle_that_cannot_be_allocated_stops_the_command);
	MU_RUN(a_refused_firmware_version_is_closed_and_freed);
	MU_RUN(a_version_that_cannot_be_read_closes_and_frees);
	MU_RUN(a_transmit_whose_teardown_was_refused_exits_one);
	MU_RUN(a_stop_while_the_board_was_opened_transmits_nothing);
	MU_RUN(a_selection_the_user_stopped_exits_zero);
	MU_RUN(an_address_the_board_refuses_releases_it);
	return mu_report();
}
