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
 * Power, relative to a reading 0..RSSI_POWER_MAX_SPAN dB stronger, in units of
 * RSSI_POWER_ONE: 10^(-d/10) scaled by RSSI_POWER_ONE. The last entry is a
 * thousandth of the first, which is as far down as a reading can affect a mean
 * by as much as the readings themselves are quantized.
 */
static const uint16_t rssi_power_down[RSSI_POWER_MAX_SPAN + 1] = {
	32768, 26029, 20675, 16423, 13045, 10362,
	 8231,  6538,  5193,  4125,  3277,  2603,
	 2068,  1642,  1305,  1036,   823,   654,
	  519,   413,   328,   260,   207,   164,
	  130,   104,    82,    65,    52,    41,
	   33,
};

/* Above this the next reading could overflow the 32-bit arithmetic below, so
 * both halves of the mean are halved, which leaves the mean itself alone. */
#define RSSI_POWER_LIMIT (1u << 28)

/* x scaled down by d dB. */
static uint32_t rssi_power_scale(uint32_t x, unsigned d)
{
	return (uint32_t)(((uint64_t)x * rssi_power_down[d]) / RSSI_POWER_ONE);
}

void rssi_power_reset(rssi_power_t* p)
{
	p->ref = INT8_MIN;
	p->sum = 0;
	p->count = 0;
}

void rssi_power_init(rssi_power_t* p, uint8_t span)
{
	p->span = (span > RSSI_POWER_MAX_SPAN) ? RSSI_POWER_MAX_SPAN : span;
	rssi_power_reset(p);
}

void rssi_power_add(rssi_power_t* p, int8_t v)
{
	unsigned d;

	if (p->count == 0) {
		p->ref = v;
		p->sum = RSSI_POWER_ONE;
		p->count = RSSI_POWER_ONE;
		return;
	}

	if (v > p->ref) {
		/* A stronger reading becomes the reference, which puts everything
		 * already added that much further below it. */
		d = (unsigned)(v - p->ref);
		if (d > p->span) {
			/* Everything already added is now beyond the window, so it
			 * counts for nothing: the mean starts again from this
			 * reading. */
			p->sum = 0;
			p->count = 0;
		} else {
			/* Still inside the window, so it all still counts -- just
			 * that much further below the new reference. */
			p->sum = rssi_power_scale(p->sum, d);
		}
		p->ref = v;
		p->sum += RSSI_POWER_ONE;
		p->count += RSSI_POWER_ONE;
	} else {
		d = (unsigned)(p->ref - v);
		if (d > p->span)
			return;
		p->sum += rssi_power_down[d];
		p->count += RSSI_POWER_ONE;
	}

	if (p->count >= RSSI_POWER_LIMIT) {
		p->sum >>= 1;
		p->count >>= 1;
	}
}

int rssi_power_measured(const rssi_power_t* p)
{
	return p->count != 0;
}

int8_t rssi_power_mean(const rssi_power_t* p)
{
	uint32_t sum = p->sum;
	uint32_t count = p->count;
	uint32_t mean;
	unsigned d;

	if (count == 0)
		return INT8_MIN;

	/* The mean power, relative to the strongest reading. Both halves are
	 * brought down first so that the division below stays 32-bit; they are
	 * brought down together, so the mean is unchanged. */
	while (sum > RSSI_POWER_ONE * 2) {
		sum >>= 1;
		count >>= 1;
	}
	mean = (sum * RSSI_POWER_ONE) / count;

	/* How far below the strongest reading that is. Every counted reading is
	 * within the window, so their mean is too, and the search ends inside the
	 * table. */
	for (d = 0; d < p->span; ++d) {
		if (mean >= rssi_power_down[d + 1])
			break;
	}
	/* Between two whole dB, take the nearer in dB rather than in power: the
	 * halfway point is the geometric mean of the two. */
	if (d < p->span && (uint64_t)mean * mean <
	    (uint64_t)rssi_power_down[d] * rssi_power_down[d + 1])
		++d;

	if (p->ref - (int)d < INT8_MIN)
		return INT8_MIN;
	return (int8_t)(p->ref - (int)d);
}

int8_t rssi_max;
int8_t rssi_min;
uint8_t rssi_count;

int32_t rssi_sum;
int16_t rssi_iir[NUM_BREDR_CHANNELS] = {0};

/* The packet's own power, and the background on the channel it arrived on. */
static rssi_power_t rssi_signal_power = { INT8_MIN, RSSI_SIGNAL_SPAN, 0, 0 };
static rssi_power_t rssi_noise_power = { INT8_MIN, RSSI_NOISE_SPAN, 0, 0 };

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
	 * The background, from the readings of this receive window that were
	 * taken between packets rather than during one.
	 *
	 * Which those are is told by their level, not by their timing. A
	 * receive window is a fixed stretch of air time -- the radio hands over
	 * a block of symbols whether or not a packet arrived in it -- and the
	 * carrier-sense line cannot say either, because the squelch it shares
	 * defaults to the bottom of the radio's range, where carrier sense is
	 * asserted the whole time. What does say is the reading itself: a
	 * packet is tens of dB above the air around it, so a reading this far
	 * below the loudest of the window was taken with the channel clear.
	 *
	 * The comparison is against the window's own loudest reading, so one
	 * burst of interference costs at most the window it lands in, and the
	 * readings it collects outlive the window because a quiet one holds
	 * nothing but background and a busy one may hold nothing but packet.
	 */
	if ((int)rssi_signal_power.ref - (int)v > RSSI_SIGNAL_SPAN)
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

void rssi_noise_add(int8_t v)
{
	rssi_power_add(&rssi_noise_power, v);
}

int rssi_noise_measured(void)
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
