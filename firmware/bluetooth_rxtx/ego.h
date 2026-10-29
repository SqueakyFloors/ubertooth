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

/* The sync word the radio is told, and how many bytes of it the host gave.
 * The mode dispatch keeps a copy across a mode start, so a code stated after
 * the mode request is not thrown away by the reset. */
extern volatile uint32_t access_code;
extern volatile int access_code_len;

/* Runs the E-GO mode named by `ego_mode`, and starts the state machine over
 * when a request names another: see ego_main(). */
void ego_main(void);
int rfcat_subcommand(uint16_t cmd, uint8_t *body, int body_len);
void ego_access_code_reset(void);

#endif /* __EGO_H */
