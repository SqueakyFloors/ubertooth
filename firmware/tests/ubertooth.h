/*
 * Minimal hardware-free definitions used when bluetooth.c is compiled as a
 * host unit test.  The production firmware gets these definitions from
 * common/ubertooth.h and host/libubertooth/src/ubertooth_interface.h.
 */
#ifndef __UBERTOOTH_H
#define __UBERTOOTH_H

#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

#define DMA_SIZE 50
#define NUM_BREDR_CHANNELS 79

typedef struct {
	uint64_t address;
	uint64_t syncword;
} bdaddr;

#endif /* __UBERTOOTH_H */
