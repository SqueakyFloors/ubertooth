/*
 * Register reads for ubertooth-debug, kept free of libusb so they can be
 * unit-tested with a fake reader.
 */
#ifndef UBERTOOTH_DEBUG_REGISTERS_H
#define UBERTOOTH_DEBUG_REGISTERS_H

#include <stdint.h>
#include <stdio.h>

/* Reads one register; returns its 16-bit value (>= 0) or a negative error. */
typedef int (*debug_read_register_fn)(void *context, uint8_t reg);

/* libubertooth's UBERTOOTH_ERROR_SHORT_REPLY, repeated so this module stays
 * free of libusb headers; ubertooth-debug.c checks they match. */
#define DEBUG_ERROR_SHORT_REPLY (-1000)

/* Read and decode every register in regs[0..n-1] to out, in order. A failed
 * read prints "error: register 0x<reg> read failed" to err (nothing for a
 * short reply, whose whole line libubertooth already printed), and the
 * remaining registers are still read. Returns 0 if every read succeeded,
 * otherwise 1. */
int debug_read_registers(debug_read_register_fn read_register, void *context,
                         const int *regs, int n, int verbose,
                         FILE *out, FILE *err);

/* One "<reg>=<value>" from -w. */
typedef struct {
	uint8_t reg;
	uint16_t value;
} debug_register_write;

/* Every CC2400 register written once is the most a single -w can usefully
 * ask for, so the list has a fixed bound and needs no allocation. */
#define DEBUG_REGISTER_WRITES_MAX 256

/* Writes one register; returns 0 or a negative error. */
typedef int (*debug_write_register_fn)(void *context, uint8_t reg,
                                       uint16_t value);

/* Parse "<reg>=<value>[,<reg>=<value>[,...]]" into writes[0..max-1], each
 * register an address 0x00-0xff or a %name as -r accepts and each value
 * 0x0000-0xffff. Returns how many were parsed, or -1 after printing one
 * "error: ..." line to err naming the item as the user wrote it. spec is read
 * and not modified. */
int debug_parse_register_writes(const char *spec, debug_register_write *writes,
                                int max, FILE *err);

/* Write writes[0..n-1] in the order given. A failed write prints
 * "error: register 0x<reg> write failed" to err and the remaining registers
 * are still written, so one refused register does not hide the rest. Returns
 * 0 if every write succeeded, otherwise 1. */
int debug_write_registers(debug_write_register_fn write_register, void *context,
                          const debug_register_write *writes, int n,
                          FILE *err);

#endif
