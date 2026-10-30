/*
 * Copyright 2015 Hannes Ellinger
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

#include "ubertooth_rssi.h"

#include <string.h>

#define RSSI_IIR_ALPHA 3       // 3/256 = .012

/*
 * Round n/d to the nearest integer (half away from zero). C integer division
 * truncates toward zero, so the old `(n + d/2) / d` idiom only rounds correctly
 * for n >= 0. RSSI values are dBm and therefore negative, so without the signed
 * form every reading was biased upward (reported ~1-2 dB stronger than it was).
 * Adding d/2 with the sign of n fixes both directions.
 */
static inline int32_t round_div(int32_t n, int32_t d)
{
	int32_t half = d / 2;
	return (n >= 0) ? (n + half) / d : (n - half) / d;
}

/*
 * Power, relative to a reading 0..RSSI_POWER_SPAN dB stronger, in units of
 * RSSI_POWER_ONE: 10^(-d/10) scaled by RSSI_POWER_ONE.
 */
static const uint16_t rssi_power_down[RSSI_POWER_SPAN + 1] = {
	32768, 26029, 20675, 16423, 13045, 10362,
	 8231,  6538,  5193,  4125,  3277,
};

/* The most readings one step of the window holds before every step is halved.
 * Halving keeps their proportions, and so the mean, to within the one reading
 * each step can lose -- a part in 134 million, far below the dB the readings
 * themselves are quantized to. A capture reaches it after 268 million readings
 * on one channel without a hop, which is hours of a quiet channel. */
#define RSSI_POWER_STEP_MAX (1u << 28)

void rssi_power_reset(rssi_power_t* p)
{
	p->ref = INT8_MIN;
	memset(p->at, 0, sizeof(p->at));
}

void rssi_power_add(rssi_power_t* p, int8_t v)
{
	unsigned d, i, rise;

	if (v > p->ref) {
		/* A stronger reading becomes the reference, which puts every reading
		 * already counted that much further below it. Those that fall past
		 * the window are forgotten, which is what keeps the mean a mean of
		 * the readings the window holds: a reading counted while the
		 * reference was low must stop counting once a loud one has left it
		 * outside. An empty accumulator takes this path too, because every
		 * reading is more than the window above INT8_MIN. */
		rise = (unsigned)((int)v - (int)p->ref);
		if (rise > RSSI_POWER_SPAN) {
			memset(p->at, 0, sizeof(p->at));
		} else {
			for (i = RSSI_POWER_SPAN + 1; i-- > rise; )
				p->at[i] = p->at[i - rise];
			for (i = 0; i < rise; i++)
				p->at[i] = 0;
		}
		p->ref = v;
		d = 0;
	} else {
		d = (unsigned)((int)p->ref - (int)v);
		if (d > RSSI_POWER_SPAN)
			return;
	}

	if (p->at[d] >= RSSI_POWER_STEP_MAX)
		for (i = 0; i <= RSSI_POWER_SPAN; i++)
			p->at[i] >>= 1;
	p->at[d]++;
}

int rssi_power_measured(const rssi_power_t* p)
{
	unsigned i;

	for (i = 0; i <= RSSI_POWER_SPAN; i++)
		if (p->at[i] != 0)
			return 1;
	return 0;
}

int8_t rssi_power_mean(const rssi_power_t* p)
{
	uint64_t sum = 0, count = 0;
	uint32_t mean;
	unsigned d;

	for (d = 0; d <= RSSI_POWER_SPAN; d++) {
		sum += (uint64_t) p->at[d] * rssi_power_down[d];
		count += p->at[d];
	}
	if (count == 0)
		return INT8_MIN;

	/* The mean power of the readings, relative to the strongest of them. It
	 * cannot exceed RSSI_POWER_ONE, the weight of a reading at that level. */
	mean = (uint32_t)(sum / count);

	/* How far below the strongest reading that is. Every counted reading is
	 * inside the window, so their mean is too, and the search ends inside the
	 * table. */
	for (d = 0; d < RSSI_POWER_SPAN; ++d) {
		if (mean >= rssi_power_down[d + 1])
			break;
	}
	/* Between two whole dB, take the nearer in dB rather than in power: the
	 * halfway point of the two is their geometric mean. */
	if (d < RSSI_POWER_SPAN &&
	    mean * mean < (uint32_t) rssi_power_down[d] * rssi_power_down[d + 1])
		++d;

	if ((int)p->ref - (int)d < INT8_MIN)
		return INT8_MIN;
	return (int8_t)((int)p->ref - (int)d);
}

int8_t rssi_max;
int8_t rssi_min;
uint8_t rssi_count;

int32_t rssi_sum;
int16_t rssi_iir[NUM_BREDR_CHANNELS] = {0};

/* The packet's own power, and the background on the channel it arrived on. */
static rssi_power_t rssi_signal_power = { INT8_MIN, { 0 } };
static rssi_power_t rssi_noise_power = { INT8_MIN, { 0 } };

/*
 * Start a new packet's readings. The mode loops call this once per DMA bank,
 * so only the four per-packet figures belong here.
 *
 * The per-channel average does not: it is an infinite impulse response filter
 * that blends 3/256 of each bank's average into the channel's running value,
 * so it needs hundreds of banks to reach what the radio is hearing. Clearing
 * it here left it one bank old forever, and 3/256 of one bank rounds back to
 * zero for every raw reading from -42 to 42, which covers the whole range the
 * radio reports in the band. Every Classic record therefore carried the same
 * noise figure -- the host reads a zero average as -55 dBm -- whatever the
 * radio heard. rssi_iir_reset() clears it where a capture ends instead.
 */
void rssi_reset(void)
{
	rssi_count = 0;
	rssi_sum = 0;
	rssi_max = INT8_MIN;
	rssi_min = INT8_MAX;
	rssi_power_reset(&rssi_signal_power);
}

/*
 * Forget what the band sounded like. The averages describe the channels of one
 * capture, so the mode dispatch runs this where a mode ends, and the next
 * capture's first banks build the filter up again from silence.
 */
void rssi_iir_reset(void)
{
	memset(rssi_iir, 0, sizeof(rssi_iir));
}

void rssi_add(int8_t v)
{
	rssi_max = (v > rssi_max) ? v : rssi_max;
	rssi_min = (v < rssi_min) ? v : rssi_min;
	rssi_sum += ((int32_t)v * 256);  // scaled int math (x256)
	rssi_count += 1;
	rssi_power_add(&rssi_signal_power, v);

	/*
	 * The estimate of the background, from the readings of this receive
	 * window that are far enough below the loudest to look like air rather
	 * than packet.
	 *
	 * It is an estimate because the board cannot time the quiet. A receive
	 * window is a fixed stretch of air time -- the radio hands over a block
	 * of symbols whether or not a packet arrived in it -- so the window
	 * does not say when within it the packet was, and the carrier-sense
	 * line cannot say either, because the squelch it shares defaults to the
	 * bottom of the radio's range, where carrier sense is asserted the
	 * whole time. The level is the only evidence there is: a packet is tens
	 * of dB above the air around it, so a reading more than
	 * RSSI_POWER_SPAN dB below the loudest of the window was most likely
	 * taken between packets. Two things follow, which is why the figure is
	 * reported as an estimate and never as a measurement of clear air. A
	 * deeply faded part of a packet can be counted in. And the readings
	 * admitted keep their own window, so a loud one -- a burst of
	 * interference, or that faded packet -- becomes their reference and
	 * leaves the true floor outside it until the next hop clears the
	 * accumulator, which holds the figure up at the burst's level for the
	 * rest of the dwell.
	 *
	 * What the comparison is against is the window's own loudest reading,
	 * so it costs nothing to a channel that is simply loud, and the
	 * readings it collects outlive the window because a quiet one holds
	 * nothing but background and a busy one may hold nothing but packet.
	 */
	if ((int)rssi_signal_power.ref - (int)v > RSSI_POWER_SPAN)
		rssi_power_add(&rssi_noise_power, v);
}

int8_t rssi_get_signal(void)
{
	return rssi_power_mean(&rssi_signal_power);
}

void rssi_noise_reset(void)
{
	rssi_power_reset(&rssi_noise_power);
}

int rssi_noise_estimated(void)
{
	return rssi_power_measured(&rssi_noise_power);
}

int8_t rssi_get_noise(void)
{
	return rssi_power_mean(&rssi_noise_power);
}

/* For sweep mode, update IIR per channel. Otherwise, use single value. */
void rssi_iir_update(uint16_t channel)
{
	int32_t avg;
	int32_t rssi_iir_acc;

	/* Use array to track 79 Bluetooth channels, or just first slot
	 * of array if the frequency is not a valid Bluetooth channel. */
	if ( channel < BT_FREQ_MIN || channel > BT_FREQ_MAX )
		channel = BT_FREQ_MIN;

	int i = channel - BT_FREQ_MIN;

	// IIR using scaled int math (x256)
	if (rssi_count != 0)
		avg = round_div(rssi_sum, rssi_count);
	else
		avg = 0; // really an error
	rssi_iir_acc = rssi_iir[i] * (256-RSSI_IIR_ALPHA);
	rssi_iir_acc += avg * RSSI_IIR_ALPHA;
	rssi_iir[i] = (int16_t)round_div(rssi_iir_acc, 256);
}

int8_t rssi_get_avg(uint16_t channel)
{
	/* Use array to track 79 Bluetooth channels, or just first slot
	 * of array if the frequency is not a valid Bluetooth channel. */
	if ( channel < BT_FREQ_MIN || channel > BT_FREQ_MAX )
		channel = BT_FREQ_MIN;

	return round_div(rssi_iir[channel - BT_FREQ_MIN], 256);
}

int rssi_threshold_n_valid(uint16_t n)
{
	return (n >= RSSI_THRESHOLD_N_MIN) && (n <= RSSI_THRESHOLD_N_MAX);
}

int16_t rssi_threshold_from_n(uint16_t n)
{
	return (int16_t)RSSI_DBM_OFFSET - (int16_t)n;
}

int rssi_above_threshold(int8_t lvl, int16_t threshold)
{
	return (int16_t)lvl > threshold;
}
