#ifndef UBERTOOTH_EGO_OPTIONS_H
#define UBERTOOTH_EGO_OPTIONS_H

#include <stdint.h>

/* The capture length in bytes: the firmware captures at most
 * EGO_MAX_PACKET_LEN (48) bytes per packet (firmware/bluetooth_rxtx/ego.c). */
#define EGO_PACKET_LENGTH_MIN 1
#define EGO_PACKET_LENGTH_MAX 48

/* RFCAT_SET_AA takes a 1 to 4 byte access code (firmware rfcat_subcommand()). */
#define EGO_ACCESS_CODE_BYTES 4

/* The channel continuous receive uses when -c is not given, in MHz. One of
 * the four the E-GO hops across (firmware/bluetooth_rxtx/ego.c, channels[]),
 * so a run without -c lands on real traffic. The tool always sends it, which
 * is what makes such a run reproducible. */
#define EGO_DEFAULT_CHANNEL 2418

/* The modes cmd_ego() asks the firmware for, numbered as its state machine
 * expects (firmware/bluetooth_rxtx/ego.h). Only continuous receive stays on
 * one channel: following and jamming hop across the E-GO's own channel list
 * and overwrite whatever channel the board was set to. */
enum ego_mode {
	EGO_MODE_FOLLOW = 0,
	EGO_MODE_CONTINUOUS_RX = 1,
	EGO_MODE_JAM = 2,
};

/* A decimal capture length in [EGO_PACKET_LENGTH_MIN, EGO_PACKET_LENGTH_MAX].
 * Returns 0 and sets *length, or -1 and leaves it alone. */
int ego_parse_packet_length(const char *text, uint8_t *length);

/* An access code as 2, 4, 6 or 8 hex digits, most significant byte first.
 * Returns 0 and sets *code (its first *length bytes), or -1 and leaves both
 * alone. <code> must have room for EGO_ACCESS_CODE_BYTES bytes. */
int ego_parse_access_code(const char *text, uint8_t *code, int *length);

#endif
