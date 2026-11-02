/*
 * Copyright 2015 Mike Ryan
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

#ifndef __EGO_H
#define __EGO_H

#include "ubertooth.h"

typedef enum _ego_mode_t {
	EGO_FOLLOW = 0,
	EGO_CONTINUOUS_RX,
	EGO_JAM,
} ego_mode_t;

/* How many bytes of EGO_DEFAULT_ACCESS_CODE (ubertooth_interface.h) the host
 * is treated as having given: all four. */
#define EGO_DEFAULT_ACCESS_CODE_LEN 4

/* How much of each packet a capture keeps when the host has not said, in
 * bytes. ubertooth-ego(1) documents this as its own default for -l. */
#define EGO_DEFAULT_PACKET_LEN 18

/* The sync word the radio is told, and how many bytes of it the host gave.
 * The mode dispatch keeps a copy across a mode start, so a code stated after
 * the mode request is not thrown away by the reset. */
extern volatile uint32_t access_code;
extern volatile int access_code_len;

/* How much of each packet a capture keeps, in bytes. Volatile because the
 * request that sets it is answered from the USB interrupt while the capture
 * loop reads it, as the access code above is. The mode dispatch keeps a copy
 * across a mode start: ubertooth-ego states this one before its mode request,
 * not after, so the reset would otherwise throw it away. */
extern volatile uint8_t packet_len;

/* Runs the E-GO mode named by `ego_mode`, and starts the state machine over
 * when a request names another: see ego_main(). */
void ego_main(void);
int rfcat_subcommand(uint16_t cmd, uint8_t *body, int body_len);
/* Put the settings a capture states -- the access code and the capture length
 * -- back to the documented defaults, so each run captures with what it asked
 * for and not with what the run before it asked for. */
void ego_capture_reset(void);

#endif /* __EGO_H */
