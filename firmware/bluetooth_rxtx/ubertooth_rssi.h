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
 * A reading is in dBm, which is a logarithm of power, and the average of a set
 * of readings is the level of their average power -- so the readings have to
 * be converted to power, averaged, and converted back. Averaging the dBm
 * figures themselves gives a different, lower number, further out the wider
 * the readings are spread: four readings of -90 dBm and one of -40 dBm average
 * to -80 dBm that way, where their mean power is -47 dBm.
 *
 * Power is carried relative to the strongest reading added so far, in units of
 * RSSI_POWER_ONE for a reading at that level, which keeps the arithmetic to
 * 32-bit integers and one small table however loud the signal is.
 *
 * A reading more than `span` dB below the strongest is left out, and so is its
 * share of the count. That is what makes the mean describe one thing rather
 * than a mixture: the readings of a receive window span the packet and the
 * quiet either side of it, and with the quiet counted in, a packet filling a
 * third of the window would read 5 dB below its real level.
 */
#define RSSI_POWER_ONE 32768u

/* The widest window the table covers. */
#define RSSI_POWER_MAX_SPAN 30

/* Within how many dB of the strongest reading a reading still belongs to the
 * packet. Wide enough for the fading across one packet, narrow enough to leave
 * out the quiet around it. */
#define RSSI_SIGNAL_SPAN 10

/* The background is measured with nothing on the channel, so every reading
 * belongs to it and the window only has to cover their spread. */
#define RSSI_NOISE_SPAN RSSI_POWER_MAX_SPAN

typedef struct {
	int8_t   ref;    /* the strongest reading added so far */
	uint8_t  span;   /* how far below it a reading still counts, in dB */
	uint32_t sum;    /* the readings' power, relative to ref */
	uint32_t count;  /* how many counted, RSSI_POWER_ONE each */
} rssi_power_t;

/* Empty the accumulator and set its window. */
void rssi_power_init(rssi_power_t* p, uint8_t span);

/* Empty it again, keeping the window. */
void rssi_power_reset(rssi_power_t* p);

void rssi_power_add(rssi_power_t* p, int8_t v);

/* Whether anything has been added since the last reset. */
int rssi_power_measured(const rssi_power_t* p);

/* The mean level of what was added, or INT8_MIN if nothing was. */
int8_t rssi_power_mean(const rssi_power_t* p);

extern int8_t rssi_max;
extern int8_t rssi_min;
extern uint8_t rssi_count;

/* Start a new packet: the minimum, maximum, sum and count only. The
 * per-channel average has to survive this, because it is built from hundreds
 * of packets; rssi_iir_reset() is what clears it. */
void rssi_reset(void);

/* The packet's own power: the mean of the readings taken while it was on the
 * air, in power, over the readings within RSSI_SIGNAL_SPAN dB of the
 * strongest. INT8_MIN if the mode took no reading. */
int8_t rssi_get_signal(void);

/*
 * The background on the channel: the mean power of the readings taken while
 * nothing was being received.
 *
 * rssi_add() collects them from the readings it is given, and a mode that can
 * tell the clear air from a packet by itself -- one whose radio delivers whole
 * packets, so that no bytes having arrived means nothing is on the channel --
 * adds them with rssi_noise_add() instead.
 *
 * It does not follow rssi_reset(), because the readings of one receive window
 * need not include any of either kind, and it starts again on a hop, because a
 * background level belongs to one channel. A record reports whatever has been
 * collected on its channel so far.
 */
void rssi_noise_reset(void);
void rssi_noise_add(int8_t v);
int rssi_noise_measured(void);
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
