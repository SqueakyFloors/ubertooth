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

/* ---- scripted library --------------------------------------------------- */

static struct {
	int rx_afh_rc;    /* returned by rx_afh() */
	int rx_afh_calls;
	int rx_afh_r_calls;
	int last_timeout; /* timeout passed to the detection phase */
	int stops;
	int no_device;    /* no board matches -U */
	int check_api_rc; /* returned by ubertooth_check_api() */
	int no_memory;    /* btbb_piconet_new() cannot allocate */
	int piconet_news; /* btbb_piconet_new() calls */
	int piconet_frees;/* btbb_piconet_unref() calls */
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
unsigned int packet_counter_max = 5;

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

void register_cleanup_handler(ubertooth_t *ut, int do_exit)
{
	(void)ut;
	(void)do_exit;
}

int rx_afh(ubertooth_t *ut, btbb_piconet *pn, int timeout)
{
	(void)ut;
	(void)pn;
	fake.rx_afh_calls++;
	fake.last_timeout = timeout;
	return fake.rx_afh_rc;
}

void rx_afh_r(ubertooth_t *ut, btbb_piconet *pn, int timeout)
{
	(void)ut;
	(void)pn;
	fake.rx_afh_r_calls++;
	fake.last_timeout = timeout;
}

void ubertooth_stop(ubertooth_t *ut)
{
	(void)ut;
	fake.stops++;
}

void print_version(void) { printf("test\n"); }

/* ---- running the tool --------------------------------------------------- */

struct run_result {
	int rc;
	char out[4096];
	/* A refused command line prints the whole usage text here. */
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

static struct run_result run_afh(const char *const *args)
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
	result.rc = ubertooth_afh_main(argc, argv);
	fflush(stdout);
	fflush(stderr);
	dup2(saved_out, STDOUT_FILENO);
	dup2(saved_err, STDERR_FILENO);
	close(saved_out);
	close(saved_err);

	/* The tool owns the piconet for the whole run, so by the time main()
	 * returns it has given back whatever it allocated, on every path. */
	mu_assert_int_eq(fake.piconet_news, fake.piconet_frees);

	read_back(out, result.out, sizeof(result.out));
	read_back(err, result.err, sizeof(result.err));
	fclose(out);
	fclose(err);
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
MU_TEST(a_detected_map_still_exits_0_with_no_message)
{
	struct run_result r;

	reset_fake();
	r = RUN("-l", "446688", "-u", "22", "-t", "3");
	mu_assert_int_eq(0, r.rc);
	mu_assert(r.err[0] == '\0', r.err);
	mu_assert_int_eq(3, fake.last_timeout);
	mu_assert_int_eq(1, fake.stops);
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

/* A library error - libbtbb, or a receive that could not be started - is not
 * reported as "no packets", and it is not reported as success either. The
 * library has already said what failed. */
MU_TEST(a_library_error_is_not_mistaken_for_no_packets)
{
	static const int errors[] = { -1, -4 /* LIBUSB_ERROR_NO_DEVICE */ };
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

/* A command line with no piconet address does nothing, so it is refused: the
 * error line and the usage text on standard error, nothing on standard
 * output, exit 1, and no device opened. */
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
	mu_assert(strstr(r.err, "keeps monitoring") != NULL, r.err);
	mu_assert_int_eq(0, fake.rx_afh_calls);
}

int main(void)
{
	MU_RUN(no_packets_in_the_detection_phase_is_reported_and_exits_2);
	MU_RUN(the_message_names_the_piconet_and_the_timeout);
	MU_RUN(a_detected_map_still_exits_0_with_no_message);
	MU_RUN(no_timeout_runs_the_monitor_as_before);
	MU_RUN(the_r_format_is_unchanged);
	MU_RUN(a_library_error_is_not_mistaken_for_no_packets);
	MU_RUN(a_run_that_never_reaches_the_receive_phase_frees_the_piconet);
	MU_RUN(a_piconet_that_cannot_be_allocated_is_refused);
	MU_RUN(the_usage_text_documents_the_monitoring_and_the_exit_status);
	MU_RUN(a_missing_piconet_address_is_refused_on_standard_error);
	return mu_report();
}
