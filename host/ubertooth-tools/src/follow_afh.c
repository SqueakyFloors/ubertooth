/*
 * Copyright 2026 Project Ubertooth contributors
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

#include "follow_afh.h"

#include <stdio.h>
#include <string.h>

int follow_afh_count_used(const uint8_t* map)
{
	int channel, used = 0;

	if (map == NULL)
		return 0;
	for (channel = 0; channel < FOLLOW_AFH_CHANNELS; channel++)
		if (map[channel / 8] & (1 << (channel % 8)))
			used++;
	return used;
}

follow_afh_result follow_afh_classify(int read_result, uint8_t mode,
                                      const uint8_t* map)
{
	int used;

	/* The read failed, so mode and map hold whatever they held before.
	 * Neither is looked at. */
	if (read_result < 0)
		return FOLLOW_AFH_READ_FAILED;

	if (mode == FOLLOW_AFH_MODE_OFF)
		return FOLLOW_AFH_MODE_DISABLED;
	if (mode != FOLLOW_AFH_MODE_ON)
		return FOLLOW_AFH_MODE_INVALID;

	used = follow_afh_count_used(map);
	if (used == 0)
		return FOLLOW_AFH_MAP_EMPTY;
	if (used < FOLLOW_AFH_MIN_USED_CHANNELS)
		return FOLLOW_AFH_TOO_FEW_CHANNELS;
	return FOLLOW_AFH_OK;
}

int follow_afh_should_retry(follow_afh_result result)
{
	return result == FOLLOW_AFH_MAP_EMPTY ||
	       result == FOLLOW_AFH_TOO_FEW_CHANNELS;
}

int follow_afh_read_map(const follow_afh_reader* reader, int attempts,
                        follow_afh_read* out)
{
	int attempt;

	if (out == NULL)
		return -1;
	memset(out, 0, sizeof(*out));
	out->result = FOLLOW_AFH_READ_FAILED;
	out->attempts = 0;
	if (reader == NULL || reader->read == NULL)
		return -1;
	if (attempts < 1)
		attempts = 1;

	for (attempt = 1; attempt <= attempts; attempt++) {
		uint8_t mode = 0;
		uint8_t map[FOLLOW_AFH_MAP_BYTES] = { 0 };
		int r = reader->read(reader->ctx, &mode, map);

		out->attempts = attempt;
		out->result = follow_afh_classify(r, mode, map);
		/* Only a successful read says anything about the mode, and only
		 * an enabled mode means the map was counted. */
		out->mode = (r < 0) ? 0 : mode;
		out->used = (out->result == FOLLOW_AFH_READ_FAILED ||
		             out->result == FOLLOW_AFH_MODE_DISABLED ||
		             out->result == FOLLOW_AFH_MODE_INVALID)
		                    ? 0
		                    : follow_afh_count_used(map);

		if (out->result == FOLLOW_AFH_OK) {
			memcpy(out->map, map, sizeof(out->map));
			return 0;
		}
		if (!follow_afh_should_retry(out->result))
			return -1;
		if (attempt < attempts && reader->wait != NULL)
			reader->wait(reader->ctx);
	}
	return -1;
}

follow_afh_action follow_afh_action_for(follow_afh_result result)
{
	switch (result) {
	case FOLLOW_AFH_OK:
		return FOLLOW_AFH_ACTION_APPLY;
	/* The remote has told us it is not hopping adaptively. That is an
	 * answer, and following it without a map is correct. */
	case FOLLOW_AFH_MODE_DISABLED:
		return FOLLOW_AFH_ACTION_DISABLE;
	/* Everything else leaves us with no usable map while the remote may
	 * well be hopping on one. Following with the wrong hop sequence
	 * receives nothing, so stop and say why instead. */
	default:
		return FOLLOW_AFH_ACTION_FAIL;
	}
}

void follow_afh_message(const follow_afh_read* out, const char* detail,
                        char* buf, size_t len)
{
	if (buf == NULL || len == 0)
		return;
	buf[0] = '\0';
	if (out == NULL)
		return;

	switch (out->result) {
	case FOLLOW_AFH_OK:
		snprintf(buf, len, "AFH map read: %d of %d channels in use",
		         out->used, FOLLOW_AFH_CHANNELS);
		break;
	case FOLLOW_AFH_READ_FAILED:
		snprintf(buf, len, "AFH map read failed: %s",
		         (detail != NULL && detail[0] != '\0') ? detail
		                                               : "no reason given");
		break;
	case FOLLOW_AFH_MODE_DISABLED:
		snprintf(buf, len,
		         "the remote device is not using AFH; following without it");
		break;
	case FOLLOW_AFH_MODE_INVALID:
		snprintf(buf, len,
		         "the remote device reported AFH mode 0x%02x, "
		         "which is neither off (0x%02x) nor on (0x%02x)",
		         out->mode, FOLLOW_AFH_MODE_OFF, FOLLOW_AFH_MODE_ON);
		break;
	case FOLLOW_AFH_MAP_EMPTY:
		snprintf(buf, len,
		         "the remote device reports AFH on but an empty channel "
		         "map (0 of %d channels) after %d read%s",
		         FOLLOW_AFH_CHANNELS, out->attempts,
		         out->attempts == 1 ? "" : "s");
		break;
	case FOLLOW_AFH_TOO_FEW_CHANNELS:
		snprintf(buf, len,
		         "the remote device reports AFH on but only %d channel%s "
		         "in use after %d read%s, and AFH needs at least %d",
		         out->used, out->used == 1 ? "" : "s", out->attempts,
		         out->attempts == 1 ? "" : "s",
		         FOLLOW_AFH_MIN_USED_CHANNELS);
		break;
	}
}

const char* follow_afh_handle_refusal(int have_remote_address, int have_handle)
{
	/* With no target address the tool reads the local adapter's own
	 * address: there is no remote device, so no ACL link and no map to
	 * read. The old code called the read with an uninitialised handle. */
	if (!have_remote_address)
		return "-a reads the AFH map over a Bluetooth connection to the "
		       "target, so it needs the whole target address: give "
		       "-t xx:xx:xx:xx:xx:xx";
	if (!have_handle)
		return "-a needs a Bluetooth connection to the target, and none "
		       "was established";
	return NULL;
}

void afh_map_line(const uint8_t* map, char* buf, size_t len)
{
	/* Two hex digits per byte of the map, and the terminator. */
	char hex[2 * FOLLOW_AFH_MAP_BYTES + 1];
	size_t byte;

	if (buf == NULL || len == 0)
		return;
	buf[0] = '\0';
	if (map == NULL)
		return;

	/* A byte at a time, so the length comes from the map and not from the
	 * number of arguments in a format string. */
	for (byte = 0; byte < FOLLOW_AFH_MAP_BYTES; byte++)
		snprintf(hex + 2 * byte, 3, "%02x", map[byte]);
	snprintf(buf, len, "AFH map: 0x%s", hex);
}
