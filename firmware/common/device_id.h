/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * How the part number and the serial number the boot ROM reports become the
 * bytes the host and the range test see. Pure arithmetic, in a header of its
 * own so the host unit tests run the firmware's own code rather than a copy of
 * it: reading the numbers needs a boot ROM, laying them out does not.
 */

#ifndef __DEVICE_ID_H
#define __DEVICE_ID_H

#include <stdint.h>

/*
 * One reply to a host that asked for one of the two numbers: the boot ROM's
 * status byte, then `words` of its result, least significant byte of each word
 * first. That is how the host has always read them, so the layout is fixed.
 */
static inline void device_id_pack(uint8_t *reply, const uint32_t *result,
                                  int words)
{
	int i;

	reply[0] = result[0] & 0xFF; /* the boot ROM's status */
	for (i = 0; i < words * 4; i++)
		reply[1 + i] = (result[1 + i / 4] >> (8 * (i % 4))) & 0xFF;
}

/*
 * The serial number's 16 bytes the other way round within each word, most
 * significant byte first, which is the order the range test puts them on the
 * air. `bytes` is the 16 bytes of a packed reply, so the status byte is not
 * part of it.
 */
static inline void device_serial_msb_first(uint8_t *out, const uint8_t *bytes)
{
	int word;
	int i;

	for (word = 0; word < 4; word++)
		for (i = 0; i < 4; i++)
			out[word * 4 + i] = bytes[word * 4 + 3 - i];
}

#endif /* __DEVICE_ID_H */
