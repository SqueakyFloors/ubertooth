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
 *  - the reply the host reads is the bytes ubertooth_interface.h describes,
 *    in that order.
 *
 * The last cases scan the firmware sources as text, the way test_frend.c does,
 * to check the feed is called from the places USB is serviced and from the
 * loops of the modes that cannot prove more than that -- and from nowhere
 * driven by a timer, which would only show the CPU is running.
 */
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

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
uint32_t fake_scb_icsr;

uint32_t fake_wdfeed_writes[FAKE_WDFEED_MAX];
unsigned fake_wdfeed_count;

void fake_wdfeed_write(uint32_t value)
{
	if (fake_wdfeed_count < FAKE_WDFEED_MAX)
		fake_wdfeed_writes[fake_wdfeed_count] = value;
	fake_wdfeed_count++;
}

/* The stack high-water measurement the reply carries. On the board these scan
 * the pattern startup wrote over the unused stack (stack_check.h); here the
 * test says what they found. */
unsigned fake_stack_used;
unsigned fake_stack_size;

unsigned stack_used(void)
{
	return fake_stack_used;
}

unsigned stack_size(void)
{
	return fake_stack_size;
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
	/* Thread mode: no exception active. A test that is about a handler sets
	 * this itself. */
	fake_scb_icsr = 0;
	fake_wdfeed_count = 0;
	/* What a board read after a five-minute connection capture, so the
	 * reply test carries the numbers a real one would. */
	fake_stack_used = 432;
	fake_stack_size = 11128;
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

/*
 * And back. set_isp() is not meant to return, but if it ever does the board
 * must not spend the next hour with a watchdog too slow to reset it and an
 * intent that would excuse the next real wedge.
 */
MU_TEST(test_unstretching_puts_the_timeout_and_the_intent_back)
{
	boot_with(RSID_POR);
	watchdog_start();
	watchdog_intent(WATCHDOG_INTENT_ISP);
	watchdog_stretch();
	fake_wdfeed_count = 0;

	watchdog_unstretch();

	mu_assert_int_eq(UBERTOOTH_WATCHDOG_TIMEOUT_MS * 1000, (int)fake_wdtc);
	mu_assert_int_eq(2, (int)fake_wdfeed_count);
	mu_assert_int_eq(WATCHDOG_INTENT_RUNNING, watchdog_crumb.live_intent);
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
 * The on-chip programmer is the other way round: it never resets, so it says
 * so to keep the stretched timeout from being read as a wedge.
 */
MU_TEST(test_a_reset_the_firmware_asked_for_is_not_a_wedge)
{
	uint8_t intents[] = { WATCHDOG_INTENT_RESET, WATCHDOG_INTENT_ISP };
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

/*
 * An interrupt handler that marks itself has to leave without claiming the
 * outer site, which is what the reply reports. watchdog_mark() would leave the
 * handler named there after it had returned, so a reply written next would say
 * the board is in a handler it has left -- and the low-energy engine's byte
 * interrupt fires once per received byte, so that would be the usual reading
 * during a capture.
 */
MU_TEST(test_leaving_a_site_does_not_claim_it_as_the_outer_one)
{
	boot_with(RSID_POR);
	watchdog_boot();

	/* the mode's loop, then its USB service pass */
	(void)watchdog_mark(WATCHDOG_SITE_NONE);
	uint8_t usb = watchdog_mark(WATCHDOG_SITE_USB);

	/* an interrupt lands in the middle of it and returns */
	uint8_t inner = watchdog_mark_inner(WATCHDOG_SITE_LE_DMA);
	mu_assert_int_eq(WATCHDOG_SITE_LE_DMA, watchdog_crumb.live_site);
	watchdog_unmark(inner);

	mu_assert_int_eq(WATCHDOG_SITE_USB, watchdog_crumb.live_site);
	mu_assert_int_eq(WATCHDOG_SITE_NONE, watchdog_crumb.live_outer_site);

	/* and the service pass still leaves the way it always did */
	watchdog_mark(usb);
	mu_assert_int_eq(WATCHDOG_SITE_USB, watchdog_crumb.live_outer_site);
}

/*
 * The same for a wait reached from inside a handler, which is the other route
 * to the same wrong reading: the crystal-oscillator wait is reached from both
 * low-energy handlers, and a hop can land in the middle of a USB service pass.
 * A wait cannot tell from its own source which it is, so watchdog_mark_wait()
 * asks the processor: in a handler it records the site and leaves the outer one
 * alone.
 */
MU_TEST(test_a_wait_inside_a_handler_claims_no_outer_site)
{
	uint8_t site;

	boot_with(RSID_POR);
	watchdog_boot();

	/* the mode's loop, then its USB service pass */
	(void)watchdog_mark(WATCHDOG_SITE_NONE);
	(void)watchdog_mark(WATCHDOG_SITE_USB);

	/* an interrupt lands in the middle of the pass and waits on the radio */
	fake_scb_icsr = 40; /* the USB exception number, as the part reports it */
	site = watchdog_mark_wait(WATCHDOG_SITE_XOSC);
	mu_assert_int_eq(WATCHDOG_SITE_XOSC, watchdog_crumb.live_site);
	watchdog_leave_wait(site);
	fake_scb_icsr = 0;

	/* back in the service pass, and the reply still names the mode's loop */
	mu_assert_int_eq(WATCHDOG_SITE_USB, watchdog_crumb.live_site);
	mu_assert_int_eq(WATCHDOG_SITE_NONE, watchdog_crumb.live_outer_site);
}

/*
 * The USB interrupt is the exception to that, and has to stay one: for the six
 * modes that do not poll USB it is the only place a reply is written, and the
 * reply reads the outer site. Were it to leave that site alone like the
 * handlers above, those modes would report whatever the main loop's last
 * service pass left -- "servicing USB" -- for as long as the mode ran. So the
 * USB service path uses watchdog_mark() wherever it runs from.
 */
MU_TEST(test_the_usb_interrupt_still_names_what_it_interrupted)
{
	uint8_t site;

	boot_with(RSID_POR);
	watchdog_boot();

	/* a mode that services USB from the interrupt, waiting for a packet */
	watchdog_mode(MODE_BT_FOLLOW_LE);
	(void)watchdog_mark(WATCHDOG_SITE_LE_RX);

	fake_scb_icsr = 40;
	site = watchdog_mark(WATCHDOG_SITE_USB);
	/* the reply is written here, and has to name the mode's wait */
	mu_assert_int_eq(WATCHDOG_SITE_LE_RX, watchdog_crumb.live_outer_site);
	watchdog_mark(site);
	fake_scb_icsr = 0;
}


/* ---- the reply ---- */

/* Every byte, in the order ubertooth_interface.h gives them: the host reads
 * them as a byte string, so the order is the interface. */
MU_TEST(test_the_reply_is_the_documented_bytes)
{
	uint8_t data[UBERTOOTH_WATCHDOG_INFO_LEN];

	boot_with(RSID_WDTR);
	crumb_left_by_a_run(MODE_BT_FOLLOW_LE, WATCHDOG_SITE_XOSC,
	                    WATCHDOG_INTENT_RUNNING, 1, 2);
	watchdog_boot();
	watchdog_start();
	watchdog_mode(MODE_SPECAN);
	/* How the board really reaches watchdog_report(): the mode was in a
	 * wait, which called the USB service path, which marked itself. */
	watchdog_mark(WATCHDOG_SITE_FS_LOCK);
	watchdog_mark(WATCHDOG_SITE_USB);

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

	/* The wider fields, low byte first. Nothing in this run counted a
	 * transfer error or faulted, so all three read as zero -- which is
	 * what the host prints nothing for. */
	mu_assert_int_eq(0, data[8]);
	mu_assert_int_eq(0, data[9]);
	mu_assert_int_eq(0, data[10] | data[11] | data[12] | data[13]);
	mu_assert_int_eq(0, data[14] | data[15] | data[16] | data[17]);

	/* The stack high-water pair, also low byte first: 432 of 11128. */
	mu_assert_int_eq(0xb0, data[18]);
	mu_assert_int_eq(0x01, data[19]);
	mu_assert_int_eq(0x78, data[20]);
	mu_assert_int_eq(0x2b, data[21]);
}

/*
 * A capture transfer error is counted, reported low byte first, and kept
 * across a reset -- the count is in the breadcrumb for the same reason the
 * reset count is: a board that has been losing packets this way says so even
 * after it has been reset.
 */
/*
 * A breadcrumb this firmware does not recognise is started again, fields and
 * all. That is the case the magic exists for: a reflash leaves the region
 * alone, so a firmware that keeps more there than the one before it finds the
 * older layout with RAM otherwise intact. Reading its own fields out of that
 * is how a board reported a processor fault that never happened.
 */
MU_TEST(test_an_older_breadcrumb_layout_starts_every_field_again)
{
	uint8_t data[UBERTOOTH_WATCHDOG_INFO_LEN];

	/* Not a power-on: RAM holds what the firmware before this one left,
	 * under a magic this one does not know. */
	boot_with(RSID_WDTR);
	crumb_left_by_a_run(MODE_BT_FOLLOW_LE, WATCHDOG_SITE_XOSC,
	                    WATCHDOG_INTENT_RUNNING, 9, 37);
	watchdog_crumb.magic = 0x57443031; /* "WD01", the layout before this */
	watchdog_crumb.transfer_errors = 54756;
	watchdog_crumb.fault_status = 0x0c0911c8;
	watchdog_crumb.fault_addr = 0xa99832ae;

	watchdog_boot();
	watchdog_report(data);

	mu_assert_int_eq(0, data[2]);                 /* resets */
	mu_assert_int_eq(1, data[3]);                 /* this boot is the first */
	mu_assert_int_eq(0, data[8] | data[9]);       /* transfer errors */
	mu_assert_int_eq(0, data[10] | data[11] | data[12] | data[13]);
	mu_assert_int_eq(0, data[14] | data[15] | data[16] | data[17]);
}

MU_TEST(test_a_transfer_error_is_counted_and_kept_across_a_reset)
{
	uint8_t data[UBERTOOTH_WATCHDOG_INFO_LEN];
	int i;

	boot_with(RSID_POR);
	watchdog_boot();

	for (i = 0; i < 300; ++i)
		watchdog_count_transfer_error();

	watchdog_report(data);
	mu_assert_int_eq(300 & 0xff, data[8]);
	mu_assert_int_eq(300 >> 8, data[9]);

	/* A watchdog reset leaves RAM alone, so the count carries over. */
	boot_with(RSID_WDTR);
	watchdog_boot();
	watchdog_report(data);
	mu_assert_int_eq(300 & 0xff, data[8]);
	mu_assert_int_eq(300 >> 8, data[9]);

	/* Power-on says nothing in RAM can be trusted. */
	boot_with(RSID_POR);
	watchdog_boot();
	watchdog_report(data);
	mu_assert_int_eq(0, data[8]);
	mu_assert_int_eq(0, data[9]);
}

/* ...and it stops at the largest value the two bytes hold rather than
 * wrapping, as the reset count does. */
MU_TEST(test_the_transfer_error_count_stops_at_its_largest_value)
{
	uint8_t data[UBERTOOTH_WATCHDOG_INFO_LEN];

	boot_with(RSID_POR);
	watchdog_boot();
	watchdog_crumb.transfer_errors = 65534;

	watchdog_count_transfer_error();
	watchdog_count_transfer_error();
	watchdog_count_transfer_error();

	watchdog_report(data);
	mu_assert_int_eq(0xff, data[8]);
	mu_assert_int_eq(0xff, data[9]);
}

/*
 * A processor fault's status register and address reach the host.
 *
 * The four fault handlers never return -- they blink a code until the
 * watchdog resets the board -- so this record is all there is to go on
 * afterwards. Which of the four it was is the site; this says where it went
 * wrong, which is the difference between "a fault happened in this mode" and
 * an address to look for.
 */
MU_TEST(test_a_fault_records_its_status_and_address)
{
	uint8_t data[UBERTOOTH_WATCHDOG_INFO_LEN];

	boot_with(RSID_POR);
	watchdog_boot();

	/* A memory management fault whose address register was valid. */
	watchdog_fault(0x82, 0x4000c008);

	/* The board then stops feeding, and the watchdog resets it. */
	boot_with(RSID_WDTR);
	watchdog_boot();
	watchdog_report(data);

	mu_assert_int_eq(0x82, data[10]);
	mu_assert_int_eq(0x00, data[11]);
	mu_assert_int_eq(0x00, data[12]);
	mu_assert_int_eq(0x00, data[13]);
	mu_assert_int_eq(0x08, data[14]);
	mu_assert_int_eq(0xc0, data[15]);
	mu_assert_int_eq(0x00, data[16]);
	mu_assert_int_eq(0x40, data[17]);

	/* Power-on clears it, so an old fault is not reported against a board
	 * that has been power-cycled since. */
	boot_with(RSID_POR);
	watchdog_boot();
	watchdog_report(data);
	mu_assert_int_eq(0, data[10] | data[11] | data[12] | data[13]);
	mu_assert_int_eq(0, data[14] | data[15] | data[16] | data[17]);
}

/*
 * The reply never says the board is servicing USB, however it was reached: a
 * reply is written from inside the USB service path, so the site one level out
 * is the only one that says anything. An idle board really is in no wait, and
 * the man page's example shows that line.
 */
MU_TEST(test_the_reply_says_the_wait_the_mode_is_in_not_the_usb_path)
{
	uint8_t data[UBERTOOTH_WATCHDOG_INFO_LEN];

	boot_with(RSID_POR);
	watchdog_boot();
	watchdog_start();

	/* Idle: the main loop's service pass is the only mark. */
	watchdog_mark(WATCHDOG_SITE_USB);
	watchdog_report(data);
	mu_assert_int_eq(WATCHDOG_SITE_NONE, data[7]);

	/* A capture waiting for a packet, servicing USB from inside that wait. */
	watchdog_mark(WATCHDOG_SITE_NONE);
	watchdog_mark(WATCHDOG_SITE_LE_RX);
	watchdog_mark(WATCHDOG_SITE_USB);
	watchdog_report(data);
	mu_assert_int_eq(WATCHDOG_SITE_LE_RX, data[7]);
	mu_assert(data[7] != WATCHDOG_SITE_USB,
	          "the reply would say the board is servicing USB, which it "
	          "always is while answering, and so says nothing");
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
 * Walk every .c and .h file under `dir`, skipping the paths in `allowed`, and
 * report the first one with `needle` on a line of code. Its path is written to
 * `offender`, and `*scanned` counts the files read, so a walk that found
 * nothing cannot pass by accident. Returns 1 if one was found, 0 if none was,
 * -1 if the directory cannot be read.
 */
static int tree_has(const char *dir, const char *needle,
                    const char *const *allowed, size_t allowed_count,
                    char *offender, size_t offender_size, int *scanned)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	int found = 0;

	if (d == NULL)
		return -1;
	while (found == 0 && (e = readdir(d)) != NULL) {
		char path[512];
		const char *dot;
		size_t i;
		int skip = 0;
		struct stat st;

		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
			continue;
		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		if (stat(path, &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode)) {
			/* "shared" holds build output: the same sources again,
			 * plus listings that quote them. */
			if (strcmp(e->d_name, "shared") == 0)
				continue;
			found = tree_has(path, needle, allowed, allowed_count,
			                 offender, offender_size, scanned);
			if (found < 0)
				found = 0;
			continue;
		}
		dot = strrchr(e->d_name, '.');
		if (dot == NULL || (strcmp(dot, ".c") != 0 && strcmp(dot, ".h") != 0))
			continue;
		for (i = 0; i < allowed_count; i++) {
			if (strcmp(path, allowed[i]) == 0)
				skip = 1;
		}
		if (skip)
			continue;
		(*scanned)++;
		if (source_has(path, needle, NULL) == 1) {
			snprintf(offender, offender_size, "%s", path);
			found = 1;
		}
	}
	closedir(d);
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
 * btbr's driver is in the list too. Its main loop feeds, but both of its
 * service paths hold the USB interrupt off and poll the controller tens of
 * thousands of times with that loop not running, so each has to feed for
 * itself: otherwise a fault dump would be cut off part-way by a reset.
 */
MU_TEST(test_every_usb_service_path_feeds)
{
	static const char *const sources[] = {
		FW_RXTX_DIR "/ubertooth_usb.c",
		FW_COMMON_DIR "/usb_serial.c",
		FW_BTBR_DIR "/src/drivers/ubertooth_usb.c",
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
 * a board sitting in one would reset itself on every pass through it.
 *
 * What the feed has to be is the first thing a loop does. A call sitting
 * anywhere else in the file would satisfy "this file feeds" while feeding
 * once and never again, so the check is that the nearest line of code above
 * each call opens a loop. Comments and a lone brace in between are skipped;
 * anything else fails.
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
		char line[512], msg[448], above[512] = "";
		int in_block = 0, lineno = 0, above_line = 0, fed = 0;
		FILE *f = fopen(sources[i], "r");

		snprintf(msg, sizeof(msg), "cannot read %s", sources[i]);
		mu_assert(f != NULL, msg);

		while (fgets(line, sizeof(line), f) != NULL) {
			char *p;

			lineno++;
			strip_comments(line, &in_block);

			if (strstr(line, "watchdog_usb_ready();") != NULL) {
				fed++;
				snprintf(msg, sizeof(msg),
				         "%s:%d: the feed's nearest line of code above is "
				         "%s:%d, which opens no loop, so this mode feeds "
				         "the watchdog once instead of once per pass",
				         sources[i], lineno, sources[i], above_line);
				mu_assert(strstr(above, "while (") != NULL ||
				          strstr(above, "while(") != NULL ||
				          strstr(above, "for (") != NULL ||
				          strstr(above, "for(") != NULL ||
				          strstr(above, "do {") != NULL, msg);
				continue;
			}

			/* Remember the line above: skip blanks, what is left of a
			 * comment, and a line that is nothing but an opening brace. */
			for (p = line; *p == ' ' || *p == '\t'; p++)
				;
			if (*p == '\0' || *p == '\n' || strcmp(p, "{\n") == 0)
				continue;
			snprintf(above, sizeof(above), "%s", line);
			above_line = lineno;
		}
		fclose(f);

		snprintf(msg, sizeof(msg),
		         "%s services USB from the USB interrupt but never calls "
		         "watchdog_usb_ready(), so a quiet host would have the "
		         "watchdog reset a working board", sources[i]);
		mu_assert(fed > 0, msg);
	}
}

/*
 * Each of them, one at a time. A mode in bluetooth_rxtx.c takes USB off the
 * interrupt by enabling it on the way in and disabling it on the way out, so
 * the text between those two lines is that mode's own code. A mode that
 * enables the interrupt and never feeds resets a working board as soon as the
 * host goes quiet, so every such stretch has to contain a feed -- however
 * many feeds a mode turns out to need.
 */
MU_TEST(test_each_mode_that_enables_the_usb_interrupt_feeds)
{
	const char *path = FW_RXTX_DIR "/bluetooth_rxtx.c";
	char line[512], msg[384];
	int in_block = 0, lineno = 0, enabled_at = 0, feeds = 0, modes = 0;
	FILE *f = fopen(path, "r");

	mu_assert(f != NULL, "cannot read bluetooth_rxtx.c");

	while (fgets(line, sizeof(line), f) != NULL) {
		lineno++;
		strip_comments(line, &in_block);

		if (strstr(line, "ISER0 = ISER0_ISE_USB;") != NULL) {
			enabled_at = lineno;
			feeds = 0;
			modes++;
			continue;
		}
		if (enabled_at == 0)
			continue;
		if (strstr(line, "watchdog_usb_ready();") != NULL)
			feeds++;
		if (strstr(line, "ICER0 = ICER0_ICE_USB;") != NULL) {
			snprintf(msg, sizeof(msg),
			         "%s:%d: this mode runs USB off its interrupt "
			         "until line %d and never feeds the watchdog, so "
			         "it resets a working board once the host goes "
			         "quiet", path, enabled_at, lineno);
			mu_assert(feeds > 0, msg);
			enabled_at = 0;
		}
	}
	fclose(f);

	/* A mode left with the interrupt on at the end of the file is a mode
	 * whose stretch was never checked. */
	snprintf(msg, sizeof(msg),
	         "%s:%d: the USB interrupt is enabled here and never disabled, "
	         "so there is no end to this mode's code to look for a feed in",
	         path, enabled_at);
	mu_assert(enabled_at == 0, msg);

	snprintf(msg, sizeof(msg), "%s: no mode enables the USB interrupt, so "
	         "the scan is not reading the source", path);
	mu_assert(modes > 0, msg);
}

/*
 * The two BLE waits that have no deadline of their own: one waits for a
 * packet on a channel that may be silent, the other for the hop that ends a
 * connection interval, which BLE allows to run to 4 s. Both are longer than
 * the watchdog timeout, so both have to feed while they wait or a board
 * following a slow connection resets itself for working correctly.
 */
MU_TEST(test_the_open_ended_ble_waits_feed)
{
	const char *path = FW_RXTX_DIR "/bluetooth_rxtx.c";
	char line[512], msg[384];
	int in_block = 0, lineno = 0, waits = 0, fed = 0, wait_line = 0;
	FILE *f = fopen(path, "r");

	mu_assert(f != NULL, "cannot read bluetooth_rxtx.c");

	while (fgets(line, sizeof(line), f) != NULL) {
		lineno++;
		strip_comments(line, &in_block);

		if (strstr(line, "while ((rx_tc == 0) && (rx_err == 0)") != NULL) {
			waits++;
			wait_line = lineno;
			fed = 0;
			continue;
		}
		if (wait_line == 0)
			continue;
		if (strstr(line, "watchdog_usb_ready();") != NULL)
			fed = 1;
		/* The wait ends where the site is put back. */
		if (strstr(line, "watchdog_mark(site);") != NULL) {
			snprintf(msg, sizeof(msg),
			         "%s:%d: this wait for a BLE packet does not feed "
			         "the watchdog, so a quiet channel or a long "
			         "connection interval resets a working board",
			         path, wait_line);
			mu_assert(fed, msg);
			wait_line = 0;
		}
	}
	fclose(f);

	snprintf(msg, sizeof(msg),
	         "%s: expected the two open-ended BLE waits, found %d",
	         path, waits);
	mu_assert(waits == 2, msg);
}

/*
 * Nothing may preempt the USB handler. The watchdog's whole argument is that a
 * handler which stops returning stops the USB handler too, and that holds only
 * while nothing outranks USB: an interrupt able to preempt a stuck radio
 * handler would keep feeding through the wedge.
 *
 * The USB interrupt keeps the reset default priority of 0, the highest, so the
 * check is that no source raises another interrupt to meet it. Both images do
 * set exactly one priority -- dma_poweron() puts GPDMA at 31, the lowest -- so
 * those two lines are named here rather than banned, and the test insists they
 * stay at 31.
 *
 * Every other firmware source is scanned, rather than a list of the files that
 * enable an interrupt today: a priority raised in a file nobody thought to
 * name would otherwise pass, and this is the assumption the whole design rests
 * on.
 */
MU_TEST(test_nothing_is_raised_above_the_usb_interrupt)
{
	/* Each image's DMA driver, the only place a priority is set. */
	static const char *const lowest[] = {
		FW_RXTX_DIR "/ubertooth_dma.c",
		FW_BTBR_DIR "/src/drivers/ubertooth_dma.c",
	};
	/* Named with the DMA drivers because the walk looks for the register
	 * name: this is the header that gives the registers their names, and
	 * declaring one is not writing it. */
	static const char *const allowed[] = {
		FW_RXTX_DIR "/ubertooth_dma.c",
		FW_BTBR_DIR "/src/drivers/ubertooth_dma.c",
		FW_COMMON_DIR "/lpc17.h",
	};
	static const char *const trees[] = {
		FW_RXTX_DIR, FW_COMMON_DIR, FW_BTBR_DIR, FW_BOOTLOADER_DIR,
	};
	size_t i;
	int scanned = 0;

	for (i = 0; i < sizeof(trees) / sizeof(trees[0]); i++) {
		char msg[640];
		char offender[512] = "";
		int found = tree_has(trees[i], "IPR", allowed,
		                     sizeof(allowed) / sizeof(allowed[0]),
		                     offender, sizeof(offender), &scanned);

		snprintf(msg, sizeof(msg), "cannot walk %s", trees[i]);
		mu_assert(found >= 0, msg);

		snprintf(msg, sizeof(msg),
		         "%s writes an interrupt priority register. The watchdog "
		         "relies on nothing outranking the USB interrupt, so that "
		         "a stuck handler also stops the USB handler feeding. If "
		         "the new priority really is below USB's, add the file to "
		         "this test's list of files allowed to set the lowest",
		         offender);
		mu_assert(found == 0, msg);
	}

	/* A walk that found nothing would pass the check above vacuously. */
	mu_assert(scanned > 20, "the firmware source walk found almost no files");

	for (i = 0; i < sizeof(lowest) / sizeof(lowest[0]); i++) {
		char line[512], msg[384];
		int in_block = 0, writes = 0, at_lowest = 0;
		FILE *f = fopen(lowest[i], "r");

		snprintf(msg, sizeof(msg), "cannot read %s", lowest[i]);
		mu_assert(f != NULL, msg);

		while (fgets(line, sizeof(line), f) != NULL) {
			strip_comments(line, &in_block);
			if (strstr(line, "IPR") == NULL)
				continue;
			writes++;
			/* 31 is the lowest priority this part offers. */
			if (strstr(line, "31") != NULL ||
			    strstr(line, "IP_DMA") != NULL)
				at_lowest++;
		}
		fclose(f);

		snprintf(msg, sizeof(msg),
		         "%s: expected one interrupt priority write, found %d",
		         lowest[i], writes);
		mu_assert(writes == 1, msg);

		snprintf(msg, sizeof(msg),
		         "%s sets an interrupt priority that is not the lowest. "
		         "Only a priority below the USB interrupt's is safe: "
		         "anything that can preempt it would let the watchdog be "
		         "fed through the wedge it exists to catch",
		         lowest[i]);
		mu_assert(at_lowest == writes, msg);
	}
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

/*
 * A reset is excused only where it happens. Saying "this reset was asked for"
 * when the request merely arrives is wrong twice over: btbr queues its reset
 * request and a full queue drops it, and either image could stop answering
 * between the request and the reset. Both leave a board running with its next
 * real wedge excused, which is the one thing the breadcrumb exists to catch.
 *
 * So every function that claims a reset must reset in that same function. The
 * on-chip programmer is the exception the enum spells out: it never resets, so
 * it is not checked here.
 */
/*
 * The breadcrumb only works because of where it is linked, and the tests above
 * cannot see that: they build for the host, where watchdog_crumb is an ordinary
 * object they can poke. So the linker script is read as text. Move the symbol
 * into .data or .bss and every reset would wipe the record, with every test
 * above still passing.
 *
 * All five images share this script, which is why one read covers them all.
 */
MU_TEST(test_the_breadcrumb_is_linked_where_a_reset_cannot_clear_it)
{
	const char *path = FW_COMMON_DIR "/sections.ld";
	char line[512];
	int in_block = 0;
	int region = 0, crumb_at_start = 0, ctrl_above = 0, reserves_32 = 0;
	int data_after = 0, seen_region = 0;
	FILE *f = fopen(path, "r");

	mu_assert(f != NULL, "cannot read sections.ld");
	while (fgets(line, sizeof(line), f) != NULL) {
		strip_comments(line, &in_block);
		/* NOLOAD keeps it out of the image, so startup loads nothing over
		 * it; being its own output section keeps it out of .bss, which
		 * startup zeroes. */
		if (strstr(line, ".persistent") != NULL &&
		    strstr(line, "NOLOAD") != NULL &&
		    strstr(line, "ORIGIN(ram)") != NULL) {
			region = 1;
			seen_region = 1;
		}
		if (strstr(line, ". += 32") != NULL)
			reserves_32 = 1;
		if (strstr(line, "watchdog_crumb") != NULL &&
		    strstr(line, "_persistent_start") != NULL &&
		    strstr(line, "+") == NULL)
			crumb_at_start = 1;
		if (strstr(line, "bootloader_ctrl") != NULL &&
		    strstr(line, "0x1C") != NULL)
			ctrl_above = 1;
		/* .data starts after the region, so it cannot overlap it. */
		if (seen_region && strstr(line, ".data") != NULL)
			data_after = 1;
	}
	fclose(f);

	mu_assert(region == 1,
	          "sections.ld has no .persistent (NOLOAD) output section at the "
	          "bottom of RAM: the breadcrumb would be loaded or zeroed at "
	          "startup and a watchdog reset would wipe what it recorded");
	mu_assert(reserves_32 == 1,
	          "the .persistent region no longer reserves its 32 bytes");
	mu_assert(crumb_at_start == 1,
	          "watchdog_crumb is no longer placed at the start of .persistent");
	mu_assert(ctrl_above == 1,
	          "bootloader_ctrl is no longer at the top of .persistent, so it "
	          "and the breadcrumb may overlap");
	mu_assert(data_after == 1, "sections.ld places .data before .persistent");

	/* The struct has to fit below bootloader_ctrl; watchdog.c asserts that at
	 * compile time, and this says what the budget is for. */
	mu_assert(sizeof(watchdog_crumb_t) <= 0x1C,
	          "the breadcrumb no longer fits below bootloader_ctrl");
}

MU_TEST(test_a_claimed_reset_is_claimed_where_it_happens)
{
	static const char *const sources[] = {
		FW_RXTX_DIR "/bluetooth_rxtx.c",
		FW_BTBR_DIR "/src/btctl/btctl.c",
		FW_BTBR_DIR "/src/btctl/vendor_request_handler.c",
	};
	size_t i;
	int claims = 0;

	for (i = 0; i < sizeof(sources) / sizeof(sources[0]); i++) {
		char line[512], msg[384], name[128] = "";
		int in_block = 0, lineno = 0, name_line = 0;
		int claimed = 0, resets = 0;
		FILE *f = fopen(sources[i], "r");

		snprintf(msg, sizeof(msg), "cannot read %s", sources[i]);
		mu_assert(f != NULL, msg);

		while (fgets(line, sizeof(line), f) != NULL) {
			lineno++;
			strip_comments(line, &in_block);
			/* A function body starts at column zero and ends with a
			 * closing brace there, as in the scans above. */
			if (name[0] == '\0') {
				if (line[0] != ' ' && line[0] != '\t' &&
				    line[0] != '\n' && line[0] != '#' &&
				    strchr(line, '(') != NULL &&
				    strchr(line, ';') == NULL) {
					snprintf(name, sizeof(name), "%s", line);
					name_line = lineno;
					claimed = resets = 0;
				}
				continue;
			}
			if (strstr(line, "watchdog_intent(WATCHDOG_INTENT_RESET)")
			    != NULL)
				claimed++;
			if (strstr(line, "reset();") != NULL)
				resets++;
			if (line[0] != '}')
				continue;

			if (claimed > 0) {
				claims++;
				snprintf(msg, sizeof(msg),
				         "%s:%d: this function says a reset was "
				         "asked for but does not reset, so a "
				         "board that stops answering before the "
				         "reset arrives would not be counted as "
				         "a wedge", sources[i], name_line);
				mu_assert(resets > 0, msg);
			}
			name[0] = '\0';
		}
		fclose(f);
	}

	/* A floor, so the scan cannot pass by finding nothing: one claim in each
	 * image. */
	mu_assert(claims >= 2, "fewer than two reset claims found, so the scan "
	          "is not reading the sources");
}

/*
 * The fault paths say where they were before they stop.
 *
 * Those paths are the four processor fault handlers in both images and btbr's
 * die(). None of them returns and none feeds, so the watchdog resets the board
 * a few seconds later and the breadcrumb is what the host reads once it comes
 * back. A path that marks nothing leaves the record naming whatever the
 * firmware was doing before it -- "not in a wait" from the main loop, or
 * "servicing USB" for a fault inside the USB path -- so the fault reads as
 * something it was not.
 *
 * A site each rather than one shared "a fault" site: counting the handler's
 * blink code used to say which of the four it was, and the watchdog cuts that
 * code off part-way.
 *
 * Two more loops are here that are not faults: the shared Default_Handler,
 * which NMI, SVC, PendSV, SysTick and every unhandled peripheral interrupt
 * alias, and the debug UART's transfer error path. Both stop the board the
 * same way, so both need a site of their own.
 *
 * This is still a list rather than a sweep over every never-returning loop in
 * the two images: the mode loops and the main loops never return either, and
 * they are right not to mark anything, so a sweep needs a rule for telling
 * them apart.
 *
 * Nothing here can be run, so the sources are read as text.
 */
/*
 * Every processor fault handler records what the fault was, in both images.
 *
 * The site says which of the four handlers ran; this is the only record of
 * where it went wrong, because none of them returns and nothing else reads the
 * fault status registers. A handler that stopped recording it would leave
 * ubertooth-util -w saying a fault happened in some mode and nothing more,
 * which is where this started.
 */
MU_TEST(test_every_fault_handler_records_the_fault)
{
	static const struct {
		const char *path;
		const char *function;
	} handlers[] = {
		{ FW_RXTX_DIR "/fault.c", "HardFault_Handler" },
		{ FW_RXTX_DIR "/fault.c", "MemManagement_Handler" },
		{ FW_RXTX_DIR "/fault.c", "BusFault_Handler" },
		{ FW_RXTX_DIR "/fault.c", "UsageFault_Handler" },
		{ FW_BTBR_DIR "/src/util/fault.c", "HardFault_Handler" },
		{ FW_BTBR_DIR "/src/util/fault.c", "MemManagement_Handler" },
		{ FW_BTBR_DIR "/src/util/fault.c", "BusFault_Handler" },
		{ FW_BTBR_DIR "/src/util/fault.c", "UsageFault_Handler" },
	};
	size_t i;

	for (i = 0; i < sizeof(handlers) / sizeof(handlers[0]); i++) {
		char line[512], msg[512];
		int in_block = 0, in_function = 0, found = 0, records = 0;
		FILE *f = fopen(handlers[i].path, "r");

		snprintf(msg, sizeof(msg), "cannot read %s", handlers[i].path);
		mu_assert(f != NULL, msg);

		while (fgets(line, sizeof(line), f) != NULL) {
			strip_comments(line, &in_block);
			if (!in_function) {
				if (line[0] == ' ' || line[0] == '\t' ||
				    line[0] == '#' || strchr(line, ';') != NULL)
					continue;
				if (!calls_identifier(line, handlers[i].function))
					continue;
				in_function = found = 1;
				continue;
			}
			if (strstr(line, "watchdog_fault(") != NULL)
				records = 1;
			if (line[0] == '}')
				in_function = 0;
		}
		fclose(f);

		snprintf(msg, sizeof(msg),
		         "%s: there is no %s(), so this case read nothing",
		         handlers[i].path, handlers[i].function);
		mu_assert(found, msg);
		snprintf(msg, sizeof(msg),
		         "%s: %s() does not record the fault, so -w says which "
		         "fault happened and nothing about where",
		         handlers[i].path, handlers[i].function);
		mu_assert(records, msg);
	}
}

MU_TEST(test_every_path_that_never_returns_marks_where_it_was)
{
	static const struct {
		const char *path;
		const char *function;
		const char *site;
	} paths[] = {
		{ FW_RXTX_DIR "/fault.c", "HardFault_Handler",
		  "WATCHDOG_SITE_FAULT_HARD" },
		{ FW_RXTX_DIR "/fault.c", "MemManagement_Handler",
		  "WATCHDOG_SITE_FAULT_MEM" },
		{ FW_RXTX_DIR "/fault.c", "BusFault_Handler",
		  "WATCHDOG_SITE_FAULT_BUS" },
		{ FW_RXTX_DIR "/fault.c", "UsageFault_Handler",
		  "WATCHDOG_SITE_FAULT_USAGE" },
		{ FW_BTBR_DIR "/src/util/fault.c", "HardFault_Handler",
		  "WATCHDOG_SITE_FAULT_HARD" },
		{ FW_BTBR_DIR "/src/util/fault.c", "MemManagement_Handler",
		  "WATCHDOG_SITE_FAULT_MEM" },
		{ FW_BTBR_DIR "/src/util/fault.c", "BusFault_Handler",
		  "WATCHDOG_SITE_FAULT_BUS" },
		{ FW_BTBR_DIR "/src/util/fault.c", "UsageFault_Handler",
		  "WATCHDOG_SITE_FAULT_USAGE" },
		{ FW_BTBR_DIR "/src/util/fault.c", "die",
		  "WATCHDOG_SITE_PANIC" },
		{ FW_COMMON_DIR "/LPC17xx_Interrupts.c", "Default_Handler",
		  "WATCHDOG_SITE_UNHANDLED_IRQ" },
		{ FW_RXTX_DIR "/bluetooth_rxtx.c", "DMA_IRQHandler",
		  "WATCHDOG_SITE_DEBUG_DMA" },
	};
	size_t i;

	for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		char line[512], msg[512], mark[128];
		int in_block = 0, in_function = 0, found = 0, marked = 0;
		FILE *f = fopen(paths[i].path, "r");

		snprintf(msg, sizeof(msg), "cannot read %s", paths[i].path);
		mu_assert(f != NULL, msg);
		snprintf(mark, sizeof(mark), "watchdog_mark(%s);", paths[i].site);

		while (fgets(line, sizeof(line), f) != NULL) {
			strip_comments(line, &in_block);
			if (!in_function) {
				/* A definition starts at column zero and is not
				 * a declaration, as in the scans above. */
				if (line[0] == ' ' || line[0] == '\t' ||
				    line[0] == '#' || strchr(line, ';') != NULL)
					continue;
				if (!calls_identifier(line, paths[i].function))
					continue;
				in_function = found = 1;
				continue;
			}
			if (strstr(line, mark) != NULL)
				marked = 1;
			if (line[0] == '}')
				in_function = 0;
		}
		fclose(f);

		snprintf(msg, sizeof(msg),
		         "%s: there is no %s(), so this case read nothing",
		         paths[i].path, paths[i].function);
		mu_assert(found, msg);
		snprintf(msg, sizeof(msg),
		         "%s: %s() never returns and does not mark %s, so "
		         "ubertooth-util -w names whatever the board was doing "
		         "before it got there instead",
		         paths[i].path, paths[i].function, paths[i].site);
		mu_assert(marked, msg);
	}
}

/*
 * An inner mark has to be given back, because the code around it is still in
 * whatever wait it was in: leaving the inner site behind would make -w name a
 * wait the board has left. Two kinds of code make one -- an interrupt
 * handler, which borrows the processor, and a step of a USB service pass,
 * which is inside a site the pass has already marked -- and both have to
 * unmark as often as they mark.
 *
 * The counts are pinned so that a new mark has to be thought about rather
 * than slipped in: add one and this test says where.
 */
MU_TEST(test_every_inner_mark_is_given_back)
{
	static const struct { const char *path; int marks; const char *what; } files[] = {
		{ FW_RXTX_DIR "/le_phy.c", 3,
		  "the low-energy engine's interrupt handlers" },
		{ FW_RXTX_DIR "/ubertooth_usb.c", 1,
		  "handing a capture packet to the bulk endpoint" },
		{ FW_COMMON_DIR "/lpcusb/target/usbhw_lpc.c", 4,
		  "the device event, the two endpoint status reads and the "
		  "wait for an endpoint to be realized" },
		{ FW_COMMON_DIR "/lpcusb/target/usbcontrol.c", 3,
		  "reading a control request, running it, writing the reply" },
	};
	char msg[256];
	size_t i;

	for (i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
		int marks = 0, unmarks = 0;

		mu_assert_int_eq(1, source_has(files[i].path,
		                               "watchdog_mark_inner(", &marks));
		mu_assert_int_eq(1, source_has(files[i].path,
		                               "watchdog_unmark(", &unmarks));
		snprintf(msg, sizeof(msg),
		         "%s marks %s: expected %d of them, found %d",
		         files[i].path, files[i].what, files[i].marks, marks);
		mu_assert(marks == files[i].marks, msg);
		snprintf(msg, sizeof(msg),
		         "%s marks %d inner sites and gives back %d, so -w can name "
		         "a site the board has left",
		         files[i].path, marks, unmarks);
		mu_assert(unmarks == marks, msg);
	}

	/* The engine's handlers must not use the wait's mark, which would claim
	 * the outer site -- the one thing a handler's site must never be. The
	 * USB files are not in this check: the pass itself marks that way, which
	 * is how the outer site comes to name the mode's own wait. */
	mu_assert_int_eq(0, source_has(FW_RXTX_DIR "/le_phy.c",
	                               "watchdog_mark(WATCHDOG_SITE_", NULL));
}

/*
 * Which pair each kind of caller uses, in the sources.
 *
 * The distinction is not visible at the call: a wait reached from a handler and
 * a wait reached from a mode loop are the same lines of code, which is why
 * watchdog_mark_wait() asks the processor. A wait left on watchdog_mark() would
 * claim the outer site from inside a handler; a USB service path moved to
 * watchdog_mark_wait() would stop naming what it interrupted, and the six modes
 * that answer the host from the USB interrupt would report "servicing USB" for
 * as long as they ran.
 */
MU_TEST(test_each_caller_uses_the_pair_for_what_it_is)
{
	static const struct {
		const char *path;
		const char *wanted;
		const char *unwanted;
	} callers[] = {
		/* the bounded waits */
		{ FW_COMMON_DIR "/cc2400_wait.c", "watchdog_mark_wait(",
		  "watchdog_mark(" },
		{ FW_BTBR_DIR "/include/ubtbr/rf.h", "watchdog_mark_wait(",
		  "watchdog_mark(" },
		/* the USB service paths, polled and interrupt-driven */
		{ FW_COMMON_DIR "/usb_serial.c", "watchdog_mark(",
		  "watchdog_mark_wait(" },
		{ FW_RXTX_DIR "/ubertooth_usb.c", "watchdog_mark(",
		  "watchdog_mark_wait(" },
		{ FW_BTBR_DIR "/src/drivers/ubertooth_usb.c", "watchdog_mark(",
		  "watchdog_mark_wait(" },
	};
	size_t i;

	for (i = 0; i < sizeof(callers) / sizeof(callers[0]); i++) {
		int found = 0, wrong = 0;
		char msg[512];

		mu_assert_int_eq(1, source_has(callers[i].path,
		                               callers[i].wanted, &found));
		snprintf(msg, sizeof(msg), "%s: no %s", callers[i].path,
		         callers[i].wanted);
		mu_assert(found > 0, msg);

		(void)source_has(callers[i].path, callers[i].unwanted, &wrong);
		snprintf(msg, sizeof(msg), "%s: %s where it should not be",
		         callers[i].path, callers[i].unwanted);
		mu_assert(wrong == 0, msg);
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
	MU_RUN(test_unstretching_puts_the_timeout_and_the_intent_back);
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
	MU_RUN(test_leaving_a_site_does_not_claim_it_as_the_outer_one);
	MU_RUN(test_a_wait_inside_a_handler_claims_no_outer_site);
	MU_RUN(test_the_usb_interrupt_still_names_what_it_interrupted);
	MU_RUN(test_every_inner_mark_is_given_back);
	MU_RUN(test_the_reply_is_the_documented_bytes);
	MU_RUN(test_an_older_breadcrumb_layout_starts_every_field_again);
	MU_RUN(test_a_transfer_error_is_counted_and_kept_across_a_reset);
	MU_RUN(test_the_transfer_error_count_stops_at_its_largest_value);
	MU_RUN(test_a_fault_records_its_status_and_address);
	MU_RUN(test_the_reply_says_the_wait_the_mode_is_in_not_the_usb_path);
	MU_RUN(test_the_reply_says_when_the_watchdog_is_not_armed);
	MU_RUN(test_every_usb_service_path_feeds);
	MU_RUN(test_every_interrupt_driven_mode_loop_feeds);
	MU_RUN(test_each_mode_that_enables_the_usb_interrupt_feeds);
	MU_RUN(test_the_open_ended_ble_waits_feed);
	MU_RUN(test_nothing_is_raised_above_the_usb_interrupt);
	MU_RUN(test_no_interrupt_handler_but_the_usb_one_feeds);
	MU_RUN(test_the_breadcrumb_is_linked_where_a_reset_cannot_clear_it);
	MU_RUN(test_a_claimed_reset_is_claimed_where_it_happens);
	MU_RUN(test_every_path_that_never_returns_marks_where_it_was);
	MU_RUN(test_every_fault_handler_records_the_fault);
	MU_RUN(test_each_caller_uses_the_pair_for_what_it_is);
	return mu_report();
}
