/*
 * Host-test stand-in for firmware/btbr/include/ubtbr/debug.h.
 *
 * The real header pulls in the console and USB headers and an ARM inline-asm
 * helper, none of which exist on the host.  The btbr sources tested here need
 * only the two diagnostics below, which the test implements.
 */
#ifndef __SC_DEBUG_H
#define __SC_DEBUG_H

void cprintf(char *fmt, ...);
void __attribute__((noreturn)) die(char *fmt, ...);
#define DIE(fmt, ...)	die("DIE|"fmt"\n", ##__VA_ARGS__)

/* The baseband diagnostic, off in the firmware too. */
#define BB_DEBUG(fmt, ...)

#endif /* __SC_DEBUG_H */
