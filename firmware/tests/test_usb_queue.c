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
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "minunit.h"
#include "source_scan.h"
#include "ubertooth_interface.h"

/* ------------------------------------------------------------------ *
 * The watchdog, as handle_usb() sees it.
 * ------------------------------------------------------------------ */

/* handle_usb() is one of the two places the watchdog is fed: a pass that got
 * all the way through means the host's requests are being answered. */
static unsigned watchdog_feeds;
static uint8_t watchdog_site;
static uint8_t watchdog_site_during_pass;

uint8_t watchdog_mark(uint8_t site)
{
	uint8_t previous = watchdog_site;

	watchdog_site = site;
	if (site == WATCHDOG_SITE_USB)
		watchdog_site_during_pass = site;
	return previous;
}

void watchdog_usb_serviced(void)
{
	watchdog_feeds++;
}

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
 *
 * The parentheses are counted because a `for` header carries two semicolons
 * of its own, and those do not settle anything: only a ';' outside the
 * parentheses ends the statement.
 */
struct brace_state {
	int depth;
	enum block_kind kind[MAX_BLOCK_DEPTH];
	enum block_kind pending;
	int parens;
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
		} else if (*p == '(') {
			b->parens++;
		} else if (*p == ')') {
			if (b->parens > 0)
				b->parens--;
		} else if (*p == ';' && b->parens == 0) {
			b->pending = BLOCK_PLAIN;
		}
		p++;
	}
}

/*
 * Where in a function body one line sits, handed to the visitor with the line
 * itself. The text alone cannot say whether a `break` ends a `switch` case or
 * belongs to a loop inside it, which is what the dispatch scan at the end of
 * this file has to know.
 */
struct body_pos {
	/* Reached through a branch: a branch block open around this line, or a
	 * condition that opened on an earlier line. */
	int branched;
	/* Brace depth at the start of the line, counting the function's own
	 * body as 1. */
	int depth;
	/*
	 * This line is the statement of an `if`, `while` or `for` that opened
	 * on an earlier line and wrote no braces, so a `break` here belongs to
	 * that statement rather than to the block around it.
	 */
	int continues_a_condition;
	/*
	 * The depth of the innermost loop or branch block around this line, or
	 * 0 if there is none. A plain `{ }` block, opened to scope a variable,
	 * does not count: what is reached inside one is reached whenever the
	 * block is. The dispatch scan needs this to decide whether an exit it
	 * meets is taken every time the case runs.
	 */
	int control_depth;
};

/* Where the next line to be read sits. Call it before advancing the brace
 * state over that line, so the line is judged by the state it was reached
 * in. */
static void body_pos_at(const struct brace_state *b, struct body_pos *pos)
{
	int i;

	pos->branched = inside_a_branch(b);
	pos->depth = b->depth;
	pos->continues_a_condition = b->pending != BLOCK_PLAIN;

	/* kind[i] is the block that depth i+1 opened, so the innermost loop or
	 * branch around this line is the deepest such i, and the block itself
	 * spans depth i+1. */
	pos->control_depth = 0;
	for (i = 0; i < b->depth && i < MAX_BLOCK_DEPTH; i++)
		if (b->kind[i] != BLOCK_PLAIN)
			pos->control_depth = i + 1;
}

/*
 * Is this line a function definition rather than a prototype or a call? It
 * names the function in front of its first '('. A line that ends the
 * statement is a declaration -- unless it also opens a body, because a
 * definition whose body starts on the signature's line carries that body's
 * semicolons too.
 *
 * The indentation says nothing: C lets a definition start in any column, so
 * the caller decides what is file scope by the brace depth and leading
 * whitespace is skipped here.
 */
static int is_definition_line(const char *line, const char *function)
{
	char name[64];

	while (*line == ' ' || *line == '\t')
		line++;
	if (*line == '\0' || *line == '\n' || *line == '#')
		return 0;
	if (strchr(line, ';') != NULL && strchr(line, '{') == NULL)
		return 0;
	return call_name(line, name, sizeof(name)) &&
	       strcmp(name, function) == 0;
}

/*
 * Read one function's body out of C source, a line at a time, with where in
 * the body each line sits. The body runs from the definition's '{' until the
 * braces balance again, so a definition that opens and closes on one line is
 * read rather than skipped.
 *
 * Returns the number of body lines read. Zero means the source holds no such
 * definition. Takes an open stream rather than a path so a test can hand it a
 * function written for the occasion, which is how the scan's own reading of a
 * `switch` is checked below.
 */
static int read_function_body_stream(FILE *f, const char *function,
                                     void (*visit)(const char *line,
                                                   const struct body_pos *pos,
                                                   void *ctx),
                                     void *ctx)
{
	char line[SCAN_LINE_MAX];
	int in_block = 0, in_fn = 0, opened = 0, lines = 0;
	struct brace_state braces;
	struct body_pos pos;

	memset(&braces, 0, sizeof(braces));

	while (fgets(line, sizeof(line), f) != NULL) {
		strip_comments(line, &in_block);
		strip_string_literals(line);

		if (!in_fn) {
			const char *body;
			const char *text = line;

			while (*text == ' ' || *text == '\t')
				text++;
			/*
			 * Track the depth between functions as well, because
			 * only a definition at file scope is the one asked
			 * for: the same text inside another function's body is
			 * not. A directive's braces are not the code's, so a
			 * '#' line is left out of the count.
			 */
			if (*text == '#')
				continue;
			if (braces.depth != 0 ||
			    !is_definition_line(line, function)) {
				brace_state_advance(&braces, line);
				continue;
			}
			in_fn = 1;
			opened = 0;
			memset(&braces, 0, sizeof(braces));
			brace_state_advance(&braces, line);
			if (braces.depth > 0)
				opened = 1;
			/*
			 * A body that starts on the signature's line is body all
			 * the same, so read what follows the '{' -- and only that,
			 * because the signature names the function in front of its
			 * own '(' and would otherwise read as a call to itself.
			 */
			body = strchr(line, '{');
			if (body != NULL) {
				lines++;
				body_pos_at(&braces, &pos);
				visit(body + 1, &pos, ctx);
			}
			if (opened && braces.depth == 0)
				in_fn = 0;
			continue;
		}

		lines++;
		body_pos_at(&braces, &pos);
		visit(line, &pos, ctx);

		brace_state_advance(&braces, line);
		if (braces.depth > 0)
			opened = 1;
		if (opened && braces.depth == 0)
			in_fn = 0;
	}

	return lines;
}

/* The same, for a source file. -1 if the file could not be opened. */
static int read_function_body(const char *path, const char *function,
                              void (*visit)(const char *line,
                                            const struct body_pos *pos,
                                            void *ctx),
                              void *ctx)
{
	int lines;
	FILE *f = fopen(path, "r");

	if (f == NULL)
		return -1;
	lines = read_function_body_stream(f, function, visit, ctx);
	fclose(f);
	return lines;
}

static void visit_usb_service(const char *line, const struct body_pos *pos,
                              void *ctx)
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
		if (!pos->branched)
			out->direct_every_turn++;
	}
	/* A call, for the same reason: naming the control service in a callback
	 * the loop rarely reaches would not keep the host's requests answered. */
	if (calls_unconditionally(line, "handle_usb_control")) {
		out->control++;
		if (!pos->branched)
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
	char line[SCAN_LINE_MAX];
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
	mu_assert(found.direct_every_turn > 0,
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
 * What tells one function's body from the next is the brace depth, not the
 * indentation: C allows a definition to start in any column, so a scan that
 * reads column 1 as "file scope" can be evaded by indenting the definition by
 * one space. At depth 0 a definition names something in front of its first
 * '(' and does not end the statement; a line ending in ';' there is a
 * declaration, and a '}' is the end of a body, so both mean the lines that
 * follow belong to no function yet.
 */
static void scan_file_for_producers(const char *dir, const char *file)
{
	char path[512], line[SCAN_LINE_MAX], name[64], current[64];
	int in_block = 0, recorded = 0;
	struct brace_state braces;
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", dir, file);
	f = fopen(path, "r");
	if (f == NULL)
		return;

	scanned_files++;
	current[0] = '\0';
	memset(&braces, 0, sizeof(braces));

	while (fgets(line, sizeof(line), f) != NULL) {
		const char *text;
		int in_a_body = 1;

		strip_comments(line, &in_block);
		strip_string_literals(line);

		text = line;
		while (*text == ' ' || *text == '\t')
			text++;

		if (braces.depth == 0) {
			in_a_body = 0;
			/* The macro guard below is what covers these, and a
			 * directive's braces are not the code's. */
			if (*text == '#')
				continue;
			/* A body opened on the signature's line is still a
			 * definition, ';' and all. */
			if (*text == '}' || (strchr(text, ';') != NULL &&
			                     strchr(text, '{') == NULL)) {
				current[0] = '\0';
			} else if (definition_name(text, name, sizeof(name))) {
				snprintf(current, sizeof(current), "%s", name);
				recorded = 0;
				/* ...and its first body line is this one. */
				in_a_body = strchr(text, '{') != NULL;
			}
			/*
			 * Anything else at file scope -- the brace that opens
			 * the body on its own line, a continuation of the
			 * signature -- leaves the name standing: the body it
			 * belongs to has not been read yet.
			 */
		}
		if (!in_a_body || current[0] == '\0' || recorded ||
		    !line_calls_enqueue(line)) {
			brace_state_advance(&braces, line);
			continue;
		}
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
		brace_state_advance(&braces, line);
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
	static const char libs[] = "$(LIBS_PATH)/";

	if (strncmp(token, "$(TARGET)", 9) == 0)
		return "bluetooth_rxtx.c";
	if (token[0] != '$')
		return token;
	/* The shared firmware directory, which is one of the scanned ones. */
	if (strncmp(token, libs, sizeof(libs) - 1) == 0)
		return token + sizeof(libs) - 1;
	/*
	 * Any other Make variable is a directory whose value this scan does
	 * not know. Stripping the prefix and keeping the last component would
	 * let a source in a directory nobody scans pass as the file of the
	 * same name in one that is scanned, so say so instead of guessing.
	 */
	return NULL;
}

static int sources_listed;
static char unscanned_source[256];

static void read_the_images_source_list(void)
{
	char path[512], line[SCAN_LINE_MAX];
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
			if ((file == NULL || !is_in_a_scanned_dir(file)) &&
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
	         "%s is compiled into this image and is not in a directory the "
	         "scans read -- or is under a Make variable they cannot resolve, "
	         "which is the same thing -- so a mode in it would never be "
	         "classified. Put it in one of them, or add its directory to "
	         "scanned_dirs[] and teach source_list_name() the variable",
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
	char path[512], line[SCAN_LINE_MAX], name[64], current[64];
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
	char path[512], line[SCAN_LINE_MAX];
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
	char path[512], line[SCAN_LINE_MAX], ident[128];
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
	char path[512], line[SCAN_LINE_MAX], ident[128];
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
	char path[512], line[SCAN_LINE_MAX];
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
	char label[48];
	char calls[MAX_CASE_CALLS][64];
	int call_count;
	int listed;
};

/*
 * Room for the cases the tables below list, plus the ones a change adds
 * before anyone lists them -- which is what the guards are for. The
 * written-out dispatch at the end of this file is larger than the real one,
 * since it holds a case for every shape, so the headroom covers it too; a
 * pass that runs out says so rather than losing a case.
 */
#define MAX_CASES_FOUND ((int)DISPATCH_COUNT + 20)

/*
 * What one pass over a `switch` found, and where the reader has got to. The
 * state is a struct rather than file-scope variables so a second pass -- the
 * fixture below, or the other image's request dispatch -- does not disturb the
 * first, and so no test depends on another having run.
 */
struct dispatch_scan {
	/* Which labels this pass is reading: "MODE_" for the mode dispatch,
	 * "BTCTL_" for the other image's request dispatch. A label with
	 * another prefix belongs to some other switch and is skipped. */
	const char *prefix;
	struct dispatch_case cases[MAX_CASES_FOUND];
	int case_count;
	/* The cases whose body is still being read: a run of them where one
	 * label falls through to the next, and every call found is recorded
	 * against all of them. */
	int pending[MAX_CASES_FOUND];
	int pending_count;
	/*
	 * The brace depth at which a `break` ends those cases. Anything deeper
	 * belongs to a loop or a branch inside the case, and the case carries
	 * on below it -- which is the whole reason the depth is tracked. It is
	 * the depth of the label, or one deeper when the case body is a block
	 * of its own (`case MODE_X: {`).
	 */
	int case_level;
	/* A label has been read and its body has not started, so a '{' now
	 * opens that block of its own. */
	int awaiting_body;
	/* The statement that leaves the case has been read. The case closes at
	 * the end of this line rather than here, so a call in that same
	 * statement -- `return the_real_loop();` -- is still recorded. */
	int leaving;
	/* That statement was a `goto`, which may be going no further than a
	 * label inside this same case. */
	int resuming;
	/* The run a `goto` left, kept until the next `case` label in case a
	 * label inside the case takes it up again. */
	int resumable[MAX_CASES_FOUND];
	int resumable_count;
	int lines;
	int overflowed;
	/*
	 * The text after a `case` that this pass could not turn into a label
	 * at all. Skipping one silently would hide a whole case: its calls
	 * would be recorded against the case above it, and the mode it handles
	 * would appear in no table. The checks below fail on it instead.
	 */
	char unreadable[64];
	/* A label at the same depth as the ones being read, carrying another
	 * name. Walking past it would hide a case of the same switch. */
	char foreign[64];
	/*
	 * Both of those are kept with the depth they were met at and judged
	 * when the pass ends, because which depth matters is only known once
	 * a label of this pass's own has been seen -- and the label in
	 * question may come first.
	 */
	int unreadable_level;
	int foreign_level;
	/* The depth the labels being read sit at, once one has been seen.
	 * -1 until then. A label deeper than this belongs to a nested
	 * switch. */
	int switch_level;
	/*
	 * A `switch` has been opened inside the case being read and has not
	 * written its '{' yet. Its labels then sit at the same depth as the
	 * dispatch's own, and nothing in the text says which switch they
	 * belong to, so they are reported rather than guessed at.
	 */
	int nested_switch_pending;
};

/* Does a ':' follow, rather than anything else? Then the word before it named
 * a label. A '::' cannot occur in C, so a single ':' is enough. */
static int names_a_label(const char *after_word)
{
	while (*after_word == ' ' || *after_word == '\t')
		after_word++;
	return *after_word == ':';
}

/* What read_case_label() made of the text after a `case`. */
enum label_read {
	/* A label carrying this pass's prefix; `out` holds it. */
	LABEL_READ,
	/* An identifier with some other prefix, so it belongs to another
	 * switch and this pass is right to walk past it. */
	LABEL_OTHER_PREFIX,
	/* Neither: a label this pass cannot read. */
	LABEL_UNREADABLE,
};

/*
 * A label this pass walked past that is at the same brace depth as the ones
 * it is reading, so it is in the same `switch` and not in some other one. It
 * is as invisible as an unreadable label and reported the same way.
 */
static const char *foreign_case_message(const char *text, const char *prefix)
{
	static char msg[384];

	snprintf(msg, sizeof(msg),
	         "the scan met `case %s:` in the same switch as the %s cases, "
	         "and walked past it because of the name: its calls are "
	         "recorded against the case above it and whatever it handles "
	         "appears in no table here. Name it with the %s prefix, or give "
	         "its switch a pass of its own -- and if it is a nested switch, "
	         "give it braces, which is how the scan tells one from the "
	         "dispatch", text, prefix, prefix);
	return msg;
}

/*
 * The label a `case` names, when it carries the prefix this pass is reading.
 * `p` is left after the label; on anything else it is left where it was, so
 * the caller reads that text as ordinary code.
 *
 * A label that is not a bare identifier (`case (MODE_X):`, `case MODE_X + 1:`)
 * and one whose identifier is too long for `out` both come back
 * LABEL_UNREADABLE, with up to `size` of the text in `out` so the failure can
 * quote it. Neither shape
 * appears in the dispatches scanned here, where every case is a bare prefixed
 * name; one turning up at the depth the pass is reading means a case it would
 * otherwise walk straight past. One deeper belongs to a nested switch -- a
 * character literal reads as unreadable, since the literal is stripped before
 * the scan sees the line -- and the caller ignores those.
 */
static enum label_read read_case_label(const char **p, const char *prefix,
                                       char *out, size_t size)
{
	const char *q = *p;
	const char *start;
	size_t len;

	while (*q == ' ' || *q == '\t')
		q++;
	start = q;
	while (is_ident_char(*q))
		q++;
	len = (size_t)(q - start);

	/*
	 * The label has to be the whole of what comes before the ':'. Without
	 * that, `case MODE_X + 1:` reads as MODE_X and is recorded as that
	 * case, so an expression label naming a different mode would be
	 * classified as the one its first name happens to be.
	 */
	if (len > 0 && !names_a_label(q))
		len = 0;

	if (len == 0 || len >= size) {
		/* Quote what is there, up to the first ':' or the end of the
		 * line, so the failure names the case. */
		size_t n = 0;

		while (start[n] != '\0' && start[n] != ':' && start[n] != '\n' &&
		       n < size - 1)
			n++;
		memcpy(out, start, n);
		out[n] = '\0';
		return LABEL_UNREADABLE;
	}
	memcpy(out, start, len);
	out[len] = '\0';
	if (strncmp(start, prefix, strlen(prefix)) != 0)
		return LABEL_OTHER_PREFIX;

	*p = q;
	return LABEL_READ;
}

/* Start a pass. `switch_level` is -1 rather than 0 because 0 is a real brace
 * depth, and until a label of this pass's own is seen there is no switch to
 * compare another label against. */
static void dispatch_scan_init(struct dispatch_scan *s, const char *prefix)
{
	memset(s, 0, sizeof(*s));
	s->prefix = prefix;
	s->switch_level = -1;
	s->unreadable_level = -1;
	s->foreign_level = -1;
}

/*
 * Finish a pass: drop a label that was met deeper than this pass's own, since
 * it belongs to a nested switch. Only now is the depth of this pass's labels
 * known, which is why the labels are judged here rather than where they were
 * met: the one worth reporting may be the switch's first case.
 */
static void dispatch_scan_done(struct dispatch_scan *s)
{
	if (s->unreadable_level != s->switch_level)
		s->unreadable[0] = '\0';
	if (s->foreign_level != s->switch_level)
		s->foreign[0] = '\0';
}

/* Record a call against every case whose body is still being read. */
static void add_call_to_pending(struct dispatch_scan *s, const char *name)
{
	int i, j;

	for (i = 0; i < s->pending_count; i++) {
		struct dispatch_case *c = &s->cases[s->pending[i]];

		for (j = 0; j < c->call_count; j++)
			if (strcmp(c->calls[j], name) == 0)
				break;
		if (j < c->call_count)
			continue;
		if (c->call_count >= MAX_CASE_CALLS) {
			s->overflowed = 1;
			continue;
		}
		snprintf(c->calls[c->call_count], sizeof(c->calls[0]), "%s",
		         name);
		c->call_count++;
	}
}

static void add_case_label(struct dispatch_scan *s, const char *label,
                           int depth)
{
	if (s->case_count >= MAX_CASES_FOUND) {
		s->overflowed = 1;
		return;
	}
	memset(&s->cases[s->case_count], 0, sizeof(s->cases[0]));
	snprintf(s->cases[s->case_count].label, sizeof(s->cases[0].label), "%s",
	         label);
	/* A new label means no label inside the previous case is coming, so
	 * the run a `goto` left there is not resumed. */
	s->resumable_count = 0;
	/* The depth this switch's labels sit at, for telling a label of
	 * another switch from one of this switch with another name. */
	if (s->switch_level < 0)
		s->switch_level = depth;
	/* The first label of a fall-through run fixes the level; the rest of
	 * the run share it, being labels on the same switch. */
	if (s->pending_count == 0)
		s->case_level = depth;
	s->pending[s->pending_count++] = s->case_count;
	s->case_count++;
	s->awaiting_body = 1;
}

/* A keyword that makes what comes after it conditional, so a `break` that
 * follows it on the same line is not the one that ends the case. */
static int opens_a_condition(const char *word)
{
	static const char *const words[] = {
		"if", "else", "while", "for", "do", "switch",
	};
	size_t i;

	for (i = 0; i < sizeof(words) / sizeof(words[0]); i++)
		if (strcmp(word, words[i]) == 0)
			return 1;
	return 0;
}

/*
 * A keyword that leaves the case, so the next label starts a new one.
 * `continue` is one of them here because this dispatch is inside a loop of
 * its own, so a case can end by going round it again.
 */
static int leaves_the_case(const char *word)
{
	return strcmp(word, "break") == 0 || strcmp(word, "return") == 0 ||
	       strcmp(word, "goto") == 0 || strcmp(word, "continue") == 0;
}

/* Is `word` the first thing on its line? A label is written that way, and
 * requiring it keeps the middle of a conditional expression (`a ? b : c`)
 * from reading as one. */
static int starts_the_line(const char *line, const char *word)
{
	const char *p = line;

	while (p < word) {
		if (*p != ' ' && *p != '\t')
			return 0;
		p++;
	}
	return 1;
}


/*
 * Read one line of a dispatch into the scan: the labels it opens, the
 * functions it calls, and whether it ends the cases being read.
 *
 * A case ends at a `break`, `return`, `goto` or `continue` of its own: one the
 * case reaches every time it runs. Taking the first `break` on any body line
 * for the end of the case misreads a case shaped
 *
 *	while (something()) { if (...) break; }
 *	the_real_loop();
 *	break;
 *
 * as ending at the inner `break`: the_real_loop() is never recorded, the row
 * for that mode is satisfied by something(), and the mode could then collect
 * packets through a loop no row in the tables above describes. The same goes
 * for a case whose first act is `if (...) break;` and whose real work is
 * below it.
 *
 * The brace depth is what tells the two kinds of `break` apart. The
 * `branched` flag cannot: it is relative to the function body, and every case
 * here is already inside main()'s `while` and `switch`, so it is 1 throughout.
 *
 * Where the line sits is taken at its start, so the scan reads one statement
 * per line -- the style the firmware is written in. A whole loop written on
 * one line (`while (x) { prep(); break; }`) is read as ending the case after
 * prep(), and a label and its statement sharing a line go together. The
 * scans above read the sources the same way, which is why the check that no
 * line is too long for them matters.
 */
static void visit_dispatch_line(const char *line, const struct body_pos *pos,
                                void *ctx)
{
	struct dispatch_scan *s = ctx;
	const char *p = line;
	int depth = pos->depth;
	int parens = 0;
	/* A condition that opened on an earlier line reaches into this one. */
	int conditional = pos->continues_a_condition;

	s->lines++;

	while (*p != '\0') {
		char word[64];
		const char *start, *after;

		if (!is_ident_char(*p)) {
			switch (*p) {
			case '{':
				depth++;
				if (s->awaiting_body && s->pending_count > 0)
					s->case_level = depth;
				s->awaiting_body = 0;
				/* The nested switch wrote its braces after
				 * all, so its labels sit deeper and are told
				 * apart by depth. */
				s->nested_switch_pending = 0;
				break;
			case '}':
				depth--;
				conditional = 0;
				s->awaiting_body = 0;
				/*
				 * Shallower than the labels means the switch
				 * body has closed, so every case in it is
				 * finished and no label below can be one of
				 * theirs -- however the scopes after the
				 * switch are nested, and whatever raw depth
				 * they reach.
				 */
				if (s->switch_level >= 0 &&
				    depth < s->switch_level)
					s->resumable_count = 0;
				break;
			case '(':
				parens++;
				break;
			case ')':
				if (parens > 0)
					parens--;
				break;
			case ';':
				/* A `for` header's own semicolons do not end
				 * the statement it heads. */
				if (parens == 0)
					conditional = 0;
				break;
			/* The label's own colon is not its body starting. */
			case ':':
			case ' ':
			case '\t':
			case '\n':
			case '\r':
				break;
			default:
				s->awaiting_body = 0;
				break;
			}
			p++;
			continue;
		}

		start = p;
		while (is_ident_char(*p))
			p++;
		if ((size_t)(p - start) >= sizeof(word))
			continue;
		memcpy(word, start, (size_t)(p - start));
		word[p - start] = '\0';

		if (strcmp(word, "case") == 0) {
			char label[sizeof(s->cases[0].label)];

			switch (read_case_label(&p, s->prefix, label,
			                        sizeof(label))) {
			case LABEL_READ:
				if (s->nested_switch_pending) {
					/* Carries the prefix, but sits where a
					 * nested switch with no braces put it,
					 * so which switch it is in cannot be
					 * told from the text. */
					if (s->unreadable[0] == '\0') {
						snprintf(s->unreadable,
						         sizeof(s->unreadable),
						         "%s", label);
						s->unreadable_level = depth;
					}
				} else if (s->switch_level >= 0 &&
				           depth > s->switch_level) {
					/* Deeper than the dispatch's own
					 * labels: a nested switch of its own,
					 * and none of this pass's business. */
					break;
				} else {
					add_case_label(s, label, depth);
				}
				break;
			case LABEL_UNREADABLE:
				if (s->unreadable[0] == '\0') {
					snprintf(s->unreadable,
					         sizeof(s->unreadable), "%s",
					         label);
					s->unreadable_level = depth;
				}
				break;
			case LABEL_OTHER_PREFIX:
				if (s->foreign[0] == '\0') {
					snprintf(s->foreign,
					         sizeof(s->foreign), "%s",
					         label);
					s->foreign_level = depth;
				}
				break;
			}
			continue;
		}

		/*
		 * A plain label at the start of a line and at the level the
		 * case's own statements sit at, which is where a `goto` inside
		 * the case lands. Such a `goto` never left the case, so the run
		 * it closed is taken up again and the work below the label is
		 * recorded against it.
		 *
		 * What keeps this to the case: the run to take up is dropped
		 * when the switch body closes, so a label after the switch is
		 * never one of these however deeply it is nested. Within the
		 * switch, the label has to be at or below the level the case's
		 * own statements sit at, with no loop or branch of the case
		 * around it, so a plain `{ }` block opened to scope a variable
		 * still holds the case's own labels.
		 */
		if (s->pending_count == 0 && s->resumable_count > 0 &&
		    pos->depth >= s->case_level &&
		    pos->control_depth <= s->case_level &&
		    starts_the_line(line, start) && strcmp(word, "default") != 0 &&
		    names_a_label(p)) {
			memcpy(s->pending, s->resumable,
			       sizeof(s->pending[0]) * (size_t)s->resumable_count);
			s->pending_count = s->resumable_count;
			continue;
		}

		if (s->pending_count == 0)
			continue;
		if (leaves_the_case(word)) {
			/*
			 * This exit ends the case when the case takes it every
			 * time it runs, which is true when no loop or branch
			 * block inside the case encloses it and no condition
			 * reaches into the line.
			 *
			 * Brace depth alone is not the test. A `break` inside
			 * a loop in the case belongs to the loop, so the case
			 * carries on below it -- the whole reason the depth is
			 * tracked -- but a `break` inside a plain `{ }` block
			 * opened to scope a variable still ends the case, and
			 * so does one written after a case body block closes,
			 * a level above the statements it follows:
			 *
			 *	case MODE_X: {
			 *		work();
			 *	}
			 *	break;
			 *
			 * So what matters is whether any loop or branch block
			 * was opened at or below the case's own level, which
			 * is what `control_depth` says. The switch's own block
			 * sits at the case's level and does not count.
			 *
			 * `awaiting_body` overrides the condition because the
			 * brace state remembers a `case` label as a branch --
			 * which is right for the checks above, where a call in
			 * a case body is a conditional call, and wrong here.
			 * While no statement has started, the only thing open
			 * is the label, so this is the case's own exit. Without
			 * that, a case whose whole body is `break;` is never
			 * ended and every later case's calls are folded into
			 * it.
			 */
			if (pos->control_depth <= s->case_level &&
			    (!conditional || s->awaiting_body)) {
				/*
				 * The statement that leaves can still carry the
				 * call that does the work (`return work();`),
				 * so the rest of the line is read before the
				 * case closes.
				 */
				s->leaving = 1;
				/*
				 * A `goto` may be going no further than a
				 * label inside this same case, which does not
				 * leave it. Remember the run so such a label
				 * can take it up again.
				 */
				if (strcmp(word, "goto") == 0)
					s->resuming = 1;
				else
					s->resuming = 0;
			}
			s->awaiting_body = 0;
			continue;
		}
		if (opens_a_condition(word)) {
			conditional = 1;
			if (strcmp(word, "switch") == 0)
				s->nested_switch_pending = 1;
			s->awaiting_body = 0;
			continue;
		}
		s->awaiting_body = 0;
		/* Every call on the line, not only the first: a case that calls
		 * something harmless before the function that does the work
		 * would otherwise be read by its first call alone. */
		after = p;
		while (*after == ' ' || *after == '\t')
			after++;
		if (*after == '(' && !is_not_a_function_name(word))
			add_call_to_pending(s, word);
	}

	/* The line that leaves the case has been read to its end, so the case
	 * can close now. */
	if (s->leaving) {
		s->leaving = 0;
		s->nested_switch_pending = 0;
		s->resumable_count = 0;
		if (s->resuming) {
			memcpy(s->resumable, s->pending,
			       sizeof(s->pending[0]) * (size_t)s->pending_count);
			s->resumable_count = s->pending_count;
			s->resuming = 0;
		}
		s->pending_count = 0;
	}
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

static void visit_reach(const char *line, const struct body_pos *pos,
                        void *ctx)
{
	struct reach *r = ctx;
	size_t i;

	(void)pos;
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

static struct dispatch_scan dispatch_found;
static int dispatch_scanned;
static char unlisted_case[64];

/* What to say about a `case` whose label the scan could not read. */
static const char *unreadable_case_message(const char *text, const char *prefix)
{
	static char msg[384];

	snprintf(msg, sizeof(msg),
	         "the scan met `case %s:`, which it cannot read as a %s name, "
	         "so that case is invisible to it: its calls are recorded "
	         "against the case above it and whatever it handles appears in "
	         "no table here. Write it as a bare name, short enough for the "
	         "scan, or teach the scan this shape", text, prefix);
	return msg;
}

static void scan_dispatch(void)
{
	char path[512];
	size_t i;
	int c;

	snprintf(path, sizeof(path), "%s/bluetooth_rxtx.c", FW_RXTX_DIR);
	dispatch_scan_init(&dispatch_found, "MODE_");
	dispatch_scanned = read_function_body(path, "main", visit_dispatch_line,
	                                      &dispatch_found) > 0;
	dispatch_scan_done(&dispatch_found);
	if (!dispatch_scanned)
		return;

	/* Match the cases the scan found against the table, and resolve what
	 * each case's calls lead to. */
	for (c = 0; c < dispatch_found.case_count; c++) {
		struct dispatch_case *found_case = &dispatch_found.cases[c];

		for (i = 0; i < DISPATCH_COUNT; i++) {
			int k;

			if (strcmp(dispatch[i].mode, found_case->label) != 0)
				continue;
			dispatch[i].found = 1;
			found_case->listed = 1;
			for (k = 0; k < found_case->call_count; k++) {
				struct reach r;

				function_reach(found_case->calls[k], &r);
				dispatch[i].loops |= r.loops;
				dispatch[i].reaches_queue |= r.producer;
			}
		}
		if (!found_case->listed && unlisted_case[0] == '\0')
			snprintf(unlisted_case, sizeof(unlisted_case), "%s",
			         found_case->label);
	}
}

MU_TEST(the_dispatch_scan_read_every_case)
{
	size_t i;

	mu_assert_fatal(dispatch_scanned,
	                "cannot read main() in " FW_RXTX_DIR
	                "/bluetooth_rxtx.c, so the checks below would pass for "
	                "want of anything to look at");
	mu_assert(!dispatch_found.overflowed,
	          "the dispatch has more cases, or a case more calls, than this "
	          "scan can hold");
	mu_assert(dispatch_found.unreadable[0] == '\0', unreadable_case_message(
	          dispatch_found.unreadable, "MODE_"));
	mu_assert(dispatch_found.foreign[0] == '\0', foreign_case_message(
	          dispatch_found.foreign, "MODE_"));
	mu_assert(dispatch_found.lines > 50,
	          "main() scanned as fewer than 50 lines, so the scan lost "
	          "track of the function");
	mu_assert(dispatch_found.case_count >= (int)DISPATCH_COUNT,
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

/* A mode the dispatch runs and nothing here classifies is the hole this table
 * exists to close: the mode streams, and no test says which transport its host
 * uses, so the mode can be wired to the wrong one and still pass. */
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

		for (c = 0; c < dispatch_found.case_count; c++) {
			const struct dispatch_case *f = &dispatch_found.cases[c];
			int k;

			if (strcmp(f->label, dispatch[i].mode) != 0)
				continue;
			calls += f->call_count;
			for (k = 0; k < f->call_count; k++)
				if (dispatch[i].runs != NULL &&
				    strcmp(f->calls[k], dispatch[i].runs) == 0)
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
 * bt_stream_rx() for a host that polls captures nothing at all -- the loop
 * hands packets to the bulk IN endpoint, which such a host never reads -- and
 * this is what says so.
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
 * The dispatch scan reads a case to its end
 * ------------------------------------------------------------------ */

/*
 * Everything above rests on the scan finding the function each case really
 * runs. No case in the firmware hides its work behind an exit that is not its
 * own, so the checks above would all pass with a scan that could not read
 * such a case -- it would simply stop noticing one being added. The shapes
 * are therefore written out here and the scan is run over them, which is what
 * makes each half of its reading fail when it is taken away.
 *
 * Each shape puts a case's work somewhere a simpler reading loses it:
 *
 *  - below a loop's own `break`, and below a bare condition's;
 *  - inside a case body that is a block, with the `break` inside the block
 *    and, separately, written after its closing brace -- a level above the
 *    statements it follows;
 *  - below a `goto` that lands on a label inside the same case. Such a `goto`
 *    never left the case. One case jumps into a plain block and works there,
 *    which is still inside it;
 *  - after a `goto` out of the switch, where the work belongs to no case. The
 *    switch's last case leaves that way, and the label it lands on sits in
 *    plain blocks of its own: counting depth alone would read that label as
 *    the case carrying on;
 *  - in the statement that leaves (`return the_work();`);
 *  - inside plain `{ }` blocks opened to scope a variable, which look like a
 *    loop's braces by depth alone but which the case passes through every
 *    time it runs;
 *  - below a `break` that belongs to a braced `if` or loop with nothing
 *    conditional on the line itself, which is the shape ordinary style
 *    produces and the one the brace-depth reading loses;
 *  - after a `continue`, which leaves the case by going round the loop the
 *    dispatch sits in;
 *  - below a condition and its `break` written on one line, and below a loop
 *    whose braces the case reaches after a statement of its own. Each of
 *    those puts a keyword where the scan has to read it: without the first
 *    the condition is missed, without the second the braces read as an
 *    ordinary block, and the `for` header is there because its own
 *    semicolons must not settle the loop it heads;
 *  - and one case that does nothing but `break`, whose body is a comment.
 *    That one has no work to lose; it is here because a case with no
 *    statement of its own is the shape most easily read as never ending.
 *
 * After each comes a case of its own, because the other half of reading a
 * case is still ending it: a scan that never ended one would find every call
 * in all of them and quietly fold the rest of the switch into the first. The
 * run of two labels and the case that leaves by `goto` are the ways out the
 * firmware's own dispatches use.
 */
static const char *const dispatch_shapes =
	"int main(void)\n"
	"{\n"
	"	while (1) {\n"
	"		switch (requested_mode) {\n"
	"		case MODE_INNER_LOOP:\n"
	"			while (waiting()) {\n"
	"				if (requested_mode != MODE_INNER_LOOP)\n"
	"					break;\n"
	"			}\n"
	"			inner_loop_work();\n"
	"			break;\n"
	"		case MODE_AFTER_INNER_LOOP:\n"
	"			after_inner_loop_work();\n"
	"			break;\n"
	"		case MODE_BARE_CONDITION:\n"
	"			if (!ready())\n"
	"				break;\n"
	"			bare_condition_work();\n"
	"			break;\n"
	"		case MODE_AFTER_BARE_CONDITION:\n"
	"			after_bare_condition_work();\n"
	"			break;\n"
	"		case MODE_BLOCK_BODY: {\n"
	"			block_body_work();\n"
	"			break;\n"
	"		}\n"
	"		case MODE_BLOCK_THEN_BREAK: {\n"
	"			block_then_break_work();\n"
	"		}\n"
	"		break;\n"
	"		case MODE_AFTER_BLOCK_THEN_BREAK:\n"
	"			after_block_then_break_work();\n"
	"			break;\n"
	"		case MODE_FALLS_THROUGH:\n"
	"		case MODE_FALLEN_INTO:\n"
	"			shared_work();\n"
	"			break;\n"
	"		case MODE_GOTO_OUT:\n"
	"			goto_out_work();\n"
	"			goto done;\n"
	"		case MODE_GOTO_INSIDE:\n"
	"			preface();\n"
	"			goto inside;\n"
	"		inside:\n"
	"			goto_inside_work();\n"
	"			break;\n"
	"		case MODE_AFTER_GOTO_INSIDE:\n"
	"			after_goto_inside_work();\n"
	"			break;\n"
	"		case MODE_GOTO_INTO_SCOPE:\n"
	"			preface();\n"
	"			goto in_scope;\n"
	"			{\n"
	"				int scoped = 0;\n"
	"			in_scope:\n"
	"				goto_into_scope_work();\n"
	"			}\n"
	"			break;\n"
	"		case MODE_AFTER_GOTO_INTO_SCOPE:\n"
	"			after_goto_into_scope_work();\n"
	"			break;\n"
	"		case MODE_NOTHING_BUT_BREAK:\n"
	"			/* a comment, and no statement of its own */\n"
	"			break;\n"
	"		case MODE_AFTER_NOTHING_BUT_BREAK:\n"
	"			after_nothing_but_break_work();\n"
	"			break;\n"
	"		case MODE_RETURNS_THE_CALL:\n"
	"			preface();\n"
	"			return returns_the_call_work();\n"
	"		case MODE_SCOPED_BLOCKS:\n"
	"			{\n"
	"				int scoped = 0;\n"
	"				{\n"
	"					scoped_blocks_work();\n"
	"					break;\n"
	"				}\n"
	"			}\n"
	"		case MODE_AFTER_SCOPED_BLOCKS:\n"
	"			after_scoped_blocks_work();\n"
	"			break;\n"
	"		case MODE_BRACED_IF:\n"
	"			if (x) {\n"
	"				break;\n"
	"			}\n"
	"			braced_if_work();\n"
	"			break;\n"
	"		case MODE_LOOP_PLAIN_BREAK:\n"
	"			while (y) {\n"
	"				prep();\n"
	"				break;\n"
	"			}\n"
	"			loop_plain_break_work();\n"
	"			break;\n"
	"		case MODE_CONTINUES:\n"
	"			continues_work();\n"
	"			continue;\n"
	"		case MODE_AFTER_CONTINUES:\n"
	"			after_continues_work();\n"
	"			break;\n"
	"		case MODE_SAME_LINE_CONDITION:\n"
	"			same_line_prep();\n"
	"			if (x) break;\n"
	"			same_line_condition_work();\n"
	"			break;\n"
	"		case MODE_AFTER_SAME_LINE_CONDITION:\n"
	"			after_same_line_condition_work();\n"
	"			break;\n"
	"		case MODE_STATEMENT_THEN_LOOP:\n"
	"			statement_then_loop_prep();\n"
	"			while (y) {\n"
	"				break;\n"
	"			}\n"
	"			statement_then_loop_work();\n"
	"			break;\n"
	"		case MODE_AFTER_STATEMENT_THEN_LOOP:\n"
	"			after_statement_then_loop_work();\n"
	"			break;\n"
	"		case MODE_FOR_HEADER:\n"
	"			for_header_prep();\n"
	"			for (i = 0; i < n; i++) {\n"
	"				break;\n"
	"			}\n"
	"			for_header_work();\n"
	"			break;\n"
	"		case MODE_AFTER_FOR_HEADER:\n"
	"			after_for_header_work();\n"
	"			break;\n"
	"		case MODE_LAST:\n"
	"			last_work();\n"
	"			break;\n"
	"		case MODE_GOTO_OUT_LAST:\n"
	"			goto_out_last_work();\n"
	"			goto done;\n"
	"		}\n"
	"	}\n"
	"	{\n"
	"		{\n"
	"		done:\n"
	"			after_the_switch_work();\n"
	"		}\n"
	"	}\n"
	"	return 0;\n"
	"}\n";

#define DISPATCH_SHAPE_COUNT 31

/* Run the case scan over a function written out as text. tmpfile() rather
 * than a path: the scans walk whole directories, so a C file left in one of
 * them would be read as firmware by every other check in this file. */
static int scan_dispatch_text(const char *text, struct dispatch_scan *out)
{
	int lines;
	FILE *f = tmpfile();

	if (f == NULL)
		return 0;
	if (fputs(text, f) == EOF) {
		fclose(f);
		return 0;
	}
	rewind(f);
	dispatch_scan_init(out, "MODE_");
	lines = read_function_body_stream(f, "main", visit_dispatch_line, out);
	dispatch_scan_done(out);
	fclose(f);
	return lines > 0;
}

static const struct dispatch_case *scanned_case(const struct dispatch_scan *s,
                                                const char *label)
{
	int i;

	for (i = 0; i < s->case_count; i++)
		if (strcmp(s->cases[i].label, label) == 0)
			return &s->cases[i];
	return NULL;
}

static int case_calls(const struct dispatch_case *c, const char *name)
{
	int i;

	for (i = 0; i < c->call_count; i++)
		if (strcmp(c->calls[i], name) == 0)
			return 1;
	return 0;
}

/* The work each case does, and the work of the case after it, which the case
 * must not have absorbed. */
static const struct {
	const char *label;
	const char *own_work;
	const char *next_work;
} dispatch_shape_cases[] = {
	{ "MODE_INNER_LOOP",           "inner_loop_work",
	  "after_inner_loop_work" },
	{ "MODE_AFTER_INNER_LOOP",     "after_inner_loop_work",
	  "bare_condition_work" },
	{ "MODE_BARE_CONDITION",       "bare_condition_work",
	  "after_bare_condition_work" },
	{ "MODE_AFTER_BARE_CONDITION", "after_bare_condition_work",
	  "block_body_work" },
	{ "MODE_BLOCK_BODY",           "block_body_work",
	  "block_then_break_work" },
	{ "MODE_BLOCK_THEN_BREAK",     "block_then_break_work",
	  "after_block_then_break_work" },
	{ "MODE_AFTER_BLOCK_THEN_BREAK", "after_block_then_break_work",
	  "shared_work" },
	{ "MODE_FALLS_THROUGH",        "shared_work",         "goto_out_work" },
	{ "MODE_FALLEN_INTO",          "shared_work",         "goto_out_work" },
	{ "MODE_GOTO_OUT",             "goto_out_work",       "preface" },
	{ "MODE_GOTO_INSIDE",          "goto_inside_work",
	  "after_goto_inside_work" },
	{ "MODE_AFTER_GOTO_INSIDE",    "after_goto_inside_work",
	  "goto_into_scope_work" },
	{ "MODE_GOTO_INTO_SCOPE",      "goto_into_scope_work",
	  "after_goto_into_scope_work" },
	{ "MODE_AFTER_GOTO_INTO_SCOPE", "after_goto_into_scope_work",
	  "after_nothing_but_break_work" },
	{ "MODE_NOTHING_BUT_BREAK",     NULL,
	  "after_nothing_but_break_work" },
	{ "MODE_AFTER_NOTHING_BUT_BREAK", "after_nothing_but_break_work",
	  "returns_the_call_work" },
	{ "MODE_RETURNS_THE_CALL",     "returns_the_call_work",
	  "scoped_blocks_work" },
	{ "MODE_SCOPED_BLOCKS",        "scoped_blocks_work",
	  "after_scoped_blocks_work" },
	{ "MODE_AFTER_SCOPED_BLOCKS",  "after_scoped_blocks_work",
	  "braced_if_work" },
	{ "MODE_BRACED_IF",            "braced_if_work",
	  "loop_plain_break_work" },
	{ "MODE_LOOP_PLAIN_BREAK",     "loop_plain_break_work",
	  "continues_work" },
	{ "MODE_CONTINUES",            "continues_work",
	  "after_continues_work" },
	{ "MODE_AFTER_CONTINUES",      "after_continues_work",
	  "same_line_condition_work" },
	{ "MODE_SAME_LINE_CONDITION",  "same_line_condition_work",
	  "after_same_line_condition_work" },
	{ "MODE_AFTER_SAME_LINE_CONDITION", "after_same_line_condition_work",
	  "statement_then_loop_work" },
	{ "MODE_STATEMENT_THEN_LOOP",  "statement_then_loop_work",
	  "after_statement_then_loop_work" },
	{ "MODE_AFTER_STATEMENT_THEN_LOOP", "after_statement_then_loop_work",
	  "for_header_work" },
	{ "MODE_FOR_HEADER",           "for_header_work",
	  "after_for_header_work" },
	{ "MODE_AFTER_FOR_HEADER",     "after_for_header_work",
	  "last_work" },
	{ "MODE_LAST",                 "last_work",
	  "goto_out_last_work" },
	{ "MODE_GOTO_OUT_LAST",        "goto_out_last_work",  NULL },
};

MU_TEST(the_dispatch_scan_reads_each_case_to_its_own_break)
{
	/* Static rather than a local: the cases and their calls are a few tens
	 * of kilobytes. */
	static struct dispatch_scan shapes;
	size_t i;

	mu_assert_fatal(scan_dispatch_text(dispatch_shapes, &shapes),
	                "cannot scan the written-out dispatch, so the checks "
	                "below would pass for want of anything to look at");
	mu_assert(!shapes.overflowed,
	          "the written-out dispatch outgrew the scan's room for cases");
	mu_assert_int_eq(DISPATCH_SHAPE_COUNT, shapes.case_count);

	for (i = 0; i < sizeof(dispatch_shape_cases) /
	                sizeof(dispatch_shape_cases[0]); i++) {
		const struct dispatch_case *c =
			scanned_case(&shapes, dispatch_shape_cases[i].label);
		char msg[384];

		snprintf(msg, sizeof(msg), "the scan found no case for %s",
		         dispatch_shape_cases[i].label);
		mu_assert_fatal(c != NULL, msg);

		if (dispatch_shape_cases[i].own_work == NULL) {
			/* A case that does nothing at all; the point of it is
			 * that it still ends. */
			snprintf(msg, sizeof(msg),
			         "%s calls nothing and the scan recorded %d "
			         "call(s) against it",
			         dispatch_shape_cases[i].label, c->call_count);
			mu_assert_int_eq(0, c->call_count);
		} else {
			snprintf(msg, sizeof(msg),
			         "%s runs %s() and the scan did not see it, so a "
			         "mode of that shape would be classified by the "
			         "wrong call and could collect packets through a "
			         "loop no row describes",
			         dispatch_shape_cases[i].label,
			         dispatch_shape_cases[i].own_work);
			mu_assert(case_calls(c, dispatch_shape_cases[i].own_work),
			          msg);
		}

		snprintf(msg, sizeof(msg),
		         "the scan read after_the_switch_work() into %s. That "
		         "call is below the switch, where a case that leaves by "
		         "`goto` lands, and belongs to no case at all",
		         dispatch_shape_cases[i].label);
		mu_assert(!case_calls(c, "after_the_switch_work"), msg);

		if (dispatch_shape_cases[i].next_work == NULL)
			continue;
		snprintf(msg, sizeof(msg),
		         "the scan read %s() into %s, which is the next case's "
		         "work: the case was never ended, so every case after it "
		         "is folded into this one",
		         dispatch_shape_cases[i].next_work,
		         dispatch_shape_cases[i].label);
		mu_assert(!case_calls(c, dispatch_shape_cases[i].next_work), msg);
	}
}

/*
 * The other way a case can go unseen: a label the scan cannot read. It walks
 * past anything after `case` that is not a bare prefixed name, and past a
 * name too long for the buffer it reads into, and either leaves the case
 * invisible -- its calls recorded against the case above it, and the mode it
 * handles in no table. Both are legal C that the firmware does not use today,
 * so they are written out here, and the scan has to say it met them.
 */
static const char *const unreadable_label_shapes =
	"int main(void)\n"
	"{\n"
	"	switch (requested_mode) {\n"
	"	case (MODE_PARENTHESISED):\n"
	"		parenthesised_work();\n"
	"		break;\n"
	"	case MODE_ORDINARY:\n"
	"		ordinary_work();\n"
	"		break;\n"
	"	}\n"
	"	return 0;\n"
	"}\n";

/*
 * A label in the same switch that simply is not spelled with the prefix. The
 * pass walks past a label with another name because a nested switch may have
 * its own, but one at the same depth as the labels being read is in the same
 * switch, and walking past that hides a case.
 *
 * Here and in the two shapes above, the odd label is the switch's first case,
 * because that is the order that tells whether the pass judged it by depth
 * at the end rather than when it was met: when it is met, the depth of this
 * pass's own labels is not known yet.
 */
static const char *const foreign_label_shapes =
	"int main(void)\n"
	"{\n"
	"	switch (requested_mode) {\n"
	"	case LEGACY_ALIAS:\n"
	"		legacy_work();\n"
	"		break;\n"
	"	case MODE_ORDINARY:\n"
	"		ordinary_work();\n"
	"		break;\n"
	"	}\n"
	"	return 0;\n"
	"}\n";

/*
 * A nested switch written without braces, with a label carrying the
 * dispatch's own prefix. Its labels sit at the dispatch's level, so nothing
 * in the text says which switch they belong to, and taking it for a dispatch
 * case would file whatever the outer case does under it. It is reported
 * instead, and the failure says to add the braces.
 */
static const char *const unbraced_nested_switch_shapes =
	"int main(void)\n"
	"{\n"
	"	switch (requested_mode) {\n"
	"	case MODE_ONE:\n"
	"		switch (sub) case MODE_NESTED_UNBRACED:\n"
	"			one_work();\n"
	"		break;\n"
	"	case MODE_ORDINARY:\n"
	"		ordinary_work();\n"
	"		break;\n"
	"	}\n"
	"	return 0;\n"
	"}\n";

/*
 * The same nested switch with braces, and its label carrying the prefix too.
 * Depth tells it apart, so it is neither read as a dispatch case nor
 * reported: it is simply not this pass's business.
 */
static const char *const braced_nested_prefix_shapes =
	"int main(void)\n"
	"{\n"
	"	switch (requested_mode) {\n"
	"	case MODE_ONE:\n"
	"		switch (sub) {\n"
	"		case MODE_NESTED_BRACED:\n"
	"			one_work();\n"
	"			break;\n"
	"		}\n"
	"		break;\n"
	"	}\n"
	"	return 0;\n"
	"}\n";

/*
 * A nested switch's own labels, which are not in the dispatch and must not be
 * reported: they sit deeper than the dispatch's labels.
 *
 * Depth is the only thing that tells them apart, so a nested switch written
 * without braces -- legal C that nobody writes -- puts its labels at the
 * dispatch's own level and has them reported rather than read. That is the
 * safe way round: the failure names the label and says to add the braces.
 */
static const char *const nested_switch_shapes =
	"int main(void)\n"
	"{\n"
	"	switch (requested_mode) {\n"
	"	case MODE_ONE:\n"
	"		switch (sub) {\n"
	"		case 'a':\n"
	"			one_work();\n"
	"			break;\n"
	"		}\n"
	"		break;\n"
	"	}\n"
	"	return 0;\n"
	"}\n";

/*
 * A label that is an expression rather than a bare name. `case MODE_X + 1:`
 * is valid C and names a different mode from MODE_X, so reading it as MODE_X
 * would file that case under the wrong mode.
 */
static const char *const expression_label_shapes =
	"int main(void)\n"
	"{\n"
	"	switch (requested_mode) {\n"
	"	case MODE_ARITHMETIC + 1:\n"
	"		arithmetic_work();\n"
	"		break;\n"
	"	case MODE_ORDINARY:\n"
	"		ordinary_work();\n"
	"		break;\n"
	"	}\n"
	"	return 0;\n"
	"}\n";

static const char *const long_label_shapes =
	"int main(void)\n"
	"{\n"
	"	switch (requested_mode) {\n"
	"	case MODE_A_NAME_TOO_LONG_FOR_THE_BUFFER_THE_SCAN_READS_IT_INTO:\n"
	"		long_name_work();\n"
	"		break;\n"
	"	case MODE_ORDINARY:\n"
	"		ordinary_work();\n"
	"		break;\n"
	"	}\n"
	"	return 0;\n"
	"}\n";

MU_TEST(a_case_label_the_scan_cannot_read_is_reported)
{
	static struct dispatch_scan shapes;

	mu_assert_fatal(scan_dispatch_text(unreadable_label_shapes, &shapes),
	                "cannot scan the written-out dispatch, so the checks "
	                "below would pass for want of anything to look at");
	mu_assert_int_eq(1, shapes.case_count);
	mu_assert(shapes.unreadable[0] != '\0',
	          "the scan walked past `case (MODE_PARENTHESISED):` without a "
	          "word, so a case written that way is classified by nothing");

	mu_assert_fatal(scan_dispatch_text(long_label_shapes, &shapes),
	                "cannot scan the written-out dispatch, so the check "
	                "below would pass for want of anything to look at");
	mu_assert_int_eq(1, shapes.case_count);
	mu_assert(shapes.unreadable[0] != '\0',
	          "the scan walked past a MODE_ label too long for its buffer "
	          "without a word, so a mode named that long is classified by "
	          "nothing");

	mu_assert_fatal(scan_dispatch_text(expression_label_shapes, &shapes),
	                "cannot scan the written-out dispatch, so the checks "
	                "below would pass for want of anything to look at");
	mu_assert_int_eq(1, shapes.case_count);
	mu_assert(shapes.unreadable[0] != '\0',
	          "the scan read `case MODE_ARITHMETIC + 1:` as a bare name "
	          "without a word, so a case labelled with an expression is "
	          "filed under whichever mode its first name happens to be");

	mu_assert_fatal(scan_dispatch_text(foreign_label_shapes, &shapes),
	                "cannot scan the written-out dispatch, so the checks "
	                "below would pass for want of anything to look at");
	mu_assert_int_eq(1, shapes.case_count);
	mu_assert(shapes.foreign[0] != '\0',
	          "the scan walked past `case LEGACY_ALIAS:` in the dispatch's "
	          "own switch without a word, so a case named that way is "
	          "classified by nothing");

	mu_assert_fatal(scan_dispatch_text(unbraced_nested_switch_shapes,
	                                   &shapes),
	                "cannot scan the written-out dispatch, so the checks "
	                "below would pass for want of anything to look at");
	mu_assert(shapes.unreadable[0] != '\0',
	          "the scan took `case MODE_NESTED_UNBRACED:` for a dispatch "
	          "case. It is a nested switch's label, written where braces "
	          "would have put it deeper, so whatever the case around it "
	          "does would be filed under it");
	mu_assert_int_eq(2, shapes.case_count);

	mu_assert_fatal(scan_dispatch_text(braced_nested_prefix_shapes,
	                                   &shapes),
	                "cannot scan the written-out dispatch, so the checks "
	                "below would pass for want of anything to look at");
	mu_assert_int_eq(1, shapes.case_count);
	mu_assert(shapes.unreadable[0] == '\0',
	          "the scan reported a braced nested switch's label, which "
	          "depth already tells apart from the dispatch's own");
	mu_assert(shapes.foreign[0] == '\0',
	          "the scan reported a braced nested switch's label as one in "
	          "the dispatch's own switch");

	mu_assert_fatal(scan_dispatch_text(nested_switch_shapes, &shapes),
	                "cannot scan the written-out dispatch, so the check "
	                "below would pass for want of anything to look at");
	mu_assert(shapes.foreign[0] == '\0',
	          "the scan reported a nested switch's own label, which is not "
	          "in the dispatch at all");
	mu_assert(shapes.unreadable[0] == '\0',
	          "the scan reported a nested switch's own label as one it "
	          "cannot read, which is not in the dispatch at all");
}

/* ------------------------------------------------------------------ *
 * The BR/EDR image: one way to the host, and every request on it
 * ------------------------------------------------------------------ */

/*
 * Everything above is about the bluetooth_rxtx image, where a host has two
 * ways to collect packets -- reading the bulk IN endpoint, or asking for them
 * with UBERTOOTH_POLL control requests -- and a mode wired to the wrong one
 * loses them. That choice is why the tables above exist and why every row has
 * to say which way its host uses.
 *
 * The btbr image shipped alongside it has no such choice. Before anything can
 * be keyed on it, that has to be said plainly, because it is the reason this
 * section has no transport column:
 *
 *  - Nothing polls. The control requests this image answers are the board's
 *    own -- LEDs, serial number, revision, reset -- and none of them hands
 *    over a captured packet. Taking a packet off the queue to the host is
 *    what a polled read is, and only usb_work() does that here.
 *  - Every captured packet goes one way: btctl_tx_enqueue() puts a message on
 *    the transmit queue, and usb_work() takes it off and writes the bulk IN
 *    endpoint. The one other write of that endpoint is usb_send_sync(), which
 *    the debug and fault printers use because the loop that would drain the
 *    queue is not running by then.
 *
 *    Not everything the image sends: a control request's reply goes back to
 *    the host on the control endpoint, which the USB stack writes from the
 *    buffer the handler fills. That is a second way to the host in general,
 *    and the reason it is not a second transport here is the point above --
 *    no request carries capture data -- which is why the requests are listed
 *    below rather than taken on trust.
 *
 * So the question the tables above ask -- which transport does this mode's
 * host use -- has one answer here for every state the image can be in, and a
 * table saying so for each would be inventing a distinction. What is worth
 * checking is what those tables protect underneath: that a capture added
 * later still goes through that one path rather than growing a send of its
 * own, which would be past the queue's bound on a host that stops reading and
 * past the framing the host puts messages back together with.
 *
 * This image is driven by host messages rather than by modes. btctl_handle_msg()
 * is its dispatch, one case per BTCTL_* request, and a request that starts a
 * capture hands over to a state machine that runs from the timer interrupt
 * rather than from a loop of its own. The rows below are therefore keyed on
 * the request, and a request added later fails the first check, which is
 * where whoever adds it reads the paragraph above.
 */
struct btctl_request {
	const char *request;  /* the BTCTL_* the dispatch has a case for */
	const char *handler;  /* the function that case calls */
	/* filled in by the scan */
	int found;
	int names_handler;
};

static struct btctl_request btctl_requests[] = {
	{ "BTCTL_DEBUG",                 "btctl_handle_debug_req",              0, 0 },
	{ "BTCTL_RESET_REQ",             "btctl_handle_reset_req",              0, 0 },
	{ "BTCTL_IDLE_REQ",              "btctl_handle_idle_req",               0, 0 },
	{ "BTCTL_SET_FREQ_OFF_REQ",      "btctl_handle_set_freq_off_req",       0, 0 },
	{ "BTCTL_SET_MAX_AC_ERRORS_REQ", "btctl_handle_set_max_ac_errors_req",  0, 0 },
	{ "BTCTL_SET_BDADDR_REQ",        "btctl_handle_set_bdaddr_req",         0, 0 },
	{ "BTCTL_INQUIRY_REQ",           "btctl_handle_inquiry_req",            0, 0 },
	{ "BTCTL_PAGING_REQ",            "btctl_handle_paging_req",             0, 0 },
	{ "BTCTL_TX_ACL_REQ",            "btctl_handle_tx_acl_req",             0, 0 },
	{ "BTCTL_INQUIRY_SCAN_REQ",      "btctl_handle_inquiry_scan_req",       0, 0 },
	{ "BTCTL_PAGE_SCAN_REQ",         "btctl_handle_page_scan_req",          0, 0 },
	{ "BTCTL_MONITOR_REQ",           "btctl_handle_monitor_req",            0, 0 },
	{ "BTCTL_SET_EIR_REQ",           "btctl_handle_set_eir_req",            0, 0 },
	{ "BTCTL_SET_AFH_REQ",           "btctl_handle_set_afh_req",            0, 0 },
};

#define BTCTL_REQUEST_COUNT \
	(sizeof(btctl_requests) / sizeof(btctl_requests[0]))

static struct dispatch_scan btctl_found;
static int btctl_scanned;
static char unlisted_btctl_request[64];

static void scan_btctl_requests(void)
{
	const char *path = FW_BTBR_DIR "/src/btctl/btctl.c";
	size_t i;
	int c;

	dispatch_scan_init(&btctl_found, "BTCTL_");
	btctl_scanned = read_function_body(path, "btctl_handle_msg",
	                                   visit_dispatch_line,
	                                   &btctl_found) > 0;
	dispatch_scan_done(&btctl_found);
	if (!btctl_scanned)
		return;

	for (c = 0; c < btctl_found.case_count; c++) {
		struct dispatch_case *found_case = &btctl_found.cases[c];

		for (i = 0; i < BTCTL_REQUEST_COUNT; i++) {
			if (strcmp(btctl_requests[i].request,
			           found_case->label) != 0)
				continue;
			btctl_requests[i].found = 1;
			found_case->listed = 1;
			if (case_calls(found_case, btctl_requests[i].handler))
				btctl_requests[i].names_handler = 1;
		}
		if (!found_case->listed && unlisted_btctl_request[0] == '\0')
			snprintf(unlisted_btctl_request,
			         sizeof(unlisted_btctl_request), "%s",
			         found_case->label);
	}
}

MU_TEST(the_br_edr_request_scan_read_every_case)
{
	mu_assert_fatal(btctl_scanned,
	                "cannot read btctl_handle_msg() in " FW_BTBR_DIR
	                "/src/btctl/btctl.c, so the checks below would pass "
	                "for want of anything to look at");
	mu_assert(!btctl_found.overflowed,
	          "the request dispatch has more cases, or a case more calls, "
	          "than this scan can hold");
	mu_assert(btctl_found.unreadable[0] == '\0', unreadable_case_message(
	          btctl_found.unreadable, "BTCTL_"));
	mu_assert(btctl_found.foreign[0] == '\0', foreign_case_message(
	          btctl_found.foreign, "BTCTL_"));
	mu_assert(btctl_found.case_count >= (int)BTCTL_REQUEST_COUNT,
	          "the scan found fewer request cases than the table lists, so "
	          "it lost track of btctl_handle_msg()");
}

/*
 * A request the dispatch handles and this table does not list is the hole the
 * tables above close for the other image: nothing says how the host collects
 * what it produces.
 */
MU_TEST(every_br_edr_request_is_listed_with_its_handler)
{
	size_t i;
	char msg[512];

	snprintf(msg, sizeof(msg),
	         "btctl_handle_msg() has a case for %s and the table in this "
	         "test does not. Add it, and check that whatever it starts "
	         "sends through btctl_tx_enqueue(): that queue is the only way "
	         "to the host in this image",
	         unlisted_btctl_request);
	mu_assert(unlisted_btctl_request[0] == '\0', msg);

	for (i = 0; i < BTCTL_REQUEST_COUNT; i++) {
		snprintf(msg, sizeof(msg),
		         "the table lists %s and btctl_handle_msg() has no case "
		         "for it, so the row says nothing about anything",
		         btctl_requests[i].request);
		mu_assert(btctl_requests[i].found, msg);

		snprintf(msg, sizeof(msg),
		         "the table says %s is handled by %s(), which its case "
		         "does not call. The row is stale",
		         btctl_requests[i].request, btctl_requests[i].handler);
		mu_assert(btctl_requests[i].names_handler, msg);
	}
}

/*
 * The other half of "nothing polls", checked rather than asserted. The
 * control requests this image answers are the board's own, and a request that
 * handed a captured packet back in its control buffer would need none of the
 * three names the next test guards -- so it would slip past that test
 * entirely. Every request the handler has a case for is therefore listed
 * here, and a new one fails this check: whoever adds it has to say whether it
 * gives the host capture data, which would make this image's answer to "which
 * transport" stop being one.
 */
static const char *const btbr_vendor_requests[] = {
	"UBERTOOTH_PING",
	"UBERTOOTH_GET_USRLED",   "UBERTOOTH_SET_USRLED",
	"UBERTOOTH_GET_RXLED",    "UBERTOOTH_SET_RXLED",
	"UBERTOOTH_GET_TXLED",    "UBERTOOTH_SET_TXLED",
	"UBERTOOTH_GET_1V8",      "UBERTOOTH_SET_1V8",
	"UBERTOOTH_GET_PARTNUM",
	"UBERTOOTH_RESET",
	"UBERTOOTH_GET_SERIAL",
	"UBERTOOTH_GET_PAEN",     "UBERTOOTH_SET_PAEN",
	"UBERTOOTH_GET_HGM",      "UBERTOOTH_SET_HGM",
	"UBERTOOTH_GET_PALEVEL",  "UBERTOOTH_SET_PALEVEL",
	"UBERTOOTH_SET_ISP",
	"UBERTOOTH_FLASH",
	"UBERTOOTH_GET_WATCHDOG",
	"UBERTOOTH_GET_REV_NUM",
	"UBERTOOTH_GET_COMPILE_INFO",
	"UBERTOOTH_GET_FW_INFO",
	"UBERTOOTH_GET_BOARD_ID",
};

#define BTBR_VENDOR_REQUEST_COUNT \
	(sizeof(btbr_vendor_requests) / sizeof(btbr_vendor_requests[0]))

static struct dispatch_scan btbr_vendor_found;
static int btbr_vendor_scanned;

static void scan_btbr_vendor_requests(void)
{
	const char *path = FW_BTBR_DIR "/src/btctl/vendor_request_handler.c";

	dispatch_scan_init(&btbr_vendor_found, "UBERTOOTH_");
	btbr_vendor_scanned = read_function_body(path, "vendor_request_handler",
	                                         visit_dispatch_line,
	                                         &btbr_vendor_found) > 0;
	dispatch_scan_done(&btbr_vendor_found);
}

MU_TEST(every_br_edr_control_request_is_listed)
{
	size_t i;
	int c;
	char msg[512];

	mu_assert_fatal(btbr_vendor_scanned,
	                "cannot read vendor_request_handler() in " FW_BTBR_DIR
	                "/src/btctl/vendor_request_handler.c, so the checks "
	                "below would pass for want of anything to look at");
	mu_assert(!btbr_vendor_found.overflowed,
	          "the request handler has more cases, or a case more calls, "
	          "than this scan can hold");
	mu_assert(btbr_vendor_found.unreadable[0] == '\0',
	          unreadable_case_message(btbr_vendor_found.unreadable,
	                                  "UBERTOOTH_"));
	mu_assert(btbr_vendor_found.foreign[0] == '\0',
	          foreign_case_message(btbr_vendor_found.foreign,
	                               "UBERTOOTH_"));

	for (c = 0; c < btbr_vendor_found.case_count; c++) {
		const char *label = btbr_vendor_found.cases[c].label;

		for (i = 0; i < BTBR_VENDOR_REQUEST_COUNT; i++)
			if (strcmp(btbr_vendor_requests[i], label) == 0)
				break;
		snprintf(msg, sizeof(msg),
		         "the BR/EDR image answers %s and the list in this test "
		         "does not have it. Add it, and say whether it gives the "
		         "host captured data: if one ever does, this image has a "
		         "second way to the host and the comment above is wrong",
		         label);
		mu_assert(i < BTBR_VENDOR_REQUEST_COUNT, msg);
	}

	for (i = 0; i < BTBR_VENDOR_REQUEST_COUNT; i++) {
		for (c = 0; c < btbr_vendor_found.case_count; c++)
			if (strcmp(btbr_vendor_found.cases[c].label,
			           btbr_vendor_requests[i]) == 0)
				break;
		snprintf(msg, sizeof(msg),
		         "the list names %s and the handler has no case for it, "
		         "so the entry says nothing about anything",
		         btbr_vendor_requests[i]);
		mu_assert(c < btbr_vendor_found.case_count, msg);
	}
}

/*
 * The claim the section above rests on, checked rather than assumed: this
 * image has one way to the host.
 *
 * The shim owns both ends of it -- the transmit queue's dequeue and the write
 * to the bulk IN endpoint -- so it is the one file allowed to name either. A
 * state that wrote the endpoint itself would be past the queue; one that
 * dequeued would be handing the host packets from somewhere other than
 * usb_work(), which is what a second transport looks like.
 *
 * One file the image compiles is not under either tree: firmware/common's
 * serial-over-USB console, which writes endpoints of its own. It is not a
 * second way to the host here because nothing in this image starts it, and
 * the last check below is what says so rather than leaving it to the comment.
 *
 * The image's makefile compiles every .c under its src/, so walking that tree
 * covers the image as it is rather than as it was listed. Its include/ tree
 * is walked as well, and headers are read alongside sources, because a
 * `static inline` in a header lands in the image through whichever source
 * includes it. The files it takes from firmware/common are shared with the
 * other image, and the scan above reads those.
 *
 * Every directory the walk should have reached is named below. A count of
 * files cannot stand in for that: losing src/btphy, where the capture states
 * live, still leaves enough files elsewhere to look healthy.
 */
static const char *const br_edr_dirs[] = {
	"/src/btctl", "/src/btphy", "/src/codec", "/src/drivers", "/src/util",
	"/include/ubtbr",
};

#define BR_EDR_DIR_COUNT (sizeof(br_edr_dirs) / sizeof(br_edr_dirs[0]))

static int br_edr_dir_seen[BR_EDR_DIR_COUNT];
static int br_edr_files_read;
/* A path the walk could not hold; see walk_br_edr_sources(). */
static char br_edr_path_too_long[1024];
static char br_edr_stray_write[256];
static char br_edr_stray_dequeue[256];
static char br_edr_stray_sync[256];

/*
 * The console's entry points -- every function firmware/common's usb_serial.c
 * offers a caller, its own setup included. Calling any of them brings the
 * console up or uses it.
 *
 * The USB interrupt vector that the same file defines is not one of them, and
 * this image does run it: it hands straight to the USB stack, which calls
 * whichever endpoint handlers were registered. The console's handler is
 * registered inside usb_serial_init() and nowhere else, so forbidding that
 * call is what keeps the console out of this image.
 */
static const char *const br_edr_console_names[] = {
	"usb_serial_init", "VCOM_init", "VCOM_putchar", "VCOM_getchar",
};

#define BR_EDR_CONSOLE_COUNT \
	(sizeof(br_edr_console_names) / sizeof(br_edr_console_names[0]))

static char br_edr_console_caller[256];
static const char *br_edr_console_called;

static int path_ends_with(const char *path, const char *tail)
{
	size_t p = strlen(path), t = strlen(tail);

	return p >= t && strcmp(path + p - t, tail) == 0;
}

/*
 * Is this line a `#define`? The '#' may be indented and the directive may be
 * separated from it by spaces -- `  #  define X` is the same directive as
 * `#define X` -- so neither is taken for granted.
 */
static int line_defines_a_macro(const char *line)
{
	const char *p = line;

	while (*p == ' ' || *p == '\t')
		p++;
	if (*p != '#')
		return 0;
	p++;
	while (*p == ' ' || *p == '\t')
		p++;
	return strncmp(p, "define", 6) == 0 && !is_ident_char(p[6]);
}

/* Does this line end in a backslash, so the next one carries on from it? */
static int line_is_continued(const char *line)
{
	size_t n = strlen(line);

	while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r' ||
	                 line[n - 1] == ' ' || line[n - 1] == '\t'))
		n--;
	return n > 0 && line[n - 1] == '\\';
}

/*
 * How many times one file names `name` somewhere that makes it reachable:
 * inside a function body, or in a macro.
 *
 * Inside a body is the whole point: a header declares the functions the
 * sources call, and `int btctl_tx_dequeue(...);` names one without calling
 * it. Braces tell the two apart -- a prototype sits outside any body, a call
 * inside one -- which also covers a `static inline` written on one line.
 *
 * Within a body the name counts wherever it appears, with no '(' required,
 * because naming it without calling it is taking its address and a send
 * reached through a pointer is a send. Outside a body the same mention counts
 * for the same reason -- a file-scope pointer is initialised there -- while
 * one with a '(' after it is the declaration and does not.
 *
 * A `#define` counts as a body wherever it is written, braces or none, and in
 * any of its legal spellings. The text of a macro is code in whatever
 * function expands it, so a send wrapped in one is a send; and a macro is
 * written at file scope, where the brace rule would otherwise look straight
 * past it. Inside one, naming the function is enough, because a macro that
 * is just another name for it turns every use of that name into a call.
 */
static int file_calls(const char *path, const char *name)
{
	char line[SCAN_LINE_MAX];
	int in_block = 0, hits = 0, depth = 0, in_macro_continuation = 0;
	size_t len = strlen(name);
	FILE *f = fopen(path, "r");

	if (f == NULL)
		return 0;

	while (fgets(line, sizeof(line), f) != NULL) {
		const char *p = line;
		int in_macro;

		strip_comments(line, &in_block);
		strip_string_literals(line);

		in_macro = in_macro_continuation || line_defines_a_macro(line);
		/* A macro written over several lines carries on while each one
		 * ends in a backslash. */
		in_macro_continuation = in_macro && line_is_continued(line);

		for (p = line; *p != '\0'; p++) {
			if (*p == '{') {
				depth++;
				continue;
			}
			if (*p == '}') {
				if (depth > 0)
					depth--;
				continue;
			}
			if (!is_ident_char(*p))
				continue;
			/* Only at the start of an identifier, so a longer name
			 * ending in this one does not match. */
			if (p != line && is_ident_char(p[-1]))
				continue;
			if (strncmp(p, name, len) == 0 &&
			    !is_ident_char(p[len])) {
				const char *after = p + len;

				while (*after == ' ' || *after == '\t')
					after++;
				/*
				 * In a body or a macro, naming it is enough
				 * either way. Outside both, only a mention
				 * with no '(' after it counts: that is its
				 * address being taken, and a send reached
				 * through a pointer is a send. A mention with
				 * a '(' there is the declaration the sources
				 * are compiled against.
				 */
				if (depth > 0 || in_macro || *after != '(')
					hits++;
			}
		}
	}

	fclose(f);
	return hits;
}

/* Does `path` sit directly in the directory ending `dir`? */
static int path_is_in(const char *path, const char *dir)
{
	const char *slash = strrchr(path, '/');
	size_t len = strlen(dir);

	if (slash == NULL || (size_t)(slash - path) < len)
		return 0;
	return strncmp(slash - len, dir, len) == 0;
}

static void check_br_edr_file(const char *path)
{
	int is_shim = path_ends_with(path, "/src/drivers/ubertooth_usb.c");
	size_t d;

	br_edr_files_read++;
	for (d = 0; d < BR_EDR_DIR_COUNT; d++)
		if (path_is_in(path, br_edr_dirs[d]))
			br_edr_dir_seen[d] = 1;

	if (!is_shim) {
		if (file_calls(path, "USBHwEPWrite") &&
		    br_edr_stray_write[0] == '\0')
			snprintf(br_edr_stray_write,
			         sizeof(br_edr_stray_write), "%s", path);
		if (file_calls(path, "btctl_tx_dequeue") &&
		    br_edr_stray_dequeue[0] == '\0')
			snprintf(br_edr_stray_dequeue,
			         sizeof(br_edr_stray_dequeue), "%s", path);
	}
	for (d = 0; d < BR_EDR_CONSOLE_COUNT; d++)
		if (file_calls(path, br_edr_console_names[d]) &&
		    br_edr_console_caller[0] == '\0') {
			snprintf(br_edr_console_caller,
			         sizeof(br_edr_console_caller), "%s", path);
			br_edr_console_called = br_edr_console_names[d];
		}

	/* The debug and fault printers are the synchronous send's callers, and
	 * the shim holds its definition. */
	if (!is_shim && !path_ends_with(path, "/src/util/debug.c") &&
	    !path_ends_with(path, "/src/util/fault.c") &&
	    file_calls(path, "usb_send_sync") && br_edr_stray_sync[0] == '\0')
		snprintf(br_edr_stray_sync, sizeof(br_edr_stray_sync), "%s",
		         path);
}

static void walk_br_edr_sources(const char *path,
                                void (*visit)(const char *path))
{
	struct dirent *entry;
	DIR *dir = opendir(path);

	if (dir == NULL)
		return;

	while ((entry = readdir(dir)) != NULL) {
		char child[512];
		size_t len = strlen(entry->d_name);
		int n;

		/* Only these two, which would recurse here forever. Any other
		 * name starting with a dot is a file like the rest, and the
		 * image's makefile compiles it like the rest. */
		if (strcmp(entry->d_name, ".") == 0 ||
		    strcmp(entry->d_name, "..") == 0)
			continue;
		n = snprintf(child, sizeof(child), "%s/%s", path,
		             entry->d_name);
		/*
		 * A path too long to hold would be opened truncated, fail, and
		 * be passed over without a word -- so a source nested that
		 * deep would be in the image and in none of the checks below.
		 * It is recorded instead.
		 */
		if (n < 0 || (size_t)n >= sizeof(child)) {
			if (br_edr_path_too_long[0] == '\0')
				snprintf(br_edr_path_too_long,
				         sizeof(br_edr_path_too_long), "%s/%s",
				         path, entry->d_name);
			continue;
		}
		/* Headers as well as sources: a `static inline` in a header
		 * that one of these sources includes puts its code in the
		 * image just as a .c file would. */
		if (len > 2 && (strcmp(entry->d_name + len - 2, ".c") == 0 ||
		                strcmp(entry->d_name + len - 2, ".h") == 0)) {
			visit(child);
			continue;
		}
		/* A subdirectory, or a file that is not source: opening a file
		 * as a directory fails and the call returns. */
		walk_br_edr_sources(child, visit);
	}

	closedir(dir);
}

/*
 * Inside the shim itself. Exempting the whole file is too coarse: a send
 * added there and called from a capture state would be past the transmit
 * queue just as one written in the state would, and the check above would
 * pass it. So rather than counting the writes and pinning a number that goes
 * stale, every write the file makes has to be in one of the two functions
 * that are allowed to make one.
 */
static const char *counted_name;
static int counted_hits;

static void count_name(const char *line, const struct body_pos *pos, void *ctx)
{
	(void)pos;
	(void)ctx;
	if (calls_identifier(line, counted_name))
		counted_hits++;
}

/* How many times `function` in `path` calls `name`, or -1 if the file or the
 * function could not be read. */
static int function_calls(const char *path, const char *function,
                          const char *name)
{
	int lines;

	counted_name = name;
	counted_hits = 0;
	lines = read_function_body(path, function, count_name, NULL);
	if (lines <= 0)
		return -1;
	return counted_hits;
}

MU_TEST(only_two_functions_in_the_br_edr_shim_send)
{
	const char *shim = FW_BTBR_DIR "/src/drivers/ubertooth_usb.c";
	/* `name`, and the functions that may call it. */
	static const struct {
		const char *name;
		const char *in[2];
	} allowed[] = {
		{ "USBHwEPWrite",     { "usb_work", "usb_send_sync" } },
		{ "btctl_tx_dequeue", { "usb_work", NULL } },
	};
	size_t i;
	char msg[512];

	for (i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
		int total = file_calls(shim, allowed[i].name), inside = 0;
		size_t f;

		snprintf(msg, sizeof(msg),
		         "the shim does not call %s() at all, so this check "
		         "passed for want of anything to look at",
		         allowed[i].name);
		mu_assert(total > 0, msg);

		for (f = 0; f < 2 && allowed[i].in[f] != NULL; f++) {
			int n = function_calls(shim, allowed[i].in[f],
			                       allowed[i].name);

			snprintf(msg, sizeof(msg),
			         "cannot read %s() in the shim, so this check "
			         "would pass for want of anything to look at",
			         allowed[i].in[f]);
			mu_assert_fatal(n >= 0, msg);
			inside += n;
		}

		snprintf(msg, sizeof(msg),
		         "the shim calls %s() %d time(s) and only %d of them "
		         "are in %s%s%s. A send anywhere else in the shim is "
		         "another way out of this image: a capture reaching it "
		         "is past the transmit queue, so nothing bounds it on a "
		         "host that stops reading and the host cannot put the "
		         "message back together",
		         allowed[i].name, total, inside, allowed[i].in[0],
		         allowed[i].in[1] != NULL ? "() or " : "()",
		         allowed[i].in[1] != NULL ? allowed[i].in[1] : "");
		mu_assert_int_eq(total, inside);
	}
}

MU_TEST(the_br_edr_image_has_one_way_to_the_host)
{
	char msg[512];
	size_t d;

	walk_br_edr_sources(FW_BTBR_DIR "/src", check_br_edr_file);
	/* The image's headers live in a tree of their own, and a `static
	 * inline` in one of them is compiled into whichever source includes
	 * it, so a send written there is as real as one in a .c file. */
	walk_br_edr_sources(FW_BTBR_DIR "/include", check_br_edr_file);

	mu_assert(br_edr_files_read > 10,
	          "nothing read under " FW_BTBR_DIR "/src or /include, so the "
	          "checks below passed for want of anything to look at");

	snprintf(msg, sizeof(msg),
	         "%s is nested too deep for the walk to name, so it was not "
	         "read and the checks below say nothing about it. The image "
	         "compiles every source under its tree whatever the depth",
	         br_edr_path_too_long);
	mu_assert(br_edr_path_too_long[0] == '\0', msg);

	for (d = 0; d < BR_EDR_DIR_COUNT; d++) {
		snprintf(msg, sizeof(msg),
		         "the walk read nothing in " FW_BTBR_DIR "%s, so the "
		         "checks below say nothing about what is there",
		         br_edr_dirs[d]);
		mu_assert(br_edr_dir_seen[d], msg);
	}

	snprintf(msg, sizeof(msg),
	         "%s writes a USB endpoint, and only the image's USB shim may. "
	         "A send of its own is past the transmit queue, so nothing "
	         "bounds it on a host that stops reading and the host cannot "
	         "put the message back together", br_edr_stray_write);
	mu_assert(br_edr_stray_write[0] == '\0', msg);

	snprintf(msg, sizeof(msg),
	         "%s takes messages off the transmit queue, and only usb_work() "
	         "in the image's USB shim may. Handing the host packets from "
	         "anywhere else is a second transport, and then which one a "
	         "capture uses has to be said somewhere",
	         br_edr_stray_dequeue);
	mu_assert(br_edr_stray_dequeue[0] == '\0', msg);

	snprintf(msg, sizeof(msg),
	         "%s calls usb_send_sync(), which is the debug and fault path: "
	         "it writes the endpoint with the USB interrupt held off and "
	         "drops the message if the host is not reading. A capture goes "
	         "through btctl_tx_enqueue()", br_edr_stray_sync);
	mu_assert(br_edr_stray_sync[0] == '\0', msg);

	snprintf(msg, sizeof(msg),
	         "%s calls %s(), which starts the serial-over-USB console that "
	         "firmware/common holds. It writes endpoints of its own, so "
	         "this image would have a second way to the host and which one "
	         "a capture uses would have to be said somewhere",
	         br_edr_console_caller, br_edr_console_called);
	mu_assert(br_edr_console_caller[0] == '\0', msg);
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

static void measure_file_lines(const char *path)
{
	int c, length = 0, lineno = 1;
	FILE *f = fopen(path, "r");

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
		         "%s:%d", path, lineno);
	}

	fclose(f);
}

static void check_file_line_length(const char *dir, const char *file)
{
	char path[512];

	snprintf(path, sizeof(path), "%s/%s", dir, file);
	measure_file_lines(path);
}

MU_TEST(no_source_line_is_too_long_for_the_scans)
{
	char msg[384];

	run_scan(1, check_file_line_length);
	/* The BR/EDR trees are read through the same buffer, by the checks
	 * further down, so they are held to the same width. */
	walk_br_edr_sources(FW_BTBR_DIR "/src", measure_file_lines);
	walk_br_edr_sources(FW_BTBR_DIR "/include", measure_file_lines);

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

/*
 * What the watchdog gets from a service pass: one feed per pass, and the site
 * marker saying "servicing USB" while the pass runs and back to what the
 * caller was in when it ends. A board that stops making these passes stops
 * answering the host, which is what the watchdog resets it for.
 */
MU_TEST(a_usb_service_pass_feeds_the_watchdog)
{
	ep_reset();
	usb_queue_init();
	watchdog_feeds = 0;
	watchdog_site = WATCHDOG_SITE_FS_LOCK;
	watchdog_site_during_pass = WATCHDOG_SITE_NONE;

	handle_usb(0);

	mu_assert_int_eq(1, (int)watchdog_feeds);
	mu_assert_int_eq(WATCHDOG_SITE_USB, watchdog_site_during_pass);
	/* The wait the pass was called from is the site again. */
	mu_assert_int_eq(WATCHDOG_SITE_FS_LOCK, watchdog_site);

	handle_usb(0);
	mu_assert_int_eq(2, (int)watchdog_feeds);
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
	MU_RUN(a_usb_service_pass_feeds_the_watchdog);

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
	MU_RUN(the_dispatch_scan_reads_each_case_to_its_own_break);
	MU_RUN(a_case_label_the_scan_cannot_read_is_reported);

	scan_btctl_requests();
	MU_RUN(the_br_edr_request_scan_read_every_case);
	MU_RUN(every_br_edr_request_is_listed_with_its_handler);

	scan_btbr_vendor_requests();
	MU_RUN(every_br_edr_control_request_is_listed);
	MU_RUN(the_br_edr_image_has_one_way_to_the_host);
	MU_RUN(only_two_functions_in_the_br_edr_shim_send);

	MU_RUN(no_source_line_is_too_long_for_the_scans);

	return mu_report();
}
