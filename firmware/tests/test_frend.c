/*
 * Unit tests for the FREND (CC2400 analog front-end control) helpers in
 * firmware/common/cc2400.h.
 *
 * `ubertooth-util -a<0-7>` sets FREND's PA_LEVEL[2:0]. The transmit paths in
 * bluetooth_rxtx.c and ego.c used to write a whole FREND value of their own
 * (0b1011, level 3, "picked from hat"; ego used 0xf), which silently replaced
 * the level the user asked for and left it changed afterwards.
 * They now go through frend_keep_pa_level(), which writes the reserved bits
 * and keeps PA_LEVEL as it is.
 *
 * These run on the host: cc2400.h is pure register definitions plus one small
 * function, and the fakes below stand in for the SPI register file, so the
 * read-modify-write the firmware performs can be checked without hardware.
 * The last test scans the firmware sources instead, to catch a transmit path
 * going back to a hard-coded level.
 */
#include <stdlib.h>
#include <string.h>

#include "minunit.h"
#include "cc2400.h"

/* Stand-in for the CC2400's register file, written over SPI on real hardware. */
static uint16_t regs[0x80];

static uint16_t cc2400_get(uint8_t reg)
{
	return regs[reg];
}

static void cc2400_set(uint8_t reg, uint16_t val)
{
	regs[reg] = val;
}

/*
 * Power-on state. The CC2400 datasheet gives FREND (0x05) a reset value of 7
 * for PA_LEVEL and 1 for the reserved bit 3, so 0x000f; the firmware does not
 * write FREND at start-up, so this is what a fresh board has.
 */
static void chip_reset(void)
{
	memset(regs, 0, sizeof(regs));
	regs[FREND] = 0x000f;
}

/* What the UBERTOOTH_SET_PALEVEL vendor request does (`ubertooth-util -a`). */
static void set_palevel(uint8_t level)
{
	cc2400_set(FREND, FREND_RESERVED_ONE | level);
}

/* What the UBERTOOTH_GET_PALEVEL vendor request returns (`ubertooth-util -a`). */
static uint8_t get_palevel(void)
{
	return cc2400_get(FREND) & FREND_PA_LEVEL;
}

/* What a transmit mode now does when it configures the front end. */
static void tx_mode_start(void)
{
	cc2400_set(FREND, frend_keep_pa_level(cc2400_get(FREND)));
}

/* Every level set with -a must survive starting a transmit mode. */
MU_TEST(test_tx_keeps_level_set_with_a)
{
	uint8_t level;

	for (level = 0; level <= 7; level++) {
		chip_reset();
		set_palevel(level);
		tx_mode_start();
		mu_assert_int_eq(level, get_palevel());
		mu_assert(cc2400_get(FREND) & FREND_RESERVED_ONE,
		          "reserved bit 3 must still be written as 1");
	}
}

/*
 * Untouched by -a, a transmit mode runs at the power-on level 7. That is what
 * ego jamming forced before the fix, so ego is unchanged by default; the
 * Bluetooth and BLE paths used to force level 3, so their default rises.
 */
MU_TEST(test_tx_at_power_on_default_is_level_7)
{
	chip_reset();
	tx_mode_start();
	mu_assert_int_eq(7, get_palevel());
	mu_assert_int_eq(0xf, cc2400_get(FREND));
}

/* The reserved bit must be written as 1 even if the register has it clear. */
MU_TEST(test_write_sets_the_reserved_bit)
{
	chip_reset();
	cc2400_set(FREND, 0x0005);      /* level 5, reserved bit 3 clear */
	tx_mode_start();
	mu_assert_int_eq(FREND_RESERVED_ONE | 0x5, cc2400_get(FREND));
}

/* Only bit 3 and PA_LEVEL are written: the rest of FREND reads back as zero,
 * which is what the fixed values the transmit paths used to write did too. */
MU_TEST(test_write_touches_only_bit_3_and_the_level)
{
	mu_assert_int_eq(FREND_RESERVED_ONE | 0x5, frend_keep_pa_level(0xfff5));
	mu_assert_int_eq(FREND_RESERVED_ONE | 0x0, frend_keep_pa_level(0xfff8));
	mu_assert_int_eq(FREND_RESERVED_ONE | 0x7, frend_keep_pa_level(0x0007));
}

/* Regression: the old write forced level 3 whatever the user had asked for. */
MU_TEST(test_tx_no_longer_forces_level_3)
{
	chip_reset();
	set_palevel(5);
	tx_mode_start();
	mu_assert(cc2400_get(FREND) != 0xb,
	          "FREND must not be forced back to the old 0b1011 (level 3)");
	mu_assert_int_eq(5, get_palevel());
}

/*
 * The tests above exercise the read-modify-write itself. This one guards the
 * call sites, which are hardware code that cannot be run here: every FREND
 * write in the transmit firmware must take its PA level from somewhere, not
 * hard-code one. The allowed forms are frend_keep_pa_level() (the transmit
 * modes), the level the user asked for in the UBERTOOTH_SET_PALEVEL vendor
 * request, and the level in a tx_generic() packet. cc2400_rangetest.c is
 * deliberately not covered: the range test and repeater step through levels
 * by design.
 */
static int frend_write_is_allowed(const char *line)
{
	return strstr(line, "frend_keep_pa_level(") != NULL
	    || strstr(line, "FREND_RESERVED_ONE | request_params[0]") != NULL
	    || strstr(line, "tx_pkt.pa_level") != NULL;
}

static int check_frend_writes(const char *path)
{
	char line[512], msg[640];
	int lineno = 0, writes = 0;
	FILE *f = fopen(path, "r");

	if (f == NULL) {
		mu_assert(0, "cannot open the firmware source to scan");
		printf("      (path %s)\n", path);
		return 0;
	}

	while (fgets(line, sizeof(line), f) != NULL) {
		lineno++;
		if (strstr(line, "cc2400_set(FREND") == NULL)
			continue;
		writes++;
		snprintf(msg, sizeof(msg),
		         "%s:%d writes FREND with a hard-coded level: %s",
		         path, lineno, line);
		mu_assert(frend_write_is_allowed(line), msg);
	}

	fclose(f);
	return writes;
}

MU_TEST(test_transmit_paths_do_not_hard_code_a_level)
{
	/* bluetooth_rxtx.c: the vendor request setter, cc2400_tx_sync(),
	 * le_transmit(), le_jam() and tx_generic(). ego.c: jamming. */
	mu_assert_int_eq(5, check_frend_writes(FW_RXTX_DIR "/bluetooth_rxtx.c"));
	mu_assert_int_eq(1, check_frend_writes(FW_RXTX_DIR "/ego.c"));
}

int main(void)
{
	printf("FREND PA level tests\n");
	MU_RUN(test_tx_keeps_level_set_with_a);
	MU_RUN(test_tx_at_power_on_default_is_level_7);
	MU_RUN(test_write_sets_the_reserved_bit);
	MU_RUN(test_write_touches_only_bit_3_and_the_level);
	MU_RUN(test_tx_no_longer_forces_level_3);
	MU_RUN(test_transmit_paths_do_not_hard_code_a_level);
	return mu_report();
}
