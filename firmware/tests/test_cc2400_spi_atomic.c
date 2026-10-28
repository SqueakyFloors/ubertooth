/*
 * Tests that every CC2400 transfer -- registers and the FIFO -- runs with
 * interrupts masked.
 *
 * The CC2400's configuration bus is bit-banged over three GPIO lines, and
 * cc2400_spi() in firmware/common/ubertooth.c is the only code that drives
 * them for a register transfer. Both the main loop and several interrupt
 * handlers reach the radio through it -- the LE PHY's DMA handler reads RSSI
 * and strobes the synthesizer, its timer handler retunes, and in the modes
 * that service USB from the USB interrupt the vendor request handler writes
 * radio registers. A handler that ran part-way through a transfer would start
 * its own with CSN already low, so both transfers would read or write a
 * corrupt word, or lose a strobe. The board then misreads registers, or goes
 * quiet because the main loop is waiting for a radio state the lost strobe
 * never produced.
 *
 * cc2400_spi() cannot run here: it is GPIO register writes, and the file it
 * lives in also configures the clock and the phase-locked loops. So this test
 * scans the source as text, the way test_mode_dispatch.c and
 * test_le_phy_order.c scan theirs, and checks the shape the fix depends on.
 */
#include <stdlib.h>

#include "minunit.h"
#include "source_scan.h"

#define UBERTOOTH_C FW_COMMON_DIR "/ubertooth.c"

/*
 * The body of one top-level function, comments stripped, as a single string
 * with the newlines kept. Ordered checks ("the mask goes on before CSN drops")
 * are then index comparisons on that string.
 */
static char *read_function(const char *path, const char *name)
{
	char line[512], found[128];
	char *body = NULL;
	size_t used = 0, size = 0;
	int in_block = 0, depth = 0, started = 0;
	FILE *f = fopen(path, "r");

	if (f == NULL)
		return NULL;

	while (fgets(line, sizeof(line), f) != NULL) {
		const char *p;

		strip_comments(line, &in_block);

		/* The definition's opening line: a call-shaped line at the top
		 * level naming the function we want. */
		if (!started && depth == 0 && call_name(line, found, sizeof(found)) &&
		    strcmp(found, name) == 0)
			started = 1;

		if (started) {
			size_t need = used + strlen(line) + 1;

			if (need > size) {
				char *grown = realloc(body, need * 2);

				if (grown == NULL) {
					free(body);
					fclose(f);
					return NULL;
				}
				size = need * 2;
				body = grown;
			}
			memcpy(body + used, line, strlen(line));
			used += strlen(line);
			body[used] = '\0';
		}

		for (p = line; *p != '\0'; p++) {
			if (*p == '{')
				depth++;
			else if (*p == '}')
				depth--;
		}

		/* Back to the top level after the body started: that was the
		 * closing brace. */
		if (started && depth == 0 && strchr(line, '}') != NULL)
			break;
	}

	fclose(f);
	return body;
}

/* Offset of `needle` in `text`, or -1. */
static int offset_of(const char *text, const char *needle)
{
	const char *p = strstr(text, needle);

	return p == NULL ? -1 : (int)(p - text);
}

/* ---- the transfer itself ---- */

/*
 * The mask has to go on before CSN drops and come off after CSN rises, so the
 * whole time the bus is held belongs to one transfer. A guard that started
 * after CSN dropped would leave the opening of every transfer exposed, which
 * is the window a handler is most likely to land in.
 */
MU_TEST(test_transfer_is_masked_from_before_csn_to_after_csn)
{
	char *body = read_function(UBERTOOTH_C, "cc2400_spi");
	int disable, csn_clr, csn_set, restore;

	mu_assert_fatal(body != NULL, "cannot read cc2400_spi() from the firmware source");

	disable = offset_of(body, "irq_save_disable()");
	csn_clr = offset_of(body, "CSN_CLR");
	csn_set = offset_of(body, "CSN_SET");
	restore = offset_of(body, "irq_restore(");

	/* Fatal, because the order checks below compare offsets: a missing
	 * needle is -1, which would satisfy them and report nothing. */
	mu_assert_fatal(disable >= 0, "cc2400_spi() does not mask interrupts");
	mu_assert_fatal(restore >= 0, "cc2400_spi() does not restore the interrupt mask");
	mu_assert_fatal(csn_clr >= 0 && csn_set >= 0,
	                "cc2400_spi() no longer drives CSN; this test needs rewriting");
	mu_assert(disable < csn_clr,
	          "cc2400_spi() drops CSN before masking interrupts");
	mu_assert(csn_set < restore,
	          "cc2400_spi() restores the interrupt mask before raising CSN");
	free(body);
}

/* One transfer per call: a second CSN_CLR would mean a second transaction
 * sharing the one guard, and a scan that found the first pair would say
 * nothing about the second. */
MU_TEST(test_transfer_drives_csn_exactly_once)
{
	char *body = read_function(UBERTOOTH_C, "cc2400_spi");
	const char *p;
	int clears = 0, sets = 0;

	mu_assert_fatal(body != NULL, "cannot read cc2400_spi() from the firmware source");

	for (p = body; (p = strstr(p, "CSN_CLR")) != NULL; p += 7)
		clears++;
	for (p = body; (p = strstr(p, "CSN_SET")) != NULL; p += 7)
		sets++;

	mu_assert_int_eq(1, clears);
	mu_assert_int_eq(1, sets);
	free(body);
}

/*
 * The mask that goes back is the one that was in force, not a fixed "on".
 * Interrupt handlers call cc2400_spi() too, and a handler that returned with
 * interrupts enabled would let another handler run inside it.
 */
MU_TEST(test_restore_puts_back_the_saved_mask)
{
	char *body = read_function(UBERTOOTH_C, "cc2400_spi");
	char flat[4096];

	mu_assert_fatal(body != NULL, "cannot read cc2400_spi() from the firmware source");

	collapse_spaces(body, flat, sizeof(flat));
	mu_assert(strstr(flat, "primask = irq_save_disable()") != NULL,
	          "cc2400_spi() does not keep what irq_save_disable() returned");
	mu_assert(strstr(flat, "irq_restore(primask)") != NULL,
	          "cc2400_spi() does not restore the mask irq_save_disable() returned");
	free(body);
}

/* No way out with interrupts still off. A `return` between the two would
 * leave the board with every interrupt masked permanently. */
MU_TEST(test_no_return_while_masked)
{
	char *body = read_function(UBERTOOTH_C, "cc2400_spi");
	int disable, restore, ret;
	char *p;

	mu_assert_fatal(body != NULL, "cannot read cc2400_spi() from the firmware source");

	disable = offset_of(body, "irq_save_disable()");
	restore = offset_of(body, "irq_restore(");
	mu_assert_fatal(disable >= 0 && restore > disable, "the guard is not in place");

	for (p = body + disable; (p = strstr(p, "return")) != NULL; p += 6) {
		ret = (int)(p - body);
		if (ret > restore)
			break;
		mu_assert(0, "cc2400_spi() returns with interrupts still masked");
	}
	free(body);
}

/*
 * Nothing slow inside the guard. The transfer is 1.6 to 4.6 microseconds; a
 * delay loop in there would push it past the clkn timer interrupt's 312.5
 * microsecond period and cost the capture timestamps a tick.
 */
MU_TEST(test_nothing_slow_inside_the_guard)
{
	char *body = read_function(UBERTOOTH_C, "cc2400_spi");
	static const char *slow[] = { "wait_us", "wait_ms", "spi_delay", "handle_usb" };
	int disable, restore;
	unsigned i;

	mu_assert_fatal(body != NULL, "cannot read cc2400_spi() from the firmware source");

	disable = offset_of(body, "irq_save_disable()");
	restore = offset_of(body, "irq_restore(");
	mu_assert_fatal(disable >= 0 && restore > disable, "the guard is not in place");

	body[restore] = '\0';
	for (i = 0; i < sizeof(slow) / sizeof(slow[0]); i++) {
		char msg[160];

		snprintf(msg, sizeof(msg),
		         "cc2400_spi() calls %s() with interrupts masked", slow[i]);
		mu_assert(!contains_identifier(body + disable, slow[i]), msg);
	}
	free(body);
}

/* ---- the two FIFO transfers ---- */

/*
 * The FIFO transfers bit-bang the same three lines themselves instead of going
 * through cc2400_spi(), so each needs a guard of its own, and it has to cover
 * the whole time CSN is low for the same reason: a handler that began its own
 * transfer part-way through would corrupt both.
 *
 * They are checked by the same scan and for the same shape as cc2400_spi(),
 * with one difference: these two do call spi_delay() inside the guard. They
 * have to -- the datasheet's hold before CSN rises is part of the transfer,
 * and the read's bit timing is made of those delays -- which is why they mask
 * for tens to hundreds of microseconds rather than a couple, and why the read
 * costs the Bluetooth clock a tick. The comment above them in the firmware
 * says which modes pay that.
 */
MU_TEST(test_the_fifo_transfers_are_masked_from_before_csn_to_after_csn)
{
	static const char *fifo[] = { "cc2400_fifo_write", "cc2400_fifo_read" };
	unsigned i;

	for (i = 0; i < sizeof(fifo) / sizeof(fifo[0]); i++) {
		char *body = read_function(UBERTOOTH_C, fifo[i]);
		char flat[8192];
		char msg[200];
		int disable, csn_clr, csn_set, restore, ret;
		const char *p;
		int clears = 0, sets = 0;

		snprintf(msg, sizeof(msg), "cannot read %s() from the firmware source",
		         fifo[i]);
		mu_assert_fatal(body != NULL, msg);

		disable = offset_of(body, "irq_save_disable()");
		csn_clr = offset_of(body, "CSN_CLR");
		csn_set = offset_of(body, "CSN_SET");
		restore = offset_of(body, "irq_restore(");

		snprintf(msg, sizeof(msg), "%s() does not mask interrupts, so an "
		         "interrupt handler that drives the radio can start its own "
		         "transfer inside this one", fifo[i]);
		mu_assert_fatal(disable >= 0, msg);
		snprintf(msg, sizeof(msg), "%s() does not restore the interrupt mask",
		         fifo[i]);
		mu_assert_fatal(restore >= 0, msg);
		snprintf(msg, sizeof(msg), "%s() no longer drives CSN; this test needs "
		         "rewriting", fifo[i]);
		mu_assert_fatal(csn_clr >= 0 && csn_set >= 0, msg);

		snprintf(msg, sizeof(msg), "%s() drops CSN before masking interrupts, "
		         "so the opening of every transfer is exposed", fifo[i]);
		mu_assert(disable < csn_clr, msg);
		snprintf(msg, sizeof(msg), "%s() restores the interrupt mask before "
		         "raising CSN", fifo[i]);
		mu_assert(csn_set < restore, msg);

		/* One transfer per call, as in cc2400_spi(): a second CSN pair
		 * would share the one guard and this scan would say nothing
		 * about it. */
		for (p = body; (p = strstr(p, "CSN_CLR")) != NULL; p += 7)
			clears++;
		for (p = body; (p = strstr(p, "CSN_SET")) != NULL; p += 7)
			sets++;
		snprintf(msg, sizeof(msg), "%s() drives CSN more than once", fifo[i]);
		mu_assert(clears == 1 && sets == 1, msg);

		/* The mask that goes back is the one that was in force, so the
		 * pair nests and is safe inside a handler. */
		collapse_spaces(body, flat, sizeof(flat));
		snprintf(msg, sizeof(msg), "%s() does not keep what "
		         "irq_save_disable() returned", fifo[i]);
		mu_assert(strstr(flat, "primask = irq_save_disable()") != NULL, msg);
		snprintf(msg, sizeof(msg), "%s() does not restore the mask "
		         "irq_save_disable() returned", fifo[i]);
		mu_assert(strstr(flat, "irq_restore(primask)") != NULL, msg);

		/* No way out with interrupts still off. */
		for (p = body + disable; (p = strstr(p, "return")) != NULL; p += 6) {
			ret = (int)(p - body);
			if (ret > restore)
				break;
			snprintf(msg, sizeof(msg), "%s() returns with interrupts still "
			         "masked", fifo[i]);
			mu_assert(0, msg);
		}
		free(body);
	}
}

/* ---- the one guard has to cover every register access ---- */

/*
 * Every register accessor reaches the bus through cc2400_spi() and drives no
 * bus line itself, so masking inside cc2400_spi() covers all of them. An
 * accessor that bit-banged its own lines would need its own guard.
 */
MU_TEST(test_accessors_go_through_cc2400_spi)
{
	static const char *accessors[] = {
		"cc2400_get", "cc2400_set", "cc2400_get8", "cc2400_set8",
		"cc2400_status", "cc2400_strobe",
	};
	static const char *bus_lines[] = { "CSN_CLR", "CSN_SET", "SCLK_SET",
	                                   "SCLK_CLR", "MOSI_SET", "MOSI_CLR" };
	unsigned i, j;

	for (i = 0; i < sizeof(accessors) / sizeof(accessors[0]); i++) {
		char *body = read_function(UBERTOOTH_C, accessors[i]);
		char msg[160];

		snprintf(msg, sizeof(msg), "cannot read %s() from the firmware source",
		         accessors[i]);
		mu_assert(body != NULL, msg);
		if (body == NULL)
			continue;

		snprintf(msg, sizeof(msg), "%s() does not go through cc2400_spi()",
		         accessors[i]);
		mu_assert(contains_identifier(body, "cc2400_spi"), msg);

		for (j = 0; j < sizeof(bus_lines) / sizeof(bus_lines[0]); j++) {
			snprintf(msg, sizeof(msg), "%s() drives %s itself, so it needs its "
			         "own interrupt guard", accessors[i], bus_lines[j]);
			mu_assert(strstr(body, bus_lines[j]) == NULL, msg);
		}
		free(body);
	}
}

/*
 * Which functions in firmware/common/ubertooth.c drive the bus at all, found
 * by looking for the five macros that clock a transfer: CSN_CLR and the SCLK
 * and MOSI pairs. CSN_SET on its own is not one of them, because cc2400_init()
 * raises CSN once to idle the bus without making a transfer.
 *
 * All three are guarded, and the tests above check each one's guard. Pinning
 * the list here means a new bus function in this file has to have that
 * decision made for it.
 *
 * It covers this file only. The btbr image has its own bit-banged FIFO write
 * in firmware/btbr/src/btphy/rf.c, which is unguarded: there it runs from the
 * slot scheduler inside the timer interrupt, and the only handler in that
 * image with a priority of its own -- GPDMA -- has the lowest, so nothing can
 * preempt it.
 */
MU_TEST(test_the_list_of_bus_functions_is_pinned)
{
	static const char *expected[] = { "cc2400_spi", "cc2400_fifo_write",
	                                  "cc2400_fifo_read" };
	static const char *bus_macros[] = { "CSN_CLR", "SCLK_SET", "SCLK_CLR",
	                                    "MOSI_SET", "MOSI_CLR" };
	/* Names seen driving the bus, each recorded once. */
	char seen[8][128];
	unsigned seen_count = 0;
	char line[512], name[128], last[128] = "";
	int in_block = 0, depth = 0;
	unsigned i, j;
	FILE *f = fopen(UBERTOOTH_C, "r");

	mu_assert_fatal(f != NULL, "cannot open the firmware source to scan");

	while (fgets(line, sizeof(line), f) != NULL) {
		const char *p;
		int drives = 0;

		strip_comments(line, &in_block);
		if (depth == 0 && call_name(line, name, sizeof(name)))
			snprintf(last, sizeof(last), "%s", name);

		for (i = 0; i < sizeof(bus_macros) / sizeof(bus_macros[0]); i++)
			if (strstr(line, bus_macros[i]) != NULL)
				drives = 1;

		if (drives) {
			int already = 0;

			for (i = 0; i < seen_count; i++)
				if (strcmp(seen[i], last) == 0)
					already = 1;
			if (!already && seen_count < sizeof(seen) / sizeof(seen[0]))
				snprintf(seen[seen_count++], sizeof(seen[0]), "%s", last);
		}

		for (p = line; *p != '\0'; p++) {
			if (*p == '{')
				depth++;
			else if (*p == '}')
				depth--;
		}
	}
	fclose(f);

	for (i = 0; i < seen_count; i++) {
		char msg[256];
		int known = 0;

		for (j = 0; j < sizeof(expected) / sizeof(expected[0]); j++)
			if (strcmp(seen[i], expected[j]) == 0)
				known = 1;
		snprintf(msg, sizeof(msg), "%s() drives the CC2400 bus; give it an "
		         "interrupt guard and add it to this test", seen[i]);
		mu_assert(known, msg);
	}
	mu_assert_int_eq(3, (int)seen_count);
}

/* ---- the mask helpers ---- */

/*
 * irq_save_disable() has to read PRIMASK before it sets it. Reading afterwards
 * would always report interrupts already masked, so irq_restore() would
 * leave them masked for ever.
 */
MU_TEST(test_primask_is_read_before_it_is_set)
{
	char *body = read_function(UBERTOOTH_C, "irq_save_disable");
	int read, set;

	mu_assert_fatal(body != NULL, "cannot read irq_save_disable() from the firmware source");

	read = offset_of(body, "mrs");
	set = offset_of(body, "cpsid");

	mu_assert(read >= 0, "irq_save_disable() does not read PRIMASK");
	mu_assert(set >= 0, "irq_save_disable() does not mask interrupts");
	mu_assert(read < set, "irq_save_disable() reads PRIMASK after masking, so it "
	                      "always reports interrupts already masked");
	free(body);
}

/* irq_restore() writes PRIMASK back, rather than enabling interrupts
 * outright with `cpsie i`. */
MU_TEST(test_restore_writes_primask_back)
{
	char *body = read_function(UBERTOOTH_C, "irq_restore");

	mu_assert_fatal(body != NULL, "cannot read irq_restore() from the firmware source");

	mu_assert(strstr(body, "msr primask") != NULL,
	          "irq_restore() does not write PRIMASK back");
	mu_assert(strstr(body, "cpsie") == NULL,
	          "irq_restore() enables interrupts outright, which would unmask "
	          "them inside an interrupt handler");
	free(body);
}

/*
 * Both helpers tell the compiler the assembly clobbers memory, which stops it
 * moving a load or store across the point where the mask changes. The GPIO
 * writes in the transfer are volatile and cannot move anyway, so nothing here
 * depends on the clobber today; it is what keeps the guard meaningful for any
 * later caller that puts ordinary variables inside it.
 */
MU_TEST(test_the_helpers_clobber_memory)
{
	char *disable = read_function(UBERTOOTH_C, "irq_save_disable");
	char *restore = read_function(UBERTOOTH_C, "irq_restore");

	mu_assert_fatal(disable != NULL && restore != NULL,
	                "cannot read the mask helpers from the firmware source");

	mu_assert(strstr(disable, "\"memory\"") != NULL,
	          "irq_save_disable() does not clobber memory, so the compiler may "
	          "move a load or store past the point where interrupts go off");
	mu_assert(strstr(restore, "\"memory\"") != NULL,
	          "irq_restore() does not clobber memory, so the compiler may move "
	          "a load or store past the point where interrupts come back");
	free(disable);
	free(restore);
}

int main(void)
{
	printf("CC2400 SPI transfer atomicity tests\n");
	MU_RUN(test_transfer_is_masked_from_before_csn_to_after_csn);
	MU_RUN(test_transfer_drives_csn_exactly_once);
	MU_RUN(test_restore_puts_back_the_saved_mask);
	MU_RUN(test_no_return_while_masked);
	MU_RUN(test_nothing_slow_inside_the_guard);
	MU_RUN(test_the_fifo_transfers_are_masked_from_before_csn_to_after_csn);
	MU_RUN(test_accessors_go_through_cc2400_spi);
	MU_RUN(test_the_list_of_bus_functions_is_pinned);
	MU_RUN(test_primask_is_read_before_it_is_set);
	MU_RUN(test_restore_writes_primask_back);
	MU_RUN(test_the_helpers_clobber_memory);
	return mu_report();
}
