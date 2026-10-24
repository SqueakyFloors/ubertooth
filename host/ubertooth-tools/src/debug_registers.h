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

#endif
