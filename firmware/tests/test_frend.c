/*
 * Unit tests for the FREND (CC2400 analog front-end control) helpers in
 * firmware/common/cc2400.h.
 *
 * `ubertooth-util -a<0-7>` sets FREND's PA_LEVEL[2:0]. The transmit paths in
 * bluetooth_rxtx.c and ego.c used to write a whole FREND value of their own
 * (0b1011, level 3, "picked from hat"; ego used 0xf), which silently replaced
 * the level the user asked for and left it changed afterwards (issue #165).
 * They now go through frend_tx(), which selects the transmit path and keeps
 * PA_LEVEL as it is.
 *
 * These run on the host: cc2400.h is pure register definitions plus one small
 * function, and the fakes below stand in for the SPI register file, so the
 * read-modify-write the firmware performs can be checked without hardware.
 */
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
 * for PA_LEVEL, with the transmit path not selected; the firmware does not
 * write FREND at start-up, so this is what a fresh board has.
 */
static void chip_reset(void)
{
	memset(regs, 0, sizeof(regs));
	regs[FREND] = 0x0007;
}

/* What the UBERTOOTH_SET_PALEVEL vendor request does (`ubertooth-util -a`). */
static void set_palevel(uint8_t level)
{
	cc2400_set(FREND, FREND_TX | level);
}

/* What the UBERTOOTH_GET_PALEVEL vendor request returns (`ubertooth-util -a`). */
static uint8_t get_palevel(void)
{
	return cc2400_get(FREND) & FREND_PA_LEVEL;
}

/* What a transmit mode now does when it configures the front end. */
static void tx_mode_start(void)
{
	cc2400_set(FREND, frend_tx(cc2400_get(FREND)));
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
		mu_assert(cc2400_get(FREND) & FREND_TX,
		          "transmit path must stay selected");
	}
}

/* Untouched by -a, a transmit mode runs at the power-on level 7. That is what
 * ego jamming forced before the fix, so its behaviour is unchanged by default. */
MU_TEST(test_tx_at_power_on_default_is_level_7)
{
	chip_reset();
	tx_mode_start();
	mu_assert_int_eq(7, get_palevel());
	mu_assert_int_eq(0xf, cc2400_get(FREND));
}

/* A transmit mode must select the transmit path even if nothing set it yet. */
MU_TEST(test_tx_selects_transmit_path)
{
	chip_reset();
	cc2400_set(FREND, 0x0005);      /* level 5, transmit path not selected */
	tx_mode_start();
	mu_assert_int_eq(FREND_TX | 0x5, cc2400_get(FREND));
}

/* Only bit 3 and PA_LEVEL are written: the rest of FREND reads back as zero,
 * which is what the fixed values the transmit paths used to write did too. */
MU_TEST(test_tx_writes_only_tx_bit_and_level)
{
	mu_assert_int_eq(FREND_TX | 0x5, frend_tx(0xfff5));
	mu_assert_int_eq(FREND_TX | 0x0, frend_tx(0xfff8));
	mu_assert_int_eq(FREND_TX | 0x7, frend_tx(0x0007));
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

int main(void)
{
	printf("FREND PA level tests\n");
	MU_RUN(test_tx_keeps_level_set_with_a);
	MU_RUN(test_tx_at_power_on_default_is_level_7);
	MU_RUN(test_tx_selects_transmit_path);
	MU_RUN(test_tx_writes_only_tx_bit_and_level);
	MU_RUN(test_tx_no_longer_forces_level_3);
	return mu_report();
}
