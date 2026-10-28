/*
 * Hardware-independent tests of ubertooth-btle's main(): which device
 * commands the options run, in which order, which stream each line it prints
 * lands on, and what it exits with.
 *
 * ubertooth-btle.c is compiled into this test with main() renamed, and the
 * libubertooth functions it calls are replaced by the fakes below, so the real
 * command-line handling and reporting run against a scripted device.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../../firmware/tests/minunit.h"

#define main ubertooth_btle_main
#include "ubertooth-btle.c"
#undef main

#include "run_main.h"

/* ---- scripted device ---------------------------------------------------- */

static struct {
	int set_crc_rc;      /* returned by cmd_set_crc_verify() */
	int get_crc_rc;      /* returned by cmd_get_crc_verify(), or the value */
	int crc;             /* what the board holds, changed by a set */
	int crc_sets;
	int crc_gets;
	int last_crc_set;
	int poll_rc;         /* returned by cmd_poll(): < 0 fails the capture */
	int polls;
	/* Every reply the loop handed to the library's short-reply report, and
	 * the last length it passed: a reply that is not a whole record carries
	 * no packet, and dropping it in silence looked like a quiet board. */
	int short_reply_reports;
	int last_short_reply_len;
	/* The stream the capture loop handed the callback. */
	FILE *callback_report_stream;
	int interrupt_the_capture; /* cmd_poll() raises SIGINT instead of
	                            * writing the handle */
	int stops;
	int closes;       /* ubertooth_close() calls */
	int frees;        /* ubertooth_free() calls */
	int connects;        /* a refused command line must open no device */
	int sniffing_calls;
	int promisc_calls;
	int follow_requested;
	int target_rc;       /* returned by cmd_btle_set_target() */
	/* The addresses the two requests carried, so a case can tell -t's
	 * target from -s's advertising address. */
	uint8_t last_target[OPT_BD_ADDR_BYTES];
	uint8_t last_target_mask;
	uint8_t last_slave[OPT_BD_ADDR_BYTES];
	int jam_rc;          /* returned by cmd_set_jam_mode() */
	int jam_mode_sets;
	int last_jam_mode;
	int slave_calls;
	/* Returned by the requests that start a mode: a board that refuses one
	 * must not be followed by a run that reports success. */
	int mode_start_rc;
	int channel_rc;      /* returned by cmd_set_channel() */
	int modulation_rc;   /* returned by cmd_set_modulation() */
	int adv_data_rc;     /* returned by cmd_le_set_adv_data() */
	int adv_data_calls;
	u16 last_channel;
	u32 access_address;
	int aa_reads;
	int aa_writes;
	char capture_file[64]; /* the name the capture was created under */
	int capture_create_rc; /* non-zero: libbtbb could not create the file */
	/* Every device call in order, one letter each: s set CRC, g get CRC,
	 * r read access address, w write access address, t set target,
	 * m mode started, p poll, S faux slave mode started. */
	char calls[64];
} fake;

static ubertooth_t fake_ut;
static fifo_t fake_fifo;
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
	memset(&fake_fifo, 0, sizeof(fake_fifo));
	fake_ut.devh = (struct libusb_device_handle *)&fake_devh;
	fake_ut.fifo = &fake_fifo;
	fake.get_crc_rc = -2; /* -2: report what the board holds */
	fake.last_jam_mode = -1; /* -1: cmd_set_jam_mode was not called */
	fake.access_address = 0x8e89bed6;
	/* main() leaves these where a previous run put them. */
	stop_requested = 0;
	cancel_follow = 0;
	signal(SIGINT, SIG_DFL);
}

ubertooth_t *ubertooth_init(void)
{
	return &fake_ut;
}

int ubertooth_connect_select(ubertooth_t *ut, const char *u_arg)
{
	(void)ut;
	(void)u_arg;
	fake.connects++;
	return 0;
}

/* The capture files, so a "-q -" run does not write a PCAP header into the
 * test's captured stdout and no file is created anywhere. */
int lell_pcap_create_file(const char *filename, lell_pcap_handle **ph)
{
	snprintf(fake.capture_file, sizeof(fake.capture_file), "%s", filename);
	*ph = NULL;
	if (fake.capture_create_rc)
		errno = ENOENT; /* the tool prints strerror(errno) */
	return fake.capture_create_rc;
}

int lell_pcap_ppi_create_file(const char *filename, int btle_ppi_version,
                              lell_pcap_handle **ph)
{
	(void)btle_ppi_version;
	snprintf(fake.capture_file, sizeof(fake.capture_file), "%s", filename);
	*ph = NULL;
	if (fake.capture_create_rc)
		errno = ENOENT; /* the tool prints strerror(errno) */
	return fake.capture_create_rc;
}

int lell_pcapng_create_file(const char *filename, const char *interface_desc,
                            lell_pcapng_handle **ph)
{
	(void)interface_desc;
	snprintf(fake.capture_file, sizeof(fake.capture_file), "%s", filename);
	*ph = NULL;
	if (fake.capture_create_rc)
		errno = ENOENT; /* the tool prints strerror(errno) */
	return fake.capture_create_rc;
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

/* Entering a mode runs reset_le() on the board, which clears crc_verify.
 * That is the whole reason the -v options run after this call, so the fake
 * clears it too and a read before the mode would report the wrong thing. */
int cmd_btle_sniffing(struct libusb_device_handle *devh, uint8_t do_follow)
{
	(void)devh;
	record('m');
	fake.sniffing_calls++;
	fake.follow_requested = do_follow;
	fake.crc = 0;
	return fake.mode_start_rc;
}

int cmd_btle_promisc(struct libusb_device_handle *devh)
{
	(void)devh;
	record('m');
	fake.promisc_calls++;
	fake.crc = 0;
	return fake.mode_start_rc;
}

/* One packetless poll, then the loop is asked to end, so a sniffing mode runs
 * through main() without waiting for a board. A case that wants the signal
 * path instead raises a real interrupt here, which only reaches the loop
 * through the handler's flag. */
int cmd_poll(struct libusb_device_handle *devh, usb_pkt_rx *p)
{
	(void)devh;
	(void)p;
	record('p');
	fake.polls++;
	if (fake.interrupt_the_capture)
		raise(SIGINT);
	else
		fake_ut.stop_ubertooth = 1;
	return fake.poll_rc;
}

int cmd_set_channel(struct libusb_device_handle *devh, u16 channel)
{
	(void)devh;
	fake.last_channel = channel;
	return fake.channel_rc;
}

int cmd_set_modulation(struct libusb_device_handle *devh, u16 mod)
{
	(void)devh;
	(void)mod;
	return fake.modulation_rc;
}

int cmd_set_jam_mode(struct libusb_device_handle *devh, int mode)
{
	(void)devh;
	/* Not in fake.calls: every sniffing mode sends this, so recording it
	 * would say nothing and would re-letter every expected call string. */
	fake.jam_mode_sets++;
	fake.last_jam_mode = mode;
	return fake.jam_rc;
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
	memcpy(fake.last_target, mac_address, sizeof(fake.last_target));
	fake.last_target_mask = mac_mask;
	record('t');
	return fake.target_rc;
}

int cmd_btle_slave(struct libusb_device_handle *devh, u8 *mac_address)
{
	(void)devh;
	memcpy(fake.last_slave, mac_address, sizeof(fake.last_slave));
	record('S');
	fake.slave_calls++;
	return fake.mode_start_rc;
}

int cmd_le_set_adv_data(struct libusb_device_handle *devh, uint8_t *data,
                        unsigned data_len)
{
	(void)devh;
	(void)data;
	(void)data_len;
	fake.adv_data_calls++;
	return fake.adv_data_rc;
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

void cb_btle(ubertooth_t *ut, void *args)
{
	btle_options *opts = (btle_options *)args;

	(void)ut;
	/* The stream the tool told the callback to report on, which is what
	 * decides whether the promiscuous mode's lines stay visible once a
	 * capture has taken standard output. */
	fake.callback_report_stream = opts != NULL ? opts->report_stream : NULL;
}

void ubertooth_warn_if_poll_reply_short(ubertooth_t *ut, int len)
{
	(void)ut;
	fake.short_reply_reports++;
	fake.last_short_reply_len = len;
}

/* ---- running the tool --------------------------------------------------- */

/* Nothing of this tool's own has to be set up or checked around a run, so the
 * shared harness is called directly. */
#define RUN(...)                                                     \
	run_tool_main(ubertooth_btle_main,                               \
	              (const char *const[]){ "ubertooth-btle",           \
	                                     __VA_ARGS__, NULL })

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

	/* Two values for one setting are refused: the last one used to win and
	 * the other was dropped without a word. */
	reset_fake();
	r = RUN("-v1", "-v0", "-v");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.out, "") == 0, r.out);
	mu_assert(strcmp(r.err, "error: option '-v' given more than once\n") == 0,
	          r.err);
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

	/* A read on its own in a sniffing mode used to be dropped silently. It
	 * reports what mode entry left, not what the board held before: the
	 * board starts at 1 here and the read says it is off. */
	reset_fake();
	fake.crc = 1;
	r = RUN("-f", "-v");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(fake.calls, "mgp") == 0, fake.calls);
	mu_assert_int_eq(1, fake.follow_requested);
	mu_assert(strcmp(r.out, "CRC: DO NOT verify\n") == 0, r.out);

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

/* The target report is a result, so it is on standard output. */
MU_TEST(the_target_report_is_on_standard_output)
{
	struct run_result r;

	reset_fake();
	r = RUN("-tnone");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(fake.calls, "t") == 0, fake.calls);
	mu_assert(strcmp(r.out, "target cleared\n") == 0, r.out);
	mu_assert(r.err[0] == '\0', r.err);

	reset_fake();
	r = RUN("-t22:44:66:88:aa:cc");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, "target set to: 22:44:66:88:aa:cc/48\n") == 0,
	          r.out);
	mu_assert(r.err[0] == '\0', r.err);
}

/* -s and -t name two different things -- the address to advertise from and
 * the connection to follow -- so one command can set both. They shared one
 * address, so whichever option came last set both, and "-s <address> -t none"
 * advertised from 00:00:00:00:00:00 without a word. */
MU_TEST(the_faux_slave_and_target_addresses_are_separate)
{
	static const uint8_t slave[OPT_BD_ADDR_BYTES] = {
		0x22, 0x44, 0x66, 0x88, 0xaa, 0xcc,
	};
	static const uint8_t target[OPT_BD_ADDR_BYTES] = {
		0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
	};
	static const uint8_t zeros[OPT_BD_ADDR_BYTES] = { 0, };
	struct run_result r;

	reset_fake();
	r = RUN("-s", "22:44:66:88:aa:cc", "-t", "11:11:11:11:11:11");
	mu_assert_int_eq(0, r.rc);
	/* the target first, then advertising, as a target set before a mode */
	mu_assert(strcmp(fake.calls, "tS") == 0, fake.calls);
	mu_assert(memcmp(fake.last_target, target, sizeof(target)) == 0,
	          "the target request carried the wrong address");
	mu_assert(memcmp(fake.last_slave, slave, sizeof(slave)) == 0,
	          "the faux slave advertised from the wrong address");
	mu_assert(strcmp(r.out, "target set to: 11:11:11:11:11:11/48\n") == 0,
	          r.out);

	/* and in the other order, which used to be the one that worked */
	reset_fake();
	r = RUN("-t", "11:11:11:11:11:11", "-s", "22:44:66:88:aa:cc");
	mu_assert_int_eq(0, r.rc);
	mu_assert(memcmp(fake.last_target, target, sizeof(target)) == 0,
	          "the target request carried the wrong address");
	mu_assert(memcmp(fake.last_slave, slave, sizeof(slave)) == 0,
	          "the faux slave advertised from the wrong address");

	/* clearing the target leaves the faux slave's address alone */
	reset_fake();
	r = RUN("-s", "22:44:66:88:aa:cc", "-tnone");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(fake.calls, "tS") == 0, fake.calls);
	mu_assert(memcmp(fake.last_target, zeros, sizeof(zeros)) == 0,
	          "the cleared target carried an address");
	mu_assert_int_eq(0, fake.last_target_mask);
	mu_assert(memcmp(fake.last_slave, slave, sizeof(slave)) == 0,
	          "the faux slave advertised from the wrong address");
	mu_assert(strcmp(r.out, "target cleared\n") == 0, r.out);
}

/* A capture on standard output ("-q -", "-c -") redirects this process's
 * stdout to /dev/null for the whole run, so the lines that answer an option
 * were done on the board and then thrown away, with exit 0. They go to
 * standard error instead. */
MU_TEST(the_reports_move_to_stderr_when_the_capture_takes_stdout)
{
	struct run_result r;

	reset_fake();
	r = RUN("-q", "-", "-a", "-v1", "-v");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(fake.calls, "rsg") == 0, fake.calls);
	/* the capture was created on the saved stdout, not on a file */
	mu_assert(strstr(fake.capture_file, "/fd/") != NULL, fake.capture_file);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strcmp(r.err, "Access address: 8e89bed6\nCRC: verify\n") == 0,
	          r.err);

	/* the -c format, and the target line, which the tool sends before a
	 * sniffing mode starts */
	reset_fake();
	r = RUN("-c", "-", "-f", "-t22:44:66:88:aa:cc");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(fake.calls, "tmp") == 0, fake.calls);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strcmp(r.err, "target set to: 22:44:66:88:aa:cc/48\n") == 0,
	          r.err);

	/* a capture to a named file leaves the reports on standard output */
	reset_fake();
	r = RUN("-q", "/dev/null", "-a");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(fake.capture_file, "/dev/null") == 0, fake.capture_file);
	mu_assert(strcmp(r.out, "Access address: 8e89bed6\n") == 0, r.out);
	mu_assert(r.err[0] == '\0', r.err);
}

/* The callback reports on the same stream, which is what keeps the lines
 * promiscuous mode writes visible when a capture has taken standard output:
 * they are the only way such a capture says which connection it is of. The
 * tool is what tells the callback, so this pins that it does. */
MU_TEST(the_callback_reports_on_the_same_stream_as_the_tool)
{
	struct run_result r;

	/* A whole record, so the capture loop reaches the callback. */
	reset_fake();
	fake.poll_rc = (int)sizeof(usb_pkt_rx);
	r = RUN("-p");
	mu_assert_int_eq(0, r.rc);
	mu_assert(fake.callback_report_stream == stdout,
	          "an ordinary run: the callback was not given standard output");

	reset_fake();
	fake.poll_rc = (int)sizeof(usb_pkt_rx);
	r = RUN("-p", "-q", "-");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strstr(fake.capture_file, "/fd/") != NULL, fake.capture_file);
	mu_assert(fake.callback_report_stream == stderr,
	          "the capture took standard output, but the callback was still "
	          "given it, so the promiscuous lines would go to /dev/null");
}

/* A Ctrl-C ends a sniffing capture, with the board taken out of its mode and
 * exit 0: the user stopped a run that was working. The loop used to read a
 * plain int the handler wrote, which a compiler may read once and keep, so
 * nothing guaranteed it would see the interrupt at all. */
MU_TEST(an_interrupted_capture_stops_and_exits_zero)
{
	struct run_result r;

	reset_fake();
	fake.interrupt_the_capture = 1;
	r = RUN("-n");
	mu_assert_int_eq(0, r.rc);
	/* One poll, so the loop left as soon as the handler had run. */
	mu_assert_int_eq(1, fake.polls);
	mu_assert_int_eq(1, fake.stops);
	mu_assert(r.err[0] == '\0', r.err);
	mu_assert(r.out[0] == '\0', r.out);
}

/* A poll that fails ends the capture short, so it is an error on standard
 * error and exit 1, not "USB error" on standard output and exit 0. */
MU_TEST(a_failed_poll_is_reported_on_stderr_and_exits_1)
{
	struct run_result r;

	reset_fake();
	fake.poll_rc = -1;
	r = RUN("-f");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(fake.calls, "mp") == 0, fake.calls);
	mu_assert_int_eq(1, fake.stops);
	mu_assert(strcmp(r.err, "error: USB poll failed\n") == 0, r.err);
	mu_assert(r.out[0] == '\0', r.out);
}

/* A reply that is neither a whole record nor the board's "nothing queued"
 * answer carries part of a packet, which the loop cannot read: it is handed
 * to the library's report rather than dropped in silence, because a capture
 * losing packets otherwise looks exactly like a quiet board. */
MU_TEST(a_short_poll_reply_is_reported)
{
	struct run_result r;

	reset_fake();
	fake.poll_rc = 32;
	r = RUN("-f");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(1, fake.short_reply_reports);
	mu_assert_int_eq(32, fake.last_short_reply_len);

	/* A whole record is a packet, so nothing is reported. */
	reset_fake();
	fake.poll_rc = (int)sizeof(usb_pkt_rx);
	r = RUN("-f");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(0, fake.short_reply_reports);

	/* A failed poll has its own error and is not reported as a short one. */
	reset_fake();
	fake.poll_rc = -1;
	r = RUN("-f");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(0, fake.short_reply_reports);
}

/* The other two diagnostics this tool used to put on standard output. */
MU_TEST(a_failed_target_set_and_jamming_are_reported_on_stderr)
{
	struct run_result r;

	reset_fake();
	fake.target_rc = -1;
	r = RUN("-t22:44:66:88:aa:cc");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.err, "error: target set failed\n") == 0, r.err);
	mu_assert(r.out[0] == '\0', r.out);

	reset_fake();
	fake.jam_rc = -1;
	r = RUN("-f", "-i");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(JAM_ONCE, fake.last_jam_mode);
	mu_assert_int_eq(0, fake.polls); /* no capture was started */
	mu_assert(strcmp(r.err,
	                 "error: jamming not supported by this firmware\n") == 0,
	          r.err);
	mu_assert(r.out[0] == '\0', r.out);

	/* a jam mode the board takes goes on to the capture */
	reset_fake();
	r = RUN("-p", "-I");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(JAM_CONTINUOUS, fake.last_jam_mode);
	mu_assert_int_eq(1, fake.promisc_calls);
	mu_assert_int_eq(1, fake.polls);
}

/* Interference acts on a connection, so it needs -f or -p. It was accepted
 * with -n, which only logs advertising packets, and the jam mode was set on
 * the board all the same. */
MU_TEST(the_interference_options_are_refused_without_f_or_p)
{
	struct run_result r;
	const char *const spellings[] = { "-i", "-I", "-j", "-J" };
	char expected[128];
	unsigned i;

	for (i = 0; i < sizeof(spellings) / sizeof(spellings[0]); i++) {
		reset_fake();
		r = RUN("-n", spellings[i]);
		mu_assert_int_eq(1, r.rc);
		snprintf(expected, sizeof(expected),
		         "error: -%c needs -f or -p: there is no connection to"
		         " interfere with otherwise\n", spellings[i][1]);
		mu_assert(strcmp(r.err, expected) == 0, r.err);
		mu_assert(r.out[0] == '\0', r.out);
		/* refused while the command line was read: no device opened,
		 * and the jam mode never reached a board */
		mu_assert_int_eq(0, fake.connects);
		mu_assert_int_eq(-1, fake.last_jam_mode);
		mu_assert(fake.calls[0] == '\0', fake.calls);
	}

	/* not only with -n: any command without -f or -p */
	reset_fake();
	r = RUN("-a", "-i");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "error: -i needs -f or -p") == r.err, r.err);
	mu_assert_int_eq(0, fake.connects);

	/* -f and -p take it */
	reset_fake();
	r = RUN("-f", "-i");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(JAM_ONCE, fake.last_jam_mode);
	reset_fake();
	r = RUN("-p", "-J");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(JAM_CONTINUOUS, fake.last_jam_mode);
}

/* A sniffing mode runs until it is interrupted, so the blocks after it used
 * to send their control requests on the handle ubertooth_stop() had closed
 * and nulled. The combination is refused while the command line is read. */
MU_TEST(a_sniffing_mode_refuses_the_access_address_and_slave_options)
{
	struct run_result r;
	const char *const modes[] = { "-f", "-n", "-p" };
	const char *const after[] = { "-a", "-a8e89bed6", "-s22:44:66:88:aa:cc" };
	const char *expected = "error: -a and -s do not go with -f, -n or -p:"
	        " a sniffing mode runs until it is interrupted\n";
	unsigned mode, option;

	for (mode = 0; mode < sizeof(modes) / sizeof(modes[0]); mode++) {
		for (option = 0; option < sizeof(after) / sizeof(after[0]);
		     option++) {
			reset_fake();
			r = RUN(modes[mode], after[option]);
			mu_assert_int_eq(1, r.rc);
			mu_assert(strcmp(r.err, expected) == 0, r.err);
			mu_assert(r.out[0] == '\0', r.out);
			/* refused before a device was opened, so no request
			 * went out on a closed handle */
			mu_assert_int_eq(0, fake.connects);
			mu_assert(fake.calls[0] == '\0', fake.calls);
			mu_assert_int_eq(0, fake.aa_reads);
			mu_assert_int_eq(0, fake.aa_writes);
			mu_assert_int_eq(0, fake.slave_calls);
		}
	}

	/* -t and -v are not in that list: -t is sent before the mode starts,
	 * and the -v options run once it has started */
	reset_fake();
	r = RUN("-f", "-t22:44:66:88:aa:cc", "-v");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(fake.calls, "tmgp") == 0, fake.calls);

	/* and on their own, -a and -s still run */
	reset_fake();
	r = RUN("-a", "-s22:44:66:88:aa:cc");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(fake.calls, "rS") == 0, fake.calls);
}

/* A mode the board will not enter must not be followed by a capture loop and
 * an exit status that reports a clean run. */
MU_TEST(a_mode_the_board_refuses_stops_the_run)
{
	struct run_result r;

	reset_fake();
	fake.mode_start_rc = -1;
	r = RUN("-f");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(0, fake.polls); /* no capture was started */
	mu_assert_int_eq(1, fake.stops); /* the board was stopped all the same */
	mu_assert(strcmp(r.err, "error: could not start follow mode\n") == 0,
	          r.err);
	mu_assert(r.out[0] == '\0', r.out);

	reset_fake();
	fake.mode_start_rc = -1;
	r = RUN("-n");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.err, "error: could not start no-follow mode\n") == 0,
	          r.err);

	reset_fake();
	fake.mode_start_rc = -1;
	r = RUN("-p");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(0, fake.polls);
	mu_assert(strcmp(r.err, "error: could not start promiscuous mode\n") == 0,
	          r.err);

	/* The request that starts the advertising is stopped on the way out: a
	 * timeout is not proof the board never got it, and a board left
	 * part-way into a low-energy mode stops answering. */
	reset_fake();
	fake.mode_start_rc = -1;
	r = RUN("-s22:44:66:88:aa:cc");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(1, fake.stops);
	mu_assert(strcmp(r.err, "error: could not start advertising\n") == 0,
	          r.err);
	mu_assert(r.out[0] == '\0', r.out);

	/* the other requests each mode sends: the modulation and the channel
	 * for a sniffing mode, the channel and the advertising data for the
	 * faux slave mode. Each is named on its own, so the user is told which
	 * request the board refused rather than only which mode failed. */
	reset_fake();
	fake.modulation_rc = -1;
	r = RUN("-f");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(0, fake.sniffing_calls); /* the mode was not started */
	mu_assert_int_eq(1, fake.stops);
	mu_assert(strcmp(r.err, "error: modulation set failed\n") == 0, r.err);

	reset_fake();
	fake.channel_rc = -1;
	r = RUN("-f");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(0, fake.sniffing_calls);
	mu_assert_int_eq(1, fake.stops);
	mu_assert(strcmp(r.err,
	                 "error: could not set the advertising channel\n") == 0,
	          r.err);
	mu_assert(r.out[0] == '\0', r.out);

	/* Neither of the two that only set something up has started a mode, so
	 * neither sends a STOP the board may not be able to answer. */
	reset_fake();
	fake.channel_rc = -1;
	r = RUN("-s22:44:66:88:aa:cc");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(0, fake.adv_data_calls);
	mu_assert_int_eq(0, fake.slave_calls);
	mu_assert_int_eq(0, fake.stops);
	mu_assert(strcmp(r.err,
	                 "error: could not set the advertising channel\n") == 0,
	          r.err);
	mu_assert(r.out[0] == '\0', r.out);

	reset_fake();
	fake.adv_data_rc = -1;
	r = RUN("-s22:44:66:88:aa:cc");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(0, fake.slave_calls);
	mu_assert_int_eq(0, fake.stops);
	mu_assert(strcmp(r.err,
	                 "error: could not set the advertising data\n") == 0,
	          r.err);
	mu_assert(r.out[0] == '\0', r.out);

	/* the -A channel index reaches the board in both modes */
	reset_fake();
	r = RUN("-A39", "-s22:44:66:88:aa:cc");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(2480, fake.last_channel);
	reset_fake();
	r = RUN("-A38", "-f");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(2426, fake.last_channel);
}

/* A capture file the tool cannot create names the file the user gave and the
 * reason, and no device is opened for it. */
MU_TEST(a_capture_file_that_cannot_be_created_is_named)
{
	struct run_result r;

	reset_fake();
	fake.capture_create_rc = -1;
	r = RUN("-f", "-q", "/no/such/dir/out.pcap");
	mu_assert_int_eq(1, r.rc);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strstr(r.err, "error: /no/such/dir/out.pcap: ") == r.err,
	          r.err);
	mu_assert_int_eq(0, fake.connects);

	reset_fake();
	fake.capture_create_rc = -1;
	r = RUN("-f", "-r", "/no/such/dir/out.pcapng");
	mu_assert_int_eq(1, r.rc);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strstr(r.err, "error: /no/such/dir/out.pcapng: ") == r.err,
	          r.err);
	mu_assert_int_eq(0, fake.connects);
}

/* The usage text says what a set and a read in one command do. */
MU_TEST(the_usage_text_documents_the_set_and_the_read)
{
	struct run_result r;

	reset_fake();
	r = RUN("-h");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strstr(r.out, "-v [01]") != NULL, r.out);
	mu_assert(strstr(r.out, "reports the new value") != NULL, r.out);
	mu_assert_int_eq(0, fake.crc_sets);
	mu_assert_int_eq(0, fake.crc_gets);
}

/* The usage text names the -j/-J jam aliases alongside -i/-I. */
MU_TEST(the_usage_text_names_the_jam_aliases)
{
	struct run_result r;

	reset_fake();
	r = RUN("-h");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strstr(r.out, "-i, -j") != NULL, r.out);
	mu_assert(strstr(r.out, "-I, -J") != NULL, r.out);
}

/* -h is what the user asked for, so the usage text is the command's output
 * and the status says it worked. It used to exit 1, so a script checking
 * "ubertooth-btle -h" saw a failure. */
MU_TEST(the_help_option_exits_0_with_the_usage_text_on_stdout)
{
	struct run_result r;

	reset_fake();
	r = RUN("-h");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strstr(r.out, "Usage:") != NULL, r.out);
	mu_assert(r.err[0] == '\0', r.err);
	mu_assert_int_eq(0, fake.connects);

	/* a bad command line alongside it is still reported, and wins */
	reset_fake();
	r = RUN("-h", "stray");
	mu_assert_int_eq(1, r.rc);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strcmp(r.err, "error: unexpected argument 'stray'\n") == 0,
	          r.err);
}

/* A bad option line comes from the shared helper, so this tool reports one
 * the way every other does: that one "error: " line on standard error, with
 * no usage text after it and standard output left empty. */
MU_TEST(a_bad_command_line_is_reported_on_stderr)
{
	struct run_result r;

	reset_fake();
	r = RUN("-Z");
	mu_assert_int_eq(1, r.rc);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strcmp(r.err, "error: unrecognized option '-Z'\n") == 0, r.err);
	mu_assert_int_eq(0, fake.connects);

	/* an option whose value is missing, which the leading ':' in the
	 * option string tells apart from an unknown option */
	reset_fake();
	r = RUN("-A");
	mu_assert_int_eq(1, r.rc);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strstr(r.err, "error: option '-A' requires a value\n") == r.err,
	          r.err);

	/* the address refusals, all from the shared parser, all on stderr with
	 * nothing on stdout. One wording for a value that is not an address,
	 * whatever is wrong with it, naming the address the option sets. */
	reset_fake();
	r = RUN("-t", "22:44:66:88:aa");
	mu_assert_int_eq(1, r.rc);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strstr(r.err, "error: target address must be"
	                 " xx:xx:xx:xx:xx:xx (got 22:44:66:88:aa)\n") == r.err,
	          r.err);

	reset_fake();
	r = RUN("-s", "22:44:66:88:aa:zz");
	mu_assert_int_eq(1, r.rc);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strstr(r.err, "error: faux slave address must be"
	                 " xx:xx:xx:xx:xx:xx (got 22:44:66:88:aa:zz)\n") == r.err,
	          r.err);

	/* The faux slave advertises from one address, so the target's "none"
	 * and "/<bits>" are not values for it. "-s none" used to advertise
	 * from 00:00:00:00:00:00. */
	reset_fake();
	r = RUN("-s", "none");
	mu_assert_int_eq(1, r.rc);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strstr(r.err, "error: faux slave address must be"
	                 " xx:xx:xx:xx:xx:xx (got none)\n") == r.err, r.err);
	mu_assert_int_eq(0, fake.connects);

	reset_fake();
	r = RUN("-s", "22:44:66:88:aa:cc/24");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, "error: faux slave address must be"
	                 " xx:xx:xx:xx:xx:xx (got 22:44:66:88:aa:cc/24)\n")
	          == r.err, r.err);

	reset_fake();
	r = RUN("-t", "22:44:66:88:aa:cc/49");
	mu_assert_int_eq(1, r.rc);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strstr(r.err, "error: MAC mask must be 0-48 (got 49)\n")
	          == r.err, r.err);

	/* a masked address is quoted whole, mask included, even though the
	 * parser splits it in place */
	reset_fake();
	r = RUN("-t", "22:44:66:88:aa:zz/24");
	mu_assert_int_eq(1, r.rc);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strstr(r.err, "error: target address must be"
	                 " xx:xx:xx:xx:xx:xx (got 22:44:66:88:aa:zz/24)\n")
	          == r.err, r.err);

	/* -q and -c write the same PCAP through one handle, so naming a file
	 * with each of them is a warning rather than an error: the first one
	 * wins and the run carries on. The same option twice is refused
	 * instead, because it is one option with two values. */
	reset_fake();
	r = RUN("-f", "-q", "/dev/null", "-c", "/dev/zero");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(fake.capture_file, "/dev/null") == 0, fake.capture_file);
	mu_assert(strcmp(r.err, "warning: ignoring extra capture file:"
	                 " /dev/zero\n") == 0, r.err);

	reset_fake();
	r = RUN("-f", "-q", "/dev/null", "-q", "/dev/zero");
	mu_assert_int_eq(1, r.rc);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strcmp(r.err, "error: option '-q' given more than once\n") == 0,
	          r.err);
}

/* -j and -J are the original jam spellings of -i and -I: each selects the same
 * jam mode its interfere spelling does. */
MU_TEST(the_jam_aliases_select_the_same_mode_as_interfere)
{
	struct run_result r;

	reset_fake();
	r = RUN("-f", "-i");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(JAM_ONCE, fake.last_jam_mode);

	reset_fake();
	r = RUN("-f", "-j");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(JAM_ONCE, fake.last_jam_mode);

	reset_fake();
	r = RUN("-f", "-I");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(JAM_CONTINUOUS, fake.last_jam_mode);

	reset_fake();
	r = RUN("-f", "-J");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(JAM_CONTINUOUS, fake.last_jam_mode);
}

/* The handle and its 72 MB fifo go back on every exit once it exists. They
 * were left to process exit, and were allocated before the command line had
 * been read, so even a refused command took 72 MB. */
MU_TEST(every_exit_path_gives_the_handle_back)
{
	struct run_result r;

	reset_fake();
	r = RUN("-v");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(1, fake.frees);

	reset_fake();
	r = RUN("-n");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(1, fake.frees);

	/* A refused command line never allocates one. */
	reset_fake();
	r = RUN("-h");
	mu_assert_int_eq(0, r.rc);
	mu_assert_int_eq(0, fake.frees);

	reset_fake();
	r = RUN("-Z");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(0, fake.frees);
}

int main(void)
{
	MU_RUN(a_set_and_a_read_of_crc_verify_both_run_set_first);
	MU_RUN(every_exit_path_gives_the_handle_back);
	MU_RUN(a_set_alone_and_a_read_alone_are_unchanged);
	MU_RUN(a_failed_set_is_reported_and_the_read_does_not_run);
	MU_RUN(a_failed_read_is_reported_once);
	MU_RUN(a_sniffing_mode_sets_and_reads_after_the_mode_starts);
	MU_RUN(a_failure_in_a_sniffing_mode_still_stops_the_board);
	MU_RUN(an_access_address_read_and_the_crc_options_all_run);
	MU_RUN(the_target_report_is_on_standard_output);
	MU_RUN(the_faux_slave_and_target_addresses_are_separate);
	MU_RUN(the_reports_move_to_stderr_when_the_capture_takes_stdout);
	MU_RUN(the_callback_reports_on_the_same_stream_as_the_tool);
	MU_RUN(an_interrupted_capture_stops_and_exits_zero);
	MU_RUN(a_failed_poll_is_reported_on_stderr_and_exits_1);
	MU_RUN(a_short_poll_reply_is_reported);
	MU_RUN(a_failed_target_set_and_jamming_are_reported_on_stderr);
	MU_RUN(the_interference_options_are_refused_without_f_or_p);
	MU_RUN(a_sniffing_mode_refuses_the_access_address_and_slave_options);
	MU_RUN(the_usage_text_documents_the_set_and_the_read);
	MU_RUN(the_usage_text_names_the_jam_aliases);
	MU_RUN(the_help_option_exits_0_with_the_usage_text_on_stdout);
	MU_RUN(a_bad_command_line_is_reported_on_stderr);
	MU_RUN(a_mode_the_board_refuses_stops_the_run);
	MU_RUN(a_capture_file_that_cannot_be_created_is_named);
	MU_RUN(the_jam_aliases_select_the_same_mode_as_interfere);
	return mu_report();
}
