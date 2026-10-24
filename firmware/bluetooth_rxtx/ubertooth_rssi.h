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

#ifndef __UBERTOOTH_RSSI_H
#define __UBERTOOTH_RSSI_H

#include "inttypes.h"
#include "ubertooth_interface.h"

extern int8_t rssi_max;
extern int8_t rssi_min;
extern uint8_t rssi_count;

void rssi_reset(void);
void rssi_add(int8_t v);
void rssi_iir_update(uint16_t channel);
int8_t rssi_get_avg(uint16_t channel);

/*
 * LED spectrum analyzer threshold.
 *
 * The CC2400 reports RSSI as a raw signed value; dBm = raw - RSSI_DBM_OFFSET.
 * The host sends the threshold as N, meaning -N dBm, within
 * UBERTOOTH_LED_SPECAN_N_MIN-MAX (20-120, ubertooth_interface.h), so the raw
 * threshold runs from 54 - 20 = 34 down to 54 - 120 = -66. The stored threshold
 * and the comparison are 16-bit so that no N, from this or any other caller,
 * can wrap: the old int8_t wrapped N >= 183 to a positive value no reading
 * reached.
 */
#define RSSI_DBM_OFFSET 54
#define RSSI_THRESHOLD_N_MIN UBERTOOTH_LED_SPECAN_N_MIN
#define RSSI_THRESHOLD_N_MAX UBERTOOTH_LED_SPECAN_N_MAX

/* Is N a threshold the firmware accepts? Matches the host's -q range. */
int rssi_threshold_n_valid(uint16_t n);

/* Raw-scale threshold for -N dBm. Check N with rssi_threshold_n_valid()
 * first: outside that range the result need not fit in an int16_t. */
int16_t rssi_threshold_from_n(uint16_t n);

/* Should a raw reading of lvl light an LED at this raw threshold? */
int rssi_above_threshold(int8_t lvl, int16_t threshold);

#endif
