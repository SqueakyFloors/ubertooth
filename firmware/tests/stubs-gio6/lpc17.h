/*
 * A fake LPC17xx register file, for the tests that ask which GPIO port the
 * GIO6 macros reach for on each board.
 *
 * The real firmware/common/lpc17.h turns each register name into a write to a
 * fixed address, which on the host is a segmentation fault. This header
 * includes the real one -- so every bit definition under test is the part's
 * own number, not a copy that could drift -- and then replaces the registers
 * these tests touch with ordinary variables. A register the tests never name
 * keeps the real address and is never read or written.
 *
 * The registers come as arrays indexed by port number, because that is the
 * question these tests ask: a test writes through a GIO6 macro and then looks
 * at every port to see which one moved.
 */
#ifndef UBERTOOTH_FW_TESTS_FAKE_LPC17_GIO6_H
#define UBERTOOTH_FW_TESTS_FAKE_LPC17_GIO6_H

#include <stdint.h>

#include "../../common/lpc17.h"

/* The part's five GPIO ports: the pin value a read returns, and the set and
 * clear registers a write goes to. */
#define FAKE_GPIO_PORTS 5
extern volatile uint32_t fake_fio_pin[FAKE_GPIO_PORTS];
extern volatile uint32_t fake_fio_set[FAKE_GPIO_PORTS];
extern volatile uint32_t fake_fio_clr[FAKE_GPIO_PORTS];

#undef FIO0PIN
#undef FIO1PIN
#undef FIO2PIN
#undef FIO3PIN
#undef FIO4PIN
#undef FIO0SET
#undef FIO1SET
#undef FIO2SET
#undef FIO3SET
#undef FIO4SET
#undef FIO0CLR
#undef FIO1CLR
#undef FIO2CLR
#undef FIO3CLR
#undef FIO4CLR
#define FIO0PIN fake_fio_pin[0]
#define FIO1PIN fake_fio_pin[1]
#define FIO2PIN fake_fio_pin[2]
#define FIO3PIN fake_fio_pin[3]
#define FIO4PIN fake_fio_pin[4]
#define FIO0SET fake_fio_set[0]
#define FIO1SET fake_fio_set[1]
#define FIO2SET fake_fio_set[2]
#define FIO3SET fake_fio_set[3]
#define FIO4SET fake_fio_set[4]
#define FIO0CLR fake_fio_clr[0]
#define FIO1CLR fake_fio_clr[1]
#define FIO2CLR fake_fio_clr[2]
#define FIO3CLR fake_fio_clr[3]
#define FIO4CLR fake_fio_clr[4]

/*
 * The GPIO interrupt registers, which the part has for ports 0 and 2 only
 * (LPC176x/5x user manual UM10360, section 9.5). Slots 1, 3 and 4 exist here
 * so a test can index by port without a special case; nothing ever writes
 * them, because the real header has no name that would reach them.
 */
extern volatile uint32_t fake_io_int_clr[FAKE_GPIO_PORTS];
extern volatile uint32_t fake_io_int_en_f[FAKE_GPIO_PORTS];
extern volatile uint32_t fake_io_int_en_r[FAKE_GPIO_PORTS];

#undef IO0IntClr
#undef IO2IntClr
#undef IO0IntEnF
#undef IO2IntEnF
#undef IO0IntEnR
#undef IO2IntEnR
#define IO0IntClr fake_io_int_clr[0]
#define IO2IntClr fake_io_int_clr[2]
#define IO0IntEnF fake_io_int_en_f[0]
#define IO2IntEnF fake_io_int_en_f[2]
#define IO0IntEnR fake_io_int_en_r[0]
#define IO2IntEnR fake_io_int_en_r[2]

/* The interrupt controller, which the carrier-sense trigger turns EINT3 on
 * and off in. */
extern volatile uint32_t fake_iser0;
extern volatile uint32_t fake_icer0;

#undef ISER0
#undef ICER0
#define ISER0 fake_iser0
#define ICER0 fake_icer0

#endif /* UBERTOOTH_FW_TESTS_FAKE_LPC17_GIO6_H */
