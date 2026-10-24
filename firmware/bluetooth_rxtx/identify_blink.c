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

void identify_blink_toggle(identify_blink_t* blink, uint32_t interval)
{
	blink->level = blink->level ? 0 : 1;
	blink->next += interval;
}
