/*
 * Host-test stand-in for firmware/common/ubertooth.h.
 *
 * The real header pulls in LPC17xx register definitions that do not exist on
 * the host.  Pure-logic firmware files such as bluetooth.c only need the
 * integer types and the shared USB interface definitions, so this header
 * provides exactly those, from the same sources the firmware uses.
 *
 * Tests whose file under test reaches the radio use stubs-radio/ubertooth.h,
 * which is this header plus the CC2400 definitions.
 */
#ifndef __UBERTOOTH_H
#define __UBERTOOTH_H

#include "../../common/types.h"
#include "../../../host/libubertooth/src/ubertooth_interface.h"

/* On the board this puts a buffer in the part's second RAM bank. A host test
 * has one address space and no linker script to say so, so it means nothing
 * here. */
#define AHB_SRAM

#endif /* __UBERTOOTH_H */
