/*
 * Copyright 2026 Project Ubertooth contributors
 *
 * This file is part of Project Ubertooth and is released under the
 * terms of the GPL. Refer to COPYING for more information.
 */

/*
 * Blink pattern of the identify mode (MODE_IDENTIFY), kept apart from the
 * hardware so it can be compiled and tested on the host; see
 * firmware/tests/test_identify_blink.c. identify.c drives the LEDs and the
 * timer from it.
 */

#ifndef __IDENTIFY_BLINK_H
#define __IDENTIFY_BLINK_H

#include <stdint.h>

typedef struct {
	uint32_t next;  /* timer tick at which the next toggle is due */
	uint8_t  level; /* level to drive the LEDs at now: 1 on, 0 off */
} identify_blink_t;

/* Starts the pattern at tick now, with the LEDs on and the first toggle one
 * interval later. */
void identify_blink_start(identify_blink_t* blink, uint32_t now, uint32_t interval);

/*
 * One toggle: flips the level and schedules the next toggle one interval after
 * the one just due, so a late interrupt does not stretch the blink. The tick
 * counter is 32 bits and wraps; so does the schedule.
 */
void identify_blink_toggle(identify_blink_t* blink, uint32_t interval);

#endif /* __IDENTIFY_BLINK_H */
