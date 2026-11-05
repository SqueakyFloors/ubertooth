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

/*
 * Averaging RSSI readings.
 *
 * A reading is in dBm, a logarithm of power, so readings have to be converted
 * to power, averaged, and converted back. Averaging the dBm figures gives a
 * lower number, further out the wider they are spread: four readings of
 * -90 dBm and one of -40 average to -80 that way, where their mean power is
 * -47.
 *
 * What is kept is the strongest reading and how many sat each whole dB below
 * it: 32-bit integers and one table of eleven entries, however loud the
 * signal is.
 *
 * A reading more than RSSI_POWER_SPAN dB below the strongest is left out,
 * which is what makes the mean describe one thing rather than a mixture: a
 * receive window spans the packet and the quiet either side of it, and with
 * the quiet counted in a packet filling a third of the window would read
 * 5 dB low.
 */
#define RSSI_POWER_ONE 32768u

/* Within how many dB of the strongest reading a reading still belongs with it.
 * Wide enough for the fading across one packet, narrow enough to leave out the
 * quiet around it. */
#define RSSI_POWER_SPAN 10

/* The most readings one step of the window holds before every step is halved.
 * Halving keeps the mean to within the one reading each step can lose, a part
 * in 134 million, far below the dB the readings are quantized to. A capture
 * reaches it after hours of readings that all land on the same step. */
#define RSSI_POWER_STEP_MAX (1u << 28)

typedef struct {
	int8_t   ref;                        /* the strongest reading so far */
	uint32_t at[RSSI_POWER_SPAN + 1];    /* how many sat each dB below it */
} rssi_power_t;

/* Forget every reading. */
void rssi_power_reset(rssi_power_t* p);

void rssi_power_add(rssi_power_t* p, int8_t v);

/* Whether anything has been added since the last reset. */
int rssi_power_measured(const rssi_power_t* p);

/* The mean level of what was added, or INT8_MIN if nothing was. */
int8_t rssi_power_mean(const rssi_power_t* p);

extern int8_t rssi_max;
extern int8_t rssi_min;
extern uint8_t rssi_count;

/* Start a new receive window. The per-channel average has to survive this,
 * because it is built from hundreds of packets; rssi_iir_reset() clears that,
 * and rssi_noise_reset() the background. */
void rssi_reset(void);

/* The packet's own power: the mean, in power, of the readings within
 * RSSI_POWER_SPAN dB of the strongest taken in this receive window. The
 * window is a fixed stretch of air time rather than the packet, so leaving
 * the quieter readings out is what makes the figure the packet's. INT8_MIN if
 * the mode took no reading. */
int8_t rssi_get_signal(void);

/*
 * The estimate of the background on the channel: the mean power of the
 * readings that sat more than RSSI_POWER_SPAN dB below the loudest taken so
 * far, where "so far" runs from the later of rssi_reset() and
 * rssi_noise_reset(), so the comparison never spans a receive window or a
 * channel.
 *
 * The readings admitted outlive the window, unlike the packet's, because a
 * quiet window holds nothing but background and a busy one may hold nothing
 * but packet. The mode loops end their collection wherever they act on the
 * hop timer -- on every hop, and 25 times a second even on a fixed channel --
 * so a record reports the last few tens of milliseconds of air on its
 * channel.
 *
 * Three things it is not. Not a measurement of clear air: a deeply faded part
 * of a packet sits that far down too and is counted in. Not the floor: the
 * mean is over the loudest RSSI_POWER_SPAN dB of what was admitted, so one
 * reading at -85 dBm among a hundred at -110 reports about -85. And not every
 * quiet reading, because the reference is a running loudest, so a window
 * whose packet arrives last contributes nothing.
 */
void rssi_noise_reset(void);
int rssi_noise_estimated(void);
int8_t rssi_get_noise(void);

/* Forget every channel's running average, where a capture ends. */
void rssi_iir_reset(void);

void rssi_add(int8_t v);
void rssi_iir_update(uint16_t channel);
int8_t rssi_get_avg(uint16_t channel);

/*
 * LED spectrum analyzer threshold.
 *
 * The CC2400 reports RSSI as a raw signed value; dBm = raw - RSSI_DBM_OFFSET.
 * The host sends the threshold as N, meaning -N dBm, within
 * UBERTOOTH_LED_SPECAN_N_MIN-MAX (20-120, ubertooth_interface.h), so the raw
 * threshold runs from 54 - 20 = 34 down to 54 - 120 = -66. The stored
 * threshold and the comparison are 16-bit so that no N from any caller can
 * wrap: in an int8_t, N >= 183 wraps to a positive value no reading reaches.
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
