/*
 * Host-test stand-in for firmware/common/ubertooth.h.
 *
 * The real header pulls in LPC17xx register definitions that do not exist on
 * the host.  Pure-logic firmware files such as bluetooth.c only need the
 * integer types and the shared USB interface definitions, so this header
 * provides exactly those, from the same sources the firmware uses.
 */
#ifndef __UBERTOOTH_H
#define __UBERTOOTH_H

#include "../../common/types.h"
#include "../../../host/libubertooth/src/ubertooth_interface.h"

#endif /* __UBERTOOTH_H */
