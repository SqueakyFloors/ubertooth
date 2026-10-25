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

#ifndef __UBERTOOTH_CALLBACK_H__
#define __UBERTOOTH_CALLBACK_H__


#include "ubertooth_control.h"
#include "ubertooth.h"

/* Returns the capture clock tracking to the state a capture starts from. A
 * handle used for a second capture must not inherit the first one's wrap
 * count, or every time it reconstructs would be a period or more too high.
 * Which clock stamps the packets (clk100ns_period) is the tool's choice, not
 * tracking state, and is left as the tool set it. */
void ubertooth_capture_clock_reset(ubertooth_t* ut);

void cb_afh_initial(ubertooth_t* ut, void* args);
void cb_afh_monitor(ubertooth_t* ut, void* args);
void cb_afh_r(ubertooth_t* ut, void* args);
void cb_btle(ubertooth_t* ut, void* args);
void cb_ego(ubertooth_t* ut, void* args __attribute__((unused)));
void cb_rx(ubertooth_t* ut, void* args);
void cb_scan(ubertooth_t* ut, void* args);

#endif /* __UBERTOOTH_CALLBACK_H__ */
