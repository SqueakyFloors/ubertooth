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

/*
 * What ubertooth-follow -a does with an AFH channel map read from a remote
 * device: whether the read can be attempted at all, whether the result is
 * usable, and what the tool does next. Plus the one line a map is reported
 * on, which both BlueZ tools print, so the two agree on the wording:
 * ubertooth-scan links this file for afh_map_line() alone.
 *
 * Deliberately free of BlueZ and libusb. ubertooth-follow itself links
 * libbluetooth and so is built on Linux only, but these decisions are the part
 * that can be got wrong, so they live here and are unit-tested everywhere,
 * including on macOS where the tool is not built at all.
 */

#ifndef __UBERTOOTH_FOLLOW_AFH_H__
#define __UBERTOOTH_FOLLOW_AFH_H__

#include <stddef.h>
#include <stdint.h>

/* Length of an AFH channel map in bytes: one bit per Basic Rate channel, 79
 * channels in 10 bytes (the last bit is unused). The same length as
 * libubertooth's AFH_MAP_BYTES and the firmware's, repeated here so this
 * header pulls in neither libusb nor BlueZ. Both tools that include this
 * header, ubertooth-follow.c and ubertooth-scan.c, check the two agree at
 * compile time. */
#define FOLLOW_AFH_MAP_BYTES 10

/* Basic Rate channels, one per bit of the map. */
#define FOLLOW_AFH_CHANNELS 79

/* An AFH hop sequence has to use at least this many channels (Bluetooth Core,
 * minimum number of used channels, N_min). A map with fewer is not a map the
 * remote can be hopping on: it is a reply that has not settled yet, or a
 * misreport. */
#define FOLLOW_AFH_MIN_USED_CHANNELS 20

/* The AFH_Mode byte of HCI_Read_AFH_Channel_Map. */
#define FOLLOW_AFH_MODE_OFF 0x00
#define FOLLOW_AFH_MODE_ON  0x01

/* Bounded retry policy for a map that looks transient. Three reads 200 ms
 * apart, so a run that gets nowhere costs at most 400 ms more than one read.
 * A read that fails outright is never retried: the map is undefined and the
 * command stops. */
#define FOLLOW_AFH_READ_ATTEMPTS 3
#define FOLLOW_AFH_RETRY_DELAY_US 200000

/* What one map read amounted to. */
typedef enum {
	/* AFH is on and the map is usable: apply it. */
	FOLLOW_AFH_OK,
	/* The read itself failed. The mode and the map are undefined and are
	 * never looked at. */
	FOLLOW_AFH_READ_FAILED,
	/* The remote reports AFH off, so there is no map to follow. */
	FOLLOW_AFH_MODE_DISABLED,
	/* A mode byte that is neither off nor on. */
	FOLLOW_AFH_MODE_INVALID,
	/* AFH on, but not one channel is enabled. */
	FOLLOW_AFH_MAP_EMPTY,
	/* AFH on, but fewer channels than AFH requires. */
	FOLLOW_AFH_TOO_FEW_CHANNELS
} follow_afh_result;

/* What the tool does with that. */
typedef enum {
	/* Hand the map to libbtbb and the board. */
	FOLLOW_AFH_ACTION_APPLY,
	/* The remote is not using AFH: follow it without a map. */
	FOLLOW_AFH_ACTION_DISABLE,
	/* -a was asked for and cannot be honoured: stop before any board state
	 * is set. */
	FOLLOW_AFH_ACTION_FAIL
} follow_afh_action;

/* The outcome of reading the map, with everything the message needs. */
typedef struct {
	follow_afh_result result;
	/* The mode byte as reported. 0 when the read failed. */
	uint8_t mode;
	/* Enabled channels counted in the map. 0 when the map was never read
	 * or never inspected. */
	int used;
	/* Reads performed, at least 1. */
	int attempts;
	/* The map, set only when result is FOLLOW_AFH_OK; all zero otherwise,
	 * so an undefined map can never reach the board. */
	uint8_t map[FOLLOW_AFH_MAP_BYTES];
} follow_afh_read;

/* One attempt at the map. Returns < 0 on failure, as hci_read_afh_map() does;
 * mode and map are then left alone. */
typedef int (*follow_afh_read_fn)(void* ctx, uint8_t* mode, uint8_t* map);

/* Waits before the next attempt. May be NULL. */
typedef void (*follow_afh_wait_fn)(void* ctx);

typedef struct {
	follow_afh_read_fn read;
	follow_afh_wait_fn wait;
	void* ctx;
} follow_afh_reader;

/* Channels enabled in map, counting the FOLLOW_AFH_CHANNELS real channels
 * only. 0 for a NULL map. */
int follow_afh_count_used(const uint8_t* map);

/* Turns one read into a result. read_result is what the read returned; when it
 * is < 0 neither mode nor map is read, so a failed read can never be reported
 * as a map. */
follow_afh_result follow_afh_classify(int read_result, uint8_t mode,
                                      const uint8_t* map);

/* 1 for a result that another read might improve: a map that is empty or too
 * small has not settled. A failed read, a disabled remote and an unknown mode
 * are answers, not transients. */
int follow_afh_should_retry(follow_afh_result result);

/* Reads the map, retrying a transient result up to attempts times (clamped to
 * at least 1). out is filled in completely either way; its map stays zero
 * unless the result is FOLLOW_AFH_OK. Returns 0 when the map is usable, -1
 * otherwise. */
int follow_afh_read_map(const follow_afh_reader* reader, int attempts,
                        follow_afh_read* out);

/* What to do with a finished read. */
follow_afh_action follow_afh_action_for(follow_afh_result result);

/* One plain line describing the outcome, with no "error: " prefix and no
 * trailing newline, written into buf. detail carries the system reason for a
 * failed read (strerror(errno)) and may be NULL. An outcome with nothing to
 * say writes an empty string. */
void follow_afh_message(const follow_afh_read* out, const char* detail,
                        char* buf, size_t len);

/* Characters afh_map_line() can write, the terminator included: "AFH map: 0x"
 * is 11, the map is two hex digits per byte. */
#define AFH_MAP_LINE_LEN (11 + 2 * FOLLOW_AFH_MAP_BYTES + 1)

/* An AFH channel map as the one line it is reported on, written into buf with
 * no trailing newline: "AFH map: 0x" and the map's bytes in order, the way
 * hcitool shows one. Both BlueZ tools print this line themselves rather than
 * having libbtbb print it for them: standard output belongs to the tool, and a
 * library must not write to it. A NULL map writes an empty string. */
void afh_map_line(const uint8_t* map, char* buf, size_t len);

/* Whether -a can be attempted at all: the map is read over an ACL link to the
 * target, so it needs an address a connection can be made to, which is the
 * whole six bytes of one. have_whole_address is therefore 1 only when the
 * whole address was given (ubertooth-follow's -t); a LAP and UAP are four of
 * the six bytes and do not count, even though they name a piconet. Returns
 * NULL when AFH can go ahead, otherwise the one line to print after
 * "error: ". */
const char* follow_afh_handle_refusal(int have_whole_address, int have_handle);

#endif /* __UBERTOOTH_FOLLOW_AFH_H__ */
