/*
 * Unit tests for firmware/common/watchdog.c
 *
 * The watchdog resets a board that has stopped servicing USB, and leaves a
 * breadcrumb in a corner of RAM that nothing initialises, so the board can say
 * where it was when that happened. Both halves run here: watchdog.c reaches
 * the part only through the register macros, which stubs-watchdog/ubertooth.h
 * replaces with ordinary variables, and the breadcrumb is an ordinary object
 * this file defines rather than a linker-placed one.
 *
 * What the tests are about:
 *
 *  - the feed is a two-write sequence the part checks, so it has to go out as
 *    0xAA then 0x55 with nothing in between, and it must do nothing at all
 *    before the watchdog is armed (the bootloader links the same code);
 *  - a watchdog reset the firmware asked for -- a reset request, a flash, the
 *    on-chip programmer -- is not the board having stopped answering, and must
 *    not be counted or recorded as one;
 *  - power-on and brown-out mean the breadcrumb cannot be trusted, and the
 *    counters start again;
 *  - the reply the host reads is the eight bytes ubertooth_interface.h
 *    describes, in that order.
 *
 * The last cases scan the firmware sources as text, the way test_frend.c does,
 * to check the feed is called from the places USB is serviced and from the
 * loops of the modes that cannot prove more than that -- and from nowhere
 * driven by a timer, which would only show the CPU is running.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "minunit.h"
#include "source_scan.h"
/* The fake registers' declarations, and the modes and sites the breadcrumb
 * records. */
#include "ubertooth.h"

/* ---- the fake registers the stub header points at ---- */

uint32_t fake_wdmod;
uint32_t fake_wdtc;
uint32_t fake_wdclksel;
uint32_t fake_rsid;
uint32_t fake_iser0;
uint32_t fake_icer0;

uint32_t fake_wdfeed_writes[FAKE_WDFEED_MAX];
unsigned fake_wdfeed_count;

void fake_wdfeed_write(uint32_t value)
{
	if (fake_wdfeed_count < FAKE_WDFEED_MAX)
		fake_wdfeed_writes[fake_wdfeed_count] = value;
	fake_wdfeed_count++;
}

/* The code under test, and the breadcrumb it works on. watchdog.c declares the
 * object and the linker script places it; here this file is its definition. */
#include "watchdog.c"

volatile watchdog_crumb_t watchdog_crumb;

/* ---- helpers ---- */

/* A board coming up: the registers as the part leaves them, the breadcrumb as
 * `cause` and the fields say, and the armed flag back to its startup value. */
static void boot_with(uint32_t cause)
{
	fake_wdmod = 0;
	fake_wdtc = 0xFF;
	fake_wdclksel = 0;
	fake_rsid = cause;
	fake_iser0 = 0;
	fake_icer0 = 0;
	fake_wdfeed_count = 0;
	/* .bss is zeroed on every boot, armed flag included. */
	watchdog_armed = 0;
}

/* A breadcrumb left by a run that was in `mode`, waiting at `site`, with
 * `intent` -- what the live fields hold when a board stops feeding. */
static void crumb_left_by_a_run(uint8_t mode, uint8_t site, uint8_t intent,
                                uint8_t resets, uint8_t boots)
{
	watchdog_crumb.magic = WATCHDOG_CRUMB_MAGIC;
	watchdog_crumb.live_mode = mode;
	watchdog_crumb.live_site = site;
	watchdog_crumb.live_intent = intent;
	watchdog_crumb.reset_cause = 0;
	watchdog_crumb.reset_mode = MODE_IDLE;
	watchdog_crumb.reset_site = WATCHDOG_SITE_NONE;
	watchdog_crumb.resets = resets;
	watchdog_crumb.boots = boots;
}

/* ---- arming it ---- */

/*
 * The timeout the board runs with, and where it comes from: the watchdog
 * counter is clocked at a microsecond a count, so the count is the timeout in
 * milliseconds times a thousand. The clock source is written before the
 * enable, because the part only accepts it while the watchdog is stopped.
 */
MU_TEST(test_arming_sets_the_timeout_and_the_reset)
{
	boot_with(RSID_POR);
	watchdog_start();

	mu_assert_int_eq(UBERTOOTH_WATCHDOG_TIMEOUT_MS * 1000, (int)fake_wdtc);
	mu_assert(fake_wdmod & WDMOD_WDEN, "the watchdog is not enabled");
	mu_assert(fake_wdmod & WDMOD_WDRESET,
	          "a time-out would interrupt rather than reset the board");
	/* Zero is the internal RC oscillator: not the PLLs, which a wedged
	 * board may have left in any state. */
	mu_assert_int_eq(0, (int)fake_wdclksel);
}

/* Arming it loads the counter, which is the feed. */
MU_TEST(test_arming_feeds_once)
{
	boot_with(RSID_POR);
	fake_wdfeed_count = 0;
	watchdog_start();

	mu_assert_int_eq(2, (int)fake_wdfeed_count);
	mu_assert_int_eq(0xAA, (int)fake_wdfeed_writes[0]);
	mu_assert_int_eq(0x55, (int)fake_wdfeed_writes[1]);
}

/* ---- feeding it ---- */

/*
 * The sequence the part demands. Anything but 0x55 after the 0xAA is itself a
 * failed feed, and with the reset enabled that resets the board -- so the two
 * writes have to be adjacent, every time.
 */
MU_TEST(test_each_feed_is_the_sequence_the_part_wants)
{
	unsigned i;

	boot_with(RSID_POR);
	watchdog_start();
	fake_wdfeed_count = 0;

	watchdog_usb_serviced();
	watchdog_usb_ready();
	watchdog_usb_serviced();

	mu_assert_int_eq(6, (int)fake_wdfeed_count);
	for (i = 0; i < fake_wdfeed_count; i += 2) {
		mu_assert_int_eq(0xAA, (int)fake_wdfeed_writes[i]);
		mu_assert_int_eq(0x55, (int)fake_wdfeed_writes[i + 1]);
	}
}

/*
 * The feed holds the USB interrupt off while it writes the pair, because the
 * USB interrupt handler feeds too and a feed landing between the two writes
 * would reset the board. It puts the interrupt back the way it found it.
 */
MU_TEST(test_the_feed_holds_the_usb_interrupt_off)
{
	boot_with(RSID_POR);
	watchdog_start();

	fake_iser0 = ISER0_ISE_USB;
	fake_icer0 = 0;
	watchdog_usb_serviced();
	mu_assert_int_eq((int)ICER0_ICE_USB, (int)fake_icer0);
	mu_assert(fake_iser0 & ISER0_ISE_USB,
	          "the interrupt was left off after the feed");

	/* A caller that already holds it off does not switch it on. */
	fake_iser0 = 0;
	watchdog_usb_serviced();
	mu_assert_int_eq(0, (int)fake_iser0);
}

/*
 * Nothing is written before the watchdog is armed. The bootloader links this
 * file's callers -- the USB hardware layer is shared -- and never arms the
 * watchdog, so a feed from there must be inert: writing the feed register
 * while the watchdog is off is harmless, but starting to write registers the
 * bootloader does not use is not something to rely on.
 */
MU_TEST(test_a_feed_before_arming_does_nothing)
{
	boot_with(RSID_POR);

	watchdog_usb_serviced();
	watchdog_usb_ready();

	mu_assert_int_eq(0, (int)fake_wdfeed_count);
	mu_assert_int_eq(0, (int)fake_wdmod);
}

/*
 * The hand-over to the on-chip programmer: it keeps the chip running without a
 * reset, so the watchdog stays armed with nothing feeding it. It cannot be
 * switched off, so the timeout goes to the longest the counter holds.
 */
MU_TEST(test_stretching_sets_the_longest_timeout_and_feeds)
{
	boot_with(RSID_POR);
	watchdog_start();
	fake_wdfeed_count = 0;

	watchdog_stretch();

	mu_assert_int_eq(-1, (int)fake_wdtc); /* 0xFFFFFFFF */
	mu_assert_int_eq(2, (int)fake_wdfeed_count);
}

/* ---- the breadcrumb at boot ---- */

/* Power-on: nothing in RAM is a breadcrumb, so the counters start at zero and
 * this boot is the first. */
MU_TEST(test_power_on_starts_the_counters_again)
{
	boot_with(RSID_POR);
	crumb_left_by_a_run(MODE_BT_FOLLOW_LE, WATCHDOG_SITE_XOSC,
	                    WATCHDOG_INTENT_RUNNING, 7, 9);

	watchdog_boot();

	mu_assert_int_eq(0, watchdog_crumb.resets);
	mu_assert_int_eq(1, watchdog_crumb.boots);
	mu_assert_int_eq(MODE_IDLE, watchdog_crumb.reset_mode);
	mu_assert_int_eq(WATCHDOG_SITE_NONE, watchdog_crumb.reset_site);
	mu_assert_int_eq(RSID_POR, watchdog_crumb.reset_cause);
}

/* Brown-out says the same thing about RAM. */
MU_TEST(test_brown_out_starts_the_counters_again)
{
	boot_with(RSID_BODR | RSID_WDTR);
	crumb_left_by_a_run(MODE_SPECAN, WATCHDOG_SITE_FS_LOCK,
	                    WATCHDOG_INTENT_RUNNING, 3, 4);

	watchdog_boot();

	mu_assert_int_eq(0, watchdog_crumb.resets);
	mu_assert_int_eq(MODE_IDLE, watchdog_crumb.reset_mode);
}

/* RAM that was never a breadcrumb is not read as one, whatever the cause. */
MU_TEST(test_an_unrecognised_breadcrumb_is_started_again)
{
	boot_with(RSID_WDTR);
	crumb_left_by_a_run(MODE_SPECAN, WATCHDOG_SITE_FS_LOCK,
	                    WATCHDOG_INTENT_RUNNING, 3, 4);
	watchdog_crumb.magic = 0xdeadbeef;

	watchdog_boot();

	mu_assert_int_eq(WATCHDOG_CRUMB_MAGIC, (int)watchdog_crumb.magic);
	mu_assert_int_eq(0, watchdog_crumb.resets);
	mu_assert_int_eq(1, watchdog_crumb.boots);
}

/*
 * The case this is all for: the board stopped feeding the watchdog while a
 * mode was running, and the breadcrumb says which mode and which wait.
 */
MU_TEST(test_a_wedge_is_counted_and_recorded)
{
	boot_with(RSID_WDTR);
	crumb_left_by_a_run(MODE_BT_FOLLOW_LE, WATCHDOG_SITE_XOSC,
	                    WATCHDOG_INTENT_RUNNING, 1, 2);

	watchdog_boot();

	mu_assert_int_eq(2, watchdog_crumb.resets);
	mu_assert_int_eq(3, watchdog_crumb.boots);
	mu_assert_int_eq(MODE_BT_FOLLOW_LE, watchdog_crumb.reset_mode);
	mu_assert_int_eq(WATCHDOG_SITE_XOSC, watchdog_crumb.reset_site);
	/* And the live fields start again, for the run beginning now. */
	mu_assert_int_eq(MODE_IDLE, watchdog_crumb.live_mode);
	mu_assert_int_eq(WATCHDOG_SITE_NONE, watchdog_crumb.live_site);
	mu_assert_int_eq(WATCHDOG_INTENT_RUNNING, watchdog_crumb.live_intent);
}

/*
 * A reset the firmware asked for ends in the same watchdog reset -- that is
 * how this part resets itself -- so without the intent every reset request and
 * every firmware flash would be recorded as a board that stopped answering.
 */
MU_TEST(test_a_reset_the_firmware_asked_for_is_not_a_wedge)
{
	uint8_t intents[] = { WATCHDOG_INTENT_RESET, WATCHDOG_INTENT_FLASH,
	                      WATCHDOG_INTENT_ISP };
	size_t i;

	for (i = 0; i < sizeof(intents) / sizeof(intents[0]); i++) {
		boot_with(RSID_WDTR);
		crumb_left_by_a_run(MODE_SPECAN, WATCHDOG_SITE_FS_LOCK,
		                    intents[i], 4, 5);

		watchdog_boot();

		mu_assert_int_eq(4, watchdog_crumb.resets);
		mu_assert_int_eq(6, watchdog_crumb.boots);
		/* And it did not overwrite an earlier wedge's record. */
		mu_assert_int_eq(MODE_IDLE, watchdog_crumb.reset_mode);
	}
}

/* The reset pin, or the bootloader handing over: RAM survives, and no wedge
 * is recorded because no watchdog reset happened. */
MU_TEST(test_an_external_reset_keeps_the_counters_and_counts_no_wedge)
{
	boot_with(RSID_EXTR);
	crumb_left_by_a_run(MODE_SPECAN, WATCHDOG_SITE_FS_LOCK,
	                    WATCHDOG_INTENT_RUNNING, 6, 7);

	watchdog_boot();

	mu_assert_int_eq(6, watchdog_crumb.resets);
	mu_assert_int_eq(8, watchdog_crumb.boots);
	mu_assert_int_eq(RSID_EXTR, watchdog_crumb.reset_cause);
}

/*
 * The source bits are sticky: left alone they would say power-on for the rest
 * of the board's life, and every later boot would read as a power-on. Clearing
 * them is what makes the next boot's cause its own.
 */
MU_TEST(test_the_reset_source_is_cleared_for_the_next_boot)
{
	boot_with(RSID_POR | RSID_WDTR);

	watchdog_boot();

	mu_assert_int_eq((int)(RSID_POR | RSID_WDTR), (int)fake_rsid);
	/* The fake register keeps what was written; the part clears the bits
	 * written as ones, which is what this writes. */
	mu_assert_int_eq((int)(RSID_POR | RSID_WDTR),
	                 (int)watchdog_crumb.reset_cause);
}

/* The time-out flag outlives the reset it caused and is cleared by software
 * only; the on-chip programmer refuses to start while it is set. */
MU_TEST(test_the_time_out_flag_is_cleared)
{
	boot_with(RSID_WDTR);
	fake_wdmod = WDMOD_WDTOF;

	watchdog_boot();

	mu_assert_int_eq(0, (int)(fake_wdmod & WDMOD_WDTOF));
}

/* Neither counter wraps round to nothing on a board that has been resetting
 * itself for a long time. */
MU_TEST(test_the_counters_stop_at_their_largest_value)
{
	boot_with(RSID_WDTR);
	crumb_left_by_a_run(MODE_SPECAN, WATCHDOG_SITE_FS_LOCK,
	                    WATCHDOG_INTENT_RUNNING, 255, 255);

	watchdog_boot();

	mu_assert_int_eq(255, watchdog_crumb.resets);
	mu_assert_int_eq(255, watchdog_crumb.boots);
}

/* ---- the marks ---- */

/* A mark returns the site it replaced, which is how a wait that services USB
 * while it polls gets its own site back. */
MU_TEST(test_a_mark_returns_the_site_it_replaced)
{
	boot_with(RSID_POR);
	watchdog_boot();

	mu_assert_int_eq(WATCHDOG_SITE_NONE,
	                 watchdog_mark(WATCHDOG_SITE_FS_LOCK));
	mu_assert_int_eq(WATCHDOG_SITE_FS_LOCK,
	                 watchdog_mark(WATCHDOG_SITE_USB));
	mu_assert_int_eq(WATCHDOG_SITE_USB,
	                 watchdog_mark(WATCHDOG_SITE_FS_LOCK));
	mu_assert_int_eq(WATCHDOG_SITE_FS_LOCK, watchdog_crumb.live_site);
}

/* ---- the reply ---- */

/* The eight bytes, in the order ubertooth_interface.h gives them: the host
 * reads them as a byte string, so the order is the interface. */
MU_TEST(test_the_reply_is_the_documented_eight_bytes)
{
	uint8_t data[UBERTOOTH_WATCHDOG_INFO_LEN];

	boot_with(RSID_WDTR);
	crumb_left_by_a_run(MODE_BT_FOLLOW_LE, WATCHDOG_SITE_XOSC,
	                    WATCHDOG_INTENT_RUNNING, 1, 2);
	watchdog_boot();
	watchdog_start();
	watchdog_mode(MODE_SPECAN);
	watchdog_mark(WATCHDOG_SITE_FS_LOCK);

	memset(data, 0xee, sizeof(data));
	watchdog_report(data);

	mu_assert_int_eq(RSID_WDTR, data[0]);         /* reset cause */
	mu_assert_int_eq(1, data[1]);                 /* armed */
	mu_assert_int_eq(2, data[2]);                 /* resets */
	mu_assert_int_eq(3, data[3]);                 /* boots */
	mu_assert_int_eq(MODE_BT_FOLLOW_LE, data[4]); /* mode at the reset */
	mu_assert_int_eq(WATCHDOG_SITE_XOSC, data[5]);/* wait at the reset */
	mu_assert_int_eq(MODE_SPECAN, data[6]);       /* mode now */
	mu_assert_int_eq(WATCHDOG_SITE_FS_LOCK, data[7]); /* wait now */
}

/* A board that has not armed it yet says so, rather than claiming a guard it
 * does not have. */
MU_TEST(test_the_reply_says_when_the_watchdog_is_not_armed)
{
	uint8_t data[UBERTOOTH_WATCHDOG_INFO_LEN];

	boot_with(RSID_POR);
	watchdog_boot();
	watchdog_report(data);

	mu_assert_int_eq(0, data[1]);
}

/* ---- where the feed is called from ---- */

/* Does `path` contain a line with `needle` on it? Comments are stripped, so a
 * mention in a comment does not count as a call. */
static int source_has(const char *path, const char *needle, int *count)
{
	char line[512];
	int in_block = 0, found = 0;
	FILE *f = fopen(path, "r");

	if (count != NULL)
		*count = 0;
	if (f == NULL)
		return -1;
	while (fgets(line, sizeof(line), f) != NULL) {
		strip_comments(line, &in_block);
		if (strstr(line, needle) == NULL)
			continue;
		found = 1;
		if (count != NULL)
			(*count)++;
	}
	fclose(f);
	return found;
}

/*
 * Every place USB is serviced feeds the watchdog. "Serviced" is one thing in
 * this firmware: a call to the stack's USBHwISR(), which drives the controller
 * and answers the host. So each function that makes that call must feed, and
 * the check is written that way rather than naming today's three -- a fourth
 * service path added without a feed would have the watchdog reset a board that
 * is capturing happily.
 *
 * btbr is not in this list: it leaves the USB interrupt on for as long as it
 * runs and feeds from its main loop instead, which
 * test_every_interrupt_driven_mode_loop_feeds covers.
 */
MU_TEST(test_every_usb_service_path_feeds)
{
	static const char *const sources[] = {
		FW_RXTX_DIR "/ubertooth_usb.c",
		FW_COMMON_DIR "/usb_serial.c",
	};
	size_t i;
	int service_paths = 0;

	for (i = 0; i < sizeof(sources) / sizeof(sources[0]); i++) {
		char line[512], msg[384], name[128] = "";
		int in_block = 0, lineno = 0, name_line = 0;
		int services = 0, feeds = 0;
		FILE *f = fopen(sources[i], "r");

		snprintf(msg, sizeof(msg), "cannot read %s", sources[i]);
		mu_assert(f != NULL, msg);

		while (fgets(line, sizeof(line), f) != NULL) {
			lineno++;
			strip_comments(line, &in_block);
			/* A function body starts at column zero and ends with a
			 * closing brace there. */
			if (name[0] == '\0') {
				if (line[0] != ' ' && line[0] != '\t' &&
				    line[0] != '\n' && line[0] != '#' &&
				    strchr(line, '(') != NULL &&
				    strchr(line, ';') == NULL) {
					snprintf(name, sizeof(name), "%s", line);
					name_line = lineno;
					services = feeds = 0;
				}
				continue;
			}
			if (strstr(line, "USBHwISR();") != NULL)
				services++;
			if (strstr(line, "watchdog_usb_serviced();") != NULL)
				feeds++;
			if (line[0] != '}')
				continue;

			if (services > 0) {
				service_paths++;
				snprintf(msg, sizeof(msg),
				         "%s:%d: this function services USB and "
				         "does not feed the watchdog, so a board "
				         "answering the host through it would be "
				         "reset after %u ms", sources[i],
				         name_line,
				         (unsigned)UBERTOOTH_WATCHDOG_TIMEOUT_MS);
				mu_assert(feeds > 0, msg);
			} else {
				snprintf(msg, sizeof(msg),
				         "%s:%d: this function feeds the watchdog "
				         "without servicing USB, which is not what "
				         "the feed is supposed to mean",
				         sources[i], name_line);
				mu_assert(feeds == 0, msg);
			}
			name[0] = '\0';
		}
		fclose(f);
	}

	/* A floor, so the scan cannot pass by finding nothing: handle_usb(),
	 * handle_usb_control() and the USB interrupt handler. */
	mu_assert(service_paths >= 3, "fewer than three USB service paths found, "
	          "so the scan is not reading the sources");
}

/*
 * The modes that service USB from the interrupt instead of polling it. They
 * get no service pass of their own while the host is quiet -- xmas mode waits
 * for a timer interrupt and nothing else -- so without a feed in their loops
 * a board sitting in one would reset itself every two seconds.
 */
MU_TEST(test_every_interrupt_driven_mode_loop_feeds)
{
	static const char *const sources[] = {
		FW_RXTX_DIR "/bluetooth_rxtx.c", /* generic le, le sync, le slave */
		FW_RXTX_DIR "/ego.c",
		FW_RXTX_DIR "/xmas.c",
		FW_RXTX_DIR "/identify.c",
		FW_BTBR_DIR "/src/btctl/main.c",
	};
	size_t i;

	for (i = 0; i < sizeof(sources) / sizeof(sources[0]); i++) {
		char msg[320];

		snprintf(msg, sizeof(msg),
		         "%s services USB from the USB interrupt but its loop "
		         "never calls watchdog_usb_ready(), so a quiet host "
		         "would have the watchdog reset a working board",
		         sources[i]);
		mu_assert(source_has(sources[i], "watchdog_usb_ready();",
		                     NULL) == 1, msg);
	}
}

/*
 * How many of them there are. Three mode functions in bluetooth_rxtx.c enable
 * the USB interrupt for the duration of their mode; a fourth added without a
 * feed would be a mode that resets a working board when the host is quiet, so
 * the count of feeds is pinned to the count of those enables.
 */
MU_TEST(test_the_mode_loops_in_the_main_image_each_feed)
{
	const char *path = FW_RXTX_DIR "/bluetooth_rxtx.c";
	int feeds = 0, enables = 0;
	char msg[320];

	mu_assert(source_has(path, "watchdog_usb_ready();", &feeds) >= 0,
	          "cannot read bluetooth_rxtx.c");
	mu_assert(source_has(path, "ISER0 = ISER0_ISE_USB;", &enables) >= 0,
	          "cannot read bluetooth_rxtx.c");

	snprintf(msg, sizeof(msg),
	         "bluetooth_rxtx.c has %d modes that enable the USB interrupt "
	         "and %d loops that feed the watchdog; a mode that enables it "
	         "without feeding resets a working board when the host is "
	         "quiet", enables, feeds);
	mu_assert(enables > 0 && feeds == enables, msg);
}

/*
 * Nothing feeds it from a timer or a DMA handler. Such a feed would show only
 * that the CPU is running, which is the thing a board that answers nothing on
 * USB still does. The USB interrupt handler is the one handler that may feed,
 * because servicing USB is what it does.
 */
MU_TEST(test_no_interrupt_handler_but_the_usb_one_feeds)
{
	static const char *const sources[] = {
		FW_RXTX_DIR "/bluetooth_rxtx.c",
		FW_RXTX_DIR "/le_phy.c",
		FW_RXTX_DIR "/xmas.c",
		FW_RXTX_DIR "/identify.c",
		FW_COMMON_DIR "/usb_serial.c",
	};
	size_t i;

	for (i = 0; i < sizeof(sources) / sizeof(sources[0]); i++) {
		char line[512], msg[384], handler[64] = "";
		int in_block = 0, lineno = 0, scanned = 0;
		FILE *f = fopen(sources[i], "r");

		snprintf(msg, sizeof(msg), "cannot read %s", sources[i]);
		mu_assert(f != NULL, msg);

		while (fgets(line, sizeof(line), f) != NULL) {
			lineno++;
			strip_comments(line, &in_block);
			if (handler[0] == '\0') {
				const char *at = strstr(line, "_IRQHandler(");

				/* The handler's own definition starts at column
				 * zero, which is what separates it from a call
				 * or a declaration inside a function. */
				if (at != NULL && strstr(line, "void ") == line) {
					snprintf(handler, sizeof(handler), "%s",
					         line);
					scanned++;
				}
				continue;
			}
			if (line[0] == '}') {
				handler[0] = '\0';
				continue;
			}
			if (strstr(line, "watchdog_usb_") == NULL)
				continue;
			if (strstr(handler, "USB_IRQHandler") != NULL)
				continue;
			snprintf(msg, sizeof(msg),
			         "%s:%d: an interrupt handler feeds the "
			         "watchdog; a feed driven by anything but USB "
			         "service shows only that the CPU is running, "
			         "which a board that answers nothing on USB "
			         "still does", sources[i], lineno);
			mu_assert(0, msg);
		}
		fclose(f);
		snprintf(msg, sizeof(msg),
		         "%s: no interrupt handler was found, so this case "
		         "passed without reading one", sources[i]);
		mu_assert(scanned > 0, msg);
	}
}

int main(void)
{
	printf("firmware watchdog tests\n");
	MU_RUN(test_arming_sets_the_timeout_and_the_reset);
	MU_RUN(test_arming_feeds_once);
	MU_RUN(test_each_feed_is_the_sequence_the_part_wants);
	MU_RUN(test_the_feed_holds_the_usb_interrupt_off);
	MU_RUN(test_a_feed_before_arming_does_nothing);
	MU_RUN(test_stretching_sets_the_longest_timeout_and_feeds);
	MU_RUN(test_power_on_starts_the_counters_again);
	MU_RUN(test_brown_out_starts_the_counters_again);
	MU_RUN(test_an_unrecognised_breadcrumb_is_started_again);
	MU_RUN(test_a_wedge_is_counted_and_recorded);
	MU_RUN(test_a_reset_the_firmware_asked_for_is_not_a_wedge);
	MU_RUN(test_an_external_reset_keeps_the_counters_and_counts_no_wedge);
	MU_RUN(test_the_reset_source_is_cleared_for_the_next_boot);
	MU_RUN(test_the_time_out_flag_is_cleared);
	MU_RUN(test_the_counters_stop_at_their_largest_value);
	MU_RUN(test_a_mark_returns_the_site_it_replaced);
	MU_RUN(test_the_reply_is_the_documented_eight_bytes);
	MU_RUN(test_the_reply_says_when_the_watchdog_is_not_armed);
	MU_RUN(test_every_usb_service_path_feeds);
	MU_RUN(test_every_interrupt_driven_mode_loop_feeds);
	MU_RUN(test_the_mode_loops_in_the_main_image_each_feed);
	MU_RUN(test_no_interrupt_handler_but_the_usb_one_feeds);
	return mu_report();
}
