/*
 * The firmware's queue of packets waiting for the host, in
 * firmware/bluetooth_rxtx/ubertooth_usb.c.
 *
 * A capture must hold only packets from the mode it started. The queue was
 * cleared when a capture mode started and never when one stopped, so a mode
 * that ended with packets no host had collected left them in the ring, and the
 * next mode handed them over before its own packets arrived.
 *
 * Clearing the ring when the mode stops is the half the board owns, and it is
 * the larger half: the ring holds over a hundred packets. The bulk IN
 * endpoint's two hardware buffers are the other half -- a packet handed to
 * them is past the ring, which is why the symptom was always exactly two stale
 * packets at the head of the next capture. The last test pins that boundary:
 * the board cannot reach those two, and the host clears them as it starts
 * reading.
 *
 * The queue is pure logic, so it runs here for real: ubertooth_usb.c is
 * compiled into this translation unit and the endpoint below stands in for the
 * hardware -- two buffers, filled by the firmware, emptied by the host.
 */
#include <stdio.h>
#include <string.h>

#include "minunit.h"

/* ------------------------------------------------------------------ *
 * The bulk IN endpoint: two buffers, as the LPC17xx has.
 * ------------------------------------------------------------------ */

#define EP_BUFFERS 2
#define EP_BUF_LEN 64

struct ep_buffer {
	int full;
	int len;
	unsigned char data[EP_BUF_LEN];
};

static struct ep_buffer ep[EP_BUFFERS];

static void ep_reset(void)
{
	memset(ep, 0, sizeof(ep));
}

/* What the host sees when it reads the endpoint: the oldest buffer, or -1 when
 * both are empty. The byte returned is the packet's pkt_type, which is what
 * tells a stale packet from a fresh one. */
static int ep_host_read(void)
{
	int i, type;

	for (i = 0; i < EP_BUFFERS; i++) {
		if (!ep[i].full)
			continue;
		type = ep[i].data[0];
		memmove(&ep[i], &ep[i + 1],
		        sizeof(ep[0]) * (size_t)(EP_BUFFERS - 1 - i));
		memset(&ep[EP_BUFFERS - 1], 0, sizeof(ep[0]));
		return type;
	}
	return -1;
}

/* ------------------------------------------------------------------ *
 * The lpcusb calls ubertooth_usb.c makes. Only the two that touch the bulk IN
 * endpoint do anything.
 * ------------------------------------------------------------------ */

#include "usbapi.h"
#include "usbhw_lpc.h"

int USBHwEPWrite(U8 bEP, U8 *pbBuf, U32 iLen)
{
	int i;

	(void)bEP;
	for (i = 0; i < EP_BUFFERS; i++) {
		if (ep[i].full)
			continue;
		ep[i].full = 1;
		ep[i].len = (int)iLen;
		memcpy(ep[i].data, pbBuf, iLen < EP_BUF_LEN ? iLen : EP_BUF_LEN);
		return (int)iLen;
	}
	/* The firmware only writes when a buffer is free, so reaching this would
	 * mean handle_usb() had stopped checking. */
	mu_assert(0, "the firmware wrote to a full endpoint");
	return 0;
}

U8 USBHwEPGetStatus(U8 bEP)
{
	U8 status = 0;

	(void)bEP;
	if (ep[0].full)
		status |= EPSTAT_B1FULL;
	if (ep[1].full)
		status |= EPSTAT_B2FULL;
	return status;
}

void USBHwEPStall(U8 bEP, BOOL fStall)
{
	(void)bEP;
	(void)fStall;
	mu_assert(0, "the firmware set the bulk IN endpoint's stall bit");
}

void USBInit(void) {}
void USBRegisterDescriptors(const U8 *d) { (void)d; }
void USBRegisterRequestHandler(int t, TFnHandleRequest *h, U8 *s)
{
	(void)t; (void)h; (void)s;
}
void USBRegisterWinusbInterface(U8 r, const char *g) { (void)r; (void)g; }
void USBHwConnect(BOOL c) { (void)c; }
void USBHwISR(void) {}

void get_device_serial(uint8_t *buffer, int *len)
{
	memset(buffer, 0, 17);
	*len = 17;
}

/* The NVIC, as stubs-usbq/ubertooth.h names it. */
uint32_t fake_iser0;
uint32_t fake_icer0;

/* The queue under test, compiled into this file so its statics are reachable
 * and nothing has to be linked from the firmware image. */
#include "ubertooth_usb.c"

/* ------------------------------------------------------------------ *
 * Helpers
 * ------------------------------------------------------------------ */

/* Queue `count` packets of one type, as a capture mode does. */
static void capture(int type, int count)
{
	int i;

	for (i = 0; i < count; i++) {
		usb_pkt_rx *p = usb_enqueue();

		mu_assert(p != NULL, "the queue filled up");
		p->pkt_type = (uint8_t)type;
	}
}

/* Run the firmware's USB service until it stops handing packets over: that is
 * what keeps the endpoint loaded while a mode runs. clkn stays small so the
 * keep-alive timer never fires. */
static void service(void)
{
	int i;

	for (i = 0; i < 4; i++)
		handle_usb(1);
}

/* Everything the host can still read, oldest first, as pkt_type values. */
static int drain(int *types, int max)
{
	int count = 0, type;

	while (count < max) {
		service();
		type = ep_host_read();
		if (type < 0)
			break;
		types[count++] = type;
	}
	return count;
}

static void start_fresh(void)
{
	ep_reset();
	usb_queue_init();
	fake_iser0 = 0;
	fake_icer0 = 0;
}

/*
 * What the host does as it starts reading. Clearing the halt feature reaches
 * the controller as "write zero to the endpoint's stall bit", and the LPC17xx
 * re-initializes the endpoint on that write whether or not it was stalled,
 * which flushes both buffers (user manual UM10360, Set Endpoint Status, bit
 * ST). That is the controller's behaviour, not this code's, so here it is a
 * model: these tests pin which packets each half of the fix is responsible
 * for, and a board is what shows the flush really happens.
 */
static void host_clear_halt(void)
{
	ep_reset();
}

/* ------------------------------------------------------------------ *
 * Tests
 * ------------------------------------------------------------------ */

/*
 * The whole point: once the stopped mode's queue has gone and the host has
 * cleared the pipe, the new mode's first read returns only the new mode's
 * packets.
 */
MU_TEST(a_new_mode_reads_only_its_own_packets)
{
	int types[160];
	int i, count;

	start_fresh();

	/* A BLE capture runs and the host collects nothing. */
	capture(LE_PACKET, 5);
	service();

	/* It stops, and the host starts a basic-rate capture. */
	usb_queue_init();
	host_clear_halt();
	capture(BR_PACKET, 3);
	count = drain(types, 160);

	mu_assert_int_eq(3, count);
	for (i = 0; i < count; i++) {
		char msg[128];

		snprintf(msg, sizeof(msg),
		         "packet %d of the new capture has pkt_type %d, from the "
		         "mode that stopped", i, types[i]);
		mu_assert(types[i] == BR_PACKET, msg);
	}
}

/*
 * Clearing the ring when the mode stops is what keeps the backlog out of the
 * next capture. Without it the next mode's reader got everything the previous
 * mode had run up, which is far more than the two the endpoint holds.
 */
MU_TEST(stopping_a_mode_drops_its_backlog)
{
	int types[160];
	int count, i, le = 0;

	start_fresh();

	capture(LE_PACKET, 100);
	service();

	usb_queue_init();
	host_clear_halt();
	capture(BR_PACKET, 4);
	count = drain(types, 160);

	for (i = 0; i < count; i++)
		if (types[i] == LE_PACKET)
			le++;
	mu_assert_int_eq(4, count);
	mu_assert_int_eq(0, le);
}

/*
 * ...and a mode that leaves its backlog behind is what the bug looked like,
 * so this is the state the fix has to change.
 */
MU_TEST(a_mode_that_keeps_its_backlog_hands_it_over)
{
	int types[160];
	int count;

	start_fresh();

	capture(LE_PACKET, 100);
	service();

	/* No clear on stop, as the firmware used to do. The host clearing the
	 * pipe takes the two in the endpoint and nothing else, so the ring's 98
	 * still arrive in front of the new capture. */
	host_clear_halt();
	capture(BR_PACKET, 4);
	count = drain(types, 160);

	mu_assert_int_eq(102, count);
	mu_assert_int_eq(LE_PACKET, types[0]);
}

/*
 * The boundary between the two halves of the fix: how many packets the board
 * cannot reach once a mode has stopped, and where they are. It is one for each
 * endpoint buffer, in front of the new capture -- the count in the bug report.
 * They go when the host clears the pipe.
 */
MU_TEST(the_endpoint_keeps_one_packet_for_each_buffer)
{
	int types[160];

	start_fresh();

	capture(LE_PACKET, 100);
	service();
	usb_queue_init();

	mu_assert_int_eq(EPSTAT_B1FULL | EPSTAT_B2FULL, USBHwEPGetStatus(0x82));
	mu_assert_int_eq(LE_PACKET, ep_host_read());
	mu_assert_int_eq(LE_PACKET, ep_host_read());
	mu_assert_int_eq(-1, ep_host_read());

	/* And with the pipe cleared instead, nothing is left at all. */
	start_fresh();
	capture(LE_PACKET, 100);
	service();
	usb_queue_init();
	host_clear_halt();

	mu_assert_int_eq(0, drain(types, 160));
}

/*
 * The keep-alive timer is part of what a stopped mode leaves behind. It holds
 * a clock reading from that mode, and leaving a mode restarts the clock at
 * zero, so the elapsed time the board computes wraps and reads as a long
 * silence: it answered the next capture's first read with a keep-alive before
 * that capture's mode had started. One byte on the wire, but the host turns
 * the endpoint's whole buffer into a record, so a dump opened with a record of
 * nothing.
 */
MU_TEST(a_stopped_modes_keep_alive_timer_does_not_follow_it)
{
	start_fresh();

	/* A capture runs for a while and the host collects everything. */
	capture(LE_PACKET, 2);
	handle_usb(900000);
	mu_assert_int_eq(LE_PACKET, ep_host_read());
	mu_assert_int_eq(LE_PACKET, ep_host_read());

	/* It stops: the ring goes, and so does the host's pipe. */
	usb_queue_init();
	host_clear_halt();

	/* The clock restarts at zero and the board is serviced in idle, with
	 * nothing queued. Nothing is owed to the host yet. */
	handle_usb(1);
	handle_usb(2);
	mu_assert_int_eq(0, USBHwEPGetStatus(0x82));
	mu_assert_int_eq(-1, ep_host_read());
}

/* Clearing the ring leaves a working queue behind, not a wedged one. */
MU_TEST(the_queue_still_works_after_a_clear)
{
	usb_pkt_rx *p;

	start_fresh();

	capture(LE_PACKET, 7);
	service();
	usb_queue_init();

	mu_assert(dequeue() == NULL, "the ring was expected to be empty");

	p = usb_enqueue();
	mu_assert(p != NULL, "the queue refused a packet after a clear");
	p->pkt_type = BR_PACKET;
	p = dequeue();
	mu_assert(p != NULL, "the queue lost the packet after a clear");
	mu_assert_int_eq(BR_PACKET, p->pkt_type);
}

int main(void)
{
	MU_RUN(a_new_mode_reads_only_its_own_packets);
	MU_RUN(stopping_a_mode_drops_its_backlog);
	MU_RUN(a_mode_that_keeps_its_backlog_hands_it_over);
	MU_RUN(the_endpoint_keeps_one_packet_for_each_buffer);
	MU_RUN(a_stopped_modes_keep_alive_timer_does_not_follow_it);
	MU_RUN(the_queue_still_works_after_a_clear);

	return mu_report();
}
