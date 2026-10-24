/*
 * Copyright 2026 Project Ubertooth contributors
 *
 * This file is part of Project Ubertooth and is released under the
 * terms of the GPL. Refer to COPYING for more information.
 */

#include "identify_blink.h"

void identify_blink_start(identify_blink_t* blink, uint32_t now, uint32_t interval)
{
	blink->level = 1;
	blink->next = now + interval;
}

void identify_blink_toggle(identify_blink_t* blink, uint32_t now, uint32_t interval)
{
	blink->level = blink->level ? 0 : 1;
	blink->next += interval;
	/*
	 * A signed difference compares correctly across the counter's wrap. If
	 * the toggle was handled so late that the next one is already due or
	 * past, schedule from now instead: a match set in the past would not
	 * fire again until the whole 32-bit counter came round, leaving the LEDs
	 * stuck for weeks.
	 */
	if ((int32_t)(blink->next - now) <= 0)
		blink->next = now + interval;
}
