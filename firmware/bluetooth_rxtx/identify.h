/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef __IDENTIFY_H
#define __IDENTIFY_H

/*
 * Blinks the USR, RX and TX LEDs together until STOP or another mode request,
 * so a board can be picked out of several. Returns with all three LEDs off.
 */
void identify_main(void);

#endif /* __IDENTIFY_H */
