/*
 * Unit tests for the btbr image's USB shim,
 * firmware/btbr/src/drivers/ubertooth_usb.c.
 *
 * These run on the host. The shim reaches the USB controller only through the
 * USBHw* calls of the vendored stack and the NVIC enable registers, so this
 * test supplies both: a fake controller whose bulk IN buffers the test decides
 * to drain or leave full, and a fake interrupt controller that behaves as the
 * part does (ISER0 write-1-to-set and reads back the enable mask, ICER0
 * write-1-to-clear).
 *
 * What usb_send_sync() has to guarantee: it is the firmware's debug and fault
 * print path, and it waits for a free bulk IN buffer. Both buffers stay full
 * until the host reads them, so a host that stopped reading -- its process was
 * killed, or it never read bulk at all -- used to leave this spinning for
 * good, with the USB interrupt masked, which is how this firmware services
 * USB. The board then still enumerated and answered nothing until it was
 * power-cycled. The wait must therefore terminate, keep answering the host
 * while it runs, drop the message it could not send and count it, and leave
 * the interrupt as it found it.
 *
 * Note: a test for a loop that used to be unbounded cannot fail politely. If
 * the bound is removed, the test binary hangs rather than reporting a failure
 * -- as in test_usbhw_wait.c and test_cc2400_wait.c, which have the same
 * shape.
 */
#include "minunit.h"

#include <stdlib.h>
#include <string.h>

/* The stubs come first on the include path, so the shim gets the fake
 * hardware headers; everything else below is the firmware's own. */
#include "type.h"
#include "usbapi.h"
#include "usbhw_lpc.h"
#include <ubtbr/ubertooth_usb.h>
#include <ubtbr/btctl_intf.h>

/* The bulk endpoint's packet size. The shim keeps its own file-local copy of
 * this; test_a_message_is_cut_to_one_packet() is what holds the two together,
 * since it asserts the length the shim actually wrote. */
#define MAX_PACKET_SIZE 64

/* ---- the fake interrupt controller ---- */

static volatile uint32_t nvic_enable;   /* the enable mask, as the part holds it */
static volatile uint32_t nvic_iser;     /* the set-enable register */
static volatile uint32_t nvic_icer;     /* the clear-enable register */

/* Apply the writes made since the last access, the way the part does. */
static void nvic_settle(void)
{
	nvic_enable |= nvic_iser;
	nvic_enable &= ~nvic_icer;
	nvic_icer = 0;
	nvic_iser = nvic_enable;
}

volatile uint32_t *fake_nvic_iser(void)
{
	nvic_settle();
	return &nvic_iser;
}

volatile uint32_t *fake_nvic_icer(void)
{
	nvic_settle();
	return &nvic_icer;
}

static int usb_irq_enabled(void)
{
	nvic_settle();
	return (nvic_enable & ISER0_ISE_USB) != 0;
}

static void set_usb_irq_enabled(int enabled)
{
	nvic_settle();
	nvic_enable = enabled ? ISER0_ISE_USB : 0;
	nvic_iser = nvic_enable;
	nvic_icer = 0;
}

/* ---- the fake USB controller ---- */

/* Endpoint status the controller reports for the bulk IN endpoint. */
static U8 bulk_in_status;
/* Packets the firmware handed to the endpoint, newest last. */
static struct { U8 data[MAX_PACKET_SIZE]; int len; } writes[8];
static unsigned write_count;
/* Times the USB interrupt was polled, and whether it was masked each time. */
static unsigned isr_calls;
static unsigned isr_calls_with_irq_enabled;
/* Frees the status after this many polls of it; zero means never. */
static unsigned drain_after_polls;
static unsigned status_polls;
/* A message the fake interrupt handler sends from inside itself, the way
 * usb_bulk_out_handler() reaches die(). Zero means it sends nothing. */
static unsigned isr_sends_at_call;
static unsigned nested_sends;

static void clear_counters(void)
{
	memset(writes, 0, sizeof(writes));
	write_count = 0;
	isr_calls = 0;
	isr_calls_with_irq_enabled = 0;
	drain_after_polls = 0;
	status_polls = 0;
	isr_sends_at_call = 0;
	nested_sends = 0;
	usb_send_drops = 0;
}

static void fake_usb_reset(void)
{
	U8 prime[1] = { 0 };

	bulk_in_status = 0;
	set_usb_irq_enabled(1);
	clear_counters();
	/* The shim remembers that a host stopped reading, so that a run of
	 * messages does not pay the bound over and over. That memory is
	 * file-local to the firmware and outlives one test, and a send that gets
	 * through is what clears it -- so each test starts with one. */
	usb_send_sync(prime, sizeof(prime));
	clear_counters();
}

U8 USBHwEPGetStatus(U8 bEP)
{
	(void)bEP;
	status_polls++;
	if (drain_after_polls != 0 && status_polls >= drain_after_polls)
		bulk_in_status = 0;
	return bulk_in_status;
}

int USBHwEPWrite(U8 bEP, U8 *pbBuf, U32 iLen)
{
	(void)bEP;
	if (write_count < sizeof(writes) / sizeof(writes[0])) {
		U32 n = iLen > MAX_PACKET_SIZE ? MAX_PACKET_SIZE : iLen;

		memcpy(writes[write_count].data, pbBuf, (size_t)n);
		writes[write_count].len = (int)iLen;
	}
	write_count++;
	return (int)iLen;
}

void USBHwISR(void)
{
	++isr_calls;
	if (usb_irq_enabled())
		++isr_calls_with_irq_enabled;
	if (isr_sends_at_call != 0 && isr_calls == isr_sends_at_call) {
		U8 msg[4] = { 'x', 0, 0, 0 };

		++nested_sends;
		usb_send_sync(msg, sizeof(msg));
	}
}

/* The rest of the stack: the shim calls these from ubertooth_usb_init(), which
 * these tests do not exercise, so they only have to link. */
int USBHwEPRead(U8 bEP, U8 *pbBuf, U32 iMaxLen)
{
	(void)bEP; (void)pbBuf; (void)iMaxLen;
	return -1;
}

BOOL USBInit(void) { return TRUE; }
void USBRegisterDescriptors(U8 *pabDescriptors) { (void)pabDescriptors; }
void USBHwConnect(BOOL fConnect) { (void)fConnect; }
void USBHwRegisterEPIntHandler(U8 bEP, TFnEPIntHandler *pfnHandler)
{
	(void)bEP; (void)pfnHandler;
}
void USBRegisterRequestHandler(int iType, TFnHandleRequest *pfnHandler,
                               U8 *pbDataStore)
{
	(void)iType; (void)pfnHandler; (void)pbDataStore;
}
void USBRegisterWinusbInterface(U8 bRequest, const char *pzGuid)
{
	(void)bRequest; (void)pzGuid;
}
void get_device_serial(uint8_t *buffer, int *len)
{
	memset(buffer, 0, 17);
	buffer[0] = 1;  /* IAP failure, so the descriptor is left alone */
	*len = 17;
}

/* ---- the firmware's own dependencies ---- */

btctl_t btctl;

/* The message pool. The shim allocates through it for messages from the host,
 * and frees messages it has sent to the host. */
void *mem_pool_alloc(unsigned size)
{
	return malloc(size);
}

void mem_pool_free(void *p)
{
	free(p);
}

void cprintf(char *fmt, ...)
{
	(void)fmt;
}

void __attribute__((noreturn)) die(char *fmt, ...)
{
	(void)fmt;
	printf("  FAIL unexpected die()\n");
	exit(1);
}

/* ---- usb_send_sync(): the host is reading ---- */

/* The normal path, so the tests below mean something: with room in the
 * endpoint the message goes out on the first pass, and nothing is dropped. */
MU_TEST(test_a_message_is_sent_when_the_endpoint_has_room)
{
	U8 msg[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

	fake_usb_reset();
	usb_send_sync(msg, sizeof(msg));

	mu_assert_int_eq(1, (int)write_count);
	mu_assert_int_eq(8, writes[0].len);
	mu_assert_int_eq(1, writes[0].data[0]);
	mu_assert_int_eq(8, writes[0].data[7]);
	mu_assert_int_eq(0, (int)usb_send_drops);
}

/* A message longer than a packet is cut to one packet, as it always was. */
MU_TEST(test_a_message_is_cut_to_one_packet)
{
	U8 msg[MAX_PACKET_SIZE + 16];

	fake_usb_reset();
	memset(msg, 0xA5, sizeof(msg));
	usb_send_sync(msg, sizeof(msg));

	mu_assert_int_eq(1, (int)write_count);
	mu_assert_int_eq(MAX_PACKET_SIZE, writes[0].len);
}

/*
 * One full buffer of the endpoint's two is still room for a packet, which is
 * what usb_work() checks for as well. This used to wait for both to drain, so
 * a host reading steadily -- the normal case, one buffer in flight -- was
 * waited on as if it were not reading at all.
 */
MU_TEST(test_one_full_buffer_is_room_enough)
{
	U8 msg[4] = { 9, 0, 0, 0 };

	fake_usb_reset();
	bulk_in_status = EPSTAT_B1FULL;

	usb_send_sync(msg, sizeof(msg));

	mu_assert_int_eq(1, (int)write_count);
	mu_assert_int_eq(1, (int)status_polls);
	mu_assert_int_eq(0, (int)usb_send_drops);

	fake_usb_reset();
	bulk_in_status = EPSTAT_B2FULL;

	usb_send_sync(msg, sizeof(msg));

	mu_assert_int_eq(1, (int)write_count);
	mu_assert_int_eq(0, (int)usb_send_drops);
}

/* ---- usb_send_sync(): the host has stopped reading ---- */

/*
 * The regression. Both buffers full and nothing draining them used to hang the
 * caller in `do { epstat = USBHwEPGetStatus(...); } while (epstat & ...)`, and
 * with it every loop in the firmware. Now the wait gives up, the message is
 * dropped, and the drop is counted.
 */
MU_TEST(test_a_host_that_never_reads_does_not_hang_the_send)
{
	U8 msg[4] = { 7, 0, 0, 0 };

	fake_usb_reset();
	bulk_in_status = EPSTAT_B1FULL | EPSTAT_B2FULL;

	usb_send_sync(msg, sizeof(msg));

	mu_assert_int_eq(0, (int)write_count);
	mu_assert_int_eq(1, (int)usb_send_drops);
}

/*
 * The wait is what keeps the board answerable, so it has to service USB. With
 * the USB interrupt held off, polling it here is the only thing that does:
 * without this a control request that arrived during the wait would go
 * unanswered for the whole of it.
 */
MU_TEST(test_usb_is_serviced_while_the_send_waits)
{
	U8 msg[4] = { 7, 0, 0, 0 };

	fake_usb_reset();
	bulk_in_status = EPSTAT_B1FULL | EPSTAT_B2FULL;

	usb_send_sync(msg, sizeof(msg));

	mu_assert(isr_calls > 1, "the wait must poll the USB interrupt");
	mu_assert_int_eq(USB_SEND_SYNC_POLLS - 1, (int)isr_calls);
}

/* And it must service USB with the interrupt held off: two contexts driving
 * the one set of controller registers would each consume the other's
 * handshake. */
MU_TEST(test_the_send_holds_the_usb_interrupt_off)
{
	U8 msg[4] = { 7, 0, 0, 0 };

	fake_usb_reset();
	bulk_in_status = EPSTAT_B1FULL | EPSTAT_B2FULL;

	usb_send_sync(msg, sizeof(msg));

	mu_assert_int_eq(0, (int)isr_calls_with_irq_enabled);
	mu_assert(usb_irq_enabled(), "the interrupt must be put back afterwards");
}

/*
 * A host that comes back part way through the wait is served: the wait is a
 * bound on how long the firmware will hold on, not a reason to give up early.
 */
MU_TEST(test_a_host_that_starts_reading_is_served)
{
	U8 msg[4] = { 5, 0, 0, 0 };

	fake_usb_reset();
	bulk_in_status = EPSTAT_B1FULL | EPSTAT_B2FULL;
	drain_after_polls = 50;

	usb_send_sync(msg, sizeof(msg));

	mu_assert_int_eq(1, (int)write_count);
	mu_assert_int_eq(5, writes[0].data[0]);
	mu_assert_int_eq(0, (int)usb_send_drops);
}

/* Every dropped message is counted, so the counter means what it says. */
MU_TEST(test_every_dropped_message_is_counted)
{
	U8 msg[4] = { 7, 0, 0, 0 };

	fake_usb_reset();
	bulk_in_status = EPSTAT_B1FULL | EPSTAT_B2FULL;

	usb_send_sync(msg, sizeof(msg));
	usb_send_sync(msg, sizeof(msg));
	usb_send_sync(msg, sizeof(msg));

	mu_assert_int_eq(3, (int)usb_send_drops);
}

/*
 * The point of a bound is that the firmware is still there afterwards: a host
 * that stops reading and then reads again must be served with no intervention.
 */
MU_TEST(test_a_send_works_again_after_a_drop)
{
	U8 msg[4] = { 3, 0, 0, 0 };

	fake_usb_reset();
	bulk_in_status = EPSTAT_B1FULL | EPSTAT_B2FULL;
	usb_send_sync(msg, sizeof(msg));
	mu_assert_int_eq(1, (int)usb_send_drops);

	bulk_in_status = 0;
	usb_send_sync(msg, sizeof(msg));

	mu_assert_int_eq(1, (int)write_count);
	mu_assert_int_eq(1, (int)usb_send_drops);
	mu_assert(usb_irq_enabled(), "the interrupt must be put back afterwards");
}

/*
 * A caller printing a run of lines pays the bound once, not per line. The
 * fault handlers dump the stack 16 bytes at a time, with all interrupts
 * masked, so a few hundred lines each waiting the full bound would be the
 * stall over again in slow motion.
 */
MU_TEST(test_a_drop_does_not_make_the_next_message_wait)
{
	U8 msg[4] = { 7, 0, 0, 0 };

	fake_usb_reset();
	bulk_in_status = EPSTAT_B1FULL | EPSTAT_B2FULL;
	usb_send_sync(msg, sizeof(msg));
	mu_assert_int_eq(1, (int)usb_send_drops);

	clear_counters();
	usb_send_sync(msg, sizeof(msg));

	mu_assert_int_eq(1, (int)status_polls);
	mu_assert_int_eq(0, (int)write_count);
	mu_assert_int_eq(1, (int)usb_send_drops);
}

/* And it is only remembered until a message gets through: a host that comes
 * back is waited for again, as it was before it left. */
MU_TEST(test_a_message_that_gets_through_is_waited_for_again)
{
	U8 msg[4] = { 8, 0, 0, 0 };

	fake_usb_reset();
	bulk_in_status = EPSTAT_B1FULL | EPSTAT_B2FULL;
	usb_send_sync(msg, sizeof(msg));

	bulk_in_status = 0;
	usb_send_sync(msg, sizeof(msg));

	clear_counters();
	bulk_in_status = EPSTAT_B1FULL | EPSTAT_B2FULL;
	usb_send_sync(msg, sizeof(msg));

	mu_assert_int_eq(USB_SEND_SYNC_POLLS, (int)status_polls);
}

/*
 * The interrupt is put back the way it was found, not simply switched on. The
 * old code ended with `ISER0 = ICER0_ICE_USB`, which enabled it for a caller
 * that ran before ubertooth_usb_init() had -- early_printf() is reachable that
 * early, and from the fault handlers.
 */
MU_TEST(test_a_send_with_the_interrupt_off_leaves_it_off)
{
	U8 msg[4] = { 2, 0, 0, 0 };

	fake_usb_reset();
	set_usb_irq_enabled(0);

	usb_send_sync(msg, sizeof(msg));

	mu_assert_int_eq(1, (int)write_count);
	mu_assert(!usb_irq_enabled(), "an interrupt that was off must stay off");
}

/*
 * Servicing USB during the wait reaches usb_bulk_out_handler(), which DIE()s
 * on a message it cannot make sense of, and die() prints through here -- so
 * this can be entered from inside itself. The nested call must not start a
 * second wait: nested waits would multiply the bound, and the recursion goes
 * through the fault path.
 */
MU_TEST(test_a_nested_send_does_not_wait_again)
{
	U8 msg[4] = { 4, 0, 0, 0 };

	fake_usb_reset();
	bulk_in_status = EPSTAT_B1FULL | EPSTAT_B2FULL;
	isr_sends_at_call = 3;

	usb_send_sync(msg, sizeof(msg));

	mu_assert_int_eq(1, (int)nested_sends);
	/* the outer wait's polls, and one for the nested call */
	mu_assert_int_eq(USB_SEND_SYNC_POLLS + 1, (int)status_polls);
	mu_assert_int_eq(2, (int)usb_send_drops);
	mu_assert(usb_irq_enabled(), "the interrupt must be put back afterwards");
}

/* A nested call still sends, when there is room for it: a fault dump is worth
 * more than the tidiness of refusing it. */
MU_TEST(test_a_nested_send_with_room_is_sent)
{
	U8 msg[4] = { 6, 0, 0, 0 };

	fake_usb_reset();
	isr_sends_at_call = 1;

	usb_send_sync(msg, sizeof(msg));

	mu_assert_int_eq(1, (int)nested_sends);
	mu_assert_int_eq(2, (int)write_count);
	mu_assert_int_eq(0, (int)usb_send_drops);
}

/* ---- usb_work() ---- */

/* usb_work() never waited for room -- it skips the write, which is the shape
 * usb_send_sync() now has too -- but it enabled the USB interrupt on the way
 * out whatever it found. */
MU_TEST(test_usb_work_with_the_interrupt_off_leaves_it_off)
{
	fake_usb_reset();
	set_usb_irq_enabled(0);

	mu_assert_int_eq(0, usb_work());
	mu_assert(!usb_irq_enabled(), "an interrupt that was off must stay off");
}

/* A full endpoint is no work and no write, with the interrupt put back. */
MU_TEST(test_usb_work_with_a_full_endpoint_does_nothing)
{
	fake_usb_reset();
	bulk_in_status = EPSTAT_B1FULL | EPSTAT_B2FULL;

	mu_assert_int_eq(0, usb_work());
	mu_assert_int_eq(0, (int)write_count);
	mu_assert(usb_irq_enabled(), "the interrupt must be put back afterwards");
}

/* A queued message goes out with its four-byte header, and is freed. */
MU_TEST(test_usb_work_sends_a_queued_message)
{
	msg_t *msg;
	U8 *p;

	fake_usb_reset();
	msg_queue_init(&btctl.tx_q);
	msg = msg_alloc(3);
	p = msg_put(msg, 3);
	p[0] = 0xAA;
	p[1] = 0xBB;
	p[2] = 0xCC;
	mu_assert_int_eq(0, btctl_tx_enqueue(msg));

	mu_assert_int_eq(1, usb_work());
	mu_assert_int_eq(1, (int)write_count);
	mu_assert_int_eq(7, writes[0].len);
	mu_assert_int_eq(3, writes[0].data[2]);   /* length, low byte */
	mu_assert_int_eq(0, writes[0].data[3]);   /* length, high byte */
	mu_assert_int_eq(0xAA, writes[0].data[4]);
	mu_assert_int_eq(0xCC, writes[0].data[6]);
	mu_assert(usb_irq_enabled(), "the interrupt must be put back afterwards");
}

int main(void)
{
	printf("btbr USB shim send tests\n");
	MU_RUN(test_a_message_is_sent_when_the_endpoint_has_room);
	MU_RUN(test_a_message_is_cut_to_one_packet);
	MU_RUN(test_one_full_buffer_is_room_enough);
	MU_RUN(test_a_host_that_never_reads_does_not_hang_the_send);
	MU_RUN(test_usb_is_serviced_while_the_send_waits);
	MU_RUN(test_the_send_holds_the_usb_interrupt_off);
	MU_RUN(test_a_host_that_starts_reading_is_served);
	MU_RUN(test_every_dropped_message_is_counted);
	MU_RUN(test_a_send_works_again_after_a_drop);
	MU_RUN(test_a_drop_does_not_make_the_next_message_wait);
	MU_RUN(test_a_message_that_gets_through_is_waited_for_again);
	MU_RUN(test_a_send_with_the_interrupt_off_leaves_it_off);
	MU_RUN(test_a_nested_send_does_not_wait_again);
	MU_RUN(test_a_nested_send_with_room_is_sent);
	MU_RUN(test_usb_work_with_the_interrupt_off_leaves_it_off);
	MU_RUN(test_usb_work_with_a_full_endpoint_does_nothing);
	MU_RUN(test_usb_work_sends_a_queued_message);
	return mu_report();
}
