/*
 * Copyright 2012 Dominic Spill
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


#ifndef __BLUETOOTH_H
#define __BLUETOOTH_H

#include "ubertooth.h"

extern bdaddr target;
extern u8 afh_enabled;
extern u8 afh_map[10];
extern u8 used_channels;

void precalc(void);
void bluetooth_set_afh_map(const u8 *map);
u16 next_hop(u32 clkn);

#endif /* __BLUETOOTH_H */
