#ifndef __HOP_H
#define __HOP_H
#include <stdint.h>
#include <ubertooth_interface.h>

typedef struct hop_state_s {
	uint8_t a27_23, a22_19, C, E;
	uint16_t a18_10;
	uint8_t x;
	/* The phase the scan substate is listening on, kept so a response that
	 * follows a scan starts from the phase that heard the packet. */
	uint8_t scan_x;
	/*
	 * Basic mapping table (Core spec Vol 2 Part B 2.6.2): even channels in
	 * ascending order followed by odd channels.  AFH consults it first, so
	 * it always describes the full 79-channel sequence.
	 */
	uint8_t basic_bank[NUM_BREDR_CHANNELS];
	/*
	 * AFH mapping table (2.6.3.1): the basic table with every unused
	 * channel removed, holding afh_chan_count entries.
	 */
	uint8_t afh_bank[NUM_BREDR_CHANNELS];
	uint8_t afh_chan_count;
	/* Channel map as the host sends it, one bit per channel, channel 0 in
	 * the least significant bit of byte 0 (btctl_set_afh_req_t.map). */
	uint8_t afh_map[10];
	uint8_t afh_enabled;
} hop_state_t;

void hop_init(uint32_t address);
uint8_t hop_basic(uint32_t clk);
uint8_t hop_inquiry(uint32_t clk);
uint8_t hop_scan(uint32_t clkn);
uint8_t hop_channel(uint32_t clk);
void hop_cfg_afh(uint8_t* buf);

/* FIXME ?*/
extern hop_state_t hop_state;

static inline uint8_t perm5(uint8_t z, uint8_t p_high, uint16_t p_low)
{
	extern const uint8_t perm5_lut[2][4096];
	uint16_t p = (p_low&0x1ff)|((p_high&0x1f)<<9);

	z &= 0x1f;
	z = perm5_lut[0][(((p>>7))<<5)|z];
	z = perm5_lut[1][((0x7f&(p>>0))<<5)|z];

	return z;
}


/* This function computes the input value X to be sent to hop_selection_kernel when the master
has clock clk, during page scan.

A slave in PAGE_SCAN mode listens for ID(1) messages for a duration of PAGE_WINDOW (>=10ms)
every PAGE_INTERVAL (either 0s, 1.28s or 2.56s), with slow frequency-hopping that only
changes every 1.28s. The used frequency is calculated by hop_selection_kernel from the slave's
BT-MAC (inputs A,B,C,D,E,F) and CLK[12:16] (input X).

To establish connection the master must send ID(1) on the correct frequency.
Knowing the slave BT-MAC (but not its CLK), the master needs to try all possible X values (2**5 = 32 options).
On every master slot, the master tries two frequencies corresponding to two guesses for X (slave CLK[12:16]),
one try on each clock of the slot. If any of them is correct, the slave will respond with ID(2)
on the corresponding clock of the following slot.

Since PAGE_WINDOW might be as short as 10ms, only 16 X values might fit a single window.
For slaves whose PAGE_INTERVAL is long (1.28s or 2.56s), CLK[12:16] will change across windows.
We want to avoid trying X values that correspond to the same guess for the slave's clock,
i.e., if X was rejected on time T, then X+1 will be rejected on T+1.28s, X+2 will be rejected on T+2.56s, etc.
Therefore we calculate a series of X values to try on each master clock (keeping in mind only
even slots are the master's), in a way that:

1. all possible X values are tried repeatedly within a 1.28s interval;
2. if the 16 values tried during the 10ms window starting at time T were (X0,...,X15), then none of the values (X0+1,...,X15+1) will be tried on time T+1.28s;
3. if the 16 values tried during the 10ms window starting at time T were (X0,...,X15), then none of the values (X0+2,...,X15+2) will be tried on time T+2.56s.

* CLK[5:2,0] is a simple incrementing counter (jumping over slave slots indicated by CLK[1]==1) to satisfy (1);
* CLK[16:12]*17 ensures adding 17 every 1.28s to satisfy (2);
* CLK[16:13]*16 ensures adding 17+17+16=50 == 18 (mod 32) every 2.56s to satisfy (3). */
static inline uint8_t calc_iterate_slave_page_scan_phase(uint32_t clk)
{
	// 	X = ( CLK[5...2,0] + CLK[16...12]*17 + CLK[16...13]*16 ) mod 32
	return ((((clk>>1) & 0x1e) | (clk&1) ) + ((clk>>12)&0x1f)*17 + ((clk>>13)&0xf)*16)%32  ;
}

/* When we recive an ID(1) acknoledgment, that is, ID(2) From the slave device we transition into PAGE_RESPONSE
we freeze this value and use a frozen value of this +N (see Master Page Response Substate hopping scheme control word)
("our" N is actually hop_state.x which is also used in bbcodec) */
static inline void hop_freeze_clock(uint32_t clk)
{
	hop_state.x = calc_iterate_slave_page_scan_phase(clk);
}

/* The X input of the page scan and inquiry scan substates: CLKN16-12 of the
 * scanning device's own clock (Vol 2, Part B, Table 2.2).  Those five bits
 * change once every 1.28 seconds, so a scanning device holds one frequency for
 * that long, and a pager or inquirer sweeping all 32 phases comes round to it
 * every sweep. */
static inline uint8_t hop_scan_phase(uint32_t clkn)
{
	return (clkn >> 12) & 0x1f;
}

/* Start the response substate that follows a scan: the peripheral page
 * response (2.6.4.3) or the inquiry response (2.6.4.6).  Both take the phase
 * the scan was listening on, with a counter N that starts at zero, so the
 * first answer goes out on the frequency that pairs with the one the page or
 * inquiry arrived on.  hop_increment() carries N from there.
 *
 * The phase comes from hop_scan(), not from the clock as it reads now: a scan
 * window here holds one channel for several slots rather than retuning in each
 * one, so reading the clock again at the end of a window that crossed a
 * 1.28-second boundary would give the next phase, not the one that heard the
 * packet.  2.6.4.3 freezes these bits for the page response in any case;
 * 2.6.4.6 leaves them live for the inquiry response, and taking the window's
 * phase departs from that, because the other end answers on the phase it
 * transmitted on and that is the one the window heard. */
static inline void hop_start_response(void)
{
	hop_state.x = hop_state.scan_x;
}

/* This function increment the x variable of for paging/inquiry hopping.
 * It must be called before each master's transmission. */
static inline void hop_increment(void)
{
	hop_state.x++;
}
#endif
