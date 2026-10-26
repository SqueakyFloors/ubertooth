/*
 * Host-test stand-in for firmware/common/ubertooth.h, for tests that compile a
 * firmware file which reaches the CC2400.
 *
 * It is the shared hardware-free stub plus the CC2400 register and strobe
 * numbers (cc2400.h is numbers only) and the two radio accessors. Those extras
 * live here rather than in stubs/ubertooth.h so they are not exported into
 * every other test that uses the shared stub: cc2400.h defines around a
 * hundred very generic macros (MAIN, RSSI, STATUS, CHANNEL).
 *
 * The test supplies cc2400_status() and cc2400_strobe().
 */
#ifndef UBERTOOTH_TEST_RADIO_STUB_H
#define UBERTOOTH_TEST_RADIO_STUB_H

#include "../stubs/ubertooth.h"
#include "../../common/cc2400.h"

u8 cc2400_status(void);
u8 cc2400_strobe(u8 reg);
u16 cc2400_get(u8 reg);

#endif /* UBERTOOTH_TEST_RADIO_STUB_H */
