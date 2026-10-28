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

int8_t rssi_max;
int8_t rssi_min;
uint8_t rssi_count;

int32_t rssi_sum;
int16_t rssi_iir[NUM_BREDR_CHANNELS] = {0};

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
