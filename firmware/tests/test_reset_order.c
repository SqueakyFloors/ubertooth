/*
 * A board asked to reset lets the host collect the answer first.
 *
 * UBERTOOTH_FLASH and UBERTOOTH_RESET arrive as control transfers, and a
 * control transfer is not finished when its handler returns: the stack calls
 * the handler from the setup packet, then writes the zero-length status stage
 * into the endpoint 0 buffer, where it sits until the host's next IN token
 * collects it -- up to a whole 1 ms frame later. reset() turns the USB pins
 * off as its first act, so a reset that runs in between throws that packet
 * away and the host sees a stalled transfer instead of an answer.
 * ubertooth-dfu reads the stall as a board refusing to enter DFU and exits 1,
 * while the board does enter the bootloader -- which carries no serial
 * number, so no `-U <serial>` can reach it again.
 *
 * So both images defer the reset out of the handler and then let time pass
 * before running it. bluetooth_rxtx asks its mode dispatch for MODE_RESET,
 * which busy-waits and resets; btbr queues a message its control loop picks
 * up, and that handler spends the time calling usb_work(), which keeps its
 * queue to the host moving while it passes. The delay is what the fix is: btbr
 * already deferred the reset, into a loop pass that came microseconds later.
 *
 * The first cases run the real control transfer handler
 * (firmware/common/lpcusb/target/usbcontrol.c) against a fake controller, so
 * the order the stack does things in is the real one. The fake has a host that
 * collects an endpoint 0 packet when it is asked to, which is the part the
 * firmware has no control over and the part that makes the wait necessary; the
 * waiting and non-waiting loops are written out around it.
 *
 * What those cases cannot show is the delay itself: it is hardware code, a
 * busy loop in one image and a hardware counter in the other. The scans at the
 * end check that both images spend it, before their reset and outside their
 * request handlers, so the loops modelled here cannot drift away from the
 * firmware.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "minunit.h"
#include "source_scan.h"

#include "type.h"
#include "usbstruct.h"
#include "usbapi.h"

/* -------------------------------------------------------------------------
 * The fake controller, and a host that reads it
 * ------------------------------------------------------------------------- */

/* Are the USB pins still on? reset() turns them off before anything else. */
static int usb_alive;
/* A packet the controller holds for endpoint 0 IN, waiting for the host. */
static int ep0_in_pending;
static int ep0_in_pending_len;
/* Packets the host has collected, and the last one's length. */
static int ep0_in_collected;
static int ep0_in_collected_len;
/* Was endpoint 0 stalled? */
static int ep0_stalled;
/* The setup packet the host sent, and how many bytes of it to hand over. */
static U8 setup_bytes[8];
static int setup_len;

int USBHwEPRead(U8 bEP, U8 *pbBuf, U32 iMaxLen)
{
	U32 taken = iMaxLen < (U32)setup_len ? iMaxLen : (U32)setup_len;

	(void)bEP;
	if (!usb_alive)
		return -1;
	if (pbBuf != NULL)
		memcpy(pbBuf, setup_bytes, taken);
	return (int)taken;
}

int USBHwEPWrite(U8 bEP, U8 *pbBuf, U32 iLen)
{
	(void)pbBuf;
	if (!usb_alive)
		return -1;
	if (bEP == 0x80) {
		ep0_in_pending = 1;
		ep0_in_pending_len = (int)iLen;
	}
	return (int)iLen;
}

void USBHwEPStall(U8 bEP, BOOL fStall)
{
	if (bEP == 0x80 && fStall)
		ep0_stalled = 1;
}

/* usbcontrol.c calls this filter before the vendor handler; the real one
 * lives in usbstdreq.c, which this test does not need. */
BOOL USBFilterOsVendorMessage(TSetupPacket *pSetup, BOOL *pfSuccess,
                              int *piLen, U8 **ppbData)
{
	(void)pSetup; (void)pfSuccess; (void)piLen; (void)ppbData;
	return FALSE;
}

/*
 * The host's next IN token, which is the firmware's deadline: a packet the
 * controller is still holding when the pins go off is gone, and the transfer
 * the host was waiting on comes back as a stall.
 */
static void host_polls_endpoint_zero(void)
{
	if (!usb_alive || !ep0_in_pending)
		return;
	ep0_in_pending = 0;
	ep0_in_collected++;
	ep0_in_collected_len = ep0_in_pending_len;
}

/* reset(), as far as this test cares: the USB pins go off first. */
static void fake_reset(void)
{
	usb_alive = 0;
	ep0_in_pending = 0;
}

/* -------------------------------------------------------------------------
 * The request handler and the loop below it
 * ------------------------------------------------------------------------- */

/* Set by the handler, acted on by the loop, as both images do it. */
static int reset_pending;
/* Had the host been answered by the time the handler ran? */
static int collected_in_handler;

static BOOL handler_asks_for_a_reset(TSetupPacket *pSetup, int *piLen,
                                     U8 **ppbData)
{
	(void)pSetup; (void)ppbData;
	collected_in_handler = ep0_in_collected;
	*piLen = 0;
	reset_pending = 1;
	return TRUE;
}

static U8 handler_data[64];

/* A vendor host-to-device request with no data stage, as cmd_flash() sends
 * UBERTOOTH_FLASH and cmd_reset() sends UBERTOOTH_RESET. */
static void host_sends_a_reset_request(void)
{
	usb_alive = 1;
	ep0_in_pending = 0;
	ep0_in_pending_len = -1;
	ep0_in_collected = 0;
	ep0_in_collected_len = -1;
	ep0_stalled = 0;
	reset_pending = 0;
	collected_in_handler = -1;

	memset(setup_bytes, 0, sizeof(setup_bytes));
	setup_bytes[0] = 0x40;  /* OUT, vendor, device */
	setup_bytes[1] = 55;    /* the request number does not matter here */
	/* wValue, wIndex and wLength all zero */
	setup_len = (int)sizeof(setup_bytes);

	USBRegisterRequestHandler(REQTYPE_TYPE_VENDOR,
	                          handler_asks_for_a_reset, handler_data);
	USBHandleControlTransfer(0x00, EP_STATUS_SETUP);
}

/* The loop as it was: the queued reset runs on the next pass, before the host
 * has had a chance to ask for the packet. */
static void loop_resets_at_once(void)
{
	if (reset_pending)
		fake_reset();
}

/* The loop as it is: the delay is time the host gets to collect the packet in,
 * and only then does the board go. */
static void loop_waits_then_resets(void)
{
	if (!reset_pending)
		return;
	host_polls_endpoint_zero();  /* what happens during the delay */
	fake_reset();
}

/*
 * The property the fix rests on: the handler cannot answer the host. The
 * status stage is written after it returns and collected later still, so
 * anything it asks for happens while the host is waiting.
 */
MU_TEST(test_the_host_is_answered_after_the_handler_returns)
{
	host_sends_a_reset_request();

	mu_assert_int_eq(0, collected_in_handler);
	mu_assert_int_eq(0, ep0_in_collected);
	mu_assert_int_eq(1, ep0_in_pending);
	mu_assert_int_eq(0, ep0_in_pending_len);
	mu_assert_int_eq(0, ep0_stalled);
}

/* The bug: resetting before the host asks for the packet throws it away, and
 * the host is left with a transfer that was never answered. */
MU_TEST(test_a_reset_before_the_host_reads_loses_the_answer)
{
	host_sends_a_reset_request();
	loop_resets_at_once();
	host_polls_endpoint_zero();

	mu_assert_int_eq(0, ep0_in_collected);
	mu_assert_int_eq(0, usb_alive);
}

/* The fix: the host collects its answer, and the board still resets. */
MU_TEST(test_waiting_first_answers_the_host_and_still_resets)
{
	host_sends_a_reset_request();
	loop_waits_then_resets();

	mu_assert_int_eq(1, ep0_in_collected);
	mu_assert_int_eq(0, ep0_in_collected_len);
	mu_assert_int_eq(0, ep0_stalled);
	mu_assert_int_eq(0, usb_alive);
}

/* -------------------------------------------------------------------------
 * The firmware's own shape
 * ------------------------------------------------------------------------- */

/* Where a region of source spends the delay and where it resets, by line
 * number, 0 for a call it does not make. */
struct reset_scan {
	int found;  /* the region was found at all */
	int delay;
	int loop;   /* the first while or for header in the region */
	char loop_text[512];
	int standby;
	int reset;
};

/*
 * The text a source gives a #define, spaces collapsed. Used to read the two
 * numbers btbr's delay is made of, which are in different files from each
 * other and from the loop that spends it.
 */
static int define_text(const char *path, const char *name, char *out,
                       size_t size)
{
	char line[512], needle[96];
	int in_block = 0, found = 0;
	FILE *file = fopen(path, "r");

	if (file == NULL)
		return 0;
	snprintf(needle, sizeof(needle), "#define %s", name);
	while (fgets(line, sizeof(line), file) != NULL) {
		strip_comments(line, &in_block);
		if (strncmp(line, needle, strlen(needle)) != 0 ||
		    is_ident_char(line[strlen(needle)]))
			continue;
		collapse_spaces(line + strlen(needle), out, size);
		found = 1;
		break;
	}
	fclose(file);
	return found;
}

/*
 * Scan `path` for the region that starts at the line containing `start` and
 * record where it calls `delays` -- a call name, matched on its first
 * characters -- and where it calls `goes_away`, the function that takes the
 * board off the bus: reset(), or set_isp(), which hands the chip to its
 * on-chip programmer and is the same thing to the host. The region ends at the
 * first `break;` when `ends_at_break` is set -- a case of a switch -- and
 * otherwise at the first closing brace in column 1, which is the end of a
 * function.
 */
static int scan_reset_region(const char *path, const char *start,
                             const char *delays, const char *goes_away,
                             int ends_at_break, struct reset_scan *s)
{
	char line[512], name[64];
	int lineno = 0, in_block = 0, in_region = 0;
	FILE *file = fopen(path, "r");

	memset(s, 0, sizeof(*s));
	if (file == NULL)
		return 0;

	while (fgets(line, sizeof(line), file) != NULL) {
		lineno++;
		strip_comments(line, &in_block);

		if (!in_region) {
			if (strstr(line, start) != NULL) {
				in_region = 1;
				s->found = lineno;
			}
			continue;
		}
		if (s->loop == 0 && (contains_identifier(line, "while") ||
		                     contains_identifier(line, "for"))) {
			s->loop = lineno;
			collapse_spaces(line, s->loop_text,
			                sizeof(s->loop_text));
		}
		if (s->standby == 0 &&
		    is_assignment_of(line, "btctl.state", "BTCTL_STATE_STANDBY"))
			s->standby = lineno;
		if (call_name(line, name, sizeof(name))) {
			if (strncmp(name, delays, strlen(delays)) == 0 &&
			    s->delay == 0)
				s->delay = lineno;
			if (strcmp(name, goes_away) == 0 && s->reset == 0)
				s->reset = lineno;
		}
		if (ends_at_break) {
			if (contains_identifier(line, "break"))
				break;
		} else if (line[0] == '}') {
			break;
		}
	}

	fclose(file);
	return 1;
}

/*
 * Both images let that time pass before they go away, each in the way its own
 * loop allows: bluetooth_rxtx's mode dispatch busy-waits (wait(), wait_ms()
 * and wait_us() are the same thing here), and btbr's queued handler spends the
 * time in a loop calling usb_work(), which is both the delay and the thing
 * that keeps its queue to the host moving while it passes.
 *
 * Four regions, because there are two ways off the bus: a reset, and the
 * hand-over to the on-chip programmer that `ubertooth-util -i` asks for. The
 * programmer is the one a user reaches by mistyping an option, and it used to
 * run from the request handler, so the host was told its request had timed out
 * for a board that had done what was asked.
 */
MU_TEST(test_both_images_wait_before_they_reset)
{
	static const struct {
		const char *path;
		const char *start;
		const char *delays;
		const char *goes_away; /* the call that takes the board off USB */
		const char *bound;  /* what the loop around it counts, if any */
		int ends_at_break;
		const char *what;
	} regions[] = {
		{ FW_RXTX_DIR "/bluetooth_rxtx.c", "case MODE_RESET:",
		  "wait", "reset", NULL, 1, "the mode dispatch's reset case" },
		{ FW_RXTX_DIR "/bluetooth_rxtx.c", "case MODE_ISP:",
		  "wait", "set_isp", NULL, 1,
		  "the mode dispatch's programmer case" },
		{ FW_BTBR_DIR "/src/btctl/btctl.c",
		  "btctl_handle_reset_req(msg_t *msg)",
		  "usb_work", "reset", "RESET_DELAY_CLKN", 0,
		  "the queued reset handler" },
		{ FW_BTBR_DIR "/src/btctl/btctl.c",
		  "btctl_handle_isp_req(msg_t *msg)",
		  "usb_work", "set_isp", "RESET_DELAY_CLKN", 0,
		  "the queued programmer handler" },
	};
	char message[1024];
	size_t i;

	for (i = 0; i < sizeof(regions) / sizeof(regions[0]); i++) {
		struct reset_scan s;

		snprintf(message, sizeof(message), "cannot open %s to scan",
		         regions[i].path);
		mu_assert_fatal(scan_reset_region(regions[i].path,
		                                  regions[i].start,
		                                  regions[i].delays,
		                                  regions[i].goes_away,
		                                  regions[i].ends_at_break, &s),
		                message);

		snprintf(message, sizeof(message),
		         "%s: the scan did not find %s, so the checks below "
		         "would pass without reading anything",
		         regions[i].path, regions[i].what);
		mu_assert_fatal(s.found > 0, message);

		snprintf(message, sizeof(message),
		         "%s:%d: %s does not call %s() at all, so the scan is "
		         "not reading the code it names", regions[i].path,
		         s.found, regions[i].what, regions[i].goes_away);
		mu_assert(s.reset > 0, message);

		snprintf(message, sizeof(message),
		         "%s:%d: %s calls %s() without giving the host time "
		         "first (no %s() call before it), so the board goes off "
		         "the bus before the host has collected the answer to "
		         "the control transfer that asked for it, and the host "
		         "sees a stall rather than an accepted request",
		         regions[i].path, s.reset, regions[i].what,
		         regions[i].goes_away, regions[i].delays);
		mu_assert(s.delay > 0 && s.delay < s.reset, message);

		if (regions[i].bound == NULL)
			continue;

		snprintf(message, sizeof(message),
		         "%s:%d: %s calls %s() once rather than from a loop, so "
		         "it spends no time there", regions[i].path, s.delay,
		         regions[i].what, regions[i].delays);
		mu_assert(s.loop > 0 && s.loop < s.delay, message);

		snprintf(message, sizeof(message),
		         "%s:%d: the loop around %s() is `%s`, which does not "
		         "count %s, so how long it runs for is not the delay "
		         "the test below measures", regions[i].path, s.loop,
		         regions[i].delays, s.loop_text, regions[i].bound);
		mu_assert(contains_identifier(s.loop_text, regions[i].bound),
		          message);

		snprintf(message, sizeof(message),
		         "%s:%d: %s does not put the controller in standby "
		         "first, so a state that is still receiving keeps "
		         "filling the queue to the host, and one that cannot "
		         "enqueue calls die() -- the board would stop here "
		         "rather than reset", regions[i].path, s.found,
		         regions[i].what);
		mu_assert(s.standby > 0 && s.standby < s.reset, message);
	}
}

/*
 * And btbr's delay is long enough to be worth spending. The number is written
 * as a share of the clock rate, in a different file from the rate itself, so
 * this reads both and works out the milliseconds.
 */
MU_TEST(test_btbrs_delay_outlasts_a_usb_frame)
{
	char text[256], message[1024];
	int rate = 0, share = 0, ms;

	mu_assert_fatal(define_text(FW_BTBR_DIR "/include/ubtbr/btphy.h",
	                            "CLKN_RATE", text, sizeof(text)),
	                "btphy.h does not define CLKN_RATE, so the delay below "
	                "cannot be worked out");
	rate = atoi(text);
	mu_assert_fatal(rate > 0, "CLKN_RATE is not a number of ticks a second");

	mu_assert_fatal(define_text(FW_BTBR_DIR "/src/btctl/btctl.c",
	                            "RESET_DELAY_CLKN", text, sizeof(text)),
	                "btctl.c does not define RESET_DELAY_CLKN, so the reset "
	                "handler's loop has no delay to count");

	snprintf(message, sizeof(message),
	         "RESET_DELAY_CLKN is `%s`, not the CLKN_RATE/N this reads", text);
	mu_assert_fatal(sscanf(text, "(CLKN_RATE/%d)", &share) == 1 && share > 0,
	                message);

	/* (rate / share) ticks, at rate ticks a second -- worked out in that
	 * order, so a share that does not divide the rate counts as the ticks
	 * the firmware would really wait. */
	ms = (rate / share) * 1000 / rate;
	snprintf(message, sizeof(message),
	         "btbr waits %d ms (CLKN_RATE/%d of a second) before it resets, "
	         "which does not clear the 1 ms USB frame the host's next token "
	         "can be away by the margin the comment claims", ms, share);
	mu_assert(ms >= 5, message);
}

/*
 * ...and neither request handler goes off the bus where it stands, by either
 * route. The wait is in the loop, so a handler that called reset() or
 * set_isp() itself would skip it -- and would also run it from the USB
 * interrupt, in the middle of the transfer it is answering.
 */
MU_TEST(test_neither_request_handler_resets_in_place)
{
	static const char *const handlers[] = {
		FW_RXTX_DIR "/bluetooth_rxtx.c",
		FW_BTBR_DIR "/src/btctl/vendor_request_handler.c",
	};
	static const char *const leaves_the_bus[] = { "reset", "set_isp" };
	char line[512], name[64], message[1024];
	size_t i, j;

	for (i = 0; i < sizeof(handlers) / sizeof(handlers[0]); i++) {
		FILE *file = fopen(handlers[i], "r");
		int lineno = 0, in_block = 0, in_switch = 0;
		int cases = 0, direct_reset = 0;

		snprintf(message, sizeof(message), "cannot open %s to scan",
		         handlers[i]);
		mu_assert_fatal(file != NULL, message);

		while (fgets(line, sizeof(line), file) != NULL) {
			lineno++;
			strip_comments(line, &in_block);

			if (strstr(line, "case UBERTOOTH_") != NULL) {
				in_switch = 1;
				cases++;
				continue;
			}
			if (!in_switch)
				continue;
			/* The switch ends at its default case. */
			if (strstr(line, "default:") != NULL)
				break;
			if (!call_name(line, name, sizeof(name)))
				continue;
			for (j = 0; j < sizeof(leaves_the_bus) /
			                sizeof(leaves_the_bus[0]); j++)
				if (strcmp(name, leaves_the_bus[j]) == 0 &&
				    direct_reset == 0)
					direct_reset = lineno;
		}
		fclose(file);

		snprintf(message, sizeof(message),
		         "%s: the scan found %d UBERTOOTH_ cases, so it is not "
		         "reading the request handler's switch", handlers[i],
		         cases);
		mu_assert(cases > 5, message);

		snprintf(message, sizeof(message),
		         "%s:%d: a request handler case calls reset() or "
		         "set_isp() itself, so the board goes off the bus from "
		         "inside the USB interrupt, in the middle of the "
		         "transfer that asked for it", handlers[i], direct_reset);
		mu_assert(direct_reset == 0, message);
	}
}

int main(void)
{
	printf("reset order tests\n");
	MU_RUN(test_the_host_is_answered_after_the_handler_returns);
	MU_RUN(test_a_reset_before_the_host_reads_loses_the_answer);
	MU_RUN(test_waiting_first_answers_the_host_and_still_resets);
	MU_RUN(test_both_images_wait_before_they_reset);
	MU_RUN(test_btbrs_delay_outlasts_a_usb_frame);
	MU_RUN(test_neither_request_handler_resets_in_place);
	return mu_report();
}
