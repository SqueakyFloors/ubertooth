/*
 * Hardware-independent tests of ubertooth-btle's main(): which device
 * commands the -v options run, in which order, and what the tool prints and
 * exits with.
 *
 * ubertooth-btle.c is compiled into this test with main() renamed, and the
 * libubertooth functions it calls are replaced by the fakes below, so the real
 * command-line handling and reporting run against a scripted device.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../../firmware/tests/minunit.h"

#define main ubertooth_btle_main
#include "ubertooth-btle.c"
#undef main

/* ---- scripted device ---------------------------------------------------- */

static struct {
	int set_crc_rc;      /* returned by cmd_set_crc_verify() */
	int get_crc_rc;      /* returned by cmd_get_crc_verify(), or the value */
	int crc;             /* what the board holds, changed by a set */
	int crc_sets;
	int crc_gets;
	int last_crc_set;
	int polls;
	int stops;
	int sniffing_calls;
	int promisc_calls;
	int follow_requested;
	u32 access_address;
	int aa_reads;
	int aa_writes;
	/* Every device call in order, one letter each: s set CRC, g get CRC,
	 * r read access address, w write access address, t set target,
	 * m mode started, p poll. */
	char calls[64];
} fake;

static ubertooth_t fake_ut;
static fifo_t fake_fifo;
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
	memset(&fake_fifo, 0, sizeof(fake_fifo));
	fake_ut.devh = (struct libusb_device_handle *)&fake_devh;
	fake_ut.fifo = &fake_fifo;
	fake.get_crc_rc = -2; /* -2: report what the board holds */
	fake.access_address = 0x8e89bed6;
	/* main() leaves these globals where a previous run put them. */
	running = 1;
	cancel_follow = 0;
}

ubertooth_t *ubertooth_init(void)
{
	return &fake_ut;
}

int ubertooth_connect_select(ubertooth_t *ut, const char *u_arg)
{
	(void)ut;
	(void)u_arg;
	return 0;
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

int cmd_set_crc_verify(struct libusb_device_handle *devh, int verify)
{
	(void)devh;
	record('s');
	fake.crc_sets++;
	fake.last_crc_set = verify;
	if (fake.set_crc_rc >= 0)
		fake.crc = verify;
	return fake.set_crc_rc;
}

int cmd_get_crc_verify(struct libusb_device_handle *devh)
{
	(void)devh;
	record('g');
	fake.crc_gets++;
	if (fake.get_crc_rc != -2)
		return fake.get_crc_rc;
	return fake.crc;
}

int cmd_read_access_address(struct libusb_device_handle *devh, u32 *access_address)
{
	(void)devh;
	record('r');
	fake.aa_reads++;
	*access_address = fake.access_address;
	return 0;
}

int cmd_set_access_address(struct libusb_device_handle *devh, u32 access_address)
{
	(void)devh;
	record('w');
	fake.aa_writes++;
	fake.access_address = access_address;
	return 0;
}

int cmd_btle_sniffing(struct libusb_device_handle *devh, uint8_t do_follow)
{
	(void)devh;
	record('m');
	fake.sniffing_calls++;
	fake.follow_requested = do_follow;
	return 0;
}

int cmd_btle_promisc(struct libusb_device_handle *devh)
{
	(void)devh;
	record('m');
	fake.promisc_calls++;
	return 0;
}

/* One packetless poll, then the loop is asked to end, so a sniffing mode runs
 * through main() without waiting for a board. */
int cmd_poll(struct libusb_device_handle *devh, usb_pkt_rx *p)
{
	(void)devh;
	(void)p;
	record('p');
	fake.polls++;
	running = 0;
	return 0;
}

int cmd_set_channel(struct libusb_device_handle *devh, u16 channel)
{
	(void)devh;
	(void)channel;
	return 0;
}

int cmd_set_modulation(struct libusb_device_handle *devh, u16 mod)
{
	(void)devh;
	(void)mod;
	return 0;
}

int cmd_set_jam_mode(struct libusb_device_handle *devh, int mode)
{
	(void)devh;
	(void)mode;
	return 0;
}

int cmd_cancel_follow(struct libusb_device_handle *devh)
{
	(void)devh;
	return 0;
}

int cmd_btle_set_target(struct libusb_device_handle *devh, uint8_t *mac_address,
                        uint8_t mac_mask)
{
	(void)devh;
	(void)mac_address;
	(void)mac_mask;
	record('t');
	return 0;
}

int cmd_btle_slave(struct libusb_device_handle *devh, u8 *mac_address)
{
	(void)devh;
	(void)mac_address;
	return 0;
}

int cmd_le_set_adv_data(struct libusb_device_handle *devh, uint8_t *data,
                        unsigned data_len)
{
	(void)devh;
	(void)data;
	(void)data_len;
	return 0;
}

void fifo_push(fifo_t *fifo, const usb_pkt_rx *packet)
{
	(void)fifo;
	(void)packet;
}

void cb_btle(ubertooth_t *ut, void *args)
{
	(void)ut;
	(void)args;
}

/* ---- running the tool --------------------------------------------------- */

struct run_result {
	int rc;
	char out[4096];
	char err[1024];
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

static struct run_result run_btle(const char *const *args)
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
	result.rc = ubertooth_btle_main(argc, argv);
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

#define RUN(...) run_btle((const char *const[]){ "ubertooth-btle", __VA_ARGS__, NULL })

/* ---- tests -------------------------------------------------------------- */

/* A set and a read of CRC verify in one command both happen, the set first,
 * so the read reports the new value. They shared one variable, so whichever
 * was written last won and the other half was dropped with exit 0. */
MU_TEST(a_set_and_a_read_of_crc_verify_both_run_set_first)
{
	struct run_result r;

	reset_fake();
	r = RUN("-v1", "-v");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(fake.calls, "sg") == 0, fake.calls);
	mu_assert_int_eq(1, fake.last_crc_set);
	mu_assert(strcmp(r.out, "CRC: verify\n") == 0, r.out);
	mu_assert(r.err[0] == '\0', r.err);

	/* the read written first is the same command */
	reset_fake();
	r = RUN("-v", "-v1");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(fake.calls, "sg") == 0, fake.calls);
	mu_assert_int_eq(1, fake.last_crc_set);
	mu_assert(strcmp(r.out, "CRC: verify\n") == 0, r.out);

	/* a separate-word value too */
	reset_fake();
	r = RUN("-v", "1", "-v");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(fake.calls, "sg") == 0, fake.calls);
	mu_assert_int_eq(1, fake.last_crc_set);
	mu_assert(strcmp(r.out, "CRC: verify\n") == 0, r.out);

	/* switching it off is a set, not a missing value */
	reset_fake();
	fake.crc = 1;
	r = RUN("-v0", "-v");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(fake.calls, "sg") == 0, fake.calls);
	mu_assert_int_eq(0, fake.last_crc_set);
	mu_assert(strcmp(r.out, "CRC: DO NOT verify\n") == 0, r.out);

	/* two sets keep the last one, and the read reports it */
	reset_fake();
	r = RUN("-v1", "-v0", "-v");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(1, fake.crc_sets);
	mu_assert_int_eq(0, fake.last_crc_set);
	mu_assert(strcmp(r.out, "CRC: DO NOT verify\n") == 0, r.out);
}

/* A set on its own, and a read on its own, are unchanged. */
MU_TEST(a_set_alone_and_a_read_alone_are_unchanged)
{
	struct run_result r;

	reset_fake();
	r = RUN("-v1");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(fake.calls, "s") == 0, fake.calls);
	mu_assert_int_eq(0, fake.crc_gets);
	mu_assert(strcmp(r.out, "CRC: verify\n") == 0, r.out);

	reset_fake();
	fake.crc = 1;
	r = RUN("-v");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(fake.calls, "g") == 0, fake.calls);
	mu_assert_int_eq(0, fake.crc_sets);
	mu_assert(strcmp(r.out, "CRC: verify\n") == 0, r.out);

	reset_fake();
	r = RUN("-v");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, "CRC: DO NOT verify\n") == 0, r.out);
}

/* A failed set is reported and ends the command, so the read cannot report a
 * value the set never reached. */
MU_TEST(a_failed_set_is_reported_and_the_read_does_not_run)
{
	struct run_result r;

	reset_fake();
	fake.set_crc_rc = -1;
	r = RUN("-v1", "-v");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(fake.calls, "s") == 0, fake.calls);
	mu_assert_int_eq(0, fake.crc_gets);
	mu_assert(strcmp(r.err, "error: CRC verify set failed\n") == 0, r.err);
	mu_assert(r.out[0] == '\0', r.out);

	/* the same for a set on its own, which used to ignore the result */
	reset_fake();
	fake.set_crc_rc = -1;
	r = RUN("-v0");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.err, "error: CRC verify set failed\n") == 0, r.err);
	mu_assert(r.out[0] == '\0', r.out);
}

/* A failed read is reported and exits 1, and a short reply, whose whole line
 * the library has already printed, gets no second line. */
MU_TEST(a_failed_read_is_reported_once)
{
	struct run_result r;

	reset_fake();
	fake.get_crc_rc = -1;
	r = RUN("-v1", "-v");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(fake.calls, "sg") == 0, fake.calls);
	mu_assert(strcmp(r.err, "error: CRC verify read failed\n") == 0, r.err);
	mu_assert(r.out[0] == '\0', r.out);

	reset_fake();
	fake.get_crc_rc = UBERTOOTH_ERROR_SHORT_REPLY;
	r = RUN("-v");
	mu_assert_int_eq(1, r.rc);
	mu_assert(r.err[0] == '\0', r.err);
	mu_assert(r.out[0] == '\0', r.out);
}

/* In a sniffing mode both halves run after the mode has started, where mode
 * entry has just cleared the setting, and before the capture loop. */
MU_TEST(a_sniffing_mode_sets_and_reads_after_the_mode_starts)
{
	struct run_result r;

	reset_fake();
	r = RUN("-n", "-v1", "-v");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(fake.calls, "msgp") == 0, fake.calls);
	mu_assert_int_eq(1, fake.sniffing_calls);
	mu_assert_int_eq(0, fake.follow_requested);
	mu_assert(strcmp(r.out, "CRC: verify\n") == 0, r.out);
	mu_assert_int_eq(1, fake.stops);

	/* a read on its own in a sniffing mode used to be dropped silently */
	reset_fake();
	fake.crc = 1;
	r = RUN("-f", "-v");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(fake.calls, "mgp") == 0, fake.calls);
	mu_assert_int_eq(1, fake.follow_requested);
	mu_assert(strcmp(r.out, "CRC: verify\n") == 0, r.out);

	reset_fake();
	r = RUN("-p", "-v1");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(fake.calls, "msp") == 0, fake.calls);
	mu_assert_int_eq(1, fake.promisc_calls);
	mu_assert(strcmp(r.out, "CRC: verify\n") == 0, r.out);
}

/* The mode is running by the time the -v options go out, so a failure there
 * has to stop it before giving up: a board abandoned in a low-energy mode is
 * the state it stops answering control requests in. */
MU_TEST(a_failure_in_a_sniffing_mode_still_stops_the_board)
{
	struct run_result r;

	reset_fake();
	fake.set_crc_rc = -1;
	r = RUN("-f", "-v1");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(fake.calls, "ms") == 0, fake.calls);
	mu_assert_int_eq(0, fake.polls); /* no capture was started */
	mu_assert_int_eq(1, fake.stops);
	mu_assert(strcmp(r.err, "error: CRC verify set failed\n") == 0, r.err);

	reset_fake();
	fake.get_crc_rc = -1;
	r = RUN("-p", "-v");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(fake.calls, "mg") == 0, fake.calls);
	mu_assert_int_eq(1, fake.stops);
	mu_assert(strcmp(r.err, "error: CRC verify read failed\n") == 0, r.err);
}

/* -v alongside an access address read, which the tool carries on after. */
MU_TEST(an_access_address_read_and_the_crc_options_all_run)
{
	struct run_result r;

	reset_fake();
	r = RUN("-a", "-v1", "-v");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(fake.calls, "rsg") == 0, fake.calls);
	mu_assert(strcmp(r.out, "Access address: 8e89bed6\nCRC: verify\n") == 0,
	          r.out);
}

/* The usage text says what a set and a read in one command do. */
MU_TEST(the_usage_text_documents_the_set_and_the_read)
{
	struct run_result r;

	reset_fake();
	r = RUN("-h");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.out, "-v[01]") != NULL, r.out);
	mu_assert(strstr(r.out, "reports the new value") != NULL, r.out);
	mu_assert_int_eq(0, fake.crc_sets);
	mu_assert_int_eq(0, fake.crc_gets);
}

int main(void)
{
	MU_RUN(a_set_and_a_read_of_crc_verify_both_run_set_first);
	MU_RUN(a_set_alone_and_a_read_alone_are_unchanged);
	MU_RUN(a_failed_set_is_reported_and_the_read_does_not_run);
	MU_RUN(a_failed_read_is_reported_once);
	MU_RUN(a_sniffing_mode_sets_and_reads_after_the_mode_starts);
	MU_RUN(a_failure_in_a_sniffing_mode_still_stops_the_board);
	MU_RUN(an_access_address_read_and_the_crc_options_all_run);
	MU_RUN(the_usage_text_documents_the_set_and_the_read);
	return mu_report();
}
