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
 *
 * Not every host reads that endpoint. A mode whose host collects packets with
 * UBERTOOTH_POLL control requests leaves them in the ring, where the request
 * finds them, and the endpoint's two buffers are then a place packets go to be
 * lost: those modes used to lose the first two packets of every session there.
 * The last part of this file scans the mode loops for which of the two USB
 * services each one calls, since that is where the rule has to hold.
 */
#include <dirent.h>
#include <stdio.h>
#include <string.h>

#include "minunit.h"
#include "source_scan.h"

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

/* ------------------------------------------------------------------ *
 * A mode whose host polls: the endpoint stays out of it
 * ------------------------------------------------------------------ */

/*
 * UBERTOOTH_POLL answers out of the ring, so a mode whose host polls has to
 * leave every packet there. handle_usb_control() services the control path and
 * touches neither the ring nor the endpoint, which is what makes the first
 * packets of the session reachable.
 */
MU_TEST(a_polled_mode_keeps_every_packet_for_the_host)
{
	int i;

	start_fresh();

	capture(LE_PACKET, 5);
	for (i = 0; i < 4; i++)
		handle_usb_control();

	mu_assert_int_eq(0, USBHwEPGetStatus(0x82));
	mu_assert_int_eq(-1, ep_host_read());

	/* What the host's poll requests find: all five, the first included. */
	for (i = 0; i < 5; i++) {
		usb_pkt_rx *p = dequeue();
		char msg[128];

		snprintf(msg, sizeof(msg), "packet %d of the session was gone "
		         "before the host could poll for it", i);
		mu_assert(p != NULL, msg);
		mu_assert_int_eq(LE_PACKET, p->pkt_type);
	}
	mu_assert(dequeue() == NULL, "the ring held more than was captured");
}

/*
 * ...and the bulk service is what used to take them. It loads both endpoint
 * buffers, so a polling host was left the session minus its first two packets.
 * This is the state the fix changed.
 */
MU_TEST(the_bulk_service_takes_the_first_packet_of_each_buffer)
{
	int i, left = 0;

	start_fresh();

	capture(LE_PACKET, 5);
	service();

	mu_assert_int_eq(EPSTAT_B1FULL | EPSTAT_B2FULL, USBHwEPGetStatus(0x82));
	for (i = 0; i < 5; i++)
		if (dequeue() != NULL)
			left++;
	mu_assert_int_eq(3, left);
}

/* ------------------------------------------------------------------ *
 * Which modes feed the bulk IN endpoint
 * ------------------------------------------------------------------ */

/*
 * Each half of the rule belongs to a mode loop: a mode whose host reads the
 * bulk endpoint has to keep it loaded, and a mode whose host polls must not
 * touch it. The loops are hardware code and cannot run here, so this scans
 * them as text, the way test_le_phy_order.c scans le_phy.c.
 *
 * Two of the three USB services feed the endpoint: handle_usb(), and
 * service_usb(), which is handle_usb() with the clock filled in. The third,
 * handle_usb_control(), does not. service_usb is also handed to the bounded
 * radio waits by name, so the scan matches the identifier rather than a call.
 *
 * Listed below is every mode whose host collects packets. The modes that
 * stream nothing -- the transmit tests, the range test, the repeater, the LED
 * spectrum display, xmas and identify -- are free to call either.
 */
enum usb_service {
	USB_BULK,      /* keeps the bulk IN endpoint loaded */
	USB_CONTROL,   /* services control requests from the loop, nothing else */
	USB_INTERRUPT, /* unmasks the USB interrupt and services USB there */
};

/* What one function's body was found to name. */
struct fn_usb {
	int lines;
	int bulk;     /* handle_usb or service_usb, named anywhere */
	int direct;   /* ...called outright, rather than handed to something */
	int control;  /* handle_usb_control, called */
	int unmask;   /* ISER0 = ISER0_ISE_USB */
};

struct mode_usb {
	const char *file;
	const char *function;
	enum usb_service service;
	/* Where an interrupt-driven mode unmasks the interrupt: the mode itself
	 * for the two BLE halves, ego_init() for ego. */
	const char *unmask_in;
	const char *host;
	/* filled in by the scan */
	struct fn_usb found;
	struct fn_usb unmask_found;
};

static struct mode_usb modes[] = {
	{ "bluetooth_rxtx.c", "bt_stream_rx", USB_BULK, NULL,
	  "ubertooth-rx, -dump, -scan, -follow and -afh read the endpoint",
	  { 0 }, { 0 } },
	{ "bluetooth_rxtx.c", "specan", USB_BULK, NULL,
	  "ubertooth-specan reads the endpoint",
	  { 0 }, { 0 } },
	{ "bluetooth_rxtx.c", "rx_generic_sync", USB_BULK, NULL,
	  "generic receive streams to the endpoint",
	  { 0 }, { 0 } },
	{ "le_phy.c", "le_phy_main", USB_CONTROL, NULL,
	  "ubertooth-btle -n and -f poll",
	  { 0 }, { 0 } },
	{ "bluetooth_rxtx.c", "bt_generic_le", USB_INTERRUPT, "bt_generic_le",
	  "ubertooth-btle -p polls",
	  { 0 }, { 0 } },
	{ "bluetooth_rxtx.c", "bt_le_sync", USB_INTERRUPT, "bt_le_sync",
	  "ubertooth-btle -p polls",
	  { 0 }, { 0 } },
	{ "ego.c", "ego_main", USB_INTERRUPT, "ego_init",
	  "ubertooth-ego polls",
	  { 0 }, { 0 } },
};

#define MODE_COUNT (sizeof(modes) / sizeof(modes[0]))

/*
 * Read one function's body out of a firmware source file. A definition is a
 * line that starts in column 1, names the function in front of its first '('
 * and does not end the statement, which is what tells it from a prototype or a
 * call; the body ends at the next closing brace in column 1.
 */
static int scan_function(const char *path, const char *function,
                         struct fn_usb *out)
{
	char line[512], name[64];
	int in_block = 0, in_fn = 0;
	FILE *f = fopen(path, "r");

	if (f == NULL)
		return 0;

	memset(out, 0, sizeof(*out));

	while (fgets(line, sizeof(line), f) != NULL) {
		strip_comments(line, &in_block);

		if (!in_fn) {
			if (line[0] != ' ' && line[0] != '\t' &&
			    strchr(line, ';') == NULL &&
			    call_name(line, name, sizeof(name)) &&
			    strcmp(name, function) == 0)
				in_fn = 1;
			continue;
		}

		out->lines++;
		/* handle_usb_control is a whole identifier of its own, so a match
		 * on handle_usb is the bulk service and nothing else. */
		if (contains_identifier(line, "handle_usb") ||
		    contains_identifier(line, "service_usb"))
			out->bulk++;
		/*
		 * A bare mention is not enough to keep a stream fed: the bounded
		 * radio waits and hop() take service_usb as a callback, so a loop
		 * could name it on a path it rarely takes -- hop() runs only when
		 * the mode hops -- and never service USB on its ordinary one. What
		 * answers for the ordinary path is a call.
		 */
		if (calls_identifier(line, "service_usb") ||
		    calls_identifier(line, "handle_usb"))
			out->direct++;
		/* A call, for the same reason: naming the control service in a
		 * callback the loop rarely reaches would not keep the host's
		 * requests answered. */
		if (calls_identifier(line, "handle_usb_control"))
			out->control++;
		if (is_assignment_to(line, "ISER0") &&
		    contains_identifier(line, "ISER0_ISE_USB"))
			out->unmask++;

		if (line[0] == '}')
			in_fn = 0;
	}

	fclose(f);
	return 1;
}

/*
 * The scan above reads one function body at a time, so it would miss a bulk
 * service reached from a helper: le_cc2400_init_rf() hands the bounded radio
 * waits a service callback, and every other mode hands them service_usb().
 * The whole of le_phy.c is the poll-read engine, so the simpler statement
 * holds there -- nothing in the file feeds the endpoint -- and that is what
 * this counts.
 */
static int scan_file_for_bulk_service(const char *path, int *lines)
{
	char line[512];
	int in_block = 0, hits = 0;
	FILE *f = fopen(path, "r");

	if (f == NULL)
		return -1;

	*lines = 0;
	while (fgets(line, sizeof(line), f) != NULL) {
		strip_comments(line, &in_block);
		(*lines)++;
		if (contains_identifier(line, "handle_usb") ||
		    contains_identifier(line, "service_usb"))
			hits++;
	}

	fclose(f);
	return hits;
}

static int scanned_modes;
static int le_phy_bulk_hits = -1;
static int le_phy_lines;

static int scan_modes(void)
{
	char path[512];
	size_t i;

	for (i = 0; i < MODE_COUNT; i++) {
		snprintf(path, sizeof(path), "%s/%s", FW_RXTX_DIR, modes[i].file);
		if (!scan_function(path, modes[i].function, &modes[i].found))
			return 0;
		if (modes[i].unmask_in != NULL &&
		    !scan_function(path, modes[i].unmask_in,
		                   &modes[i].unmask_found))
			return 0;
	}

	le_phy_bulk_hits = scan_file_for_bulk_service(FW_RXTX_DIR "/le_phy.c",
	                                              &le_phy_lines);
	return le_phy_bulk_hits >= 0;
}

/*
 * The scan runs once, in main(), and the tests below read its result, so no
 * test depends on another having run first.
 */
MU_TEST(the_scan_found_every_mode_loop)
{
	size_t i;

	mu_assert(scanned_modes, "cannot open the firmware sources to scan");

	/* Without a body, every check below would pass for want of anything to
	 * look at. */
	for (i = 0; i < MODE_COUNT; i++) {
		char msg[256];

		snprintf(msg, sizeof(msg),
		         "%s: %s() scanned as %d lines, so the scan lost track of "
		         "the function", modes[i].file, modes[i].function,
		         modes[i].found.lines);
		mu_assert(modes[i].found.lines > 10, msg);
	}
}

/* A mode whose host reads the endpoint has to keep handing packets to it. */
MU_TEST(a_bulk_read_mode_keeps_the_endpoint_loaded)
{
	size_t i;

	for (i = 0; i < MODE_COUNT; i++) {
		char msg[384];

		if (modes[i].service != USB_BULK)
			continue;
		snprintf(msg, sizeof(msg),
		         "%s: %s() no longer calls handle_usb() or service_usb() on "
		         "its own path (%d mentions, %d calls), so nothing loads the "
		         "bulk IN endpoint while it runs -- and %s", modes[i].file,
		         modes[i].function, modes[i].found.bulk,
		         modes[i].found.direct, modes[i].host);
		mu_assert(modes[i].found.direct > 0, msg);
	}
}

/*
 * ...and a mode whose host polls must not, because nothing reads the endpoint
 * while it runs: a packet handed over there is lost, and the endpoint has two
 * buffers, which is why it was the first two packets of the session.
 */
MU_TEST(a_polled_mode_does_not_feed_the_endpoint)
{
	size_t i;

	for (i = 0; i < MODE_COUNT; i++) {
		char msg[384];

		if (modes[i].service == USB_BULK)
			continue;
		snprintf(msg, sizeof(msg),
		         "%s: %s() names handle_usb() or service_usb(), which hands "
		         "packets to the bulk IN endpoint -- but %s, so those "
		         "packets are lost", modes[i].file, modes[i].function,
		         modes[i].host);
		mu_assert(modes[i].found.bulk == 0, msg);
	}
}

/*
 * Not feeding the endpoint must not mean not servicing USB at all: a mode that
 * stops answering control requests cannot be polled, or stopped. Each polled
 * mode services the control path its own way -- from its loop, or from the USB
 * interrupt it unmasks on the way in.
 */
MU_TEST(a_polled_mode_still_services_control_requests)
{
	size_t i;

	for (i = 0; i < MODE_COUNT; i++) {
		char msg[384];

		if (modes[i].service == USB_CONTROL) {
			snprintf(msg, sizeof(msg),
			         "%s: %s() does not call handle_usb_control(), so "
			         "nothing services USB while it runs", modes[i].file,
			         modes[i].function);
			mu_assert(modes[i].found.control > 0, msg);
		}
		if (modes[i].service == USB_INTERRUPT) {
			snprintf(msg, sizeof(msg),
			         "%s: %s() no longer unmasks the USB interrupt, so "
			         "nothing services USB while %s() runs", modes[i].file,
			         modes[i].unmask_in, modes[i].function);
			mu_assert(modes[i].unmask_found.unmask > 0, msg);
		}
	}
}

/* Nowhere in the low-energy engine, not just not in its loop. */
MU_TEST(nothing_in_the_le_engine_feeds_the_endpoint)
{
	char msg[384];
	const char *path = FW_RXTX_DIR "/le_phy.c";

	snprintf(msg, sizeof(msg), "%s: scanned as %d lines, so the scan did not "
	         "read the file", path, le_phy_lines);
	mu_assert(le_phy_lines > 100, msg);

	snprintf(msg, sizeof(msg),
	         "%s: %d lines name handle_usb() or service_usb(). This file is "
	         "the engine behind ubertooth-btle -n and -f, whose host polls, "
	         "so anything it hands to the bulk IN endpoint -- from its loop "
	         "or from a radio wait's service callback -- is lost",
	         path, le_phy_bulk_hits);
	mu_assert(le_phy_bulk_hits == 0, msg);
}

/* ------------------------------------------------------------------ *
 * Every mode that produces packets is in the table above
 * ------------------------------------------------------------------ */

/*
 * The table above is written by hand, so a mode added later could stream
 * packets without being in it, and its loop would be free to call either
 * service with nothing here to notice.
 *
 * What a new mode cannot avoid is the queue. Every packet a host ever reads is
 * put there by usb_enqueue(), so the scan below reads every source file of
 * this image, finds each function that enqueues, and requires it to be
 * accounted for here -- either as a mode loop that is a row above, or as a
 * helper that belongs to no single mode. A function that enqueues and is not
 * listed fails the test, which is where the new mode gets classified.
 */
struct producer {
	const char *file;
	const char *function;
	/*
	 * The loop in modes[] whose host collects what this enqueues, or NULL
	 * for a helper that runs under several modes or none. What the tests
	 * check is that the name, where there is one, is a loop the table
	 * classifies; whether a row names the right loop, or should have named
	 * one at all, is read by whoever adds it.
	 */
	const char *mode_loop;
	int found;
	/* both filled in by the scan, for a helper */
	int classified_caller;
	char unlisted_caller[128];
};

/*
 * The promisc path switches its packet callback as it learns the connection,
 * and the callbacks run under both of its loops. Both loops are rows above and
 * both are classified the same way, so naming either is accurate about how
 * those packets reach the host.
 */
static struct producer producers[] = {
	{ "bluetooth_rxtx.c", "enqueue",              NULL,              0, 0, { 0 } },
	{ "bluetooth_rxtx.c", "enqueue_with_ts",      NULL,              0, 0, { 0 } },
	{ "bluetooth_rxtx.c", "bt_stream_rx",         "bt_stream_rx",    0, 0, { 0 } },
	{ "bluetooth_rxtx.c", "specan",               "specan",          0, 0, { 0 } },
	{ "bluetooth_rxtx.c", "rx_generic_sync",      "rx_generic_sync", 0, 0, { 0 } },
	{ "bluetooth_rxtx.c", "bt_le_sync",           "bt_le_sync",      0, 0, { 0 } },
	{ "bluetooth_rxtx.c", "cb_follow_le",         "bt_le_sync",      0, 0, { 0 } },
	{ "bluetooth_rxtx.c", "cb_le_promisc",        "bt_generic_le",   0, 0, { 0 } },
	{ "bluetooth_rxtx.c", "le_promisc_state",     "bt_generic_le",   0, 0, { 0 } },
	{ "le_phy.c",         "usb_enqueue_le",       NULL,              0, 0, { 0 } },
	{ "le_phy.c",         "le_phy_main",          "le_phy_main",     0, 0, { 0 } },
	{ "ego.c",            "cap_state",            "ego_main",        0, 0, { 0 } },
	{ "ego.c",            "continuous_cap_state", "ego_main",        0, 0, { 0 } },
};

#define PRODUCER_COUNT (sizeof(producers) / sizeof(producers[0]))

#define MAX_FOUND 64

struct found_producer {
	char dir[64];
	char file[64];
	char function[64];
	int listed;
};

static struct found_producer found[MAX_FOUND];
static int found_count;
static int scanned_files;
static int scan_overflowed;

/*
 * usb_enqueue() is the queue's own name for it, and the modes reach it through
 * wrappers: enqueue(), enqueue_with_ts(), usb_enqueue_le(). Rather than list
 * them, this takes any identifier with "enqueue" in it, so a wrapper added
 * later is found too.
 */
static int enqueue_identifier_at(const char *line, int n, char *out,
                                 size_t size, int *called)
{
	const char *p = line;
	int seen = 0;

	while (*p != '\0') {
		const char *start, *after;

		if (!is_ident_char(*p)) {
			p++;
			continue;
		}
		start = p;
		while (is_ident_char(*p))
			p++;
		if ((size_t)(p - start) >= size)
			continue;
		memcpy(out, start, (size_t)(p - start));
		out[p - start] = '\0';
		if (strstr(out, "enqueue") == NULL)
			continue;
		if (seen++ != n)
			continue;
		/* Its own '(' right after the name is what makes the mention a
		 * call, a prototype or a definition rather than a hand-over. */
		after = p;
		while (*after == ' ' || *after == '\t')
			after++;
		*called = *after == '(';
		return 1;
	}
	out[0] = '\0';
	return 0;
}

static int enqueue_identifier(const char *line, int must_call)
{
	char ident[128];
	int n, called;

	for (n = 0; enqueue_identifier_at(line, n, ident, sizeof(ident),
	                                  &called); n++)
		if (!must_call || called)
			return 1;
	return 0;
}

static int line_enqueues(const char *line)
{
	return enqueue_identifier(line, 0);
}

/*
 * Words that read as a name followed by its arguments without being one:
 * `if (...)` in column 1 would otherwise be taken for a definition, and an
 * attribute sits in front of the name it decorates.
 */
static int is_not_a_function_name(const char *name)
{
	static const char *const words[] = {
		"if", "else", "for", "while", "do", "switch", "return",
		"sizeof", "__attribute__",
	};
	size_t i;

	for (i = 0; i < sizeof(words) / sizeof(words[0]); i++)
		if (strcmp(name, words[i]) == 0)
			return 1;
	return 0;
}

/*
 * The name a definition declares: the first identifier with its own '(' that
 * is a name at all. Reading only the word in front of the line's first '(',
 * as call_name() does, gives "__attribute__" for a decorated definition and
 * so loses the whole body.
 */
static int definition_name(const char *line, char *out, size_t size)
{
	const char *p = line;

	while (*p != '\0') {
		const char *start, *after;

		if (!is_ident_char(*p)) {
			p++;
			continue;
		}
		start = p;
		while (is_ident_char(*p))
			p++;
		after = p;
		while (*after == ' ' || *after == '\t')
			after++;
		if (*after != '(' || (size_t)(p - start) >= size)
			continue;
		memcpy(out, start, (size_t)(p - start));
		out[p - start] = '\0';
		if (!is_not_a_function_name(out))
			return 1;
	}
	return 0;
}

/*
 * Record every function in one file that enqueues, once each.
 *
 * Everything at file scope starts in column 1, so that is what tells one
 * function's body from the next. A definition names something in front of its
 * first '(' and does not end the statement; a line ending in ';' there is a
 * declaration, and a '}' is the end of a body, so both mean the lines that
 * follow belong to no function yet.
 */
static void scan_file_for_producers(const char *dir, const char *file)
{
	char path[512], line[512], name[64], current[64];
	int in_block = 0, recorded = 0;
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", dir, file);
	f = fopen(path, "r");
	if (f == NULL)
		return;

	scanned_files++;
	current[0] = '\0';

	while (fgets(line, sizeof(line), f) != NULL) {
		strip_comments(line, &in_block);

		if (line[0] != ' ' && line[0] != '\t' && line[0] != '\n' &&
		    line[0] != '\0') {
			/* The macro guard below is what covers these. */
			if (line[0] == '#')
				continue;
			/* A body opened on the signature's line is still a
			 * definition, ';' and all. */
			if (line[0] == '}' || (strchr(line, ';') != NULL &&
			                       strchr(line, '{') == NULL)) {
				current[0] = '\0';
				continue;
			}
			if (definition_name(line, name, sizeof(name))) {
				snprintf(current, sizeof(current), "%s", name);
				recorded = 0;
			} else {
				continue;
			}
			/* ...and its first body line is this one. */
			if (strchr(line, '{') == NULL)
				continue;
		}
		if (current[0] == '\0' || recorded || !line_enqueues(line))
			continue;
		if (found_count >= MAX_FOUND) {
			scan_overflowed = 1;
			break;
		}
		snprintf(found[found_count].dir, sizeof(found[0].dir), "%s",
		         dir);
		snprintf(found[found_count].file, sizeof(found[0].file), "%s",
		         file);
		snprintf(found[found_count].function,
		         sizeof(found[0].function), "%s", current);
		found_count++;
		recorded = 1;
	}

	fclose(f);
}

/*
 * Both source trees this image is built from, so a file added to either is
 * scanned without being listed anywhere. The third, lpcusb, is the USB stack
 * itself: the endpoint writes live there by definition, and the two guards
 * after the table are what keep the rest of the firmware out of them.
 */
static const char *const scanned_dirs[] = { FW_RXTX_DIR, FW_COMMON_DIR };

#define SCANNED_DIR_COUNT (sizeof(scanned_dirs) / sizeof(scanned_dirs[0]))

/*
 * Visit the files of one directory, `.c` alone or headers too. Each scan
 * counts what it opened, because a directory that cannot be read would
 * otherwise leave its guard passing for want of anything to look at.
 */
static int scan_dir(const char *path, int headers,
                    void (*visit)(const char *, const char *))
{
	struct dirent *entry;
	int files = 0;
	DIR *dir = opendir(path);

	if (dir == NULL)
		return 0;

	while ((entry = readdir(dir)) != NULL) {
		size_t len = strlen(entry->d_name);
		const char *tail = entry->d_name + len - 2;

		if (len < 3)
			continue;
		if (strcmp(tail, ".c") != 0 &&
		    !(headers && strcmp(tail, ".h") == 0))
			continue;
		visit(path, entry->d_name);
		files++;
	}

	closedir(dir);
	return files;
}

/* Each directory read by every scan, so none of them is quietly skipped. */
static int files_seen[SCANNED_DIR_COUNT];

static void run_scan(int headers, void (*visit)(const char *, const char *))
{
	size_t i;

	for (i = 0; i < SCANNED_DIR_COUNT; i++)
		files_seen[i] += scan_dir(scanned_dirs[i], headers, visit);
}

MU_TEST(every_scanned_directory_was_read)
{
	size_t i;

	for (i = 0; i < SCANNED_DIR_COUNT; i++) {
		char msg[256];

		snprintf(msg, sizeof(msg),
		         "%s: no source file read, so every scan below passed for "
		         "want of anything to look at", scanned_dirs[i]);
		mu_assert(files_seen[i] > 0, msg);
	}
}

/*
 * Resolve the scan against the table here rather than in a test, so neither
 * of the two tests that read the result depends on the other having run.
 */
static void resolve_producers(void)
{
	int i;
	size_t j;

	for (i = 0; i < found_count; i++)
		for (j = 0; j < PRODUCER_COUNT; j++)
			if (strcmp(producers[j].file, found[i].file) == 0 &&
			    strcmp(producers[j].function,
			           found[i].function) == 0) {
				producers[j].found = 1;
				found[i].listed = 1;
			}
}

/*
 * A producer with no mode loop is a helper, which means some classified
 * producer calls it. Without that, "helper" is the cheapest answer to the
 * failure above: a new loop could list its own enqueueing helper as one and
 * never reach the transport table. So find each helper's callers.
 *
 * Every one of them has to be in the table, not just one. A new mode that
 * enqueues only through an existing helper names nothing with "enqueue" in it,
 * so the scan above does not see the mode at all; what gives it away is that
 * the helper is suddenly called from a function nobody has classified.
 */
static void scan_file_for_helper_callers(const char *dir, const char *file)
{
	char path[512], line[512], name[64], current[64];
	int in_block = 0;
	size_t i, j;
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", dir, file);
	f = fopen(path, "r");
	if (f == NULL)
		return;

	current[0] = '\0';

	while (fgets(line, sizeof(line), f) != NULL) {
		strip_comments(line, &in_block);

		if (line[0] != ' ' && line[0] != '\t' && line[0] != '\n' &&
		    line[0] != '\0') {
			if (line[0] == '#')
				continue;
			if (line[0] == '}' || (strchr(line, ';') != NULL &&
			                       strchr(line, '{') == NULL)) {
				current[0] = '\0';
				continue;
			}
			if (!definition_name(line, name, sizeof(name)))
				continue;
			snprintf(current, sizeof(current), "%s", name);
			if (strchr(line, '{') == NULL)
				continue;
		}
		if (current[0] == '\0')
			continue;

		for (i = 0; i < PRODUCER_COUNT; i++) {
			int listed = 0;

			if (producers[i].mode_loop != NULL)
				continue;
			if (!calls_identifier(line, producers[i].function))
				continue;
			for (j = 0; j < PRODUCER_COUNT; j++) {
				if (strcmp(producers[j].function, current) != 0)
					continue;
				listed = 1;
				if (producers[j].mode_loop != NULL)
					producers[i].classified_caller = 1;
			}
			/* A caller that is itself a helper is listed too, and
			 * answers for its own callers in the same way. */
			if (!listed && producers[i].unlisted_caller[0] == '\0')
				snprintf(producers[i].unlisted_caller,
				         sizeof(producers[i].unlisted_caller),
				         "%s/%s: %s()", dir, file, current);
		}
	}

	fclose(f);
}

static void scan_producers(void)
{
	run_scan(0, scan_file_for_producers);
	resolve_producers();
	run_scan(0, scan_file_for_helper_callers);
}

MU_TEST(every_helper_is_called_by_a_classified_producer)
{
	size_t i;

	for (i = 0; i < PRODUCER_COUNT; i++) {
		char msg[384];

		if (producers[i].mode_loop != NULL)
			continue;
		snprintf(msg, sizeof(msg),
		         "%s: %s() is listed as a helper that belongs to no single "
		         "mode, but no producer with a mode loop calls it. Name the "
		         "loop whose host collects what it enqueues",
		         producers[i].file, producers[i].function);
		mu_assert(producers[i].classified_caller, msg);
	}
}

/* ...and nobody outside the table calls a helper, which is how a mode that
 * enqueues only through one still gets classified. */
MU_TEST(every_caller_of_a_helper_is_accounted_for)
{
	size_t i;

	for (i = 0; i < PRODUCER_COUNT; i++) {
		char msg[384];

		if (producers[i].mode_loop != NULL)
			continue;
		snprintf(msg, sizeof(msg),
		         "%s calls %s(), which enqueues, and is not in producers[]. "
		         "Add it, and say which mode loop collects what it "
		         "enqueues -- if that loop is new, it also needs a row in "
		         "modes[] saying whether its host reads the bulk IN "
		         "endpoint or polls", producers[i].unlisted_caller,
		         producers[i].function);
		mu_assert(producers[i].unlisted_caller[0] == '\0', msg);
	}
}

MU_TEST(the_producer_scan_read_the_sources)
{
	mu_assert(scanned_files > 0,
	          "cannot open " FW_RXTX_DIR " to scan for packet producers");
	mu_assert(!scan_overflowed,
	          "more functions enqueue than this scan can hold");
	/* Without this, a scan that found nothing would pass the next check for
	 * want of anything to look at. */
	mu_assert(found_count > 0,
	          "the scan read the sources and found nothing that enqueues");
}

/* A function that enqueues and is not in the table above is a mode nobody has
 * classified. */
MU_TEST(every_packet_producer_is_accounted_for)
{
	int i;

	for (i = 0; i < found_count; i++) {
		char msg[384];

		snprintf(msg, sizeof(msg),
		         "%s/%s: %s() puts packets in the queue and is not in "
		         "producers[]. Add it, and say which mode loop collects "
		         "what it enqueues -- if that loop is new, it also needs a "
		         "row in modes[] saying whether its host reads the bulk IN "
		         "endpoint or polls", found[i].dir, found[i].file,
		         found[i].function);
		mu_assert(found[i].listed, msg);
	}
}

/* ...and a row that no longer matches the sources is a rule nothing enforces.
 */
MU_TEST(every_listed_producer_was_found)
{
	size_t i;

	for (i = 0; i < PRODUCER_COUNT; i++) {
		char msg[384];

		snprintf(msg, sizeof(msg),
		         "%s: %s() is listed in producers[] but no longer enqueues, "
		         "so the row is stale", producers[i].file,
		         producers[i].function);
		mu_assert(producers[i].found, msg);
	}
}

/*
 * The queue has to be reached by a call, because that is what the scan above
 * can follow. A macro would hide it: uses of the macro carry no identifier
 * with "enqueue" in it, so a mode that streamed packets through one would be
 * invisible here. Make it a function instead.
 */
static int macro_enqueue_hits;
static char macro_enqueue_where[256];

/*
 * `# define` with spaces after the '#' is the same directive and this tree
 * already spells it that way, so read past the whitespace rather than matching
 * one spelling.
 */
static int is_define_line(const char *line)
{
	const char *p = line;

	if (*p != '#')
		return 0;
	p++;
	while (*p == ' ' || *p == '\t')
		p++;
	return strncmp(p, "define", 6) == 0 && !is_ident_char(p[6]);
}

/* A line continued with a trailing backslash: the directive carries on. */
static int line_continues(const char *line)
{
	size_t n = strlen(line);

	while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
		n--;
	return n > 0 && line[n - 1] == '\\';
}

static void check_file_for_enqueue_macro(const char *dir, const char *file)
{
	char path[512], line[512];
	int in_block = 0, in_define = 0;
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", dir, file);
	f = fopen(path, "r");
	if (f == NULL)
		return;

	while (fgets(line, sizeof(line), f) != NULL) {
		int continued;

		strip_comments(line, &in_block);
		/* A macro body can run over several lines, and the call that
		 * reaches the queue may sit on any of them. */
		if (!in_define && !is_define_line(line))
			continue;
		continued = line_continues(line);
		if (!line_enqueues(line)) {
			in_define = continued;
			continue;
		}
		in_define = continued;
		macro_enqueue_hits++;
		if (macro_enqueue_where[0] == '\0')
			snprintf(macro_enqueue_where,
			         sizeof(macro_enqueue_where), "%s/%s: %s", dir,
			         file, line);
	}

	fclose(f);
}

MU_TEST(nothing_reaches_the_queue_through_a_macro)
{
	char msg[512];

	/* Headers too: a macro defined in one is used by every file that
	 * includes it. */
	run_scan(1, check_file_for_enqueue_macro);

	snprintf(msg, sizeof(msg),
	         "%s -- a macro around the queue hides the mode that uses it "
	         "from the scan above. Make it a function and add it to "
	         "producers[]", macro_enqueue_where);
	mu_assert(macro_enqueue_hits == 0, msg);
}

/*
 * ...and nothing hands the queue over by name either. The scan reads calls, so
 * a mode reaching usb_enqueue() through a function pointer would be invisible
 * to it. Every mention of the queue is a call, a prototype or a definition,
 * each of which has the name's own '(' right after it; taking the address does
 * not.
 *
 * Only the names that are functions count. "enqueue" in a name is what finds
 * the wrappers without listing them, and an ordinary variable can carry it
 * too, so the first pass collects the names seen with a '(' somewhere -- which
 * a variable never is -- and the guard reports a bare mention of one of those.
 */
#define MAX_QUEUE_NAMES 32

static char queue_names[MAX_QUEUE_NAMES][64];
static int queue_name_count;
static int queue_names_overflowed;

static int is_queue_name(const char *ident)
{
	int i;

	for (i = 0; i < queue_name_count; i++)
		if (strcmp(queue_names[i], ident) == 0)
			return 1;
	return 0;
}

static void collect_file_queue_names(const char *dir, const char *file)
{
	char path[512], line[512], ident[128];
	int in_block = 0, n, called;
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", dir, file);
	f = fopen(path, "r");
	if (f == NULL)
		return;

	while (fgets(line, sizeof(line), f) != NULL) {
		strip_comments(line, &in_block);
		for (n = 0; enqueue_identifier_at(line, n, ident, sizeof(ident),
		                                  &called); n++) {
			if (!called || is_queue_name(ident))
				continue;
			if (queue_name_count >= MAX_QUEUE_NAMES) {
				queue_names_overflowed = 1;
				break;
			}
			snprintf(queue_names[queue_name_count],
			         sizeof(queue_names[0]), "%s", ident);
			queue_name_count++;
		}
	}

	fclose(f);
}

static int queue_addresses_taken;
static char queue_address_where[256];

static void check_file_for_queue_address(const char *dir, const char *file)
{
	char path[512], line[512], ident[128];
	int in_block = 0, n, called;
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", dir, file);
	f = fopen(path, "r");
	if (f == NULL)
		return;

	while (fgets(line, sizeof(line), f) != NULL) {
		strip_comments(line, &in_block);
		for (n = 0; enqueue_identifier_at(line, n, ident, sizeof(ident),
		                                  &called); n++) {
			if (called || !is_queue_name(ident))
				continue;
			queue_addresses_taken++;
			if (queue_address_where[0] == '\0')
				snprintf(queue_address_where,
				         sizeof(queue_address_where),
				         "%s/%s: %s", dir, file, line);
		}
	}

	fclose(f);
}

MU_TEST(nothing_takes_the_queue_by_address)
{
	char msg[512];

	run_scan(1, collect_file_queue_names);
	mu_assert(queue_name_count > 0,
	          "the scan read the sources and found no queue function to "
	          "watch for");
	mu_assert(!queue_names_overflowed,
	          "more queue functions than this scan can hold");

	run_scan(1, check_file_for_queue_address);

	snprintf(msg, sizeof(msg),
	         "%s -- the queue is named without being called. Reached through "
	         "a pointer it is invisible to the scan above, so the mode using "
	         "it would never be classified", queue_address_where);
	mu_assert(queue_addresses_taken == 0, msg);
}

/*
 * ...and the queue is the only way to the endpoint. A mode that wrote to the
 * bulk IN endpoint itself would be past both the queue and the table, so the
 * one file that owns the endpoint is the only one allowed to name the write.
 */
static int stray_endpoint_writes;
static char stray_endpoint_where[256];

static void check_file_for_endpoint_write(const char *dir, const char *file)
{
	char path[512], line[512];
	int in_block = 0;
	FILE *f;

	/*
	 * Two files write an endpoint and are not a mode. ubertooth_usb.c owns
	 * the bulk IN endpoint, which is what every producer above goes
	 * through. usb_serial.c is the serial console's own endpoints: it is
	 * compiled into this image but nothing in it starts the console, so no
	 * mode reaches it.
	 */
	if (strcmp(file, "ubertooth_usb.c") == 0 ||
	    strcmp(file, "usb_serial.c") == 0)
		return;

	snprintf(path, sizeof(path), "%s/%s", dir, file);
	f = fopen(path, "r");
	if (f == NULL)
		return;

	while (fgets(line, sizeof(line), f) != NULL) {
		strip_comments(line, &in_block);
		/* Any endpoint: the address is often a variable, so asking for
		 * the bulk one by name would let that through. */
		if (!calls_identifier(line, "USBHwEPWrite"))
			continue;
		stray_endpoint_writes++;
		if (stray_endpoint_where[0] == '\0')
			snprintf(stray_endpoint_where,
			         sizeof(stray_endpoint_where), "%s/%s: %s", dir,
			         file, line);
	}

	fclose(f);
}

MU_TEST(only_the_queue_writes_to_the_bulk_endpoint)
{
	char msg[512];

	run_scan(0, check_file_for_endpoint_write);

	snprintf(msg, sizeof(msg),
	         "%s -- only ubertooth_usb.c hands packets to the bulk IN "
	         "endpoint. A mode writing to an endpoint directly is past the "
	         "queue, so nothing above can tell whether its host reads that "
	         "endpoint", stray_endpoint_where);
	mu_assert(stray_endpoint_writes == 0, msg);
}

/* The mode loop a producer names has to be one the transport table classifies,
 * or naming it proves nothing. */
MU_TEST(every_producers_mode_loop_is_classified)
{
	size_t i, j;

	for (i = 0; i < PRODUCER_COUNT; i++) {
		char msg[384];
		int classified = 0;

		if (producers[i].mode_loop == NULL)
			continue;
		for (j = 0; j < MODE_COUNT; j++)
			if (strcmp(modes[j].function,
			           producers[i].mode_loop) == 0)
				classified = 1;

		snprintf(msg, sizeof(msg),
		         "%s: %s() enqueues for %s(), which has no row in modes[], "
		         "so nothing says whether its host reads the bulk IN "
		         "endpoint or polls", producers[i].file,
		         producers[i].function, producers[i].mode_loop);
		mu_assert(classified, msg);
	}
}

int main(void)
{
	MU_RUN(a_new_mode_reads_only_its_own_packets);
	MU_RUN(stopping_a_mode_drops_its_backlog);
	MU_RUN(a_mode_that_keeps_its_backlog_hands_it_over);
	MU_RUN(the_endpoint_keeps_one_packet_for_each_buffer);
	MU_RUN(a_stopped_modes_keep_alive_timer_does_not_follow_it);
	MU_RUN(the_queue_still_works_after_a_clear);
	MU_RUN(a_polled_mode_keeps_every_packet_for_the_host);
	MU_RUN(the_bulk_service_takes_the_first_packet_of_each_buffer);

	scanned_modes = scan_modes();
	MU_RUN(the_scan_found_every_mode_loop);
	MU_RUN(a_bulk_read_mode_keeps_the_endpoint_loaded);
	MU_RUN(a_polled_mode_does_not_feed_the_endpoint);
	MU_RUN(a_polled_mode_still_services_control_requests);
	MU_RUN(nothing_in_the_le_engine_feeds_the_endpoint);

	scan_producers();
	MU_RUN(the_producer_scan_read_the_sources);
	MU_RUN(every_packet_producer_is_accounted_for);
	MU_RUN(every_listed_producer_was_found);
	MU_RUN(every_helper_is_called_by_a_classified_producer);
	MU_RUN(every_caller_of_a_helper_is_accounted_for);
	MU_RUN(nothing_reaches_the_queue_through_a_macro);
	MU_RUN(nothing_takes_the_queue_by_address);
	MU_RUN(only_the_queue_writes_to_the_bulk_endpoint);
	MU_RUN(every_producers_mode_loop_is_classified);
	MU_RUN(every_scanned_directory_was_read);

	return mu_report();
}
