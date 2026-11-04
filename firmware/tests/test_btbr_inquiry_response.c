/*
 * Unit tests for the inquiry response of the BR/EDR image:
 * firmware/btbr/src/btphy/inquiry_scan_state.c.
 *
 * A board answering an inquiry sends two packets. The FHS carries its address
 * and clock, and is what the inquirer is listening for; the extended inquiry
 * response that follows it carries the device name and the rest of the fields,
 * and is only meaningful to an inquirer that heard the FHS and knows a second
 * packet is coming.
 *
 * The transmit task reports the end of a transmit's slots whether or not the
 * packet reached the air -- that report is what schedules the next thing the
 * board does, so a missing one would leave the board with nothing to run --
 * and says which of the two happened. These cases pin what the inquiry
 * response does with that: an extended inquiry response is sent only after an
 * FHS that went out, and a dropped FHS takes the board back to scanning, where
 * the next sweep of the inquirer finds it.
 *
 * The state machine is included into this translation unit, which is how its
 * transmit callbacks can be called at all: they are static. Everything under
 * it is faked -- the scan and transmit tasks, the radio's mode and hop state,
 * the FHS payload codec and the host interface -- because none of it decides
 * the question here.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "minunit.h"

/* The diagnostics every btbr source can reach, declared by
 * stubs-btbr/ubtbr/debug.h, plus the console an answered inquiry is marked on.
 * All three are defined among the fakes. */
void cprintf(char *fmt, ...);
void __attribute__((noreturn)) die(char *fmt, ...);
void console_putc(char c);

/* TIMER1's counter, which the state machine reads as CENTRAL_CLKN. Here it is
 * the clock field the tests move on by one tick -- 312.5 usec -- at a time. */
#define T1TC btphy.central_clkn

#include <ubtbr/btphy.h>
#include <ubtbr/bb.h>
#include <ubtbr/btctl_intf.h>
#include <ubtbr/hop.h>
#include <ubtbr/inquiry_scan_state.h>
#include <ubtbr/scan_task.h>
#include <ubtbr/tx_task.h>

/* ---- the firmware state the inquiry response reads ---- */

btphy_t btphy;
btctl_t btctl;

/* ---- the fakes under it ---- */

static char console[256];
static unsigned console_len;

void cprintf(char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	console_len += vsnprintf(console + console_len,
				sizeof(console) - console_len, fmt, args);
	va_end(args);
	/* vsnprintf() returns the length it would have written, so a line that
	 * filled the buffer would otherwise leave console_len past the end and
	 * the next call's remaining size negative. */
	if (console_len > sizeof(console) - 1)
		console_len = sizeof(console) - 1;
}

void console_putc(char c) { (void)c; }

void __attribute__((noreturn)) die(char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	vprintf(fmt, args);
	va_end(args);
	abort();
}

/* The state the board last reported to the host. */
static btctl_state_t reported_state;

void btctl_set_state(btctl_state_t state, btctl_reason_t reason)
{
	(void)reason;
	btctl.state = state;
	reported_state = state;
}

void btphy_set_mode(btphy_mode_t mode, uint32_t lap, uint8_t uap)
{
	btphy.chan_lap = lap;
	btphy.chan_uap = uap;
	btphy.mode = mode;
}

/* The hop sequence's own state, which the response substate reads a phase out
 * of. Nothing here is about frequencies, so it is only here to be written
 * to. */
hop_state_t hop_state;

void bbpkt_fhs_prepare_payload(uint8_t *data, uint64_t parity, uint32_t lap,
		uint8_t uap, uint16_t nap, uint32_t cls, uint8_t ltaddr,
		uint8_t has_eir)
{
	(void)data; (void)parity; (void)lap; (void)uap;
	(void)nap; (void)cls; (void)ltaddr; (void)has_eir;
}

void bbpkt_fhs_finalize_payload(uint8_t *fhs_data, uint32_t clk27_2)
{
	(void)fhs_data; (void)clk27_2;
}

/* The scan window the state machine arms, and the transmit it hands over. Each
 * is armed one at a time, and a test runs it. */
static scan_task_cb_t armed_scan_cb;
static void *armed_scan_arg;
static unsigned scans_armed;
/* Whether the scheduler has room for the next scan window. A slot holds four
 * tasks and refuses the fifth, which is what the real scan task reports as
 * -1; test_btbr_scan_schedule.c drives that through the firmware's own
 * scheduler. Here it is a switch, so a test can say which arm is refused. */
static int scan_arm_refused;

int scan_task_schedule(scan_task_cb_t cb, void *cbarg)
{
	if (scan_arm_refused)
		return -1;
	armed_scan_cb = cb;
	armed_scan_arg = cbarg;
	scans_armed++;
	return 0;
}

static tx_task_cb_t armed_tx_cb;
static void *armed_tx_arg;
static unsigned transmits_armed;
/* The baseband header of each transmit, so a test can say which packet it
 * was: the FHS carries the FHS type code and the extended inquiry response
 * carries whatever the host composed. */
static uint8_t transmit_type[4];

void tx_task_schedule(unsigned delay, tx_task_cb_t cb, void *cbarg,
			bbhdr_t *hdr, uint8_t *data)
{
	(void)delay; (void)data;
	armed_tx_cb = cb;
	armed_tx_arg = cbarg;
	if (transmits_armed < sizeof(transmit_type) / sizeof(transmit_type[0]))
		transmit_type[transmits_armed] = hdr ? hdr->type : 0xff;
	transmits_armed++;
}

/* ---- the state machine under test ---- */

#include "../btbr/src/btphy/inquiry_scan_state.c"

/* ---- driving it ---- */

/* The extended inquiry response the host composed, which the scan sends after
 * its FHS. Only its presence and its header matter here. */
static btctl_tx_pkt_t an_eir_packet;

#define EIR_TYPE	BB_TYPE_DM1

static void an_inquiry_scan(int with_an_eir)
{
	memset(&btphy, 0, sizeof(btphy));
	memset(&btctl, 0, sizeof(btctl));
	memset(&an_eir_packet, 0, sizeof(an_eir_packet));
	an_eir_packet.bb_hdr.type = EIR_TYPE;
	btctl.eir_pkt = with_an_eir ? &an_eir_packet : NULL;

	armed_scan_cb = NULL;
	armed_tx_cb = NULL;
	scans_armed = 0;
	transmits_armed = 0;
	memset(transmit_type, 0, sizeof(transmit_type));
	reported_state = BTCTL_STATE_STANDBY;
	scan_arm_refused = 0;
	console_len = 0;
	console[0] = '\0';

	inquiry_scan_state_setup();
	if (armed_scan_cb == NULL)
		die("the inquiry scan opened no scan window\n");
}

/* An inquiry arrives in the window the scan opened, which is what makes the
 * board answer. The callback is taken first, so a scan re-armed afterwards can
 * be told apart from this one. */
static void an_inquiry_arrives(void)
{
	scan_task_cb_t cb = armed_scan_cb;
	void *arg = armed_scan_arg;

	armed_scan_cb = NULL;
	scans_armed = 0;
	cb(1, arg);
}

/* The transmit's slots are over, with `sent` saying whether the packet reached
 * the air. */
static void the_transmit_ends(int sent)
{
	tx_task_cb_t cb = armed_tx_cb;
	void *arg = armed_tx_arg;

	if (cb == NULL)
		die("the inquiry response armed no transmit\n");
	armed_tx_cb = NULL;
	cb(sent, arg);
}

/* ---- an FHS that goes out ---- */

/* The ordinary case: the inquirer heard the FHS, so the fields follow it. */
MU_TEST(an_fhs_that_goes_out_is_followed_by_the_extended_inquiry_response)
{
	an_inquiry_scan(1);
	an_inquiry_arrives();

	mu_assert_int_eq(1, (int)transmits_armed);
	mu_assert_int_eq(BB_TYPE_FHS, transmit_type[0]);

	the_transmit_ends(1);

	mu_assert_int_eq(2, (int)transmits_armed);
	mu_assert_int_eq(EIR_TYPE, transmit_type[1]);
	/* The board is answering, not scanning. */
	mu_assert_int_eq(0, (int)scans_armed);
}

/* A board with no extended inquiry response of its own has said all it has to
 * say, and goes back to scanning. */
MU_TEST(an_fhs_with_no_fields_to_follow_it_goes_back_to_scanning)
{
	an_inquiry_scan(0);
	an_inquiry_arrives();
	the_transmit_ends(1);

	mu_assert_int_eq(1, (int)transmits_armed);
	mu_assert_int_eq(1, (int)scans_armed);
	mu_assert_int_eq(BT_MODE_INQUIRY_SCAN, btphy.mode);
}

/* ---- an FHS that never went on the air ---- */

/*
 * The regression. The extended inquiry response belongs to an FHS the
 * inquirer heard: on its own it says "and here are my fields" to a device that
 * has heard nothing from this board and is not listening for a second packet.
 * Sending it would put a packet on the air for nobody and spend the slot the
 * board could be scanning in.
 *
 * So a dropped FHS takes the board back to the inquiry scan instead. An
 * inquirer sweeps the band over and over, so it asks again, and the scan the
 * user started still has most of its minute to run.
 */
MU_TEST(a_dropped_fhs_sends_no_extended_inquiry_response)
{
	an_inquiry_scan(1);
	an_inquiry_arrives();

	the_transmit_ends(0);

	/* The FHS is still the only transmit. */
	mu_assert_int_eq(1, (int)transmits_armed);
	/* And the board is scanning again, on the scan substate rather than
	 * left on the response's. */
	mu_assert_int_eq(1, (int)scans_armed);
	mu_assert_int_eq(BT_MODE_INQUIRY_SCAN, btphy.mode);
	mu_assert_int_eq(BTCTL_STATE_INQUIRY_SCAN, btctl.state);
}

/* The user sees why: a console line saying the FHS was not sent. */
MU_TEST(a_dropped_fhs_says_so_on_the_console)
{
	an_inquiry_scan(1);
	an_inquiry_arrives();

	the_transmit_ends(0);

	mu_assert(strstr(console, "not sent") != NULL, console);
}

/* ---- a scan window the scheduler has no room for ---- */

/*
 * A slot holds four tasks and refuses the fifth, so the slot a scan window
 * would be armed in can be full, and the scan task reports that back. An
 * inquiry scan that was not told would sit in the state the host asked for
 * with no task left to run: listening to nothing forever and never saying
 * why. It ends the session instead.
 */
MU_TEST(a_scan_window_that_cannot_be_armed_ends_the_scan)
{
	an_inquiry_scan(1);
	an_inquiry_arrives();
	scan_arm_refused = 1;

	the_transmit_ends(0);

	mu_assert_int_eq(0, (int)scans_armed);
	mu_assert(armed_scan_cb == NULL,
		"a window was armed after the scheduler refused it");
	mu_assert_int_eq(BTCTL_STATE_STANDBY, btctl.state);
}

/*
 * A dropped extended inquiry response is a different matter: the answer the
 * inquirer was listening for has already gone out, and there is nothing after
 * it either way. The board goes back to scanning, as it does after one that
 * was sent.
 */
MU_TEST(a_dropped_extended_inquiry_response_goes_back_to_scanning)
{
	an_inquiry_scan(1);
	an_inquiry_arrives();
	the_transmit_ends(1);

	mu_assert_int_eq(2, (int)transmits_armed);
	the_transmit_ends(0);

	mu_assert_int_eq(1, (int)scans_armed);
	mu_assert_int_eq(BT_MODE_INQUIRY_SCAN, btphy.mode);
}

int main(void)
{
	printf("BR/EDR inquiry response tests\n");
	MU_RUN(an_fhs_that_goes_out_is_followed_by_the_extended_inquiry_response);
	MU_RUN(an_fhs_with_no_fields_to_follow_it_goes_back_to_scanning);
	MU_RUN(a_dropped_fhs_sends_no_extended_inquiry_response);
	MU_RUN(a_dropped_fhs_says_so_on_the_console);
	MU_RUN(a_dropped_extended_inquiry_response_goes_back_to_scanning);
	MU_RUN(a_scan_window_that_cannot_be_armed_ends_the_scan);
	return mu_report();
}
