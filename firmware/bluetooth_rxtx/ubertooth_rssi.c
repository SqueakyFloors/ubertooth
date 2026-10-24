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
int16_t rssi_iir[79] = {0};

void rssi_reset(void)
{
	memset(rssi_iir, 0, sizeof(rssi_iir));

	rssi_count = 0;
	rssi_sum = 0;
	rssi_max = INT8_MIN;
	rssi_min = INT8_MAX;
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
	if ( channel < 2402 || channel > 2480 )
		channel = 2402;

	int i = channel - 2402;

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
	if ( channel < 2402 || channel > 2480 )
		channel = 2402;

	return round_div(rssi_iir[channel-2402], 256);
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
