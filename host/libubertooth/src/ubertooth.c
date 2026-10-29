/*
 * Copyright 2010, 2011 Michael Ossmann
 *
 * This file is part of Project Ubertooth.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

/* dladdr(), which print_version() uses to say where libbtbb was loaded from.
 * glibc declares it only for this macro; macOS and the BSDs declare it
 * whatever is asked for. It has to come before the first header, because that
 * is where glibc reads it.
 *
 * Here rather than beside _DEFAULT_SOURCE in host/CMakeLists.txt, where the
 * other feature macro is set for every target: _GNU_SOURCE changes what the
 * standard headers declare, strerror_r's return type among it, so it is asked
 * for in the one file that needs it. */
#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "ubertooth.h"
#include "ubertooth_callback.h"
#include "ubertooth_control.h"
#include "ubertooth_interface.h"

#ifndef RELEASE
#define RELEASE "unknown"
#endif
#ifndef VERSION
#define VERSION "unknown"
#endif

uint32_t systime;
FILE* infile = NULL;
FILE* dumpfile = NULL;
/* Printed after "no Ubertooth at index": indexes are positions in the current
 * device list, so they shift when boards are added or removed. */
#define INDEX_SHIFT_HINT \
	"indexes change when boards are added or removed; select a board by " \
	"serial with -U<serial>\n"

int max_ac_errors = 2;

/* UBERTOOTH_TEST_NO_DEVICES=1 makes every selection path behave as if no
 * Ubertooth were attached, without listing or opening anything on the bus.
 * The tool tests set it for the tools they run, so a default `ctest` run can
 * neither depend on an attached board nor open one that another process is
 * using. Nothing in the tools or the library sets it, so a normal run is
 * unaffected. Exactly "1" turns it on, so a variable that happens to hold
 * anything else cannot hide a user's board. A tool that enumerates the bus
 * itself, such as ubertooth-dfu, calls this too. */
int ubertooth_devices_hidden(void)
{
	const char* value = getenv("UBERTOOTH_TEST_NO_DEVICES");

	return value != NULL && strcmp(value, "1") == 0;
}

/* ubertooth_devices_hidden() for the paths that report what they found: the
 * first of them to hide a device names the switch, so a variable left in an
 * environment does not look like a missing board. The quiet paths pass
 * announce = 0 and stay silent, as their callers require. */
static int devices_hidden(int announce)
{
	static int announced = 0;

	if (!ubertooth_devices_hidden())
		return 0;
	if (announce && !announced) {
		fprintf(stderr, "note: UBERTOOTH_TEST_NO_DEVICES is set, so no board "
		        "is selected\n");
		announced = 1;
	}
	return 1;
}

int do_exit = 1;
pthread_t poll_thread;

unsigned int packet_counter_max;

/*
 * The file this run reads libbtbb from, or NULL if the loader cannot say.
 *
 * Asked of the loader rather than recorded when the tools were built, because
 * the question a machine with two libbtbb installs asks is which one is being
 * used now: a build directory configured against one, and a run that finds
 * another first, are the same program with different packet decoding.
 *
 * Two builds answer with the program instead of a library, and both answers
 * are honest. One linked libbtbb into the program. The other was built
 * without position-independent code, where the program's own entry for the
 * function is its canonical address and the program exports it, so every way
 * of asking -- this one and a dlsym() by name -- resolves inside the program.
 * Every toolchain this fork supports builds position-independent by default.
 */
static const char* loaded_libbtbb_path(void)
{
	/* A function pointer, written to the member it fits and read from the
	 * other: converting one to void* directly is not something C promises,
	 * and -Wpedantic says so. */
	union {
		const char* (*function)(void);
		const void* address;
	} probe;
	Dl_info info;

	probe.function = btbb_get_version;
	if (dladdr(probe.address, &info) == 0)
		return NULL;
	if (info.dli_fname == NULL || info.dli_fname[0] == '\0')
		return NULL;
	return info.dli_fname;
}

/* Labelled like every other version line a tool prints, so the host tools'
 * version cannot be mistaken for a board's firmware revision, and one line
 * per thing asked about so each can be grepped for on its own. The stream is
 * the caller's: the tools pass their standard output, where the user asked
 * for it. */
void ubertooth_print_host_version(FILE* out) {
	const char* btbb_path = loaded_libbtbb_path();

	fprintf(out, UBERTOOTH_VERSION_LABEL_HOST ": libubertooth %s (%s), "
	        "libbtbb %s (%s)\n",
	        VERSION, RELEASE, btbb_get_version(), btbb_get_release());
	/* Which libbtbb, not just which version of it: a distribution or
	 * Homebrew package beside this fork's build reports a version too, and
	 * the path is the only thing that tells the two apart. */
	fprintf(out, "libbtbb loaded from: %s\n",
	        btbb_path != NULL ? btbb_path : "unknown");
}

void print_version(void) {
	ubertooth_print_host_version(stdout);
}

/* The handle register_cleanup_handler() was given, for cleanup_exit() below.
 * It is the only handle the library keeps of its own, and a tool cannot reach
 * it, so ubertooth_free() clears it: a signal arriving after a tool gave its
 * handle back must not find a pointer to freed memory. */
static ubertooth_t* cleanup_devh = NULL;

/* Forgets a handle the library was holding for its signal handlers, so the
 * caller may free it. Done inside ubertooth_free() rather than asked of each
 * tool: the handlers are the library's, so keeping track of them is too.
 *
 * Storing a pointer is one instruction on the processors this fork builds
 * for, so a signal arriving here finds either the handle or nothing, never
 * half of either. */
static void forget_registered_handle(ubertooth_t* ut)
{
	if (cleanup_devh == ut)
		cleanup_devh = NULL;
}

/* 1 once the user has asked the run to stop: Ctrl-C, SIGTERM, SIGHUP,
 * SIGQUIT, or an alarm set by something outside the tool. Set by the handlers
 * below and by nothing else.
 *
 * A handler may touch very little: of the objects that outlive it, C17
 * 7.14.1.1p5 lets it assign to one declared volatile sig_atomic_t and read
 * nothing else. The handlers used to find the capture handle through a
 * pointer variable and store a byte in it, which is two things the handler
 * was not allowed to do, so nothing guaranteed that the capture loop would
 * ever see the stop. ubertooth_stopping() below is where this flag reaches
 * the handle, on the run's own thread. */
static volatile sig_atomic_t stop_requested;

/* 1 once the -t timer has gone off. Kept apart from stop_requested above
 * because the two are forgotten at different times: a run of two phases
 * forgets the timer between them (see ubertooth_clear_timeout_stop()), and
 * must not forget that the user asked it to stop. */
static volatile sig_atomic_t timeout_expired;

static void cleanup(int sig __attribute__((unused)))
{
	stop_requested = 1;
}

int ubertooth_stopping(ubertooth_t* ut)
{
	if (stop_requested || timeout_expired)
		ut->stop_ubertooth = 1;
	return ut->stop_ubertooth;
}

int ubertooth_stop_requested(void)
{
	return stop_requested != 0;
}

void ubertooth_clear_timeout_stop(ubertooth_t* ut)
{
	timeout_expired = 0;
	ut->stop_ubertooth = 0;
}

void ubertooth_reset_stop_signals(void)
{
	stop_requested = 0;
	timeout_expired = 0;
}

/* Unreachable: no tool passes do_exit = 1 to register_cleanup_handler(), and
 * none should while this runs ubertooth_stop() from a signal handler.
 * ubertooth_stop() finishes the receive transfer, which means a libusb event
 * loop and libusb's own locks; entering those from a handler can deadlock
 * against the thread the signal interrupted. Stopping the capture from the
 * handler and tearing down on the way out, which cleanup() above does, is the
 * way to end a run. */
static void cleanup_exit(int sig __attribute__((unused)))
{
	if (cleanup_devh)
		ubertooth_stop(cleanup_devh);

	exit(0);
}

/* Documented with the declaration in ubertooth.h.
 *
 * SIGHUP (terminal closed) and SIGALRM (a timeout wrapper such as
 * `perl -e 'alarm N; exec ...'`) stop the device cleanly as Ctrl-C does;
 * killed by them, a tool left its mode running on the board. A tool that
 * calls ubertooth_set_timeout() later installs its own SIGALRM handler.
 *
 * Installed with signal(), which on every system this fork builds for leaves
 * the handler in place after it has run, and which replaces an inherited
 * SIG_IGN rather than keeping it. Keeping it is the usual idiom for a tool
 * that must respect a parent's "ignore this"; here it would reinstate the very
 * window this closes, because a shell gives a background job SIGINT and
 * SIGQUIT set to SIG_IGN. The same override means a capture run under nohup
 * stops cleanly when the terminal closes rather than carrying on with its mode
 * left running on the board, which is what the SIGHUP handler is for. */
void ubertooth_catch_stop_signals(void)
{
	signal(SIGINT, cleanup);
	signal(SIGQUIT, cleanup);
	signal(SIGTERM, cleanup);
	signal(SIGHUP, cleanup);
	signal(SIGALRM, cleanup);
}

void register_cleanup_handler(ubertooth_t* ut, int do_exit) {
	cleanup_devh = ut;

	if (do_exit) {
		signal(SIGINT, cleanup_exit);
		signal(SIGQUIT, cleanup_exit);
		signal(SIGTERM, cleanup_exit);
		signal(SIGHUP, cleanup_exit);
		signal(SIGALRM, cleanup_exit);
		return;
	}
	ubertooth_catch_stop_signals();
}

/* The -t timer, which is a phase of the run ending rather than the user
 * asking the tool to stop: its own flag, so a two-phase run can tell them
 * apart. */
static void stop_transfers(int sig __attribute__((unused))) {
	timeout_expired = 1;
}

/* The handle is not needed: the alarm stops whichever capture is running,
 * through the same flag Ctrl-C uses, and a handler cannot follow a pointer to
 * a handle anyway. It stays in the signature because that is what a caller
 * means by it, and every call site has exactly one capture. */
void ubertooth_set_timeout(ubertooth_t* ut __attribute__((unused)), int seconds) {
	/* Upon SIGALRM, call stop_transfers() */
	if (signal(SIGALRM, stop_transfers) == SIG_ERR) {
		fprintf(stderr, "error: cannot catch SIGALRM: %s\n",
		        strerror(errno));
		exit(1);
	}
	alarm(seconds);
}

/* Set once ubertooth_count() has reported a bus it could not list. */
static int list_failure_said;

int ubertooth_count(void) {
	struct libusb_device **usb_list = NULL;
	struct libusb_device_descriptor desc;
	int usb_devs, i, r;
	int ubertooth_device_count = 0;

	if (devices_hidden(1))
		return 0;

	r = libusb_init(NULL);
	if (r < 0) {
		fprintf(stderr, "error: libusb_init failed (got 1.0?)\n");
		return -1;
	}

	usb_devs = libusb_get_device_list(NULL, &usb_list);
	/* Cleared on a list that worked, so a bus that comes back and then fails
	 * again is reported again rather than once per process. */
	if (usb_devs >= 0)
		list_failure_said = 0;
	if (usb_devs < 0) {
		/* Not an empty bus: reported as itself, and as a count the caller
		 * can tell from zero, which is what the "no Ubertooth" reports
		 * read. Said once per run, because this count is polled -- every
		 * 100 ms while ubertooth-util -r waits for a board to come back --
		 * and a bus that cannot be listed stays that way. */
		if (!list_failure_said) {
			list_failure_said = 1;
			fprintf(stderr, "error: could not list USB devices: %s "
			        "(libusb %d)\n", libusb_strerror(usb_devs), usb_devs);
		}
		libusb_exit(NULL);
		return -1;
	}

	for(i = 0 ; i < usb_devs ; ++i) {
		r = libusb_get_device_descriptor(usb_list[i], &desc);
		if(r < 0) {
			fprintf(stderr, "warning: could not read the USB descriptor of "
			        "device %d, so it is not counted\n", i);
			continue; /* desc was not filled in */
		}
		if ((desc.idVendor == TC13_VENDORID && desc.idProduct == TC13_PRODUCTID)
		    || (desc.idVendor == U0_VENDORID && desc.idProduct == U0_PRODUCTID)
		    || (desc.idVendor == U1_VENDORID && desc.idProduct == U1_PRODUCTID))
		{
			ubertooth_device_count++;
		}
	}

	libusb_free_device_list(usb_list,1);
	/* Balance the libusb_init() above: the count may be polled many times. */
	libusb_exit(NULL);
	return ubertooth_device_count;
}

/* Which entry of the found-Ubertooth list a -U index selects, or -1 for none;
 * ut_ct is how many Ubertooth devices that list holds. A negative index means
 * "no -U given": it selects the only device, or nothing when several are
 * attached. An index must be present in the list, so an index past the end
 * selects nothing, with one device attached as with several.
 * Shared by the loud and the quiet path of find_ubertooth_device_opt() so both
 * accept and refuse exactly the same selections. */
static int pick_ubertooth_device(int ubertooth_device, int ut_ct)
{
	if (ubertooth_device < 0)
		return (ut_ct == 1) ? 0 : -1;
	return (ubertooth_device < ut_ct) ? ubertooth_device : -1;
}

/*
 * Why the last open failed, for ubertooth_open_failure_reason(). One phrase
 * per failure the open paths below can report, set as each is taken and
 * cleared when an open starts. A string literal, so there is nothing to free
 * and nothing that goes stale while a caller holds it.
 */
static const char* open_failure_reason = NULL;

/* Set when the last open gave up because the user asked the run to stop,
 * rather than because it could not find or open a board. Kept apart from the
 * reason string above so a caller can test it without comparing words: the
 * two outcomes need different exit statuses. */
static int open_stopped;

static void note_open_failure(const char* reason)
{
	open_failure_reason = reason;
	/* Any reason recorded here replaces a stop recorded earlier, so the
	 * flag always describes the last thing that happened to an open rather
	 * than something an earlier one left. The stopped path sets it again
	 * straight after its own call. */
	open_stopped = 0;
}

/* Documented with the declaration in ubertooth.h. */
int ubertooth_open_stopped(void)
{
	return open_stopped;
}

/* Documented with the declaration in ubertooth.h. */
const char* ubertooth_open_failure_reason(void)
{
	return open_failure_reason != NULL ? open_failure_reason
	                                   : "it could not be opened";
}

/* The refusal for a selection that found no board at all, in the wording
 * every tool uses for it: ubertooth-util -U all and ubertooth-btbr say the
 * same line. The caller used to print "could not open Ubertooth device"
 * instead, after whatever the selection had already said, so a bad index or
 * several attached boards got a second line with no prefix that named no
 * cause of its own.
 */
static void report_no_devices(void)
{
	fprintf(stderr, "error: no Ubertooth devices found\n");
}

int ubertooth_device_gone(const void* board)
{
	struct libusb_device* dev;
	struct libusb_device** usb_list;
	ssize_t count, i;
	int present = 0;

	if (board == NULL)
		return 0;
	/* The device object behind the open handle. The handle holds a
	 * reference to it, so it stays valid even for a board that has been
	 * unplugged; what goes away is its place in the list below. */
	dev = libusb_get_device((struct libusb_device_handle*)board);
	if (dev == NULL)
		return 0;
	count = libusb_get_device_list(NULL, &usb_list);
	/* A bus that could not be listed says nothing about this board, so the
	 * caller is left with what it already had. */
	if (count < 0)
		return 0;
	/* libusb hands back the same device object for a board it has already
	 * discovered, and leaves out one that has gone, so the comparison is
	 * of pointers and reaches no hardware. */
	for (i = 0; i < count; i++) {
		if (usb_list[i] == dev) {
			present = 1;
			break;
		}
	}
	libusb_free_device_list(usb_list, 1);
	return !present;
}

/* Opens the Ubertooth at enumeration index ubertooth_device, or the only one
 * when ubertooth_device is negative. When quiet, nothing is printed and no
 * command is sent: an index that is not present, or several devices with no
 * index, simply return NULL (without listing serials, which would mean sending
 * a command to every device). */
/* Keeps the board's firmware API version against the handle just opened, for
 * the reports about requests that board refuses: they name the firmware that
 * refused them. Done here, where every open happens, so a tool that never
 * asks for the version still has it quoted -- ubertooth-util asks for it only
 * under -I. The board reports the version as its USB device version, so this
 * is the descriptor libusb has already read and the board is sent nothing. A
 * descriptor that cannot be read leaves nothing recorded, and the reports
 * then give only their own half. */
static void note_opened_board_api(struct libusb_device_handle* devh,
                                  libusb_device* dev)
{
	struct libusb_device_descriptor desc;

	if (libusb_get_device_descriptor(dev, &desc) == 0)
		ubertooth_note_api_version(devh, desc.bcdDevice);
}

static struct libusb_device_handle* find_ubertooth_device_opt(int ubertooth_device,
                                                              int quiet)
{
	struct libusb_device **usb_list = NULL;
	struct libusb_device_handle *devh = NULL;
	struct libusb_device_descriptor desc;
	int usb_devs, i, r, ret, pick, ut_ct = 0;
	int *ubertooth_devs = NULL;

	/* Hidden devices: the same outcome as nothing attached, and reported as
	 * that, so a test run reads like a bench with no board on it. */
	if (devices_hidden(!quiet)) {
		note_open_failure("no Ubertooth is attached");
		if (!quiet)
			report_no_devices();
		return NULL;
	}

	usb_devs = libusb_get_device_list(NULL, &usb_list);
	if (usb_devs <= 0) {
		if (usb_devs == 0)
			libusb_free_device_list(usb_list, 1);
		/* A bus that could not be listed is not an empty bus, so it says
		 * what happened rather than "no Ubertooth". */
		if (usb_devs < 0) {
			note_open_failure("the USB devices could not be listed");
			if (!quiet)
				fprintf(stderr, "error: could not list USB devices: %s "
				        "(libusb %d)\n", libusb_strerror(usb_devs), usb_devs);
		} else {
			note_open_failure("no Ubertooth is attached");
			if (!quiet)
				report_no_devices();
		}
		return NULL;
	}
	ubertooth_devs = calloc(usb_devs, sizeof(int));
	if (ubertooth_devs == NULL) {
		libusb_free_device_list(usb_list, 1);
		note_open_failure("the host ran out of memory");
		if (!quiet)
			fprintf(stderr, "error: out of memory listing the USB devices\n");
		return NULL;
	}

	for(i = 0 ; i < usb_devs ; ++i) {
		r = libusb_get_device_descriptor(usb_list[i], &desc);
		if(r < 0) {
			/* desc was not filled in: skip this device rather than test
			 * whatever the previous iteration left in it. */
			if (!quiet)
				fprintf(stderr, "warning: could not read the USB descriptor "
				        "of device %d, so it is skipped\n", i);
			continue;
		}
		if ((desc.idVendor == TC13_VENDORID && desc.idProduct == TC13_PRODUCTID)
		    || (desc.idVendor == U0_VENDORID && desc.idProduct == U0_PRODUCTID)
		    || (desc.idVendor == U1_VENDORID && desc.idProduct == U1_PRODUCTID))
		{
			ubertooth_devs[ut_ct] = i;
			ut_ct++;
		}
	}
	/* Only a selected entry of ubertooth_devs[] is ever used as an index into
	 * usb_list[]; an index that is not in the list selects nothing. */
	pick = pick_ubertooth_device(ubertooth_device, ut_ct);

	if (quiet) {
		if (pick >= 0) {
			if (libusb_open(usb_list[ubertooth_devs[pick]], &devh) != 0)
				devh = NULL;
			else
				note_opened_board_api(devh,
				                      usb_list[ubertooth_devs[pick]]);
		}
		free(ubertooth_devs);
		libusb_free_device_list(usb_list, 1);
		return devh;
	}
	if (pick >= 0) {
		ret = libusb_open(usb_list[ubertooth_devs[pick]], &devh);
		if (ret) {
			/* The usual cause on Linux is that the udev rules are not
			 * installed, which a report saying "no board" would send the
			 * user looking for the wrong thing. */
			note_open_failure(ret == LIBUSB_ERROR_ACCESS
			                  ? "access to the board was denied"
			                  : "it could not be opened");
			show_libusb_error(ret);
			devh = NULL;
		}
		else
			note_opened_board_api(devh, usb_list[ubertooth_devs[pick]]);
	}
	else if (ut_ct == 0) {
		note_open_failure("no Ubertooth is attached");
		report_no_devices();
	}
	else if (ubertooth_device < 0) {
		/* One line that says what to do, then the boards, so the list is
		 * something to read and not part of the refusal. -U takes a serial
		 * or the first characters of one as well as an index, which the
		 * old "Use '-U' to specify device number" did not say. The wording
		 * is ubertooth-btbr's, so every tool refuses this the same way. */
		fprintf(stderr, "error: multiple Ubertooth devices found; select one "
		        "with -U <index|serial>\n");
		for(i = 0 ; i < ut_ct ; ++i) {
			char serial_c[UBERTOOTH_SERIAL_STR_LEN] = {0};

			ret = libusb_open(usb_list[ubertooth_devs[i]], &devh);
			if (ret) {
				/* Self-contained, because show_libusb_error() reports some
				 * codes as a whole line of their own, and libusb's own
				 * naming is the useful part here: the usual cause of an open
				 * that fails is missing permissions. */
				fprintf(stderr, "  Device %d: could not be opened: %s "
				        "(libusb %d)\n", i, libusb_strerror(ret), ret);
				continue;
			}
			/* cmd_get_serial()'s own timeout, which the loud serial scan
			 * below uses as well. */
			if (ubertooth_read_serial_string(devh, serial_c, sizeof(serial_c),
			                                 1000) == 0)
				fprintf(stderr, "  Device %d: %s\n", i, serial_c);
			else
				fprintf(stderr, "  Device %d: serial unreadable\n", i);
			libusb_close(devh);
		}
		devh = NULL;
		note_open_failure("more than one board is attached and none was "
		                  "selected");
	}
	else {
		note_open_failure("no board answered at that index");
		fprintf(stderr, "error: no Ubertooth at index %d (%d attached)\n"
		        INDEX_SHIFT_HINT, ubertooth_device, ut_ct);
	}
	free(ubertooth_devs);
	libusb_free_device_list(usb_list,1);
	return devh;
}

static struct libusb_device_handle* find_ubertooth_device(int ubertooth_device)
{
	return find_ubertooth_device_opt(ubertooth_device, 0);
}

/* How a message names the board a selection is working on: by serial, by
 * index, or not at all when neither is known. Normally the whole serial of the
 * board that matched; a longer string is truncated rather than refused, which
 * only a caller passing a raw -U argument could reach. The result lives in one
 * static buffer, so it is printed straight away and not held across another
 * call to this. */
static const char* board_label(const char* serial, int index)
{
	static char label[sizeof("Ubertooth ") + UBERTOOTH_SERIAL_STR_LEN];

	if (serial != NULL && serial[0] != '\0')
		snprintf(label, sizeof(label), "Ubertooth %s", serial);
	else if (index >= 0)
		snprintf(label, sizeof(label), "Ubertooth at index %d", index);
	else
		snprintf(label, sizeof(label), "the Ubertooth");
	return label;
}

/* Documented with the declaration in ubertooth.h. Why ACCESS is another
 * program here and a permissions problem in show_libusb_error(): the call. A
 * board nobody may open fails at the open and never reaches a claim, so by
 * this point the interface exists and belongs to somebody else. */
void ubertooth_report_claim_failed(const char* board, int err)
{
	if (board == NULL)
		board = "the Ubertooth";
	if (err == LIBUSB_ERROR_BUSY || err == LIBUSB_ERROR_ACCESS) {
		note_open_failure("in use by another program");
		fprintf(stderr, "error: %s is in use by another program "
		        "(libusb %d)\n", board, err);
	} else {
		note_open_failure("its USB interface could not be claimed");
		fprintf(stderr, "error: claiming the USB interface of %s failed "
		        "(libusb %d)\n", board, err);
	}
}

/* Documented with the declaration in ubertooth.h. */
int ubertooth_read_serial_string(struct libusb_device_handle* devh, char* out,
                                 size_t size, unsigned timeout_ms)
{
	uint8_t serial[UBERTOOTH_SERIAL_LEN] = {0};
	int sr, j;

	if (size < UBERTOOTH_SERIAL_STR_LEN)
		return -1;
	sr = libusb_control_transfer(devh,
	        LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_IN,
	        UBERTOOTH_GET_SERIAL, 0, 0, serial, UBERTOOTH_SERIAL_LEN, timeout_ms);
	/* serial[0] is a status byte from the firmware, not part of the number. */
	if (sr != UBERTOOTH_SERIAL_LEN || serial[0] != 0)
		return -1;
	for (j = 1; j < UBERTOOTH_SERIAL_LEN; j++)
		sprintf(&out[(j - 1) * 2], "%02x", serial[j]);
	return 0;
}

/* Does a -U serial string select this board? The string matches the whole
 * serial or its start (a prefix), compared case-insensitively because -s
 * prints lowercase hex while a serial copied from elsewhere may be upper case.
 * Ubertooth serials differ in their leading digits and share their trailing
 * ones, so only the start is matched; the caller refuses a string that matches
 * more than one board instead of opening whichever enumerated first. */
static int serial_matches(const char* query, const char* serial)
{
	size_t ql = strlen(query), sl = strlen(serial);

	if (ql == 0 || ql > sl)
		return 0;
	return strncasecmp(query, serial, ql) == 0;
}

/* Reports the attached Ubertooth devices a serial scan could not read, which
 * changes what its result means: the string the user gave may well be the
 * serial of one of them. A board that could not be opened at all usually means
 * a permissions problem (missing udev rules on Linux), so the first open error
 * is shown with the count. */
static void report_unscanned(int unopened, int open_err, int unread)
{
	if (unopened) {
		fprintf(stderr, "%d attached Ubertooth device%s could not be opened\n",
		        unopened, unopened == 1 ? "" : "s");
		/* A note, not a second error: the refusal is the line above, and
		 * this says why one of the boards it counted could not be read.
		 * libusb supplies the words; the usual cause of a refused open is
		 * missing permissions, which on Linux is the udev rules. */
		if (open_err)
			fprintf(stderr, "note: %s (libusb %d)\n",
			        libusb_strerror(open_err), open_err);
	}
	if (unread) {
		fprintf(stderr, "%d attached Ubertooth device%s did not answer the "
		        "serial read\n", unread, unread == 1 ? "" : "s");
		/* The count alone does not say why, and the string the user gave may
		 * well be the serial of one of those boards. This runs once per
		 * command, at the end of the scan, so the note is not repeated. The
		 * remedy is given by index, because a board whose serial could not
		 * be read cannot be selected by serial; the index path reads none. */
		fprintf(stderr, "note: a board that does not answer may be in use by "
		        "another program, or busy in a mode; stop a mode with "
		        "ubertooth-util -U <index> -S, or reset a board that "
		        "answers nothing with -r\n");
	}
}

/* One extra line after a selection that found no Ubertooth, when a board is
 * sitting in the DFU bootloader. Such a board is being flashed, or a flash
 * left it there, which is not the same as a board that is not attached -- and
 * the two read alike: "no Ubertooth found" in an overnight regression log was
 * the one event nobody could explain until it turned out another process was
 * flashing the board at that moment.
 *
 * Only the message changes. The selection still found nothing, and the caller
 * still fails as it did before.
 *
 * It enumerates the bus itself rather than counting during the scan above,
 * because this runs once, on the path where nothing was found. */
void ubertooth_note_dfu_bootloader(void)
{
	struct libusb_device** usb_list = NULL;
	struct libusb_device_descriptor desc;
	ssize_t usb_devs;
	int i, boards = 0;

	if (ubertooth_devices_hidden())
		return;
	/* A session of its own, because this is called from both sides: from a
	 * selection that failed, which still holds one, and from -V with no
	 * board, where the count has already given its own back -- and listing
	 * devices without a session is undefined (it crashed). libusb counts
	 * sessions, so the caller that holds one keeps it. */
	if (libusb_init(NULL) < 0)
		return;
	usb_devs = libusb_get_device_list(NULL, &usb_list);
	if (usb_devs <= 0) {
		if (usb_devs == 0)
			libusb_free_device_list(usb_list, 1);
		libusb_exit(NULL);
		return;
	}
	for (i = 0; i < usb_devs; ++i) {
		if (libusb_get_device_descriptor(usb_list[i], &desc) < 0)
			continue; /* desc was not filled in: skip this device */
		if (desc.idVendor == U1_DFU_VENDORID
		    && desc.idProduct == U1_DFU_PRODUCTID)
			boards++;
	}
	libusb_free_device_list(usb_list, 1);
	libusb_exit(NULL);

	if (boards == 0)
		return;
	/* The bootloader reports no serial number, so the note cannot say which
	 * board it is; the count is all there is to give. */
	if (boards == 1)
		fprintf(stderr, "note: a board is in the DFU bootloader, so no tool "
		        "can open it: it is being flashed, or a flash left it "
		        "there\n");
	else
		fprintf(stderr, "note: %d boards are in the DFU bootloader, so no "
		        "tool can open them: they are being flashed, or a flash "
		        "left them there\n", boards);
}

/* Find an Ubertooth by serial-number string (hex, as printed by -s), matched
 * by serial_matches(). Every attached device is read before one is chosen, so
 * a string that matches several is refused with the matching serials instead
 * of selecting one of them at random. Returns an open handle or NULL. */
static struct libusb_device_handle* find_ubertooth_device_serial_opt(
		const char* serial_number, int quiet, char matched[UBERTOOTH_SERIAL_STR_LEN])
{
	struct libusb_device **usb_list = NULL;
	struct libusb_device_handle *devh = NULL;
	struct libusb_device_descriptor desc;
	char (*matched_serials)[UBERTOOTH_SERIAL_STR_LEN] = NULL;
	int *matched_devs = NULL;
	int usb_devs, i, r, ret, matches = 0, unread = 0, unopened = 0, open_err = 0;
	/* Set once the scan gave up because the user asked the run to stop. */
	int stopped = 0;

	/* Hidden devices: nothing to match, so no board is opened and no serial
	 * is read; the "no Ubertooth found" report below is unchanged. */
	usb_devs = devices_hidden(!quiet) ? 0 : libusb_get_device_list(NULL, &usb_list);
	if (usb_devs < 0) {
		/* A bus that could not be listed is not the same as a serial that is
		 * not attached, so do not report it as one. */
		note_open_failure("the USB devices could not be listed");
		if (!quiet) {
			fprintf(stderr, "error: could not list USB devices: %s "
			        "(libusb %d)\n", libusb_strerror(usb_devs), usb_devs);
		}
		return NULL;
	}
	if (usb_devs == 0) {
		libusb_free_device_list(usb_list, 1);
		note_open_failure("no Ubertooth is attached");
		if (!quiet)
			fprintf(stderr, "error: no Ubertooth found with serial "
			        "matching '%s'\n", serial_number);
		return NULL;
	}
	matched_devs = calloc(usb_devs, sizeof(int));
	matched_serials = calloc(usb_devs, sizeof(*matched_serials));
	if (matched_devs == NULL || matched_serials == NULL) {
		free(matched_devs);
		free(matched_serials);
		libusb_free_device_list(usb_list, 1);
		/* As the index path does: the caller reports nothing of its own, so
		 * a silent NULL would exit 1 with no line at all. */
		note_open_failure("the host ran out of memory");
		if (!quiet)
			fprintf(stderr, "error: out of memory listing the USB devices\n");
		return NULL;
	}

	for (i = 0; i < usb_devs; ++i) {
		char serial_c[UBERTOOTH_SERIAL_STR_LEN] = {0};

		/* The user has asked the run to stop, so stop looking for a board.
		 * This is the slow part of starting a tool: every attached board is
		 * opened and asked for its serial, and one that does not answer
		 * costs a second. Working through the rest of the bus would hold
		 * the stop up for as long as that took. No board is chosen either:
		 * one opened here would only be stopped again. */
		if (ubertooth_stop_requested()) {
			stopped = 1;
			break;
		}

		r = libusb_get_device_descriptor(usb_list[i], &desc);
		if (r < 0)
			continue; /* desc was not filled in: skip this device */
		if (!((desc.idVendor == TC13_VENDORID && desc.idProduct == TC13_PRODUCTID)
		      || (desc.idVendor == U0_VENDORID && desc.idProduct == U0_PRODUCTID)
		      || (desc.idVendor == U1_VENDORID && desc.idProduct == U1_PRODUCTID)))
			continue;
		ret = libusb_open(usb_list[i], &devh);
		if (ret || devh == NULL) {
			/* Reported with the result, not here: an error printed mid-scan
			 * would belong to no board the user asked for. The first code is
			 * kept because the cause is usually the same for every board,
			 * missing udev rules being the common one. */
			if (unopened == 0)
				open_err = ret;
			unopened++;
			devh = NULL;
			continue;
		}
		/* The quiet caller polls a device that may be re-enumerating, so it
		 * waits briefly; the loud path keeps cmd_get_serial()'s timeout. */
		r = ubertooth_read_serial_string(devh, serial_c, sizeof(serial_c),
		                                 quiet ? UBERTOOTH_SERIAL_READ_QUIET_MS
		                                       : 1000);
		/* Closed again whether it matched or not: the chosen device is
		 * reopened below, once it is known to be the only match. */
		libusb_close(devh);
		devh = NULL;
		if (r != 0) {
			unread++; /* reported with the result */
			continue;
		}
		if (serial_matches(serial_number, serial_c)) {
			matched_devs[matches] = i;
			memcpy(matched_serials[matches], serial_c, sizeof(serial_c));
			matches++;
		}
	}

	/* Nothing is reported for a scan the user stopped: it found no board
	 * because it was not allowed to finish, which is not a failure. The
	 * caller reads ubertooth_open_stopped() and ends the run quietly.
	 *
	 * A scan that did finish is treated the same way only when it matched a
	 * board, because the flag is read again here and not only at the top of
	 * each turn above: a signal that arrived during the last board's serial
	 * read would otherwise open and claim a board the caller is about to
	 * stop, and a tool that transmits would have started before its loop
	 * looked at the flag. A finished scan that matched nothing still reports
	 * that, and the caller still exits 1: the answer is true whether or not
	 * the user interrupted the run, and a script that reads the status to
	 * learn whether its board is there must not be told it is. */
	if (stopped || (matches > 0 && ubertooth_stop_requested())) {
		note_open_failure("the run was stopped");
		open_stopped = 1;
		free(matched_devs);
		free(matched_serials);
		libusb_free_device_list(usb_list, 1);
		return NULL;
	}

	if (matches == 1) {
		ret = libusb_open(usb_list[matched_devs[0]], &devh);
		if (ret) {
			/* The board matched, so it is there: the selector was right and
			 * the open was not allowed. */
			note_open_failure(ret == LIBUSB_ERROR_ACCESS
			                  ? "access to the board was denied"
			                  : "it could not be opened");
			if (!quiet)
				show_libusb_error(ret);
			devh = NULL;
		}
		else {
			note_opened_board_api(devh, usb_list[matched_devs[0]]);
			/* The whole serial of the board that was selected, so a later
			 * failure names the board rather than the few characters of it
			 * the user happened to type. */
			if (matched != NULL)
				memcpy(matched, matched_serials[0],
				       UBERTOOTH_SERIAL_STR_LEN);
		}
	}
	else if (matches > 1) {
		note_open_failure("that serial matched more than one board");
		if (!quiet) {
			int same = 1;

			fprintf(stderr, "error: '%s' matches %d Ubertooth devices:",
			        serial_number, matches);
			for (i = 0; i < matches; ++i) {
				fprintf(stderr, "%s %s", i ? "," : "", matched_serials[i]);
				if (strcmp(matched_serials[0], matched_serials[i]) != 0)
					same = 0;
			}
			/* A longer string can only help while the matching serials
			 * differ: boards reporting one serial cannot be told apart by
			 * this selector. */
			if (same)
				fprintf(stderr, "\nthese Ubertooth devices report the same "
				        "serial number; select one by index with -U<index>\n");
			else
				fprintf(stderr, "\nselect one of them by a longer prefix of "
				        "its serial number\n");
			report_unscanned(unopened, open_err, unread);
		}
	}
	else {
		/* A board whose serial could not be read cannot be matched, so a
		 * bus where none could be opened is not "that serial is not here":
		 * on Linux the usual cause is that the udev rules are missing. */
		note_open_failure(unopened > 0 && matches == 0 && unread == 0
		                  ? "the attached boards could not be opened"
		                  : "no board matched that serial");
		if (!quiet) {
			fprintf(stderr, "error: no Ubertooth found with serial "
			        "matching '%s'\n", serial_number);
			report_unscanned(unopened, open_err, unread);
			ubertooth_note_dfu_bootloader();
		}
	}
	free(matched_devs);
	free(matched_serials);
	libusb_free_device_list(usb_list, 1);
	return devh;
}

static struct libusb_device_handle* find_ubertooth_device_serial(
		char* serial_number, char matched[UBERTOOTH_SERIAL_STR_LEN])
{
	return find_ubertooth_device_serial_opt(serial_number, 0, matched);
}

/*
 * based on http://libusb.sourceforge.net/api-1.0/group__asyncio.html#ga9fcb2aa23d342060ebda1d0cf7478856
 */
static void rx_xfer_status(int status)
{
	char *error_name = "";

	/* A stalled receive endpoint is the board refusing to send the capture
	 * at all, which is what firmware without the mode the host asked for
	 * looks like from here, so it gets the same report as a refused
	 * request. The status is not a libusb error code, so there is none to
	 * quote. */
	if (status == LIBUSB_TRANSFER_STALL) {
		ubertooth_report_unsupported_request("receive the capture", 0);
		return;
	}

	switch (status) {
		case LIBUSB_TRANSFER_ERROR:
			error_name="Transfer error.";
			break;
		/* No LIBUSB_TRANSFER_TIMED_OUT or LIBUSB_TRANSFER_CANCELLED case:
		 * both are ordinary parts of a capture, handled by the caller
		 * before it reaches this. */
		case LIBUSB_TRANSFER_NO_DEVICE:
			error_name="Device disconnected.";
			break;
		case LIBUSB_TRANSFER_OVERFLOW:
			error_name="Device sent more data than requested.";
			break;
	}
	fprintf(stderr, "error: receive transfer failed: %s (%d)\n",
	        error_name, status);
}

/* Records a stream that died. Nothing is arriving any more, so the capture
 * stops here rather than running out its timeout and then reporting a clean
 * run. The status is kept as libusb gave it, and the first failure wins: what
 * ended the stream is what explains it. The caller has already printed the
 * message.
 *
 * Only a transfer that really failed gets here. A bulk timeout is what a
 * quiet board looks like, and a cancellation is how the teardown takes its
 * transfer back, so neither is a failure.
 *
 * This runs on the poll thread and writes only these two fields; the transfer
 * itself is left to the callback that is giving it up. The capture's own
 * thread reads the fields back after joining the poll thread. */
static void rx_stream_failed(ubertooth_t* ut, int status)
{
	if (ut->rx_stream_status == 0)
		ut->rx_stream_status = status != 0 ? status : LIBUSB_TRANSFER_ERROR;
	ut->stop_ubertooth = 1;
}

int ubertooth_stream_failed(ubertooth_t* ut)
{
	return ut != NULL && ut->rx_stream_status != 0;
}

int ubertooth_stop_failed(ubertooth_t* ut)
{
	return ut != NULL && ut->stop_status != 0;
}

/* Guards every read and write of ubertooth_t.rx_xfer and rx_xfer_state, the
 * receive transfer and who owns it. Two threads reach them: the transfer's
 * callback, which libusb runs on the poll thread, and the receive loop and
 * teardown on the run's own thread. Without a lock that is a data race, and a
 * compiler may keep either field in a register across the calls that read it,
 * however well the generated code happened to behave.
 *
 * One lock for the library, not one per handle. It is held for a couple of
 * assignments at a time and a process runs one capture, so there is nothing
 * to win by splitting it -- and a handle a caller zeroed itself would carry a
 * mutex that was never initialised.
 *
 * Nothing calls libusb while holding it. libusb runs the callback with its
 * own locks held, so a thread that held this one and then waited for libusb
 * could deadlock against the callback waiting for this one. */
static pthread_mutex_t rx_xfer_lock = PTHREAD_MUTEX_INITIALIZER;

/* The transfer and, when state is not NULL, who owns it: read together, so a
 * caller cannot see one field from before a change and the other from after. */
static struct libusb_transfer* rx_xfer_peek(ubertooth_t* ut, uint8_t* state)
{
	struct libusb_transfer* xfer;

	pthread_mutex_lock(&rx_xfer_lock);
	xfer = ut->rx_xfer;
	if (state != NULL)
		*state = ut->rx_xfer_state;
	pthread_mutex_unlock(&rx_xfer_lock);
	return xfer;
}

/* Records which transfer this handle has and who owns it. Written together,
 * so the other thread never sees a transfer paired with the state of the one
 * before it. */
static void rx_xfer_store(ubertooth_t* ut, struct libusb_transfer* xfer,
                          uint8_t state)
{
	pthread_mutex_lock(&rx_xfer_lock);
	ut->rx_xfer = xfer;
	ut->rx_xfer_state = state;
	pthread_mutex_unlock(&rx_xfer_lock);
}

/* Says that xfer's cancellation is on its way, for as long as xfer is still
 * this handle's transfer. Only the state is written, and only after checking
 * that: the callback may have given the transfer up and freed it between the
 * cancel being accepted and this call, and storing the pointer back would
 * leave the teardown looking at freed memory and reporting a good run as a
 * transfer that never came back. */
static void rx_xfer_mark_cancelling(ubertooth_t* ut,
                                    const struct libusb_transfer* xfer)
{
	pthread_mutex_lock(&rx_xfer_lock);
	if (ut->rx_xfer == xfer && ut->rx_xfer_state == RX_XFER_IN_FLIGHT)
		ut->rx_xfer_state = RX_XFER_CANCELLING;
	pthread_mutex_unlock(&rx_xfer_lock);
}

/* Gives up a transfer nothing is holding any more. The handle lets go of it
 * before it is freed, so a teardown looking at this handle sees either a
 * transfer it may not touch or no transfer at all, never one it may free
 * twice. The free itself is outside the lock, as every libusb call is. */
static void rx_xfer_release(ubertooth_t* ut, struct libusb_transfer* xfer)
{
	rx_xfer_store(ut, NULL, RX_XFER_OURS);
	libusb_free_transfer(xfer);
}

/* Says that a receive transfer arrived truncated, which means the capture is
 * losing packets: the USB stack or the firmware cut the transfer short, and
 * part of a packet is not something a consumer can read, so it is dropped.
 *
 * The one short transfer a board sends on purpose is the keep-alive a quiet
 * capture gets: a single byte holding the packet type KEEP_ALIVE. It is let
 * through on that byte and not on its length alone, because a transfer cut
 * short after one byte is just as possible, and that one is a fault.
 *
 * A transfer that brought no bytes at all is the ordinary quiet-board case, a
 * bulk read that found nothing, and says nothing.
 *
 * Said once per handle. A fault that truncates one transfer usually truncates
 * many, and a line each would bury the capture's own output. */
static void warn_if_transfer_was_truncated(ubertooth_t* ut,
                                           const struct libusb_transfer* xfer)
{
	if (ut->short_xfer_warned)
		return;
	if (xfer->actual_length <= 0 || xfer->actual_length == PKT_LEN)
		return;
	/* The keep-alive. Its one byte is the first field of a receive slot,
	 * usb_pkt_rx.pkt_type. */
	if (xfer->actual_length == 1 && xfer->buffer != NULL
	    && xfer->buffer[0] == KEEP_ALIVE)
		return;

	ut->short_xfer_warned = 1;
	fprintf(stderr, "warning: a receive transfer arrived with %d of a "
	        "packet's %d bytes and was dropped; the capture is losing "
	        "packets\n", xfer->actual_length, PKT_LEN);
}

/* The same report for the other way a capture reads packets. ubertooth-btle
 * and ubertooth-ego ask for theirs one at a time with UBERTOOTH_POLL control
 * requests rather than through the bulk endpoint, so they never reach
 * cb_xfer() above. A reply that is not a whole record cannot be read as one,
 * so the tool drops it, and without this it dropped it in silence: a capture
 * losing packets then looked exactly like a quiet board.
 *
 * A reply of one byte or none is the board's "nothing queued" answer, which
 * is what every poll of a quiet capture gets, and says nothing.
 *
 * Said once per handle, like the bulk path's: a fault that truncates one
 * reply usually truncates many, and a line each would bury the capture's own
 * output. */
void ubertooth_warn_if_poll_reply_short(ubertooth_t* ut, int len)
{
	if (ut->short_xfer_warned)
		return;
	if (len <= 1 || len == PKT_LEN)
		return;

	ut->short_xfer_warned = 1;
	fprintf(stderr, "warning: a poll reply arrived with %d of a packet's "
	        "%d bytes and was dropped; the capture is losing packets\n",
	        len, PKT_LEN);
}

static void cb_xfer(struct libusb_transfer *xfer)
{
	ubertooth_t* ut = (ubertooth_t*)xfer->user_data;
	/* Who owns this transfer, and whether something is waiting for it
	 * rather than for more packets. */
	uint8_t state;
	int wanted_back;
	/* The libusb status that ended the stream, or 0 if it has not ended. */
	int failed = 0;
	int r;

	/* Something is waiting for this transfer when a teardown has cancelled
	 * it, or when the capture is stopping. Read before anything below
	 * changes the state, because a cancelled transfer can come back as a
	 * bulk timeout -- libusb reports that when the timeout expired before
	 * the cancellation took effect -- and putting that one back in flight
	 * leaves the teardown waiting for a completion that is no longer on its
	 * way. The teardown's cancel is the only signal that covers it: a
	 * teardown between the phases of a run leaves stop_ubertooth clear.
	 *
	 * The stop flags are read here rather than through
	 * ubertooth_stopping(), which writes the handle: this runs on the poll
	 * thread, and the capture's own thread is the one that writes.
	 *
	 * Reading a flag a handler sets from a second thread is not something
	 * the standard promises either; only an atomic would be. It is what the
	 * old code did too, and it holds on the processors this fork builds
	 * for, where each flag is one load. */
	rx_xfer_peek(ut, &state);
	wanted_back = state == RX_XFER_CANCELLING
	              || stop_requested || timeout_expired
	              || ut->stop_ubertooth;

	/* libusb hands the transfer back for the duration of this callback. The
	 * state is cleared only by rx_xfer_release() below, so nothing can free
	 * this transfer while the callback is still using it. */
	switch (xfer->status) {
	case LIBUSB_TRANSFER_COMPLETED:
	case LIBUSB_TRANSFER_TIMED_OUT:
		/* A packet, or the board having had nothing to send. Either way the
		 * same transfer goes back in flight, and neither is reported: both
		 * are what a running capture looks like. A capture that is ending
		 * keeps the packets it already has rather than reading into another
		 * slot of a ring buffer nobody is draining. */
		if (wanted_back)
			break;
		/* A whole 64-byte packet is a packet, whichever of the two
		 * statuses came with it: libusb reports a transfer that received
		 * every byte asked for as completed, and it can also report the
		 * timeout expiring on that same transfer, and the packet in the
		 * buffer is a packet either way. How many bytes arrived is this
		 * transfer's own count, because libusb_submit_transfer() starts
		 * it at zero, so a slot nothing filled in cannot read as full.
		 *
		 * Short of 64 bytes is not a packet. When a capture goes quiet
		 * the firmware answers the bulk endpoint with a one-byte
		 * keep-alive, which sets pkt_type in the receive slot and leaves
		 * the other 63 bytes as they were. Handing that slot on publishes
		 * a packet nothing filled in, and a consumer that writes records
		 * out then saves 64 bytes of which only pkt_type was ever set. The
		 * slot is kept instead, for the next real packet to read into.
		 *
		 * Anything short other than that keep-alive is a fault, and
		 * dropping it in silence left a capture losing packets looking
		 * like a quiet board, so it is reported. */
		if (xfer->actual_length == PKT_LEN) {
			/* When this packet arrived, stamped here because this is
			 * the moment it did. The loop that drains the ring
			 * buffer can be a long way behind: reading the clock
			 * there instead measured when the loop got round to a
			 * packet, which understated every gap a stalled loop
			 * then caught up on. */
			fifo_inc_write_ptr(ut->fifo, ubertooth_packet_arrival_ns());
			xfer->buffer = (uint8_t*)fifo_get_write_element(ut->fifo);
		} else {
			warn_if_transfer_was_truncated(ut, xfer);
		}
		r = libusb_submit_transfer(xfer);
		if (r == 0) {
			/* It was already RX_XFER_IN_FLIGHT and still is: the state is
			 * never set to anything else while libusb holds it. */
			return;
		}
		fprintf(stderr, "error: resubmitting the receive transfer failed "
		        "(%d)\n", r);
		/* Refused, so libusb is not holding it: given up below rather than
		 * left to refuse the next ubertooth_bulk_init() on this handle.
		 * Nothing is in flight either, so the board has nowhere to send to
		 * and the stream is as dead as a failed transfer. */
		failed = LIBUSB_TRANSFER_ERROR;
		break;
	case LIBUSB_TRANSFER_CANCELLED:
		/* The teardown's own cancellation, which is how it gets the transfer
		 * back: expected, so nothing is said about it. */
		break;
	default:
		rx_xfer_status(xfer->status);
		failed = xfer->status;
		break;
	}
	/* The handle gives up the transfer before a failure is published.
	 * Publishing it releases the capture's own thread, and what that thread
	 * then looks at must not be a transfer this has already freed. */
	rx_xfer_release(ut, xfer);
	if (failed)
		rx_stream_failed(ut, failed);
}

static void* poll_thread_main(void* arg __attribute__((unused)))
{
	int r = 0;

	while (!do_exit) {
		struct timeval tv = { 1, 0 };
		r = libusb_handle_events_timeout(NULL, &tv);
		/* A signal that arrived while this was waiting is not a failure:
		 * the wait was cut short, so it is taken again. Ending the thread
		 * here left the capture with nobody to complete its transfers, so
		 * nothing more arrived and the loop waited with no line to say
		 * why. Ctrl-C does hit this -- the handlers catch it on whichever
		 * thread the system picks. The short sleep below still runs, so a
		 * signal arriving over and over cannot spin this thread. */
		if (r < 0 && r != LIBUSB_ERROR_INTERRUPTED) {
			do_exit = 1;
			break;
		}
		usleep(1);
	}

	return NULL;
}

/* Whether poll_thread holds a thread that was really created. Waiting on a
 * pthread_t that pthread_create() never filled in is undefined, and callers
 * do reach the stop without a successful start: the Kismet plugin's teardown
 * calls it on paths that set a capture up in a different order. */
static int poll_thread_running;

int ubertooth_bulk_thread_start()
{
	int r;

	do_exit = 0;

	r = pthread_create(&poll_thread, NULL, poll_thread_main, NULL);
	if (r == 0)
		poll_thread_running = 1;
	return r;
}

void ubertooth_bulk_thread_stop()
{
	do_exit = 1;

	if (!poll_thread_running)
		return;
	poll_thread_running = 0;
	pthread_join(poll_thread, NULL);
}

/* The one line for a capture the library was starting whose collecting thread
 * could not be created. The host tools print the same wording from their own
 * shared report (report_capture_thread_failure()); it is written out twice
 * because nothing in libubertooth may link against the tools. Keep the two in
 * step if either is reworded. */
static void report_thread_failure(int error)
{
	fprintf(stderr, "error: could not start the thread that collects the "
	        "capture: %s\n", strerror(error));
}

/* How long ubertooth_bulk_stop() waits for a cancelled receive transfer to
 * come back, as a number of short waits. libusb completes a cancellation as
 * soon as the kernel reports it, so this is a bound on a device that never
 * does rather than a wait anything normally reaches. */
#define XFER_CANCEL_WAITS 50
#define XFER_CANCEL_WAIT_US 10000

/* Finishes the receive transfer ubertooth_bulk_init() started, so it is gone
 * before the next phase of the same run allocates one (ubertooth-afh detects
 * a map, then monitors it) and before the device is closed.
 *
 * A transfer libusb is holding must not be freed, so it is cancelled and the
 * completion is handled here: by the time a stream tears down its poll thread
 * has been joined, so nothing else would handle it, and the earlier code left
 * the transfer to a cancellation that might never arrive. cb_xfer() frees the
 * transfer and clears rx_xfer when the cancellation arrives.
 *
 * What may be freed is decided by rx_xfer_state, not by whether libusb
 * accepted the cancel: a receive loop that stopped with a packet in hand has
 * already cancelled the transfer, so the cancel here is refused for one that
 * is very much still in flight. The wait is bounded, and a transfer that
 * never comes back is left allocated rather than freed while libusb may still
 * write to it; the next ubertooth_bulk_init() on this handle then refuses to
 * start instead. */
static void ubertooth_bulk_stop(ubertooth_t* ut)
{
	struct libusb_transfer* xfer;
	uint8_t state;
	int i, r;

	xfer = rx_xfer_peek(ut, &state);
	if (xfer == NULL)
		return;

	if (state == RX_XFER_IN_FLIGHT && libusb_cancel_transfer(xfer) == 0)
		rx_xfer_mark_cancelling(ut, xfer);

	for (i = 0; i < XFER_CANCEL_WAITS; i++) {
		struct timeval tv = { 0, XFER_CANCEL_WAIT_US };

		if (rx_xfer_peek(ut, &state) == NULL || state == RX_XFER_OURS)
			break;
		/* Outside the lock: this is where the cancellation completes, so
		 * it can run the callback on this very thread. */
		r = libusb_handle_events_timeout(NULL, &tv);
		/* A signal that cut the wait short is not a failure. This runs
		 * just after a Ctrl-C, so it is the likeliest place for a second
		 * signal to land, and giving up here leaves the transfer
		 * allocated and the next capture on this handle refusing to
		 * start. The turns are counted, so taking the wait again is
		 * still bounded. */
		if (r == LIBUSB_ERROR_INTERRUPTED)
			continue;
		if (r < 0)
			break;
	}

	xfer = rx_xfer_peek(ut, &state);
	if (xfer == NULL || state != RX_XFER_OURS)
		return;

	rx_xfer_release(ut, xfer);
}

int ubertooth_bulk_init(ubertooth_t* ut)
{
	struct libusb_transfer* xfer;
	int r;

	/* A second phase of the same run calls this again on the same handle.
	 * The transfer the previous phase left is finished here rather than
	 * dropped: overwriting the pointer leaked the transfer, and one that is
	 * still in flight must not be replaced while libusb writes to it. */
	ubertooth_bulk_stop(ut);
	if (rx_xfer_peek(ut, NULL) != NULL) {
		fprintf(stderr, "error: the previous receive transfer did not "
		        "finish\n");
		return -1;
	}

	/*
	 * Start from an empty pipe. The board's bulk IN endpoint has two
	 * hardware buffers, and a capture that ended with packets this host
	 * never collected leaves one in each: they are past the firmware's
	 * queue, so the board cannot drop them when the mode stops, and they
	 * were delivered to the next reader before its own mode had started -- a
	 * basic-rate dump taken after a BLE capture began with two low-energy
	 * packets.
	 *
	 * Clearing the halt feature is what discards them, even though the
	 * endpoint is not halted. The request reaches the board's USB controller
	 * as "write zero to the endpoint's stall bit", and the LPC17xx
	 * re-initializes an endpoint on that write whether or not it was stalled
	 * (user manual UM10360, Set Endpoint Status, bit ST): the buffers are
	 * flushed and an IN endpoint writes a DATA0 PID next. libusb puts this
	 * host's toggle back to DATA0 to match, so the two stay in step.
	 *
	 * Doing it before the mode request is what keeps it from discarding
	 * anything the caller wants: nothing has been asked for yet. It does not
	 * close the window on a board still running a mode somebody else
	 * started, whose loop can hand the endpoint another packet between this
	 * and the mode request. A tool that exits normally sends STOP, so the
	 * board this runs against is idle and has already dropped its queue.
	 *
	 * A board that refuses it is worth a word but not a failure: the stream
	 * works, it may just open with a packet or two from before.
	 */
	r = libusb_clear_halt(ut->devh, DATA_IN);
	if (r < 0)
		fprintf(stderr, "warning: could not clear the receive endpoint "
		        "(%d); the capture may start with packets from before\n", r);

	xfer = libusb_alloc_transfer(0);
	if (xfer == NULL) {
		fprintf(stderr, "error: allocating the receive transfer failed\n");
		return -1;
	}
	libusb_fill_bulk_transfer(xfer, ut->devh, DATA_IN, (uint8_t*)fifo_get_write_element(ut->fifo), PKT_LEN, cb_xfer, ut, TIMEOUT);
	/* On the handle before it is submitted: once libusb has it, its callback
	 * can run on the poll thread and look this handle's transfer up. */
	rx_xfer_store(ut, xfer, RX_XFER_OURS);

	r = libusb_submit_transfer(xfer);
	if (r < 0) {
		fprintf(stderr, "error: submitting the receive transfer failed "
		        "(%d)\n", r);
		/* It was never in flight, so it is freed here; leaving it in place
		 * would refuse the next ubertooth_bulk_init() on this handle. */
		rx_xfer_release(ut, xfer);
		return -1;
	}
	rx_xfer_store(ut, xfer, RX_XFER_IN_FLIGHT);
	return 0;
}

void ubertooth_bulk_wait(ubertooth_t* ut)
{
	while (fifo_empty(ut->fifo) && !ubertooth_stopping(ut))
		usleep(1);
}

int ubertooth_bulk_receive(ubertooth_t* ut, rx_callback cb, void* cb_args)
{
	if (!fifo_empty(ut->fifo)) {
		(*cb)(ut, cb_args);
		if(ubertooth_stopping(ut)) {
			/* The transfer is not cancelled here, where the poll thread is
			 * still running and may be freeing it in cb_xfer() at this very
			 * moment: the cancel would then be made on freed memory. It is
			 * not needed either. cb_xfer() stops putting the transfer back
			 * in flight as soon as stop_ubertooth is set, which is before
			 * this returns, so the board has nowhere to send to within one
			 * transfer; ubertooth_bulk_stop() cancels once the poll thread
			 * has been joined, where it is the only thread left. */
			return 1;
		}
		fflush(stderr);
		return 0;
	} else {
		usleep(1);
		return -1;
	}
}

/*
 * `start` is the request that puts the board in the mode whose packets the
 * caller wants: cmd_rx_syms() for plain symbol capture, cmd_afh() for AFH
 * detection. It is sent once the transfers are in place, and it is the last
 * mode request the board gets -- sending cmd_rx_syms() on top of cmd_afh()
 * left AFH detection receiving under MODE_RX_SYMBOLS instead.
 */
static int stream_rx_usb(ubertooth_t* ut, rx_callback cb, void* cb_args,
                         int (*start)(struct libusb_device_handle*))
{
	// init USB transfer
	int r = ubertooth_bulk_init(ut);
	if (r < 0)
		return r;

	r = ubertooth_bulk_thread_start();
	if (r != 0) {
		/* Not "< 0": pthread_create() reports a failure as a positive
		 * errno, so one used to read as success here. The capture was then
		 * started with nobody to complete its transfers and waited forever
		 * with nothing arriving, and no line said why. */
		report_thread_failure(r);
		ubertooth_bulk_stop(ut);
		return -1;
	}

	// tell ubertooth to send packets
	r = start(ut->devh);
	if (r < 0) {
		/* The poll thread is running by now and nothing else joins it:
		 * ubertooth_stop() frees the libusb context the thread is polling. */
		ubertooth_bulk_thread_stop();
		ubertooth_bulk_stop(ut);
		return r;
	}

	// receive and process each packet
	while(!ubertooth_stopping(ut)) {
		ubertooth_bulk_wait(ut);
		r = ubertooth_bulk_receive(ut, cb, cb_args);
	}

	ubertooth_bulk_thread_stop();
	/* The transfer goes with the stream, so a caller that starts another one
	 * (rx_afh() monitors after it has detected) does not leave this one
	 * behind. */
	ubertooth_bulk_stop(ut);

	/* A stream that died is not an end of capture: the caller is told so
	 * rather than left to read this as the normal "stopped" return. */
	if (ut->rx_stream_status != 0)
		return UBERTOOTH_ERROR_STREAM_FAILED;

	return 1;
}

/* Why a replay stopped short of a whole record: a read error (minus its
 * errno), a last record cut short (the bytes of it that were there), or a
 * clean end of file (0). */
static int stream_rx_file_end(FILE* fp, size_t partial)
{
	if (ferror(fp))
		return -(errno ? errno : EIO);
	return (int)partial;
}

/* file should be in full USB packet format (ubertooth-dump -f) */
int stream_rx_file(ubertooth_t* ut, FILE* fp, rx_callback cb, void* cb_args)
{
	uint8_t buf[PKT_LEN];
	size_t nbytes;

	/* A replay the user has asked to stop ends here rather than at the end
	 * of the file, which can hold hours of packets. It reads as a clean end
	 * of file, so the caller reports no error: the user asked for this. */
	while(!ubertooth_stopping(ut)) {
		uint32_t systime_be;
		errno = 0;
		nbytes = fread(&systime_be, 1, sizeof(systime_be), fp);
		if (nbytes != sizeof(systime_be))
			return stream_rx_file_end(fp, nbytes);
		systime = (time_t)be32toh(systime_be);

		errno = 0;
		nbytes = fread(buf, 1, PKT_LEN, fp);
		if (nbytes != PKT_LEN)
			return stream_rx_file_end(fp, sizeof(systime_be) + nbytes);
		/* The host time this record carries, read into systime just
		 * above: that is what the arrival reading is for as long as
		 * infile is set, which is how a replay gets here. */
		fifo_push(ut->fifo, (usb_pkt_rx*)buf,
		          ubertooth_packet_arrival_ns());
		(*cb)(ut, cb_args);
	}
	return 0;
}

int rx_file_report(int result, const char* filename)
{
	if (result < 0) {
		fprintf(stderr, "error: reading %s failed: %s\n",
		        filename, strerror(-result));
		return 1;
	}
	if (result > 0) {
		fprintf(stderr, "error: %s ends in a partial record: %d of %d "
		        "bytes, ignored\n", filename, result, RX_FILE_RECORD_LEN);
		return 1;
	}
	return 0;
}


/* 1 when not one channel has been seen, which is what the initial detection
 * phase leaves when no packet from the piconet was received: the map starts
 * empty (btbb_piconet_new() zeroes it) and cb_afh_initial() sets a channel
 * seen for every packet it matches. An all-zero map is not a map: printing it
 * claims a piconet that uses no channel at all. */
static int afh_map_empty(btbb_piconet* pn)
{
	uint8_t* afh_map = btbb_piconet_get_afh_map(pn);
	int i;

	for (i = 0; i < AFH_MAP_BYTES; i++)
		if (afh_map[i] != 0)
			return 0;
	return 1;
}

int rx_afh(ubertooth_t* ut, btbb_piconet* pn, int timeout)
{
	int r = btbb_init(max_ac_errors);
	if (r < 0)
		return -1;

	/* Sweep the whole band: a channel above the Bluetooth band is how the
	 * firmware is told to take its channel from the AFH hopping instead. A
	 * board that refused this stays on the one channel it was left on, so
	 * the map would hold that channel alone and still read as the piconet's
	 * -- so the refusal ends the receive phase here rather than being
	 * carried past it. cmd_set_channel() has already said why. */
	r = cmd_set_channel(ut->devh, 9999);
	if (r < 0)
		return r;

	if (timeout) {
		ubertooth_set_timeout(ut, timeout);

		/* cmd_afh() both starts the stream and is the mode it streams in:
		 * it clears the board's map, turns on AFH hopping and asks for
		 * MODE_AFH. */
		r = stream_rx_usb(ut, cb_afh_initial, pn, cmd_afh);

		/* Out of AFH mode before the monitor phase starts it again -- but
		 * not when this phase's transfer was never handed back, for the
		 * reason ubertooth_cleanup() gives: with no status for the lost
		 * transfer, the board cannot be known to be there to answer. A
		 * monitor phase on such a handle refuses to start anyway. */
		if (rx_xfer_peek(ut, NULL) == NULL)
			cmd_stop(ut->devh);
		/* The timer that ended the detection phase is forgotten so the
		 * monitoring phase below can start at all. A stop the user asked
		 * for survives this, and is what the check below the map reads. */
		ubertooth_clear_timeout_stop(ut);

		/* A phase that could not even receive has not shown that the piconet
		 * is quiet; stream_rx_usb() has already reported why. Whatever it
		 * did detect before it failed is still printed, as the other tools
		 * print the partial results of a capture that died, and the error
		 * decides the exit status. */
		if (r < 0) {
			if (!afh_map_empty(pn))
				btbb_print_afh_map(pn);
			return r;
		}

		/* Nothing was received: there is no map to print, and monitoring
		 * changes to a map we never found would report nothing either. Let
		 * the caller say so instead. */
		if (afh_map_empty(pn))
			return UBERTOOTH_AFH_NO_PACKETS;

		btbb_print_afh_map(pn);

		/* The user interrupted the detection phase: the run ends here,
		 * with the map it managed to detect. Starting to monitor would
		 * mean waiting for a second interrupt, which is what the user
		 * asking once used to get. */
		if (ubertooth_stopping(ut))
			return 0;
	}

	/*
	 * Monitor changes in AFH channel map. cmd_afh() clears the board's map
	 * as it starts the stream, so the monitor begins from an empty one
	 * whether or not the detection phase above ran.
	 */
	r = stream_rx_usb(ut, cb_afh_monitor, pn, cmd_afh);
	return r < 0 ? r : 0;
}

int rx_afh_r(ubertooth_t* ut, btbb_piconet* pn, int timeout __attribute__((unused)))
{
	static uint32_t lasttime;

	int r = btbb_init(max_ac_errors);
	int i, j;
	if (r < 0)
		return -1;

	/* The band sweep, for the reason rx_afh() gives. */
	r = cmd_set_channel(ut->devh, 9999);
	if (r < 0)
		return r;

	// init USB transfer
	r = ubertooth_bulk_init(ut);
	if (r < 0)
		return r;

	r = ubertooth_bulk_thread_start();
	if (r != 0) {
		/* as in stream_rx_usb(): a positive errno, and a capture without
		 * the thread waits forever */
		report_thread_failure(r);
		ubertooth_bulk_stop(ut);
		return -1;
	}

	// tell ubertooth to send packets, in the AFH detection mode (see rx_afh)
	r = cmd_afh(ut->devh);
	if (r < 0) {
		/* as in stream_rx_usb(): the poll thread has to be joined here */
		ubertooth_bulk_thread_stop();
		ubertooth_bulk_stop(ut);
		return r;
	}

	// receive and process each packet
	while(!ubertooth_stopping(ut)) {
		ubertooth_bulk_receive(ut, cb_afh_r, pn);
		if(lasttime < time(NULL)) {
			lasttime = time(NULL);
			printf("%u ", (uint32_t)time(NULL));
			// btbb_print_afh_map(pn);

			uint8_t* afh_map = btbb_piconet_get_afh_map(pn);
			for (i=0; i<AFH_MAP_BYTES; i++)
				for (j=0; j<8; j++)
					if (afh_map[i] & (1<<j))
						printf("1");
					else
						printf("0");
			printf("\n");
		}
	}

	ubertooth_bulk_thread_stop();
	ubertooth_bulk_stop(ut);
	return 0;
}

void ubertooth_unpack_symbols(const uint8_t* buf, char* unpacked)
{
	int i, j;

	for (i = 0; i < SYM_LEN; i++) {
		/* output one byte for each received symbol (0x00 or 0x01) */
		for (j = 0; j < 8; j++) {
			unpacked[i * 8 + j] = ((buf[i] << j) & 0x80) >> 7;
		}
	}
}

static void cb_dump_bitstream(ubertooth_t* ut, void* args __attribute__((unused)))
{
	int i;
	char nl = '\n';

	usb_pkt_rx usb = fifo_pop(ut->fifo);
	usb_pkt_rx* rx = &usb;
	char bitstream[BANK_LEN];

	/* A keep-alive only says a quiet capture is still alive. It carries no
	 * symbols, so there is no bitstream to write. */
	if (rx->pkt_type == KEEP_ALIVE)
		return;

	ubertooth_count_capture_losses(ut, rx);

	ubertooth_unpack_symbols((uint8_t*)rx->data, bitstream);

	// convert to ascii
	for (i = 0; i < BANK_LEN; ++i)
		bitstream[i] += 0x30;

	fprintf(stderr, "rx block timestamp %u * 100 nanoseconds\n",
	        rx->clk100ns);
	if (dumpfile == NULL) {
		fwrite(bitstream, sizeof(uint8_t), BANK_LEN, stdout);
		fwrite(&nl, sizeof(uint8_t), 1, stdout);
	} else {
		fwrite(bitstream, sizeof(uint8_t), BANK_LEN, dumpfile);
		fwrite(&nl, sizeof(uint8_t), 1, dumpfile);
	}
}

static void cb_dump_full(ubertooth_t* ut, void* args __attribute__((unused)))
{
	usb_pkt_rx usb = fifo_pop(ut->fifo);
	usb_pkt_rx* rx = &usb;
	uint64_t arrival_ns = fifo_popped_arrival_ns(ut->fifo);

	/* A keep-alive only says a quiet capture is still alive. Writing it out
	 * would put a record in the dump whose packet fields nothing ever set,
	 * and a reader of the dump cannot tell such a record from data. */
	if (rx->pkt_type == KEEP_ALIVE)
		return;

	ubertooth_count_capture_losses(ut, rx);

	fprintf(stderr, "rx block timestamp %u * 100 nanoseconds\n", rx->clk100ns);
	/* The host time this record arrived. */
	systime = ubertooth_packet_systime(arrival_ns);
	uint32_t time_be = htobe32(systime);
	if (dumpfile == NULL) {
		fwrite(&time_be, 1, sizeof(time_be), stdout);
		fwrite((uint8_t*)rx, sizeof(uint8_t), PKT_LEN, stdout);
	} else {
		fwrite(&time_be, 1, sizeof(time_be), dumpfile);
		fwrite((uint8_t*)rx, sizeof(uint8_t), PKT_LEN, dumpfile);
		fflush(dumpfile);
	}
}

/* dump received symbols to stdout */
int rx_dump(ubertooth_t* ut, int bitstream)
{
	int r;

	if (bitstream)
		r = stream_rx_usb(ut, cb_dump_bitstream, NULL, cmd_rx_syms);
	else
		r = stream_rx_usb(ut, cb_dump_full, NULL, cmd_rx_syms);
	return r < 0 ? r : 0;
}

static void ubertooth_cleanup(ubertooth_t* ut, int send_stop)
{
	/* Finish the receive transfer before the device it reads from is closed.
	 * Cancelling it and closing anyway, as this did, left the transfer
	 * allocated for the rest of the process. */
	ubertooth_bulk_stop(ut);
	if (rx_xfer_peek(ut, NULL) != NULL) {
		/* The transfer never came back, so libusb may still be holding this
		 * device and this session. Closing them underneath it, or ending the
		 * session, is worse than leaving both: a capture ends here, so the
		 * handle goes when the process does. The capture files below are
		 * still closed, so nothing a run wrote is lost.
		 *
		 * Nothing more is asked of the board either, not even the stop.
		 * Which board this is cannot be told from here: one that has left
		 * the bus or stopped answering, or a live one whose cancellation
		 * did not arrive within the wait above. The transfer's callback
		 * never ran, so there is no status to tell them apart, and a stop
		 * sent blind would reach only the live one.
		 *
		 * Sending it would also hand this thread an event loop. cmd_stop()
		 * uses libusb's synchronous control transfer, which handles events
		 * until its own completes -- and the receive transfer is still in
		 * flight, so its cancellation could complete inside that call and
		 * run cb_xfer() here, on the one thread that is meant to be past
		 * the stream (the poll thread was joined above). The teardown has
		 * already chosen this branch by then.
		 *
		 * The warning covers every one of those boards: it says this one
		 * may still be capturing, and ubertooth(7) names ubertooth-util -S
		 * for a board that is.
		 *
		 * Said once per handle, because a handle left in this state is
		 * closed again by a tool that cleans up on two paths, and the
		 * board is in no worse a state the second time. */
		if (!ut->xfer_lost_warned) {
			ut->xfer_lost_warned = 1;
			fprintf(stderr, "warning: the receive transfer did not finish; "
			        "the Ubertooth is left open and may still be "
			        "capturing\n");
		}
		/* The run is a failure all the same, so a caller can tell it from a
		 * capture that finished: this one lost its stream, and left a board
		 * that may still be sending. The message stays a warning because it
		 * is about what was left behind; nothing can be said about what
		 * went wrong, since the transfer's callback never ran to report it.
		 * The first failure wins, as in rx_stream_failed(); stop_ubertooth
		 * is left alone, because the capture it would stop is already
		 * over. */
		if (ut->rx_stream_status == 0)
			ut->rx_stream_status = LIBUSB_TRANSFER_ERROR;
	}
	else {
		if (ut->devh != NULL) {
			if (send_stop) {
				/* A board that would not stop is left running
				 * the mode, and no other tool can open it
				 * until it is reset or re-plugged. cmd_stop()
				 * has already said what it refused; keeping
				 * the code is what lets the tool's exit status
				 * say the run did not finish. */
				int stopped = cmd_stop(ut->devh);

				if (stopped < 0 && ut->stop_status == 0)
					ut->stop_status = stopped;
			}
			libusb_release_interface(ut->devh, 0);
			/* Any firmware API version read off this board goes with
			 * it, so a report about the next board cannot quote this
			 * one's. Before the close, while the handle is still a
			 * handle rather than a value of a closed object. */
			ubertooth_forget_api_version(ut->devh);
			libusb_close(ut->devh);
			ut->devh = NULL;
		}
		/* Only the session this handle opened, and only once: this used to
		 * run outside the check above, so closing a handle twice ended a
		 * libusb session that was no longer this handle's, and a handle
		 * that never opened one (a replay, which reads a file) ended a
		 * session it had never started. That is why a tool had to guard
		 * its close with a look at ut->devh. */
		if (ut->usb_session_open) {
			ut->usb_session_open = 0;
			libusb_exit(NULL);
		}
	}

	if (ut->h_pcap_bredr) {
		btbb_pcap_close(ut->h_pcap_bredr);
		ut->h_pcap_bredr = NULL;
	}
	if (ut->h_pcap_le) {
		lell_pcap_close(ut->h_pcap_le);
		ut->h_pcap_le = NULL;
	}

	if (ut->h_pcapng_bredr) {
		btbb_pcapng_close(ut->h_pcapng_bredr);
		ut->h_pcapng_bredr = NULL;
	}
	if (ut->h_pcapng_le) {
		lell_pcapng_close(ut->h_pcapng_le);
		ut->h_pcapng_le = NULL;
	}

	/* The capture has stopped here, so the clock tracking goes back to
	 * where a capture starts: a handle used again would otherwise carry the
	 * wrap count of the capture that just ended into the next one. The same
	 * goes for a long packet whose last records never arrived. */
	ubertooth_capture_clock_reset(ut);
	le_usb_reassembly_reset(&ut->le_reassembly);
}

void ubertooth_close(ubertooth_t* ut)
{
	ubertooth_cleanup(ut, 0);
}

void ubertooth_stop(ubertooth_t* ut)
{
	ubertooth_cleanup(ut, 1);
}

void ubertooth_free(ubertooth_t* ut)
{
	if (ut == NULL)
		return;
	/* A transfer libusb is still holding points its callback at this handle
	 * and reads into this ring buffer, so both outlive the run rather than
	 * being freed under it. The teardown has already said the transfer did
	 * not finish; a process ending here gives the memory back anyway. */
	if (rx_xfer_peek(ut, NULL) != NULL)
		return;
	/* The library's signal handlers let go of this handle before it is
	 * freed, so a signal arriving next cannot reach freed memory. */
	forget_registered_handle(ut);
	free(ut->fifo);
	free(ut);
}

ubertooth_t* ubertooth_init()
{
	/* Zeroed, so a field added to the handle starts at nothing rather than
	 * at whatever the heap last held there. The assignments below are the
	 * handle's resting state spelled out; only clk100ns_period is not zero.
	 * The quiet selection below allocates the same way. */
	ubertooth_t* ut = (ubertooth_t*)calloc(1, sizeof(ubertooth_t));
	if(ut == NULL) {
		fprintf(stderr, "error: unable to allocate memory\n");
		return NULL;
	}

	ut->fifo = fifo_init();
	if(ut->fifo == NULL) {
		/* every later fifo_* call would dereference NULL */
		fprintf(stderr, "error: unable to initialize ringbuffer\n");
		free(ut);
		return NULL;
	}

	ut->devh = NULL;
	ut->selector = NULL;
	ut->rx_xfer = NULL;
	ut->rx_xfer_state = RX_XFER_OURS;
	ut->rx_stream_status = 0;
	ut->stop_status = 0;
	ut->short_xfer_warned = 0;
	ut->stop_ubertooth = 0;
	ubertooth_capture_clock_reset(ut);
	/* Which clock a mode stamps with is the tool's to say, and it says so
	 * before it starts such a mode; until then, the CLKN-derived one. */
	ut->clk100ns_period = CLK100NS_PERIOD;
	memset(&ut->le_reassembly, 0, sizeof(ut->le_reassembly));

	ut->h_pcap_bredr = NULL;
	ut->h_pcap_le = NULL;
	ut->h_pcapng_bredr = NULL;
	ut->h_pcapng_le = NULL;

	return ut;
}

int ubertooth_connect(ubertooth_t* ut, int ubertooth_device)
{
	int r;

	note_open_failure(NULL);
	r = libusb_init(NULL);
	if (r < 0) {
		note_open_failure("libusb could not be started");
		fprintf(stderr, "error: libusb_init failed (got 1.0?)\n");
		return -1;
	}
	/* The session this handle has to end again, whatever happens below. */
	ut->usb_session_open = 1;

	/* The finder has said why it found nothing, in more detail than this
	 * site could: no board attached, no board at that index, access denied,
	 * several attached with none selected. */
	ut->devh = find_ubertooth_device(ubertooth_device);
	if (ut->devh == NULL) {
		ubertooth_note_dfu_bootloader();
		ubertooth_stop(ut);
		return -1;
	}

	r = libusb_claim_interface(ut->devh, 0);
	if (r < 0) {
		ubertooth_report_claim_failed(board_label(NULL, ubertooth_device), r);
		/* Closed without a STOP: the interface was never claimed, so there
		 * is no mode of ours to end -- and when another program holds the
		 * board, a STOP would interrupt the capture this message has just
		 * blamed it for. */
		ubertooth_close(ut);
		return -1;
	}

	return 1;
}

ubertooth_t* ubertooth_start(int ubertooth_device)
{
	ubertooth_t* ut = ubertooth_init();
	/* Before ubertooth_connect() has run, so this path sets the reason
	 * itself rather than leaving the one an earlier open did. */
	if (ut == NULL) {
		note_open_failure("the host ran out of memory");
		return NULL;
	}

	int r = ubertooth_connect(ut, ubertooth_device);
	if (r < 0) {
		/* ubertooth_connect() has already closed the device and any pcap
		 * handles; release the memory allocated by ubertooth_init() so the
		 * failed-connect path does not leak. */
		ubertooth_free(ut);
		return NULL;
	}

	return ut;
}

int ubertooth_connect_serial(ubertooth_t* ut, char* serial_number)
{
	char matched[UBERTOOTH_SERIAL_STR_LEN] = "";
	int r;

	note_open_failure(NULL);
	r = libusb_init(NULL);
	if (r < 0) {
		note_open_failure("libusb could not be started");
		fprintf(stderr, "error: libusb_init failed (got 1.0?)\n");
		return -1;
	}
	/* as in ubertooth_connect() */
	ut->usb_session_open = 1;

	/* The finder has said why it found nothing: no board matched, a board
	 * matched but would not open, or the serial matched several. */
	ut->devh = find_ubertooth_device_serial(serial_number, matched);
	if (ut->devh == NULL) {
		ubertooth_stop(ut);
		return -1;
	}

	r = libusb_claim_interface(ut->devh, 0);
	if (r < 0) {
		ubertooth_report_claim_failed(
		        board_label(matched[0] != '\0' ? matched : serial_number, -1),
		        r);
		/* as in ubertooth_connect(): nothing of ours to stop, and a STOP
		 * here would interrupt whatever has the board */
		ubertooth_close(ut);
		return -1;
	}

	return 1;
}

ubertooth_t* ubertooth_start_serial(char* serial_number)
{
	ubertooth_t* ut = ubertooth_init();
	/* as in ubertooth_start(): before the reason is cleared */
	if (ut == NULL) {
		note_open_failure("the host ran out of memory");
		return NULL;
	}

	int r = ubertooth_connect_serial(ut, serial_number);
	if (r < 0) {
		ubertooth_free(ut);
		return NULL;
	}

	return ut;
}

/* What a -U argument asks for. */
enum selector_kind {
	SELECTOR_DEFAULT,  /* no -U given: the only attached device */
	SELECTOR_INDEX,    /* an enumeration index */
	SELECTOR_SERIAL,   /* a serial-number string */
	SELECTOR_NO_INDEX  /* all digits, but no device can have that index */
};

/* Classify a -U argument, writing the index for SELECTOR_INDEX. An all-digits
 * argument is an index, anything else is matched as a serial number. A number
 * that does not fit in an int cannot be an attached device's index, and must
 * not be converted into one that is: atoi() would read 4294967296 as index 0
 * and 2147483648 as a negative index, which means "the only device". */
static enum selector_kind classify_selector(const char* u_arg, int* index)
{
	const char* p;
	long value;

	if (u_arg == NULL || u_arg[0] == '\0')
		return SELECTOR_DEFAULT;
	for (p = u_arg; *p != '\0'; ++p)
		if (*p < '0' || *p > '9')
			return SELECTOR_SERIAL;

	/* The scan above leaves only digits, so strtol() stops at the end of the
	 * string and cannot return a negative value; only the range can fail. */
	errno = 0;
	value = strtol(u_arg, NULL, 10);
	if (errno != 0 || value > INT_MAX)
		return SELECTOR_NO_INDEX;
	*index = (int)value;
	return SELECTOR_INDEX;
}

/* The same message as the loud selection path, for an index that could not
 * even be a device index. The count comes from a separate enumeration, so a
 * libusb that cannot be used at all (ubertooth_count() < 0, which reports
 * itself) is left out rather than reported as "0 attached". */
static void report_no_index(const char* u_arg)
{
	int attached = ubertooth_count();

	if (attached < 0)
		fprintf(stderr, "error: no Ubertooth at index %s\n"
		        INDEX_SHIFT_HINT, u_arg);
	else
		fprintf(stderr, "error: no Ubertooth at index %s (%d attached)\n"
		        INDEX_SHIFT_HINT, u_arg, attached);
}

/* Shared -U argument handling for all tools: accepts either a numeric device
 * index (existing behaviour) or a serial-number string. A NULL or empty arg
 * means "default device". An all-digits arg is treated as an index; anything
 * else is matched as a serial number. */
ubertooth_t* ubertooth_start_select(const char* u_arg)
{
	int index = -1;
	ubertooth_t* ut;

	switch (classify_selector(u_arg, &index)) {
	case SELECTOR_DEFAULT:
		return ubertooth_start(-1);
	case SELECTOR_INDEX:
		ut = ubertooth_start(index);
		break;
	case SELECTOR_NO_INDEX:
		note_open_failure("no board can have that index");
		report_no_index(u_arg);
		return NULL;
	case SELECTOR_SERIAL:
	default:
		/* cast away const: the serial path does not modify the string */
		ut = ubertooth_start_serial((char*)u_arg);
		break;
	}
	/* Which board the user named, for the messages that are about this one
	 * board. Only where they did name one: the default opens whichever
	 * board is there. */
	if (ut != NULL)
		ut->selector = u_arg;
	return ut;
}

/* Quiet variant of ubertooth_start_select(), for polling a device that may be
 * absent, for example while it re-enumerates after a reset. Prints nothing and
 * sends no command other than the serial-number read needed to match a serial.
 * If the device is not found or cannot be claimed it is closed WITHOUT sending
 * STOP, so a failed attempt leaves the device state unchanged. An index is
 * chosen as in ubertooth_start_select(): an index that is not present opens
 * nothing. */
ubertooth_t* ubertooth_start_select_quiet(const char* u_arg)
{
	struct libusb_device_handle* devh;
	ubertooth_t* ut;
	enum selector_kind kind;
	int index = -1;

	/* Cleared here as the loud paths do, so a caller that reads the reason
	 * after this never gets one left by an earlier open. */
	note_open_failure(NULL);
	kind = classify_selector(u_arg, &index);
	if (kind == SELECTOR_NO_INDEX)
		return NULL; /* quiet: no message, and nothing to open */

	if (libusb_init(NULL) < 0)
		return NULL;
	if (kind == SELECTOR_DEFAULT)
		devh = find_ubertooth_device_opt(-1, 1);
	else if (kind == SELECTOR_INDEX)
		devh = find_ubertooth_device_opt(index, 1);
	else
		devh = find_ubertooth_device_serial_opt(u_arg, 1, NULL);
	if (devh == NULL || libusb_claim_interface(devh, 0) < 0) {
		if (devh != NULL) {
			/* The open above recorded this board's firmware API
			 * version; nothing is going to use the board, so the
			 * version goes with it rather than waiting to be quoted
			 * for another. A reset wait polls this path until the
			 * board comes back, so it is walked over and over. */
			ubertooth_forget_api_version(devh);
			libusb_close(devh);
		}
		libusb_exit(NULL);
		return NULL;
	}

	ut = (ubertooth_t*)calloc(1, sizeof(ubertooth_t));
	if (ut != NULL)
		ut->fifo = fifo_init();
	if (ut == NULL || ut->fifo == NULL) {
		free(ut);
		libusb_release_interface(devh, 0);
		ubertooth_forget_api_version(devh);
		libusb_close(devh);
		libusb_exit(NULL);
		return NULL;
	}
	ut->devh = devh;
	ut->usb_session_open = 1;
	/* As in ubertooth_start_select(): a reset wait reopens the board the
	 * user named, and a message about it afterwards still names it. */
	if (kind == SELECTOR_INDEX || kind == SELECTOR_SERIAL)
		ut->selector = u_arg;
	return ut;
}

/* Connect-variant of the shared -U handling, for tools that use the
 * ubertooth_init() + ubertooth_connect() pattern rather than ubertooth_start().
 * Returns >=0 on success, <0 on failure (matching ubertooth_connect). */
int ubertooth_connect_select(ubertooth_t* ut, const char* u_arg)
{
	int index = -1;

	switch (classify_selector(u_arg, &index)) {
	case SELECTOR_DEFAULT:
		return ubertooth_connect(ut, -1);
	case SELECTOR_INDEX:
		/* As in ubertooth_start_select(): the board the user named, for
		 * the messages that are about this one board. */
		ut->selector = u_arg;
		return ubertooth_connect(ut, index);
	case SELECTOR_NO_INDEX:
		note_open_failure("no board can have that index");
		report_no_index(u_arg);
		return -1;
	case SELECTOR_SERIAL:
	default:
		ut->selector = u_arg;
		return ubertooth_connect_serial(ut, (char*)u_arg);
	}
}

int ubertooth_get_api(ubertooth_t *ut, uint16_t *version) {
	int result;
	libusb_device* dev;
	struct libusb_device_descriptor desc;
	dev = libusb_get_device(ut->devh);
	/* No request goes to the board: the descriptor is the one libusb read
	 * when it enumerated, so this reads the copy it kept. Every open has
	 * already put the version in note_opened_board_api(); this is for the
	 * caller that wants to see it. */
	result = libusb_get_device_descriptor(dev, &desc);
	if (result < 0) {
		show_libusb_error(result);
		return result;
	}
	*version = desc.bcdDevice;
	return 0;
}

/* The image a tool that makes this check needs on the board. The other image
 * this tree builds, btbr, answers the shared requests and nothing else; it is
 * driven by ubertooth-btbr, which is a Python tool and does not come through
 * here. ubertooth-util and ubertooth-dfu make no such check, because they
 * have to work on whatever firmware a board is carrying. */
#define UBERTOOTH_TOOLS_IMAGE "bluetooth_rxtx"

/* Refuses a board running another of this tree's firmware images; the
 * contract is on ubertooth_check_image() in ubertooth.h.
 *
 * Both images report the same API version, so the version check lets such a
 * board through, and the tool then fails on the first request that image does
 * not answer: btbr's request handler returns nothing for anything outside the
 * shared set, so the request stalls and the tool reports a libusb error about
 * a board that is simply carrying the wrong firmware. Naming the image to
 * flash is what the user needs instead.
 *
 * The message does not name the tool that printed it: the user typed it.
 *
 * A board that would not say which image it runs is let through: that is left
 * to the request that needs it. Two boards answer nothing here -- one too
 * busy to take this read, and one carrying firmware from before the info
 * block existed, which a development build reaches because it reports a
 * version mismatch and runs on. Both are boards somebody means to use, and
 * neither can be told from the other, so refusing them would refuse the very
 * case a development build exists for.
 */
int ubertooth_check_image(ubertooth_t *ut)
{
	struct ubertooth_fw_info info;

	if (cmd_get_firmware_info_quiet(ut->devh, &info) < 0)
		return 0;
	if (strcmp(info.image, UBERTOOTH_TOOLS_IMAGE) == 0)
		return 0;

	fprintf(stderr, "error: the Ubertooth is running %s; this tool needs "
	        "%s\n", info.image, UBERTOOTH_TOOLS_IMAGE);
	/* The board this message is about and no other, as the version
	 * mismatch's note does: -U takes the same index or serial the tool
	 * took, and without one ubertooth-dfu takes the board that is attached
	 * and refuses when there is more than one. */
	if (ut->selector != NULL && ut->selector[0] != '\0')
		fprintf(stderr, "note: flash it with ubertooth-dfu -U %s -d "
		        "firmware/" UBERTOOTH_TOOLS_IMAGE "/"
		        UBERTOOTH_TOOLS_IMAGE ".dfu -r\n", ut->selector);
	else
		fprintf(stderr, "note: flash it with ubertooth-dfu -d "
		        "firmware/" UBERTOOTH_TOOLS_IMAGE "/"
		        UBERTOOTH_TOOLS_IMAGE ".dfu -r\n");
	/* Closed without a STOP, unlike the version mismatch above: nothing has
	 * started a mode on this board yet, and btbr is one of the images that
	 * does not answer STOP, so sending it would add a second failure --
	 * about the teardown -- to a report that has already said everything. */
	ubertooth_close(ut);
	return -1;
}

int ubertooth_check_api(ubertooth_t *ut) {
	uint16_t version;
	int result;
	result = ubertooth_get_api(ut, &version);
	if (result < 0) {
		return result;
	}

	/* Host and firmware are released together, so a release build refuses
	 * any other version rather than run with behaviour the host does not
	 * expect. A development build reports it and goes on, because host
	 * tools and firmware built from different commits are the normal state
	 * of lab and debugging work. */
	if (version != UBERTOOTH_API_VERSION
	    && ubertooth_report_api_mismatch(version, ut->selector) < 0) {
		ubertooth_stop(ut);
		return -1;
	}
	/* After the version, because firmware from another tree is the bigger
	 * mismatch of the two and is the one to report. */
	return ubertooth_check_image(ut);
}
