/*
 * Register reads for ubertooth-debug. See debug_registers.h.
 */
#include "debug_registers.h"
#include "cc2400.h"

int debug_read_registers(debug_read_register_fn read_register, void *context,
                         const int *regs, int n, int verbose,
                         FILE *out, FILE *err)
{
	int status = 0;
	int i;

	for (i = 0; i < n; i++) {
		int value = read_register(context, (uint8_t)regs[i]);

		if (value < 0) {
			if (value != DEBUG_ERROR_SHORT_REPLY)
				fprintf(err, "error: register 0x%02x read failed\n", regs[i]);
			status = 1;
			continue;
		}
		cc2400_decode(out, regs[i], (unsigned short)value, verbose);
	}
	return status;
}
