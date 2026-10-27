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
	/* No host collects packets from this mode, because it produces none, so
	 * it may service USB whichever way suits it. Only the dispatch table at
	 * the end of this file uses it: every loop in modes[] streams. */
	USB_NONE,
};

static const char *service_name(enum usb_service s)
{
	switch (s) {
	case USB_BULK:      return "reads the bulk IN endpoint";
	case USB_CONTROL:   return "polls, with the loop servicing control "
	                           "requests";
	case USB_INTERRUPT: return "polls, with USB serviced from its interrupt";
	case USB_NONE:      return "collects no packets";
	}
	return "unknown";
}

/* What one function's body was found to name. */
struct fn_usb {
	int lines;
	int bulk;     /* handle_usb or service_usb, named anywhere */
	int direct;   /* ...called outright, rather than handed to something */
	/* ...on a path the function always reaches: no branch on the call's own
	 * line and none around it either. */
	int direct_every_turn;
	int control;  /* handle_usb_control, called */
	int control_every_turn;
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

/* A line continued with a trailing backslash: the statement carries on. */
static int line_continues(const char *line)
{
	size_t n = strlen(line);

	while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
		n--;
	return n > 0 && line[n - 1] == '\\';
}

/*
 * Does this line call `name` on the path it is on, rather than on a branch off
 * it? A condition can share the line with the call it guards --
 * `if (rare) service_usb();` services USB only when `rare` holds -- and such a
 * call does not answer for what the loop does every turn, which is what the
 * bulk and control guards below ask about.
 */
static int calls_unconditionally(const char *line, const char *name)
{
	static const char *const branches[] = {
		"if", "else", "while", "for", "switch", "case",
	};
	size_t len = strlen(name), i;
	const char *p = line;

	while ((p = strstr(p, name)) != NULL) {
		char prev = (p == line) ? ' ' : p[-1];
		const char *after = p + len;
		char before[512];

		while (*after == ' ' || *after == '\t')
			after++;
		if (is_ident_char(prev) || *after != '(' ||
		    (size_t)(p - line) >= sizeof(before)) {
			p += len;
			continue;
		}
		memcpy(before, line, (size_t)(p - line));
		before[p - line] = '\0';
		for (i = 0; i < sizeof(branches) / sizeof(branches[0]); i++)
			if (contains_identifier(before, branches[i]))
				break;
		if (i == sizeof(branches) / sizeof(branches[0]))
			return 1;
		p += len;
	}
	return 0;
}

/*
 * Which kind of block a '{' opens. A call inside a branch answers for the
 * path the branch is on and no other; a call inside a loop is on the
 * ordinary path of that loop, which for a streaming mode is the path that
 * matters.
 */
enum block_kind { BLOCK_PLAIN, BLOCK_LOOP, BLOCK_BRANCH };

#define MAX_BLOCK_DEPTH 32

/*
 * Where in a function's braces a line sits. The kind a '{' opens is decided
 * by the keyword in front of it, which can be lines earlier when the
 * condition is wrapped -- bt_stream_rx()'s mode loop spans five lines -- so
 * the keyword is remembered until a '{' or a ';' settles it.
 */
struct brace_state {
	int depth;
	enum block_kind kind[MAX_BLOCK_DEPTH];
	enum block_kind pending;
};

static int block_keyword(const char *word, enum block_kind *kind)
{
	static const char *const branches[] = { "if", "else", "switch", "case" };
	static const char *const loops[] = { "while", "for", "do" };
	size_t i;

	for (i = 0; i < sizeof(branches) / sizeof(branches[0]); i++)
		if (strcmp(word, branches[i]) == 0) {
			*kind = BLOCK_BRANCH;
			return 1;
		}
	for (i = 0; i < sizeof(loops) / sizeof(loops[0]); i++)
		if (strcmp(word, loops[i]) == 0) {
			*kind = BLOCK_LOOP;
			return 1;
		}
	return 0;
}

/* Is the line about to be read inside a branch? The blocks around it, plus a
 * condition that has opened on an earlier line and not closed yet. */
static int inside_a_branch(const struct brace_state *b)
{
	int i;

	if (b->pending == BLOCK_BRANCH)
		return 1;
	for (i = 0; i < b->depth && i < MAX_BLOCK_DEPTH; i++)
		if (b->kind[i] == BLOCK_BRANCH)
			return 1;
	return 0;
}

/* Carry the brace state across one line. Call it after reading the line, so
 * the line itself is judged by the state it was reached in. */
static void brace_state_advance(struct brace_state *b, const char *line)
{
	const char *p = line;

	while (*p != '\0') {
		if (is_ident_char(*p)) {
			char word[32];
			const char *start = p;
			enum block_kind kind;

			while (is_ident_char(*p))
				p++;
			if ((size_t)(p - start) < sizeof(word)) {
				memcpy(word, start, (size_t)(p - start));
				word[p - start] = '\0';
				if (block_keyword(word, &kind))
					b->pending = kind;
			}
			continue;
		}
		if (*p == '{') {
			if (b->depth < MAX_BLOCK_DEPTH)
				b->kind[b->depth] = b->pending;
			b->depth++;
			b->pending = BLOCK_PLAIN;
		} else if (*p == '}') {
			if (b->depth > 0)
				b->depth--;
		} else if (*p == ';') {
			b->pending = BLOCK_PLAIN;
		}
		p++;
	}
}

/*
 * Is this line a function definition rather than a prototype or a call? It
 * starts in column 1 and names the function in front of its first '('. A
 * line that ends the statement is a declaration -- unless it also opens a
 * body, because a definition whose body starts on the signature's line
 * carries that body's semicolons too.
 */
static int is_definition_line(const char *line, const char *function)
{
	char name[64];

	if (line[0] == ' ' || line[0] == '\t' || line[0] == '\0')
		return 0;
	if (strchr(line, ';') != NULL && strchr(line, '{') == NULL)
		return 0;
	return call_name(line, name, sizeof(name)) &&
	       strcmp(name, function) == 0;
}

/*
 * Read one function's body out of a firmware source file, a line at a time,
 * with whether that line sits inside a branch. The body runs from the
 * definition's '{' until the braces balance again, so a definition that opens
 * and closes on one line is read rather than skipped.
 *
 * Returns the number of body lines read, or -1 if the file could not be
 * opened. Zero means the file holds no such definition.
 */
static int read_function_body(const char *path, const char *function,
                              void (*visit)(const char *line, int branched,
                                            void *ctx),
                              void *ctx)
{
	char line[SCAN_LINE_MAX];
	int in_block = 0, in_fn = 0, opened = 0, lines = 0;
	struct brace_state braces;
	FILE *f = fopen(path, "r");

	if (f == NULL)
		return -1;

	memset(&braces, 0, sizeof(braces));

	while (fgets(line, sizeof(line), f) != NULL) {
		strip_comments(line, &in_block);
		strip_string_literals(line);

		if (!in_fn) {
			if (!is_definition_line(line, function))
				continue;
			in_fn = 1;
			opened = 0;
			memset(&braces, 0, sizeof(braces));
			/* The signature itself is not body, but it may open the
			 * body -- and then it carries the body's first statement,
			 * which the brace walk below accounts for. */
			brace_state_advance(&braces, line);
			if (braces.depth > 0)
				opened = 1;
			if (opened && braces.depth == 0)
				in_fn = 0;
			continue;
		}

		lines++;
		visit(line, inside_a_branch(&braces), ctx);

		brace_state_advance(&braces, line);
		if (braces.depth > 0)
			opened = 1;
		if (opened && braces.depth == 0)
			in_fn = 0;
	}

	fclose(f);
	return lines;
}

static void visit_usb_service(const char *line, int branched, void *ctx)
{
	struct fn_usb *out = ctx;

	out->lines++;
	/* handle_usb_control is a whole identifier of its own, so a match on
	 * handle_usb is the bulk service and nothing else. */
	if (contains_identifier(line, "handle_usb") ||
	    contains_identifier(line, "service_usb"))
		out->bulk++;
	/*
	 * A bare mention is not enough to keep a stream fed: the bounded radio
	 * waits and hop() take service_usb as a callback, so a loop could name
	 * it on a path it rarely takes -- hop() runs only when the mode hops --
	 * and never service USB on its ordinary one. What answers for the
	 * ordinary path is a call the loop always makes.
	 */
	if (calls_unconditionally(line, "service_usb") ||
	    calls_unconditionally(line, "handle_usb")) {
		out->direct++;
		/*
		 * ...and one it always reaches, not one inside a branch it rarely
		 * takes. calls_unconditionally() reads the line the call is on, so
		 * it rules out a condition sharing that line; `branched` rules out
		 * a condition several lines above it.
		 */
		if (!branched)
			out->direct_every_turn++;
	}
	/* A call, for the same reason: naming the control service in a callback
	 * the loop rarely reaches would not keep the host's requests answered. */
	if (calls_unconditionally(line, "handle_usb_control")) {
		out->control++;
		if (!branched)
			out->control_every_turn++;
	}
	if (is_assignment_to(line, "ISER0") &&
	    contains_identifier(line, "ISER0_ISE_USB"))
		out->unmask++;
}

static int scan_function(const char *path, const char *function,
                         struct fn_usb *out)
{
	memset(out, 0, sizeof(*out));
	return read_function_body(path, function, visit_usb_service, out) >= 0;
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
		strip_string_literals(line);
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
		         "a path it always reaches (%d mentions, %d calls, %d of "
		         "them outside any branch), so nothing loads the bulk IN "
		         "endpoint while it runs -- and %s", modes[i].file,
		         modes[i].function, modes[i].found.bulk,
		         modes[i].found.direct, modes[i].found.direct_every_turn,
		         modes[i].host);
		mu_assert(modes[i].found.direct_every_turn > 0, msg);
	}
}

/*
 * The guard above accepts either name because the modes reach the service
 * through service_usb(), which is one line: it calls handle_usb(). That one
 * line is what every bulk-read mode above rests on, so if it ever stopped
 * leading to the queue's own service, each of those modes would keep its call
 * and its host would still go empty.
 */
MU_TEST(the_wrapper_every_bulk_mode_calls_reaches_the_queues_service)
{
	char path[512];
	struct fn_usb found;

	snprintf(path, sizeof(path), "%s/bluetooth_rxtx.c", FW_RXTX_DIR);
	mu_assert(scan_function(path, "service_usb", &found),
	          "cannot read service_usb() in bluetooth_rxtx.c");
	mu_assert(found.lines > 0,
	          "service_usb()'s body is empty, so the bulk IN endpoint is "
	          "never loaded however many modes call it");
	mu_assert(found.direct > 0,
	          "service_usb() no longer calls handle_usb(), so every mode "
	          "above that calls it leaves the bulk IN endpoint empty and its "
	          "host reads nothing");
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
			         "%s: %s() does not call handle_usb_control() on a path "
			         "it always reaches (%d calls, %d of them outside any "
			         "branch), so the host's requests go unanswered on the "
			         "loop's ordinary path", modes[i].file,
			         modes[i].function, modes[i].found.control,
			         modes[i].found.control_every_turn);
			mu_assert(modes[i].found.control_every_turn > 0, msg);
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
	/* Both halves of where it lives. A file name alone is not an identity:
	 * the two directories scanned below could each hold a file of the same
	 * name, and a row would then match a function in either of them. */
	const char *dir;
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
	{ FW_RXTX_DIR, "bluetooth_rxtx.c", "enqueue",              NULL,              0, 0, { 0 } },
	{ FW_RXTX_DIR, "bluetooth_rxtx.c", "enqueue_with_ts",      NULL,              0, 0, { 0 } },
	{ FW_RXTX_DIR, "bluetooth_rxtx.c", "bt_stream_rx",         "bt_stream_rx",    0, 0, { 0 } },
	{ FW_RXTX_DIR, "bluetooth_rxtx.c", "specan",               "specan",          0, 0, { 0 } },
	{ FW_RXTX_DIR, "bluetooth_rxtx.c", "rx_generic_sync",      "rx_generic_sync", 0, 0, { 0 } },
	{ FW_RXTX_DIR, "bluetooth_rxtx.c", "bt_le_sync",           "bt_le_sync",      0, 0, { 0 } },
	{ FW_RXTX_DIR, "bluetooth_rxtx.c", "cb_follow_le",         "bt_le_sync",      0, 0, { 0 } },
	{ FW_RXTX_DIR, "bluetooth_rxtx.c", "cb_le_promisc",        "bt_generic_le",   0, 0, { 0 } },
	{ FW_RXTX_DIR, "bluetooth_rxtx.c", "le_promisc_state",     "bt_generic_le",   0, 0, { 0 } },
	{ FW_RXTX_DIR, "le_phy.c",         "usb_enqueue_le",       NULL,              0, 0, { 0 } },
	{ FW_RXTX_DIR, "le_phy.c",         "le_phy_main",          "le_phy_main",     0, 0, { 0 } },
	{ FW_RXTX_DIR, "ego.c",            "cap_state",            "ego_main",        0, 0, { 0 } },
	{ FW_RXTX_DIR, "ego.c",            "continuous_cap_state", "ego_main",        0, 0, { 0 } },
};

/* Is this the file a row names? Both halves, so a file of the same name in
 * the other scanned directory is a different file. */
static int producer_is_in(const struct producer *p, const char *dir,
                          const char *file)
{
	return strcmp(p->dir, dir) == 0 && strcmp(p->file, file) == 0;
}

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

/*
 * A call, which is what puts a packet in the queue. A name is not enough: an
 * ordinary variable can carry "enqueue" too, and an `int enqueue_count` would
 * otherwise make the function holding it a producer that has to be classified.
 */
static int line_calls_enqueue(const char *line)
{
	return enqueue_identifier(line, 1);
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
		strip_string_literals(line);

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
		if (current[0] == '\0' || recorded || !line_calls_enqueue(line))
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
		const char *tail;

		/* Before naming the last two characters, because "." has only
		 * one and the pointer would be before the name. */
		if (len < 3)
			continue;
		tail = entry->d_name + len - 2;
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
 * ...and the two directories are the whole image. The scans read what is in
 * them, so a source file the image compiles from anywhere else -- a new
 * subdirectory, a new tree -- would never be read and the mode in it would
 * never be classified. The image's own source list is what says which files
 * those are, so read it and require each one to be somewhere a scan looks.
 *
 * lpcusb is the exception the comment above the directories explains: it is
 * the USB stack, where the endpoint writes live by definition.
 */
static int is_in_a_scanned_dir(const char *file)
{
	char path[512];
	size_t i;

	/* The scans read a directory's own entries, not its subdirectories, so
	 * a name with a path left in it is not one of them. */
	if (strchr(file, '/') != NULL)
		return 0;
	for (i = 0; i < SCANNED_DIR_COUNT; i++) {
		FILE *f;

		snprintf(path, sizeof(path), "%s/%s", scanned_dirs[i], file);
		f = fopen(path, "r");
		if (f != NULL) {
			fclose(f);
			return 1;
		}
	}
	return 0;
}

/*
 * The name a source list entry stands for. A leading make variable is the
 * directory the file sits in, which the lookup above covers, and $(TARGET) is
 * the image itself. Anything else keeps whatever path it has, so a real
 * subdirectory is reported as unscanned, which it is.
 */
static const char *source_list_name(const char *token)
{
	const char *close;

	if (strncmp(token, "$(TARGET)", 9) == 0)
		return "bluetooth_rxtx.c";
	if (token[0] != '$')
		return token;
	close = strstr(token, ")/");
	return close == NULL ? token : close + 2;
}

static int sources_listed;
static char unscanned_source[256];

static void read_the_images_source_list(void)
{
	char path[512], line[512];
	int in_src = 0;
	FILE *f;

	snprintf(path, sizeof(path), "%s/Makefile", FW_RXTX_DIR);
	f = fopen(path, "r");
	if (f == NULL)
		return;

	while (fgets(line, sizeof(line), f) != NULL) {
		char *token, *rest = line;

		if (!in_src) {
			if (strncmp(line, "SRC", 3) != 0 ||
			    strchr(line, '=') == NULL)
				continue;
			in_src = 1;
			rest = strchr(line, '=') + 1;
		}
		if (!line_continues(line))
			in_src = 0;

		for (token = strtok(rest, " \t\r\n\\"); token != NULL;
		     token = strtok(NULL, " \t\r\n\\")) {
			const char *file;
			size_t len = strlen(token);

			if (len < 3 || strcmp(token + len - 2, ".c") != 0)
				continue;
			if (strstr(token, "LPCUSB_PATH") != NULL)
				continue;
			file = source_list_name(token);
			sources_listed++;
			if (!is_in_a_scanned_dir(file) &&
			    unscanned_source[0] == '\0')
				snprintf(unscanned_source,
				         sizeof(unscanned_source), "%s", token);
		}
	}

	fclose(f);
}

MU_TEST(every_source_the_image_compiles_is_scanned)
{
	char msg[512];

	read_the_images_source_list();
	mu_assert(sources_listed > 0,
	          "cannot read the image's source list in "
	          FW_RXTX_DIR "/Makefile, so the check below passed for want of "
	          "anything to look at");
	snprintf(msg, sizeof(msg),
	         "%s is compiled into this image and is in neither directory the "
	         "scans read, so a mode in it would never be classified. Put it "
	         "in one of them, or add its directory to scanned_dirs[]",
	         unscanned_source);
	mu_assert(unscanned_source[0] == '\0', msg);
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
			if (producer_is_in(&producers[j], found[i].dir,
			                   found[i].file) &&
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
		strip_string_literals(line);

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
				/* The caller's own file too: a function of this
				 * name in another file is another function. */
				if (!producer_is_in(&producers[j], dir, file) ||
				    strcmp(producers[j].function, current) != 0)
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
		strip_string_literals(line);
		/* A macro body can run over several lines, and the call that
		 * reaches the queue may sit on any of them. */
		if (!in_define && !is_define_line(line))
			continue;
		continued = line_continues(line);
		if (!line_calls_enqueue(line)) {
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
		strip_string_literals(line);
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
		strip_string_literals(line);
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
		strip_string_literals(line);
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

/* ------------------------------------------------------------------ *
 * Every mode the dispatch runs, and the host that collects its packets
 * ------------------------------------------------------------------ */

/*
 * modes[] is keyed on the firmware loop, and three modes share one loop:
 * MODE_RX_SYMBOLS, MODE_BT_FOLLOW and MODE_AFH all run bt_stream_rx(). So
 * everything above is a statement about a loop, not about a mode. A dispatch
 * case added later could run that loop for a host that polls, and every check
 * above would pass while the mode lost the first two packets of every session
 * to the bulk IN endpoint -- the failure modes[] exists to stop.
 *
 * Which transport is used is a property of the (mode, host) pair, so this
 * table is keyed on the mode. Each row is one case of main()'s dispatch: the
 * function that case runs, and how that mode's host collects packets. A new
 * case has to be added here, and what it may claim is then checked against
 * the loop it actually runs.
 */
struct dispatch_mode {
	const char *mode;          /* the MODE_* the dispatch has a case for */
	/* The function the case calls, or NULL for a case that calls none. */
	const char *runs;
	enum usb_service service;
	/* filled in by the scan */
	int found;
	unsigned loops;            /* modes[] rows reachable from this case */
	int reaches_queue;
};

static struct dispatch_mode dispatch[] = {
	{ "MODE_RESET",        "reset",            USB_NONE,      0, 0, 0 },
	{ "MODE_AFH",          "bt_stream_rx",     USB_BULK,      0, 0, 0 },
	{ "MODE_RX_SYMBOLS",   "bt_stream_rx",     USB_BULK,      0, 0, 0 },
	{ "MODE_BT_FOLLOW",    "bt_stream_rx",     USB_BULK,      0, 0, 0 },
	{ "MODE_TX_SYMBOLS",   "br_transmit",      USB_NONE,      0, 0, 0 },
	{ "MODE_BT_FOLLOW_LE", "bt_follow_le",     USB_CONTROL,   0, 0, 0 },
	{ "MODE_BT_PROMISC_LE","bt_promisc_le",    USB_INTERRUPT, 0, 0, 0 },
	{ "MODE_BT_SLAVE_LE",  "bt_slave_le",      USB_NONE,      0, 0, 0 },
	{ "MODE_TX_TEST",      "cc2400_txtest",    USB_NONE,      0, 0, 0 },
	{ "MODE_RANGE_TEST",   "cc2400_rangetest", USB_NONE,      0, 0, 0 },
	{ "MODE_REPEATER",     "cc2400_repeater",  USB_NONE,      0, 0, 0 },
	{ "MODE_SPECAN",       "specan",           USB_BULK,      0, 0, 0 },
	{ "MODE_LED_SPECAN",   "led_specan",       USB_NONE,      0, 0, 0 },
	{ "MODE_EGO",          "ego_main",         USB_INTERRUPT, 0, 0, 0 },
	{ "MODE_RX_GENERIC",   "rx_generic",       USB_BULK,      0, 0, 0 },
	{ "MODE_TX_GENERIC",   "tx_generic",       USB_NONE,      0, 0, 0 },
	{ "MODE_XMAS",         "xmas_main",        USB_NONE,      0, 0, 0 },
	{ "MODE_IDENTIFY",     "identify_main",    USB_NONE,      0, 0, 0 },
	{ "MODE_IDLE",         NULL,               USB_NONE,      0, 0, 0 },
};

#define DISPATCH_COUNT (sizeof(dispatch) / sizeof(dispatch[0]))

#define MAX_CASE_CALLS 12

/* What the scan read out of one dispatch case: its label and every function
 * its body calls. A case that falls through to the next shares the next
 * one's calls, which is how MODE_AFH runs bt_stream_rx(). */
struct dispatch_case {
	char mode[48];
	char calls[MAX_CASE_CALLS][64];
	int call_count;
	int listed;
};

static struct dispatch_case cases_found[DISPATCH_COUNT + 8];
static int cases_found_count;
static int dispatch_lines;
static int dispatch_overflowed;
static char unlisted_case[64];

/* The MODE_* a `case` label names, if it names one. */
static int case_label(const char *line, char *out, size_t size)
{
	const char *p = line;
	const char *start;

	while (*p == ' ' || *p == '\t')
		p++;
	if (strncmp(p, "case", 4) != 0 || is_ident_char(p[4]))
		return 0;
	p += 4;
	while (*p == ' ' || *p == '\t')
		p++;
	start = p;
	while (is_ident_char(*p))
		p++;
	if (p == start || (size_t)(p - start) >= size ||
	    strncmp(start, "MODE_", 5) != 0)
		return 0;
	memcpy(out, start, (size_t)(p - start));
	out[p - start] = '\0';
	return 1;
}

/* Record a call against every case label still waiting for one. */
static void add_call_to_pending(const int *pending, int pending_count,
                                const char *name)
{
	int i, j;

	for (i = 0; i < pending_count; i++) {
		struct dispatch_case *c = &cases_found[pending[i]];

		for (j = 0; j < c->call_count; j++)
			if (strcmp(c->calls[j], name) == 0)
				break;
		if (j < c->call_count)
			continue;
		if (c->call_count >= MAX_CASE_CALLS) {
			dispatch_overflowed = 1;
			continue;
		}
		snprintf(c->calls[c->call_count], sizeof(c->calls[0]), "%s",
		         name);
		c->call_count++;
	}
}

static int pending_cases[DISPATCH_COUNT + 8];
static int pending_count;

static void visit_dispatch_line(const char *line, int branched, void *ctx)
{
	char name[64];
	const char *p = line;

	(void)branched;
	(void)ctx;
	dispatch_lines++;

	if (case_label(line, name, sizeof(name))) {
		if (cases_found_count >=
		    (int)(sizeof(cases_found) / sizeof(cases_found[0]))) {
			dispatch_overflowed = 1;
			return;
		}
		memset(&cases_found[cases_found_count], 0,
		       sizeof(cases_found[0]));
		snprintf(cases_found[cases_found_count].mode,
		         sizeof(cases_found[0].mode), "%s", name);
		if (pending_count <
		    (int)(sizeof(pending_cases) / sizeof(pending_cases[0])))
			pending_cases[pending_count++] = cases_found_count;
		cases_found_count++;
		return;
	}

	if (pending_count == 0)
		return;

	/* Every call on the line, not only the first: a case that calls
	 * something harmless before the function that does the work would
	 * otherwise be read by its first call alone. */
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
		if (*after != '(' || (size_t)(p - start) >= sizeof(name))
			continue;
		memcpy(name, start, (size_t)(p - start));
		name[p - start] = '\0';
		if (!is_not_a_function_name(name))
			add_call_to_pending(pending_cases, pending_count, name);
	}

	/*
	 * The case ends here, so the next label starts a new one. A `break` of
	 * an inner loop would end it early and the calls after it would be
	 * missed -- which shows up as the row no longer naming what its case
	 * calls, rather than as a check quietly passing. No case has an inner
	 * loop today.
	 */
	if (contains_identifier(line, "break") ||
	    contains_identifier(line, "return"))
		pending_count = 0;
}

/*
 * What one function leads to: whether its body reaches the packet queue, and
 * which of the loops in modes[] it runs. One step is enough for the dispatch,
 * because three of its cases call a function that picks a loop --
 * bt_follow_le(), bt_promisc_le() and rx_generic() -- and the rest call a loop
 * outright.
 *
 * A name that is not defined in either scanned directory is a library call or
 * a compiler intrinsic, which cannot reach the queue:
 * every_source_the_image_compiles_is_scanned above is what makes that true.
 */
struct reach {
	int producer;   /* its body puts packets in the queue, or calls what does */
	unsigned loops; /* bitmask of modes[] rows it runs */
};

static void visit_reach(const char *line, int branched, void *ctx)
{
	struct reach *r = ctx;
	size_t i;

	(void)branched;
	if (line_calls_enqueue(line))
		r->producer = 1;
	for (i = 0; i < PRODUCER_COUNT; i++)
		if (calls_identifier(line, producers[i].function))
			r->producer = 1;
	for (i = 0; i < MODE_COUNT; i++)
		if (calls_identifier(line, modes[i].function))
			r->loops |= 1u << i;
}

static void function_reach(const char *name, struct reach *out)
{
	struct dirent *entry;
	size_t i;

	memset(out, 0, sizeof(*out));

	/* A name that is itself a loop or a producer answers for itself. */
	for (i = 0; i < MODE_COUNT; i++)
		if (strcmp(modes[i].function, name) == 0)
			out->loops |= 1u << i;
	for (i = 0; i < PRODUCER_COUNT; i++)
		if (strcmp(producers[i].function, name) == 0)
			out->producer = 1;

	for (i = 0; i < SCANNED_DIR_COUNT; i++) {
		DIR *dir = opendir(scanned_dirs[i]);

		if (dir == NULL)
			continue;
		while ((entry = readdir(dir)) != NULL) {
			char path[512];
			size_t len = strlen(entry->d_name);

			if (len < 3 ||
			    strcmp(entry->d_name + len - 2, ".c") != 0)
				continue;
			snprintf(path, sizeof(path), "%s/%s", scanned_dirs[i],
			         entry->d_name);
			read_function_body(path, name, visit_reach, out);
		}
		closedir(dir);
	}
}

static int dispatch_scanned;

static void scan_dispatch(void)
{
	char path[512];
	size_t i;
	int c;

	snprintf(path, sizeof(path), "%s/bluetooth_rxtx.c", FW_RXTX_DIR);
	cases_found_count = 0;
	pending_count = 0;
	dispatch_lines = 0;
	dispatch_scanned =
		read_function_body(path, "main", visit_dispatch_line, NULL) > 0;
	if (!dispatch_scanned)
		return;

	/* Match the cases the scan found against the table, and resolve what
	 * each case's calls lead to. */
	for (c = 0; c < cases_found_count; c++) {
		for (i = 0; i < DISPATCH_COUNT; i++) {
			int k;

			if (strcmp(dispatch[i].mode, cases_found[c].mode) != 0)
				continue;
			dispatch[i].found = 1;
			cases_found[c].listed = 1;
			for (k = 0; k < cases_found[c].call_count; k++) {
				struct reach r;

				function_reach(cases_found[c].calls[k], &r);
				dispatch[i].loops |= r.loops;
				dispatch[i].reaches_queue |= r.producer;
			}
		}
		if (!cases_found[c].listed && unlisted_case[0] == '\0')
			snprintf(unlisted_case, sizeof(unlisted_case), "%s",
			         cases_found[c].mode);
	}
}

MU_TEST(the_dispatch_scan_read_every_case)
{
	size_t i;

	mu_assert_fatal(dispatch_scanned,
	                "cannot read main() in " FW_RXTX_DIR
	                "/bluetooth_rxtx.c, so the checks below would pass for "
	                "want of anything to look at");
	mu_assert(!dispatch_overflowed,
	          "the dispatch has more cases, or a case more calls, than this "
	          "scan can hold");
	mu_assert(dispatch_lines > 50,
	          "main() scanned as fewer than 50 lines, so the scan lost "
	          "track of the function");
	mu_assert(cases_found_count >= (int)DISPATCH_COUNT,
	          "the scan found fewer dispatch cases than the table lists, so "
	          "it lost track of main()");

	for (i = 0; i < DISPATCH_COUNT; i++) {
		char msg[256];

		snprintf(msg, sizeof(msg),
		         "%s is in the table but main() has no case for it, so the "
		         "row says nothing about anything", dispatch[i].mode);
		mu_assert(dispatch[i].found, msg);
	}
}

/* A mode the dispatch runs and nothing here classifies is the hole #434 came
 * out of: the mode streams, and no test says which transport its host uses. */
MU_TEST(every_mode_the_dispatch_runs_is_classified)
{
	char msg[384];

	snprintf(msg, sizeof(msg),
	         "main() has a case for %s and dispatch[] does not. Add it, and "
	         "say how that mode's host collects packets: reading the bulk IN "
	         "endpoint, or polling", unlisted_case);
	mu_assert(unlisted_case[0] == '\0', msg);
}

/* ...and the row has to name the function the case really runs, or the checks
 * below are reading a different mode's loop. */
MU_TEST(every_row_names_the_function_its_case_calls)
{
	size_t i;
	int c;

	for (i = 0; i < DISPATCH_COUNT; i++) {
		char msg[384];
		int named = dispatch[i].runs == NULL, calls = 0;

		for (c = 0; c < cases_found_count; c++) {
			int k;

			if (strcmp(cases_found[c].mode, dispatch[i].mode) != 0)
				continue;
			calls += cases_found[c].call_count;
			for (k = 0; k < cases_found[c].call_count; k++)
				if (dispatch[i].runs != NULL &&
				    strcmp(cases_found[c].calls[k],
				           dispatch[i].runs) == 0)
					named = 1;
		}

		snprintf(msg, sizeof(msg),
		         "dispatch[] says %s runs %s(), which main()'s case for it "
		         "does not call (%d calls found). The row is stale",
		         dispatch[i].mode,
		         dispatch[i].runs ? dispatch[i].runs : "nothing", calls);
		mu_assert(named, msg);

		if (dispatch[i].runs == NULL) {
			snprintf(msg, sizeof(msg),
			         "dispatch[] says %s calls nothing, but main()'s case "
			         "for it makes %d calls", dispatch[i].mode, calls);
			mu_assert(calls == 0, msg);
		}
	}
}

/*
 * The check the loop-keyed table above cannot make: a mode that runs one of
 * those loops has to use the transport that loop provides. A new case running
 * bt_stream_rx() for a host that polls is #434 again -- the loop hands packets
 * to the bulk IN endpoint, which such a host never reads -- and this is what
 * says so.
 */
MU_TEST(a_mode_uses_the_transport_of_the_loop_it_runs)
{
	size_t i, j;

	for (i = 0; i < DISPATCH_COUNT; i++)
		for (j = 0; j < MODE_COUNT; j++) {
			char msg[512];

			if (!(dispatch[i].loops & (1u << j)))
				continue;
			if (modes[j].service == dispatch[i].service)
				continue;
			snprintf(msg, sizeof(msg),
			         "%s runs %s(), whose host %s, but dispatch[] says "
			         "this mode's host %s. One of the two is wrong: a "
			         "mode whose host polls loses to the bulk IN "
			         "endpoint every packet the loop hands it, and a "
			         "mode whose host reads that endpoint reads nothing "
			         "if the loop never loads it",
			         dispatch[i].mode, modes[j].function,
			         service_name(modes[j].service),
			         service_name(dispatch[i].service));
			mu_assert(0, msg);
		}
}

/*
 * A mode that streams needs a loop the table above classifies. Without this,
 * a new case could run a loop of its own that enqueues through an existing
 * producer -- a call to le_promisc_state() names nothing with "enqueue" in it,
 * so the producer scan does not see the new loop at all -- and claim any
 * transport it liked.
 */
MU_TEST(a_mode_that_streams_runs_a_classified_loop)
{
	size_t i;

	for (i = 0; i < DISPATCH_COUNT; i++) {
		char msg[512];

		if (dispatch[i].service == USB_NONE) {
			snprintf(msg, sizeof(msg),
			         "%s reaches the packet queue, so a host does collect "
			         "its packets. Say which transport that host uses",
			         dispatch[i].mode);
			mu_assert(!dispatch[i].reaches_queue, msg);
			continue;
		}

		snprintf(msg, sizeof(msg),
		         "dispatch[] says %s's host %s, but %s() runs none of the "
		         "loops in modes[], so nothing checks that the loop it does "
		         "run keeps that promise. Add a row to modes[] for that "
		         "loop", dispatch[i].mode,
		         service_name(dispatch[i].service),
		         dispatch[i].runs ? dispatch[i].runs : "the case");
		mu_assert(dispatch[i].loops != 0, msg);
	}
}

/* ------------------------------------------------------------------ *
 * The scans can read what is there
 * ------------------------------------------------------------------ */

/*
 * Every scan in this file reads a line at a time into a fixed buffer, and a
 * longer line comes back split. A split that lands in the middle of a call
 * hides that call: the mode using it would pass every check above by being
 * invisible. So the sources have to stay narrower than the buffer, and this
 * is what says so rather than assuming it.
 */
static int longest_line;
static char longest_line_where[256];

static void check_file_line_length(const char *dir, const char *file)
{
	char path[512];
	int c, length = 0, lineno = 1;
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", dir, file);
	f = fopen(path, "r");
	if (f == NULL)
		return;

	while ((c = fgetc(f)) != EOF) {
		if (c == '\n') {
			lineno++;
			length = 0;
			continue;
		}
		length++;
		if (length <= longest_line)
			continue;
		longest_line = length;
		snprintf(longest_line_where, sizeof(longest_line_where),
		         "%s/%s:%d", dir, file, lineno);
	}

	fclose(f);
}

MU_TEST(no_source_line_is_too_long_for_the_scans)
{
	char msg[384];

	run_scan(1, check_file_line_length);

	mu_assert(longest_line > 0,
	          "no source was read, so this check passed for want of "
	          "anything to look at");
	/* The line plus its newline and the terminator have to fit. */
	snprintf(msg, sizeof(msg),
	         "%s is %d characters long and the scans read %d at a time, so "
	         "that line comes back split and a call on it can be hidden "
	         "from them. Wrap it, or widen SCAN_LINE_MAX in source_scan.h",
	         longest_line_where, longest_line, SCAN_LINE_MAX);
	mu_assert(longest_line + 2 <= SCAN_LINE_MAX, msg);
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
	MU_RUN(the_wrapper_every_bulk_mode_calls_reaches_the_queues_service);
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
	MU_RUN(every_source_the_image_compiles_is_scanned);

	scan_dispatch();
	MU_RUN(the_dispatch_scan_read_every_case);
	MU_RUN(every_mode_the_dispatch_runs_is_classified);
	MU_RUN(every_row_names_the_function_its_case_calls);
	MU_RUN(a_mode_uses_the_transport_of_the_loop_it_runs);
	MU_RUN(a_mode_that_streams_runs_a_classified_loop);

	MU_RUN(no_source_line_is_too_long_for_the_scans);

	return mu_report();
}
