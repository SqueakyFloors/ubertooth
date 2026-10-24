/*
 * Host-test stand-in for firmware/common/ubertooth.h.
 *
 * The real header pulls in LPC17xx register definitions that do not exist on
 * the host.  Pure-logic firmware files such as bluetooth.c only need the
 * integer types and the shared USB interface definitions, so this header
 * provides exactly those, from the same sources the firmware uses.
 *
 * cc2400.h is register and strobe numbers only, so it is included as-is; the
 * two radio access functions are declared here and supplied by the test.
 */
#ifndef __UBERTOOTH_H
#define __UBERTOOTH_H

#include "../../common/types.h"
#include "../../common/cc2400.h"
#include "../../../host/libubertooth/src/ubertooth_interface.h"

u8 cc2400_status(void);
u8 cc2400_strobe(u8 reg);

#endif /* __UBERTOOTH_H */
