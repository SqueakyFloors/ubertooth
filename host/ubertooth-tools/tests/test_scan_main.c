/*
 * Hardware-independent tests of ubertooth-scan's main(): the sweep the scan
 * needs, and what the tool reports and exits with when the board refuses it.
 *
 * ubertooth-scan.c is compiled into this test with main() renamed, and the
 * libubertooth functions it calls are replaced by the fakes below, so the real
 * command-line handling and reporting run against a scripted device. The
 * faked receive ends the capture loop on its first turn, so no case waits on a
 * board.
 *
 * BlueZ is faked throughout: finding and opening a local adapter, which every
 * run does before it reaches the Ubertooth, the inquiry -s asks for, and every
 * read the extended report of -x makes of a device the inquiry found. No case
 * needs a Bluetooth adapter on the machine running the tests, which is the
 * only way those paths can be covered: the lab host has every BlueZ tool and
 * no adapter. Each of them can be made to fail, so what the tool says, which
 * stream it says it on, and the exit status are pinned here.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../../firmware/tests/minunit.h"

#define main ubertooth_scan_main
#include "ubertooth-scan.c"
#undef main

#include "run_main.h"

/* The -V answer, which libubertooth gives in every tool: faked here, so this
 * test stays about the tool's own command-line handling. The answer's own
 * cases are in test_version_report.c. */
int ubertooth_print_versions(FILE *out, const char *selector)
{
	(void)selector;
	fprintf(out, "version (test)\n");
	return 0;
}

/* ---- scripted device ---------------------------------------------------- */

static struct {
	int start_fails;     /* no board answers the -U selection */
	/* The stop a signal recorded while the board was being opened, which
	 * the tool reads before it asks anything of the board. The tool reads
	 * it more than once, so stop_after_reads says from which read the stop
	 * is seen: 0 for the first, which is a stop while the board was being
	 * opened, 1 for one that arrived during the inquiry. */
	int stop_requested;
	int stop_after_reads;
	int stop_reads;
	int api_rc;          /* returned by ubertooth_check_api() */
	int api_released;    /* whether that call released the board itself */
	int closes;          /* ubertooth_close() */
	int frees;           /* ubertooth_free() */
	int channel_rc;      /* returned by cmd_set_channel_quiet() */
	int modulation_rc;   /* returned by cmd_set_modulation_quiet() */
	int last_modulation; /* the modulation the capture was set to */
	int last_channel;    /* the channel the capture was set to, in MHz */
	int timeout;         /* seconds passed to ubertooth_set_timeout() */
	int stops;
	int registrations;   /* ubertooth_catch_stop_signals() */
	int receives;        /* turns of the capture loop */
	int inquiries;       /* BlueZ inquiries the run ran */
	/* Returned by the three calls that set the capture up, so that each
	 * failure path can be run: the transfer, the poll thread, the request
	 * that starts the capture. */
	int bulk_init_rc;
	int thread_start_rc;
	int rx_syms_rc;
	/* The two BlueZ failures: no adapter of that name, and one that is
	 * there but will not open. Each is the errno the call leaves behind. */
	int devid_fails;
	int devid_errno;     /* 0 for a failure that left no errno behind */
	int open_fails;
	int open_errno;      /* likewise for the open */
	/* What the inquiry -s asks for reports: how many devices it found, or
	 * a failure and the errno it left behind. */
	int inquiry_responses;
	int inquiry_fails;
	int inquiry_errno;
	/* The extended report -x asks of a device found: whether the
	 * connection can be made at all, and what the remote says about AFH. */
	int connect_fails;
	int connect_errno;
	int afh_read_fails;
	int afh_errno;
	uint8_t afh_mode;
	uint8_t afh_map[AFH_MAP_BYTES];
	/* The reads of the report that can each fail on their own, and the
	 * errno they all leave behind. */
	int name_fails;
	int version_fails;
	int features_fail;
	int devinfo_fails;
	int clock_fails;
	int ext_features_fail;   /* every extended-features read fails */
	int ext_pages;           /* feature pages the device reports beyond 0 */
	int ext_page_fails;      /* that one page fails, 0 for none */
	int ext_feature_reads;
	int read_errno;
	unsigned int slept;  /* seconds the run would have waited */
	char adapter[16];    /* the adapter name -b asked for */
	int dev_fd;          /* the adapter handle the tool was given */
	int dev_fd_closed;   /* whether the run gave it back */
	/* Every device call in order, one letter each: c set the channel,
	 * s start sending symbols. */
	char calls[16];
	/* 1 to have the teardown's STOP refused, so the board is left running
	 * the mode the capture started. */
	int stop_refused;
} fake;

static ubertooth_t fake_ut;
static int fake_devh;

/* A libubertooth global the tool reads; it lives in ubertooth.c, which is not
 * linked here. */
int max_ac_errors = 2;

static void record(char call)
{
	size_t length = strlen(fake.calls);

	if (length + 1 < sizeof(fake.calls)) {
		fake.calls[length] = call;
		fake.calls[length + 1] = '\0';
	}
}

static void reset_survey(void);

static void reset_fake(void)
{
	memset(&fake, 0, sizeof(fake));
	memset(&fake_ut, 0, sizeof(fake_ut));
	fake_ut.devh = (struct libusb_device_handle*)&fake_devh;
	fake.dev_fd = -1;
	reset_survey();
}

/* ---- faked BlueZ adapter ------------------------------------------------ */

int hci_devid(const char* str)
{
	snprintf(fake.adapter, sizeof(fake.adapter), "%s", str ? str : "");
	if (fake.devid_fails) {
		/* The tool clears errno before the call, so 0 here is a failure
		 * that set none -- which BlueZ does not promise to do. */
		errno = fake.devid_errno;
		return -1;
	}
	return 0;
}

/* A real descriptor, so the tool's close() is a real close and giving the
 * handle back can be checked, but one that is not a Bluetooth socket. */
int hci_open_dev(int dev_id)
{
	(void)dev_id;
	if (fake.open_fails) {
		/* As above: the tool clears errno before the call, so 0 here is
		 * a failure that set none. */
		errno = fake.open_errno;
		return -1;
	}
	fake.dev_fd = open("/dev/null", O_RDONLY);
	return fake.dev_fd;
}

/* The inquiry -s runs, faked so that no case needs a Bluetooth adapter. It
 * finds whatever the case asked for, or fails. */
int hci_inquiry(int dev_id, int len, int num_rsp, const uint8_t* lap,
                inquiry_info** ii, long flags)
{
	int found;

	(void)dev_id;
	(void)len;
	(void)num_rsp;
	(void)lap;
	(void)flags;
	fake.inquiries++;
	if (fake.inquiry_fails) {
		/* The tool clears errno before the call, so 0 here is a failure
		 * that set none. */
		errno = fake.inquiry_errno;
		return -1;
	}
	/* The tool allocates the array and hands it over for the call to fill
	 * in; one address is enough to reach the per-device report. */
	for (found = 0; found < fake.inquiry_responses; found++)
		memset(&(*ii)[found].bdaddr, 0x11 + found,
		       sizeof((*ii)[found].bdaddr));
	return fake.inquiry_responses;
}

/* ---- faked remote device, for the extended report of -x ----------------- */
/*
 * Everything -x asks of a device found: the local adapter's settings, the
 * connection, the remote's name, version and features, its clock offset and
 * its AFH channel map. Faked so that the report, and which stream each of its
 * lines goes to, can be pinned on a machine with no Bluetooth adapter -- which
 * includes the lab host, so this is the only check the moved lines in
 * extra_info() get.
 */

int hci_devinfo(int dev_id, struct hci_dev_info* di)
{
	(void)dev_id;
	if (fake.devinfo_fails) {
		errno = fake.read_errno;
		return -1;
	}
	memset(di, 0, sizeof(*di));
	/* The local adapter supports extended features, which is what makes the
	 * tool ask the remote for them. */
	di->features[7] |= LMP_EXT_FEAT;
	return 0;
}

/* The extended features, which the tool reads once for page 0 and then once
 * per page the device says it has. pages is how many pages beyond 0 the device
 * reports; ext_features_fail fails every call. */
int hci_read_remote_ext_features(int dd, uint16_t handle, uint8_t page,
                                 uint8_t* max_page, uint8_t* features, int to)
{
	(void)dd;
	(void)handle;
	(void)to;
	fake.ext_feature_reads++;
	if (fake.ext_features_fail
	    || (fake.ext_page_fails != 0 && page == fake.ext_page_fails)) {
		errno = fake.read_errno;
		return -1;
	}
	if (max_page != NULL)
		*max_page = fake.ext_pages;
	memset(features, 0, 8);
	/* Page 0 says the device has extended features, which is how the tool
	 * reaches the pages above it. */
	if (page == 0)
		features[7] |= LMP_EXT_FEAT;
	return 0;
}

int hci_create_connection(int dd, const bdaddr_t* ba, uint16_t ptype,
                          uint16_t clkoffset, uint8_t rswitch,
                          uint16_t* handle, int to)
{
	(void)dd;
	(void)ba;
	(void)ptype;
	(void)clkoffset;
	(void)rswitch;
	(void)to;
	if (fake.connect_fails) {
		errno = fake.connect_errno;
		return -1;
	}
	*handle = 42;
	return 0;
}

/* The name is reported by default. The first line of a device's results reads
 * "[unknown]" when this fails, which is the only place a failed read is
 * reported as a value rather than left out. */
int hci_read_remote_name(int dd, const bdaddr_t* ba, int len, char* name,
                         int to)
{
	(void)dd;
	(void)ba;
	(void)to;
	if (fake.name_fails) {
		errno = fake.read_errno;
		return -1;
	}
	snprintf(name, len, "a nearby device");
	return 0;
}

int hci_read_remote_version(int dd, uint16_t handle, struct hci_version* ver,
                            int to)
{
	(void)dd;
	(void)handle;
	(void)to;
	if (fake.version_fails) {
		errno = fake.read_errno;
		return -1;
	}
	memset(ver, 0, sizeof(*ver));
	return 0;
}

int hci_read_remote_features(int dd, uint16_t handle, uint8_t* features, int to)
{
	(void)dd;
	(void)handle;
	(void)to;
	if (fake.features_fail) {
		errno = fake.read_errno;
		return -1;
	}
	memset(features, 0, 8);
	/* The device supports extended features, so the tool goes on to read
	 * them, as it does against a device of any recent vintage. */
	features[7] |= LMP_EXT_FEAT;
	return 0;
}

int hci_read_clock_offset(int dd, uint16_t handle, uint16_t* offset, int to)
{
	(void)dd;
	(void)handle;
	(void)to;
	if (fake.clock_fails) {
		errno = fake.read_errno;
		return -1;
	}
	*offset = 0;
	return 0;
}

int hci_read_afh_map(int dd, uint16_t handle, uint8_t* mode, uint8_t* map,
                     int to)
{
	(void)dd;
	(void)handle;
	(void)to;
	if (fake.afh_read_fails) {
		/* The mode and the map are left exactly as the caller had them,
		 * which is what a failed read does. */
		errno = fake.afh_errno;
		return -1;
	}
	*mode = fake.afh_mode;
	memcpy(map, fake.afh_map, sizeof(fake.afh_map));
	return 0;
}

int hci_disconnect(int dd, uint16_t handle, uint8_t reason, int to)
{
	(void)dd;
	(void)handle;
	(void)reason;
	(void)to;
	return 0;
}

/* The tool waits a second after making a connection, for the link to settle.
 * No case here depends on real time, and a dozen of them would otherwise pay
 * that wait each, so the wait is recorded rather than taken. */
unsigned int sleep(unsigned int seconds)
{
	fake.slept += seconds;
	return 0;
}

/* ---- faked survey results ----------------------------------------------- */
/*
 * The piconets the Ubertooth half gathered. libbtbb's own survey is real and
 * finds nothing in a test, so the piconet handed out here is a real one with a
 * real map set on it: what the tool reads back is libbtbb's, and only the
 * walk over the results is faked.
 */

static btbb_piconet* survey_result;
static int survey_handed_out;

static void reset_survey(void)
{
	if (survey_result != NULL)
		btbb_piconet_unref(survey_result);
	survey_result = NULL;
	survey_handed_out = 0;
}

btbb_piconet* btbb_next_survey_result(void)
{
	if (survey_result == NULL || survey_handed_out)
		return NULL;
	survey_handed_out = 1;
	return survey_result;
}

/* One piconet with the given LAP and AFH map. uap is the piconet's UAP, or -1
 * for one the scan never recovered: the results line is then the "??:??:??"
 * form and the scan has no address to ask anything of. */
static void one_survey_result(uint32_t lap, int uap, const uint8_t* map)
{
	survey_result = btbb_piconet_new();
	mu_assert(survey_result != NULL, "btbb_piconet_new failed");
	btbb_init_piconet(survey_result, lap);
	if (uap >= 0) {
		btbb_piconet_set_uap(survey_result, (uint8_t)uap);
		btbb_piconet_set_flag(survey_result, BTBB_UAP_VALID, 1);
	}
	btbb_piconet_set_afh_map(survey_result, (uint8_t*)map);
	survey_handed_out = 0;
}

/* ---- faked Ubertooth ---------------------------------------------------- */

ubertooth_t* ubertooth_start_select(const char* u_arg)
{
	(void)u_arg;
	if (fake.start_fails)
		return NULL; /* the library has reported it already */
	return &fake_ut;
}

int ubertooth_check_api(ubertooth_t* ut)
{
	/* The library releases the board itself when the firmware reports a
	 * version this host will not drive; a version it could not read at all
	 * leaves the board open for the caller to close. */
	if (fake.api_rc < 0 && fake.api_released) {
		ut->devh = NULL;
		fake.stops++;
	}
	return fake.api_rc;
}

void ubertooth_close(ubertooth_t* ut)
{
	ut->devh = NULL;
	fake.closes++;
}

void ubertooth_stop(ubertooth_t* ut)
{
	ut->devh = NULL; /* as the library's cleanup leaves it */
	fake.stops++;
}

/* The report about a refused request asks whether the board has left the bus
 * before it blames the board's firmware. Nothing here unplugs a board, so it
 * is always still there. */
int ubertooth_device_gone(const void* board)
{
	(void)board;
	return 0;
}

/* Whether the teardown's STOP was refused, which leaves the board running the
 * mode the capture started. Scripted, because the fake board above always
 * takes it. */
int ubertooth_stop_failed(ubertooth_t* ut)
{
	(void)ut;
	return fake.stop_refused;
}

/* Counted rather than freeing: the handle above is this test's own static. */
void ubertooth_free(ubertooth_t* ut)
{
	(void)ut;
	fake.frees++;
}

int cmd_set_channel_quiet(struct libusb_device_handle* devh, u16 channel)
{
	(void)devh;
	record('c');
	fake.last_channel = channel;
	return fake.channel_rc;
}

/* The quiet form: it prints nothing, so what the cases below see on standard
 * error is the tool's own report. */
int cmd_rx_syms_quiet(struct libusb_device_handle* devh)
{
	(void)devh;
	record('s');
	return fake.rx_syms_rc;
}

int cmd_set_modulation_quiet(struct libusb_device_handle* devh, u16 mod)
{
	(void)devh;
	record('m');
	fake.last_modulation = mod;
	return fake.modulation_rc;
}

/* The real one folds the signal handler's own flag into the handle; the
 * fake handler below sets the handle directly, so reading it is enough here. */
int ubertooth_stopping(ubertooth_t* ut)
{
	return ut->stop_ubertooth;
}

void ubertooth_catch_stop_signals(void)
{
	fake.registrations++;
}

int ubertooth_stop_requested(void)
{
	return fake.stop_requested && fake.stop_reads++ >= fake.stop_after_reads;
}

/* The fake open always finds the board, so it never gives up on a stop. */
int ubertooth_open_stopped(void)
{
	return 0;
}

void ubertooth_set_timeout(ubertooth_t* ut, int seconds)
{
	(void)ut;
	fake.timeout = seconds;
}

int ubertooth_bulk_init(ubertooth_t* ut)
{
	(void)ut;
	if (fake.bulk_init_rc < 0)
		/* As the library reports a transfer libusb would not take: the
		 * libusb code in the line, -1 to the caller. */
		fprintf(stderr, "error: submitting the receive transfer failed "
		        "(%d)\n", LIBUSB_ERROR_NO_DEVICE);
	return fake.bulk_init_rc;
}

/* pthread_create() returns an errno, which is positive, and the tool has to
 * read it that way: the fake hands back whatever the case asked for. */
int ubertooth_bulk_thread_start()
{
	return fake.thread_start_rc;
}

void ubertooth_bulk_thread_stop()
{
}

/* Ends the capture loop on its first turn: decoding is covered by the
 * callback's own tests, and nothing here should wait on a board. */
int ubertooth_bulk_receive(ubertooth_t* ut, rx_callback cb, void* cb_args)
{
	(void)cb;
	(void)cb_args;
	fake.receives++;
	ut->stop_ubertooth = 1;
	return 0;
}

void cb_scan(ubertooth_t* ut, void* args)
{
	(void)ut;
	(void)args;
}

/* No case here fails a stream: that path has its own tests. */
/* The end-of-capture gap report, which the tool calls and no case here reads:
 * a capture whose records never carried FIFO_OVERFLOW has nothing to say. */
void ubertooth_report_capture_losses(ubertooth_t* ut)
{
	(void)ut;
}

int ubertooth_stream_failed(ubertooth_t* ut)
{
	(void)ut;
	return 0;
}

/* ---- running the tool --------------------------------------------------- */

static struct run_result run_scan(const char* const* args)
{
	struct run_result result = run_tool_main(ubertooth_scan_main, args);

	/* Read as soon as the run is over, before this file opens anything
	 * that could be given the descriptor's number. */
	fake.dev_fd_closed = fake.dev_fd >= 0
	                     && fcntl(fake.dev_fd, F_GETFD) == -1
	                     && errno == EBADF;
	return result;
}

#define RUN(...) run_scan((const char* const[]){ "ubertooth-scan", __VA_ARGS__, NULL })

/* ---- tests -------------------------------------------------------------- */

/* The scan the board took: the modulation it reads is stated, the capture is
 * asked for, the sweep is stated with a channel above the band, then the
 * results. Nothing goes to standard error.
 *
 * The order is the point. Stating the sweep first lost it whenever a mode was
 * already running, because a mode start puts the board's hop mode back to "no
 * hopping" -- so a scan after a killed tool listened to one channel and
 * printed an AFH map that means nothing. The modulation goes the other way
 * round, before the capture request: that is what the mode start programs the
 * demodulator from, on a board an interrupted `ubertooth-dump -l` had left set
 * to low energy, and every reset that could throw it away leaves Basic Rate. */
MU_TEST(a_scan_the_board_took_sweeps_and_exits_0)
{
	struct run_result r;

	reset_fake();
	r = RUN("-t", "5");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(fake.calls, "msc") == 0, fake.calls);
	mu_assert_int_eq(9999, fake.last_channel);
	mu_assert_int_eq(5, fake.timeout);
	mu_assert_int_eq(1, fake.registrations);
	mu_assert_int_eq(1, fake.receives);
	mu_assert_int_eq(1, fake.stops);
	/* The handle and its 72 MB fifo go back, rather than being left to
	 * process exit. */
	mu_assert_int_eq(1, fake.frees);
	mu_assert(strcmp(fake.adapter, "hci0") == 0, fake.adapter);
	mu_assert(fake.dev_fd_closed, "the run gave the adapter handle back");
	/* The two headings say which half of the scan is running, so they are
	 * progress and go to standard error. A scan that found nothing
	 * therefore leaves standard output empty, and a caller collecting it
	 * gets the devices alone. */
	mu_assert(strcmp(r.err, "\nUbertooth scan\n\nScan results:\n") == 0,
	          r.err);
	mu_assert(r.out[0] == '\0', r.out);
}

#ifndef TEST_REAL_API_REPORTS
/* Built against an installed libubertooth, so there is no library source to
 * compile in: the report for a request the board's firmware has not got is a
 * stub here, and STALLED below then expects no line from it. */
void ubertooth_report_unsupported_request(const char* what, int code)
{
	(void)what;
	(void)code;
}
#endif

/* The whole report for a refused request: one error line naming the request,
 * why it failed and the libusb code, and a note only where there is something
 * to try. */
#ifdef TEST_REAL_API_REPORTS
#define STALLED(what) \
	"error: could not " what \
	": the board's firmware does not support it (stalled, libusb -9)\n"
#else
#define STALLED(what) ""
#endif
#define NO_ANSWER \
	"error: could not set sweep mode" \
	": the board did not answer in time (libusb -7)\n" \
	"note: if a mode is running, stop it with ubertooth-util -S; " \
	"a board that does not answer at all usually resets itself within " \
	"a few seconds, so run the command again, then reset it with " \
	"ubertooth-util -r on the same board if it stays silent, and " \
	"unplug it or power-cycle its hub port if that does not reach " \
	"it either\n"

/* A scan the board would not stop leaves that board running the capture mode,
 * and no other tool can open it until it is reset or re-plugged, so the run
 * did not do what was asked. The results it found are still printed, and
 * libubertooth has already named the refused request. */
MU_TEST(a_scan_whose_teardown_was_refused_exits_1)
{
	struct run_result r;

	reset_fake();
	fake.stop_refused = 1;
	r = RUN("-t", "5");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(1, fake.stops);
	/* The results were still reached: the heading they go under is on
	 * standard error with the rest of the tool's progress. */
	mu_assert(strstr(r.err, "Scan results:") != NULL, r.err);
	mu_assert(strstr(r.err, "error:") == NULL, r.err);
}

/* A refused sweep is named, with the reason and what to try, on standard
 * error, and ends the command there: it used to be ignored, so the board
 * stayed on whatever channel it was left on and the tool printed an AFH map
 * that meant nothing, reporting success. */
MU_TEST(a_refused_sweep_is_named_and_exits_1)
{
	struct run_result r;

	reset_fake();
	fake.channel_rc = -9; /* LIBUSB_ERROR_PIPE, how a board refuses one */
	r = RUN("-t", "5");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(fake.calls, "msc") == 0, fake.calls);
	/* The capture had started, so it is stopped, which gives the board
	 * back; the loop that would have gathered the results is never
	 * reached. */
	mu_assert_int_eq(0, fake.receives);
	mu_assert_int_eq(1, fake.stops);
	mu_assert_int_eq(0, fake.closes);
	mu_assert(fake.dev_fd_closed, "the run gave the adapter handle back");
	/* Standard output stays empty: the heading the Ubertooth results go
	 * under is printed once the board is sweeping, so a run that gathers no
	 * results does not leave a heading over nothing. */
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strcmp(r.err, STALLED("set sweep mode")) == 0, r.err);

	/* A board that took the request and said nothing is the case the note
	 * is about. */
	reset_fake();
	fake.channel_rc = -7; /* LIBUSB_ERROR_TIMEOUT */
	r = RUN("-t", "5");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(0, fake.receives);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strcmp(r.err, NO_ANSWER) == 0, r.err);

	/* A code with no plain words still names the request and the number. */
	reset_fake();
	fake.channel_rc = -5;
	r = RUN("-t", "5");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.err,
	          "error: could not set sweep mode (libusb -5)\n") == 0, r.err);
}

/* The sweep is now stated after the capture has been asked for, which is after
 * the BlueZ inquiry of -s has run. A refused sweep therefore ends a scan that
 * has already printed what BlueZ found: that half needs no Ubertooth, and what
 * it reported is what it saw. The Ubertooth half still gathers nothing, and
 * does not print its heading. */
MU_TEST(a_refused_sweep_keeps_the_bluez_results)
{
	struct run_result r;

	reset_fake();
	fake.channel_rc = -9;
	r = RUN("-s", "-t", "5");
	mu_assert_int_eq(1, r.rc);
	mu_assert_int_eq(1, fake.inquiries);
	/* The inquiry found nothing, so its heading on standard error is all
	 * there is to show that half ran, and the Ubertooth half announces
	 * nothing it did not do. */
	mu_assert(strcmp(r.err, "HCI scan\n" STALLED("set sweep mode")) == 0,
	          r.err);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert_int_eq(0, fake.receives);
}

/* An inquiry that failed is half of what -s asked for, so it is named on
 * standard error and the status says so. It used to print the bare name of the
 * BlueZ function, with no prefix, and the run exited 0 as though the inquiry
 * had simply found nothing. The Ubertooth half still runs: it needs no
 * adapter. */
MU_TEST(an_inquiry_that_failed_is_named_and_exits_1)
{
	char expected[160];
	struct run_result r;

	reset_fake();
	fake.inquiry_fails = 1;
	fake.inquiry_errno = EIO;
	r = RUN("-s", "-t", "5");
	snprintf(expected, sizeof(expected),
	         "HCI scan\nerror: the BlueZ inquiry failed: %s\n"
	         "\nUbertooth scan\n\nScan results:\n", strerror(EIO));
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.err, expected) == 0, r.err);
	mu_assert(r.out[0] == '\0', r.out);
	/* The board was still used, and given back. */
	mu_assert_int_eq(1, fake.receives);
	mu_assert_int_eq(1, fake.stops);

	/* An inquiry that failed without leaving an errno behind says what it
	 * knows rather than printing "Undefined error: 0" as the reason. */
	reset_fake();
	fake.inquiry_fails = 1;
	r = RUN("-s", "-t", "5");
	mu_assert_int_eq(1, r.rc);
	mu_assert(strcmp(r.err, "HCI scan\nerror: the BlueZ inquiry failed\n"
	          "\nUbertooth scan\n\nScan results:\n") == 0, r.err);
	mu_assert(r.out[0] == '\0', r.out);

	/* An inquiry a stop cut short is not the adapter's failure: BlueZ ends
	 * the inquiry on the signal, but the user asked for that, so nothing is
	 * named and the run ends with status 0. The stop arrives after the
	 * board has been opened, so the earlier check lets the inquiry start. */
	reset_fake();
	fake.inquiry_fails = 1;
	fake.inquiry_errno = EINTR;
	fake.stop_requested = 1;
	fake.stop_after_reads = 1;
	r = RUN("-s", "-t", "5");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.err, "HCI scan\n") == 0, r.err);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert_int_eq(0, fake.receives);
}

/* The piconets the Ubertooth half found are the result, so they and their AFH
 * maps are the whole of standard output. The map line used to come from inside
 * libbtbb, which must not write to that stream: it is the same line, printed
 * by the tool. */
MU_TEST(the_survey_results_are_the_whole_of_standard_output)
{
	const uint8_t map[AFH_MAP_BYTES] = {
		0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef, 0x12, 0x34
	};
	struct run_result r;

	reset_fake();
	one_survey_result(0x9e8b33, -1, map);
	r = RUN("-t", "5");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strcmp(r.out, "??:??:??:9E:8B:33\n"
	          "AFH map: 0x0123456789abcdef1234\n") == 0, r.out);
	mu_assert(strcmp(r.err, "\nUbertooth scan\n\nScan results:\n") == 0,
	          r.err);
}

/* The extended report of -x: every line describing the device goes to standard
 * output, and the progress line that precedes it goes to standard error, so a
 * caller collecting the report does not find "Requesting information ..." in
 * it. No Bluetooth adapter is needed for any of this, which is why it is
 * checked here: the lab host has none, so a real run cannot reach these lines
 * at all. */
MU_TEST(the_extended_report_of_a_device_is_on_standard_output)
{
	struct run_result r;

	reset_fake();
	fake.inquiry_responses = 1;
	fake.afh_mode = FOLLOW_AFH_MODE_ON;
	memset(fake.afh_map, 0xff, sizeof(fake.afh_map));
	r = RUN("-s", "-x", "-t", "5");
	mu_assert_int_eq(0, r.rc);
	/* The device the inquiry found, then the report -x asked for. */
	mu_assert(strstr(r.out, "11:11:11:11:11:11\ta nearby device\n") != NULL,
	          r.out);
	mu_assert(strstr(r.out, "\tBD Address:  11:11:11:11:11:11\n") != NULL,
	          r.out);
	mu_assert(strstr(r.out, "\tDevice Name: a nearby device\n") != NULL,
	          r.out);
	mu_assert(strstr(r.out, "\tLMP Version: ") != NULL, r.out);
	mu_assert(strstr(r.out, "\tFeatures: 0x00 ") != NULL, r.out);
	mu_assert(strstr(r.out, "\tClock offset: 0x0000\n") != NULL, r.out);
	mu_assert(strstr(r.out, "\tAFH Map: 0xffffffffffffffffffff\n") != NULL,
	          r.out);
	/* Progress and headings on standard error, and no failure. */
	mu_assert(strstr(r.err, "Requesting information ...\n") != NULL, r.err);
	mu_assert(strstr(r.out, "Requesting information") == NULL, r.out);
	mu_assert(strstr(r.err, "error:") == NULL, r.err);
	/* The report waits for the connection it just made to settle. */
	mu_assert_int_eq(1, (int)fake.slept);
}

/* A device with feature pages above page 0 has each of them read and reported,
 * and a page it will not give up is named like the other reads. */
MU_TEST(the_feature_pages_of_a_device_are_read_and_reported)
{
	char expected[128];
	struct run_result r;

	reset_fake();
	fake.inquiry_responses = 1;
	fake.ext_pages = 2;
	r = RUN("-s", "-x", "-t", "5");
	mu_assert_int_eq(0, r.rc);
	/* Page 0 is named as such once there are more pages, then one line per
	 * page above it. */
	mu_assert(strstr(r.out, "\tFeatures page 0: ") != NULL, r.out);
	mu_assert(strstr(r.out, "\tFeatures page 1: ") != NULL, r.out);
	mu_assert(strstr(r.out, "\tFeatures page 2: ") != NULL, r.out);
	mu_assert(strstr(r.err, "error:") == NULL, r.err);

	/* A page the device will not give up: named, that page left out, and
	 * the pages around it still read. */
	reset_fake();
	fake.inquiry_responses = 1;
	fake.ext_pages = 2;
	fake.ext_page_fails = 2;
	fake.read_errno = EIO;
	r = RUN("-s", "-x", "-t", "5");
	snprintf(expected, sizeof(expected),
	         "error: reading features page 2 failed: %s\n", strerror(EIO));
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, expected) != NULL, r.err);
	mu_assert(strstr(r.out, "\tFeatures page 1: ") != NULL, r.out);
	mu_assert(strstr(r.out, "\tFeatures page 2: ") == NULL, r.out);
}

/* Each read of the report can fail on its own. The line it would have filled
 * in is left out, the read is named on standard error, and the run exits 1:
 * the report is not what -x asked for. They used to be left out in silence,
 * and a failed features read had its zeroed buffer printed as the device's
 * own features. */
MU_TEST(each_read_of_the_report_that_fails_is_named)
{
	struct {
		int* flag;
		const char* message;
		const char* missing;
	} cases[] = {
		{ &fake.name_fails, "error: reading the device name failed",
		  "Device Name" },
		{ &fake.version_fails, "error: reading the LMP version failed",
		  "LMP Version" },
		{ &fake.features_fail, "error: reading the features failed",
		  "Features" },
		/* This one reads the local adapter, not the device, so the line
		 * says which; it used to call exit(1). */
		{ &fake.devinfo_fails,
		  "error: cannot get the local adapter's settings",
		  "BD Address" },
		{ &fake.clock_fails, "error: reading clock offset failed",
		  "Clock offset" },
		{ &fake.ext_features_fail,
		  "error: reading the extended features failed", NULL },
	};
	char expected[128];
	struct run_result r;
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		reset_fake();
		fake.inquiry_responses = 1;
		fake.afh_mode = FOLLOW_AFH_MODE_ON;
		fake.read_errno = EIO;
		*cases[i].flag = 1;
		r = RUN("-s", "-x", "-t", "5");
		snprintf(expected, sizeof(expected), "%s: %s\n",
		         cases[i].message, strerror(EIO));
		mu_assert_int_eq(1, r.rc);
		mu_assert(strstr(r.err, expected) != NULL, r.err);
		if (cases[i].missing != NULL)
			mu_assert(strstr(r.out, cases[i].missing) == NULL, r.out);
		/* The run carried on to the end rather than exiting where the
		 * read failed: three of these called exit(1), which left the
		 * board open in whatever mode it was in. */
		mu_assert(strstr(r.err, "Scan results:") != NULL, r.err);
		mu_assert_int_eq(1, fake.stops);
		mu_assert(fake.dev_fd_closed, "the run gave the adapter handle back");
		/* The device the inquiry found is still a result. */
		mu_assert(strstr(r.out, "11:11:11:11:11:11\t") != NULL, r.out);
	}
}

/* A remote that is not using AFH has no map to report, which is a note about
 * the device rather than a line of its report: standard error, and nothing
 * where the map would have been. */
MU_TEST(a_remote_without_afh_is_noted_on_standard_error)
{
	struct run_result r;

	reset_fake();
	fake.inquiry_responses = 1;
	fake.afh_mode = FOLLOW_AFH_MODE_OFF;
	r = RUN("-s", "-x", "-t", "5");
	/* The device answered, so this is not a failure of the report. */
	mu_assert_int_eq(0, r.rc);
	mu_assert(strstr(r.err, "note: AFH disabled\n") != NULL, r.err);
	mu_assert(strstr(r.out, "AFH") == NULL, r.out);

	/* A mode byte that is neither off nor on says so: it used to be taken
	 * for AFH off, so a device reporting nonsense was reported as a device
	 * that had answered. */
	reset_fake();
	fake.inquiry_responses = 1;
	fake.afh_mode = 0x42;
	r = RUN("-s", "-x", "-t", "5");
	mu_assert_int_eq(1, r.rc);
	/* The wording ubertooth-follow uses for the same reply. */
	mu_assert(strstr(r.err, "error: the remote device reported AFH mode "
	          "0x42, which is neither off (0x00) nor on (0x01)\n") != NULL,
	          r.err);
	mu_assert(strstr(r.err, "AFH disabled") == NULL, r.err);
	mu_assert(strstr(r.out, "AFH") == NULL, r.out);
}

/* A map read that failed is named on standard error, and no map is printed:
 * the reply was never written, so the report used to read an uninitialised
 * mode byte and could print a map that had never been read. */
MU_TEST(a_failed_afh_map_read_is_named_and_prints_no_map)
{
	char expected[128];
	struct run_result r;

	reset_fake();
	fake.inquiry_responses = 1;
	fake.afh_read_fails = 1;
	fake.afh_errno = EIO;
	r = RUN("-s", "-x", "-t", "5");
	snprintf(expected, sizeof(expected),
	         "error: HCI read AFH map request failed: %s\n", strerror(EIO));
	/* Part of what -x asked for did not happen, so the status says so even
	 * though the scan printed the rest. */
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, expected) != NULL, r.err);
	mu_assert(strstr(r.out, "AFH") == NULL, r.out);
	/* The rest of the device's report is still there. */
	mu_assert(strstr(r.out, "\tClock offset: 0x0000\n") != NULL, r.out);
}

/* A device that will not take a connection ends its report there, with one
 * "error: " line on standard error. */
MU_TEST(a_connection_that_cannot_be_made_is_named)
{
	char expected[128];
	struct run_result r;

	reset_fake();
	fake.inquiry_responses = 1;
	fake.connect_fails = 1;
	fake.connect_errno = EHOSTDOWN;
	r = RUN("-s", "-x", "-t", "5");
	snprintf(expected, sizeof(expected),
	         "error: cannot create connection: %s\n", strerror(EHOSTDOWN));
	mu_assert_int_eq(1, r.rc);
	mu_assert(strstr(r.err, expected) != NULL, r.err);
	/* The address the inquiry found is still reported; nothing of the
	 * report that needed the connection is. */
	mu_assert(strstr(r.out, "11:11:11:11:11:11\ta nearby device\n") != NULL, r.out);
	mu_assert(strstr(r.out, "BD Address") == NULL, r.out);
}

/* A piconet the Ubertooth half found is given to -x by an address whose two
 * most significant bytes are a guess, because ordinary piconet traffic does not
 * carry them, so a connection that cannot be made says nothing about what was
 * asked for: a note, and status 0. Reported as a failure it would have made
 * exit 1 the ordinary outcome of ubertooth-scan -x. */
MU_TEST(a_survey_result_that_cannot_be_reached_is_a_note_not_a_failure)
{
	const uint8_t map[AFH_MAP_BYTES] = { 0 };
	char expected[160];
	struct run_result r;

	reset_fake();
	/* A piconet whose UAP the scan recovered, which is the only kind -x is
	 * given: the NAP is still guessed. */
	one_survey_result(0x9e8b33, 0x44, map);
	fake.connect_fails = 1;
	fake.connect_errno = EHOSTDOWN;
	r = RUN("-x", "-t", "5");
	mu_assert_int_eq(0, r.rc);
	snprintf(expected, sizeof(expected),
	         "note: no extended report: %s; a sniffed piconet gives no NAP, "
	         "so the address this connects to is a guess\n",
	         strerror(EHOSTDOWN));
	mu_assert(strstr(r.err, expected) != NULL, r.err);
	mu_assert(strstr(r.err, "error:") == NULL, r.err);
	/* The piconet is still a result, map and all. */
	mu_assert(strstr(r.out, "??:??:44:9E:8B:33\ta nearby device\n") != NULL, r.out);
	mu_assert(strstr(r.out, "AFH map: 0x00000000000000000000\n") != NULL,
	          r.out);
}

/* Each of the three steps that set the capture up can fail. Each exits 1 with
 * one "error: " line on standard error and nothing on standard output, and
 * each gives back both the board and the BlueZ adapter handle this tool holds
 * alongside it. The handle is the one easily forgotten: the scan opens it
 * before it touches the Ubertooth at all.
 *
 * The status is the point of the three cases together. Each used to hand the
 * failing call's own return value to the exit status, which a shell wraps into
 * a byte, so a script read -1 as 255 and the libusb codes as 247 and 249.
 *
 * The poll thread's case asks for a positive code, because that is what
 * pthread_create() returns: a "< 0" test let a failure through, and the
 * capture then had nobody to complete its transfers and waited forever with
 * nothing arriving. */
MU_TEST(a_capture_that_cannot_be_set_up_exits_1_and_gives_everything_back)
{
	char thread_failed[128];
	struct {
		const char* calls;   /* the requests made before the failure */
		int bulk_init_rc;
		int thread_start_rc;
		int rx_syms_rc;
		const char* message;
	} cases[] = {
		{ "",  -1, 0, 0,
		  "error: submitting the receive transfer failed (-4)\n" },
		{ "",  0, EAGAIN, 0, NULL /* filled in below */ },
		/* The capture request is named the way the sweep request is, so
		 * the line names the request rather than leaving it to
		 * libubertooth's shared sink, which names none. */
		{ "ms", 0, 0, LIBUSB_ERROR_TIMEOUT,
		  "error: could not start the capture: the board did not answer "
		  "in time (libusb -7)\n"
		  "note: if a mode is running, stop it with ubertooth-util -S; "
		  "a board that does not answer at all usually resets itself "
		  "within a few seconds, so run the command again, then reset "
		  "it with ubertooth-util -r on the same board if it stays "
		  "silent, and unplug it or power-cycle its hub port if that "
		  "does not reach it either\n" },
		{ "ms", 0, 0, LIBUSB_ERROR_PIPE, STALLED("start the capture") },
	};
	struct run_result r;
	size_t i;

	snprintf(thread_failed, sizeof(thread_failed),
	         "error: could not start the thread that collects the capture:"
	         " %s\n", strerror(EAGAIN));
	cases[1].message = thread_failed;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		reset_fake();
		fake.bulk_init_rc = cases[i].bulk_init_rc;
		fake.thread_start_rc = cases[i].thread_start_rc;
		fake.rx_syms_rc = cases[i].rx_syms_rc;
		r = RUN("-t", "5");
		mu_assert_int_eq(1, r.rc);
		mu_assert(strcmp(fake.calls, cases[i].calls) == 0, fake.calls);
		/* No sweep was stated, and no results were gathered. */
		mu_assert_int_eq(0, fake.receives);
		/* The board goes back with a STOP, since a mode may be
		 * running by the time any of these fail. */
		mu_assert_int_eq(1, fake.stops);
		mu_assert_int_eq(0, fake.closes);
		mu_assert_int_eq(1, fake.frees);
		mu_assert(fake.dev_fd_closed, "the run gave the adapter handle back");
		mu_assert(r.out[0] == '\0', r.out);
		mu_assert(strcmp(r.err, cases[i].message) == 0, r.err);
	}
}

/* The two BlueZ failures, each one "error: " line on standard error. The
 * first used to go to standard output, where a caller redirecting the scan
 * results to a file collected it with them and saw nothing on the terminal;
 * the second was a perror() line with no "error: " prefix. */
MU_TEST(a_bluez_failure_is_reported_on_standard_error)
{
	char expected[128];
	struct run_result r;

	/* No adapter of that name. */
	reset_fake();
	fake.devid_fails = 1;
	fake.devid_errno = ENODEV;
	r = RUN("-b", "hci7");
	snprintf(expected, sizeof(expected),
	         "error: Bluetooth device hci7 not found: %s\n",
	         strerror(ENODEV));
	mu_assert_int_eq(1, r.rc);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strcmp(r.err, expected) == 0, r.err);
	/* Nothing was opened: the adapter is found before the board. */
	mu_assert(fake.calls[0] == '\0', fake.calls);
	mu_assert_int_eq(0, fake.stops);

	/* The same lookup failing without leaving an errno behind says what it
	 * knows rather than printing "Undefined error: 0" as the reason. */
	reset_fake();
	fake.devid_fails = 1;
	r = RUN("-b", "hci7");
	mu_assert_int_eq(1, r.rc);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strcmp(r.err,
	          "error: Bluetooth device hci7 not found\n") == 0, r.err);

	/* An adapter that is there but will not open. */
	reset_fake();
	fake.open_fails = 1;
	fake.open_errno = EPERM;
	r = RUN("-t", "5");
	snprintf(expected, sizeof(expected),
	         "error: cannot open Bluetooth device hci0: %s\n",
	         strerror(EPERM));
	mu_assert_int_eq(1, r.rc);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strcmp(r.err, expected) == 0, r.err);
	mu_assert(fake.calls[0] == '\0', fake.calls);
	mu_assert_int_eq(0, fake.stops);

	/* And the open failing without leaving an errno behind, which would
	 * otherwise read "Undefined error: 0" as the reason. */
	reset_fake();
	fake.open_fails = 1;
	r = RUN("-t", "5");
	mu_assert_int_eq(1, r.rc);
	mu_assert(r.out[0] == '\0', r.out);
	mu_assert(strcmp(r.err,
	          "error: cannot open Bluetooth device hci0\n") == 0, r.err);
}

/* A board that cannot be opened, and a firmware the host will not drive, each
 * end the command before the sweep request. */
/* A stop that arrived while the board was being opened ends the run before
 * any capture starts, with the board and the adapter handle given back. */
MU_TEST(a_stop_while_the_board_was_opened_captures_nothing)
{
	struct run_result r;

	reset_fake();
	fake.stop_requested = 1;
	r = RUN("-t", "5");
	mu_assert_int_eq(0, r.rc);
	mu_assert(fake.calls[0] == '\0', fake.calls);
	mu_assert_int_eq(0, fake.receives);
	mu_assert_int_eq(1, fake.closes);
	mu_assert_int_eq(1, fake.frees);
	mu_assert(fake.dev_fd_closed, "the run gave the adapter handle back");
}

MU_TEST(a_board_or_firmware_the_tool_cannot_use_exits_1)
{
	struct run_result r;

	reset_fake();
	fake.start_fails = 1;
	r = RUN("-t", "5");
	mu_assert_int_eq(1, r.rc);
	mu_assert(fake.calls[0] == '\0', fake.calls);
	mu_assert(fake.dev_fd_closed, "the run gave the adapter handle back");

	/* A version mismatch: the library has released the board. The tool
	 * closes it all the same, which does nothing on a board already
	 * released, and gives the handle back. */
	reset_fake();
	fake.api_rc = -1;
	fake.api_released = 1;
	r = RUN("-t", "5");
	mu_assert_int_eq(1, r.rc);
	mu_assert(fake.calls[0] == '\0', fake.calls);
	mu_assert(fake.dev_fd_closed, "the run gave the adapter handle back");
	mu_assert_int_eq(1, fake.closes);
	mu_assert_int_eq(1, fake.frees);

	/* A version that could not be read at all leaves the board open, so
	 * this is the close that releases it. */
	reset_fake();
	fake.api_rc = -1;
	r = RUN("-t", "5");
	mu_assert_int_eq(1, r.rc);
	mu_assert(fake.calls[0] == '\0', fake.calls);
	mu_assert(fake.dev_fd_closed, "the run gave the adapter handle back");
	mu_assert_int_eq(1, fake.closes);
	mu_assert_int_eq(1, fake.frees);
}

/* -h is what the user asked for, so it goes to standard output and exits 0,
 * without opening anything. */
MU_TEST(help_exits_0_on_standard_output)
{
	struct run_result r;

	reset_fake();
	r = RUN("-h");
	mu_assert_int_eq(0, r.rc);
	mu_assert(strstr(r.out, "Usage:") != NULL, r.out);
	mu_assert(r.err[0] == '\0', r.err);
	mu_assert(fake.calls[0] == '\0', fake.calls);
}

int main(void)
{
	/* This binary does not link libubertooth: the fakes above are the whole
	 * device, so any library call the tool grows becomes a link error
	 * rather than a silently real one. */
	MU_RUN(a_scan_the_board_took_sweeps_and_exits_0);
	MU_RUN(a_scan_whose_teardown_was_refused_exits_1);
	MU_RUN(a_refused_sweep_is_named_and_exits_1);
	MU_RUN(a_refused_sweep_keeps_the_bluez_results);
	MU_RUN(an_inquiry_that_failed_is_named_and_exits_1);
	MU_RUN(the_survey_results_are_the_whole_of_standard_output);
	MU_RUN(the_extended_report_of_a_device_is_on_standard_output);
	MU_RUN(a_remote_without_afh_is_noted_on_standard_error);
	MU_RUN(a_failed_afh_map_read_is_named_and_prints_no_map);
	MU_RUN(a_connection_that_cannot_be_made_is_named);
	MU_RUN(a_survey_result_that_cannot_be_reached_is_a_note_not_a_failure);
	MU_RUN(each_read_of_the_report_that_fails_is_named);
	MU_RUN(the_feature_pages_of_a_device_are_read_and_reported);
	MU_RUN(a_capture_that_cannot_be_set_up_exits_1_and_gives_everything_back);
	MU_RUN(a_bluez_failure_is_reported_on_standard_error);
	MU_RUN(a_stop_while_the_board_was_opened_captures_nothing);
	MU_RUN(a_board_or_firmware_the_tool_cannot_use_exits_1);
	MU_RUN(help_exits_0_on_standard_output);
	return mu_report();
}
