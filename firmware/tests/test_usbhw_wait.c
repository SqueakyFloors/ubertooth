/*
 * Unit tests for the bounded waits in the USB hardware layer,
 * firmware/common/lpcusb/target/usbhw_lpc.c.
 *
 * These run on the host. usbhw_lpc.c reaches the controller only through the
 * register macros in lpc17.h, so stubs-usb/lpc17.h puts ordinary memory where
 * the registers were and this test decides what the controller does.
 *
 * What the waits have to guarantee: every one of them is on the path of every
 * control request, and none of them used to have a bound. USB in this firmware
 * is serviced from whatever loop is running (handle_usb()), plus the USB
 * interrupt in six modes, so a wait that never finishes stops the board
 * answering any USB request at all -- it still enumerates, every control
 * request times out (-7), and only a power cycle brings it back. That state
 * has been reported from the bench repeatedly. The waits must therefore
 * always terminate, say that they gave up, and leave the controller in a
 * state the next call can use.
 *
 * Note: a test for a loop that used to be unbounded cannot fail politely. If
 * a bound is removed, the test binary hangs rather than reporting a failure --
 * as in test_cc2400_wait.c, which has the same shape.
 */
#include "minunit.h"

#include <string.h>

/* The fake register file, then the code under test. type.h and usbhw_lpc.h
 * come from the firmware; stubs-usb is ahead of them on the include path, so
 * its lpc17.h is the one usbhw_lpc.c gets. */
#include "lpc17.h"
#include "type.h"
#include "usbhw_lpc.h"
#include "usbapi.h"

/* ---- the fake controller ---- */

volatile uint32_t fake_devinten, fake_devintpri;
volatile uint32_t fake_epinten, fake_epintpri;
volatile uint32_t fake_reep, fake_epin, fake_maxpsize;
volatile uint32_t fake_txplen, fake_ctrl;
volatile uint32_t fake_cmdcode, fake_cmddata;
volatile uint32_t fake_epdmaen, fake_epdmadis, fake_udcah;
volatile uint32_t fake_clkctrl, fake_clkst, fake_pconp;
volatile uint32_t fake_pinsel1, fake_pinsel3, fake_pinsel4;

/* The last value written to each of the acknowledge registers, for a test
 * that wants to see what the firmware asked for rather than its effect. */
static uint32_t fake_devintclr, fake_devintset, fake_epintclr;

/* Does the protocol engine answer commands? */
static int sie_answers;
/* One command the engine will not answer, as its command byte, or -1 for
 * none. This is how a test reaches a failure in the middle of a sequence --
 * the select or the buffer validation that follows a packet write, the buffer
 * release that follows a read -- rather than only the first handshake. The
 * byte is taken from the command register, so it matches the data phase of a
 * command whose data happens to equal it; none of the commands used here has
 * a data phase. */
static int sie_fail_cmd;
/* Event bits the interrupt handler should see (FRAME, DEV_STAT). */
static uint32_t devintst_events;
/* Endpoint events waiting, by their bit in USBEpIntSt. */
static uint32_t ep_pending;
/* The status the controller reports from the write that clears an endpoint's
 * interrupt, when a test wants it to differ from the one Select Endpoint
 * reported: that is an event arriving between the two commands. -1 for no
 * change. */
static int ep_status_at_clear;
/* When set, the write that clears an endpoint's interrupt never reports its
 * answer: the hardware still takes the event away, but the firmware's wait
 * for the status gives up. */
static int clear_answer_withheld;
static unsigned clear_polls_left;
/*
 * EP_SLOW as the controller latches it: set when an endpoint event arrives or
 * the firmware writes USBDevIntSet, cleared by a write to USBDevIntClr. The
 * part may also re-derive it from USBEpIntSt; this fake does not, so a test
 * shows whether the firmware gets back to a deferred event on its own.
 */
static uint32_t devint_latch;

/*
 * One-deep staging for the registers whose write changes the controller's
 * state. The write lands in `staged_value`, and the next register access
 * applies it -- so a second write applies the first, and the firmware never
 * sees a stale answer.
 */
enum staged_reg {
	STAGED_NONE = 0,
	STAGED_DEVINTCLR,
	STAGED_DEVINTSET,
	STAGED_EPINTCLR,
};
static enum staged_reg staged_which;
static volatile uint32_t staged_value;

static void fake_apply_writes(void)
{
	enum staged_reg which = staged_which;
	uint32_t v = staged_value;

	staged_which = STAGED_NONE;
	staged_value = 0;
	switch (which) {
	case STAGED_DEVINTCLR:
		fake_devintclr = v;
		devint_latch &= ~v;
		break;
	case STAGED_DEVINTSET:
		fake_devintset |= v;
		devint_latch |= v;
		break;
	case STAGED_EPINTCLR:
		fake_epintclr = v;
		/* The hardware's Select Endpoint/Clear Interrupt: the event is
		 * consumed here and nowhere else, and it answers with the
		 * endpoint's state as it is now. */
		ep_pending &= ~v;
		if (ep_status_at_clear >= 0)
			fake_cmddata = (uint32_t)ep_status_at_clear;
		if (clear_answer_withheld)
			clear_polls_left = USBHW_POLLS + 1;
		break;
	case STAGED_NONE:
		break;
	}
}

volatile uint32_t *fake_devintclr_reg(void)
{
	fake_apply_writes();
	staged_which = STAGED_DEVINTCLR;
	return &staged_value;
}

volatile uint32_t *fake_devintset_reg(void)
{
	fake_apply_writes();
	staged_which = STAGED_DEVINTSET;
	return &staged_value;
}

volatile uint32_t *fake_epintclr_reg(void)
{
	fake_apply_writes();
	staged_which = STAGED_EPINTCLR;
	return &staged_value;
}

static volatile uint32_t epintst_value;

volatile uint32_t *fake_epintst_reg(void)
{
	fake_apply_writes();
	epintst_value = ep_pending;
	return &epintst_value;
}

/* An endpoint event arrives: its bit goes up and EP_SLOW with it. */
static void fake_raise_ep_event(uint32_t bit)
{
	fake_apply_writes();
	ep_pending |= bit;
	devint_latch |= EP_SLOW;
}

/* Is this endpoint's event still waiting to be handled? */
static int fake_ep_event_pending(uint32_t bit)
{
	fake_apply_writes();
	return (ep_pending & bit) != 0;
}

/* Would the controller bring the firmware back into the endpoint loop? */
static int fake_ep_slow_set(void)
{
	fake_apply_writes();
	return (devint_latch & EP_SLOW) != 0;
}

/* What the firmware has written to USBDevIntSet since this was last cleared.
 * A write the firmware made last is still staged, so it has to be applied
 * before the answer means anything. */
static uint32_t fake_devintset_written(void)
{
	fake_apply_writes();
	return fake_devintset;
}

static void fake_forget_devintset(void)
{
	fake_apply_writes();
	fake_devintset = 0;
}
/* Is a received packet reported as readable, is it valid, how long is it? */
static int packet_ready;
static int packet_valid;
static uint32_t packet_len;
/* Words written to the transmit register, and the number after which the
 * controller clears WR_EN. Zero means it never does -- the stuck case. */
static unsigned tx_words;
static unsigned tx_wr_en_words;

/* The bytes the controller hands over when an endpoint is read, and how many
 * words of them have been taken. NULL means "whatever fake_rx_word holds",
 * which is enough for the tests that only check one word. */
static const U8 *fake_setup_bytes;
static uint32_t fake_rx_word;
static unsigned rx_words;

static volatile uint32_t devintst_value;
static volatile uint32_t rxplen_value;
static volatile uint32_t txdata_value;
static volatile uint32_t rxdata_value;

volatile uint32_t *fake_devintst(void)
{
	/* A working engine has the completion bits up by the time they are read;
	 * one that has stopped answering never raises them. The event bits the
	 * interrupt handler dispatches on are separate, so a test can ask for an
	 * endpoint or device-status event without pretending the engine answers. */
	int answers = sie_answers;

	uint32_t ready;

	fake_apply_writes();
	if (sie_fail_cmd >= 0
	    && (int)((fake_cmdcode >> 16) & 0xffu) == sie_fail_cmd)
		answers = 0;
	ready = answers ? (CCEMTY | CDFULL | EP_RLZED) : 0u;
	/* The interrupt clear's own answer, withheld for one whole wait. */
	if (clear_polls_left > 0) {
		--clear_polls_left;
		ready &= ~(uint32_t)CDFULL;
	}
	devintst_value = devintst_events | devint_latch | ready;
	return &devintst_value;
}

volatile uint32_t *fake_rxplen(void)
{
	rxplen_value = packet_ready
		? (PKT_RDY | (packet_valid ? DV : 0u) | packet_len)
		: 0u;
	return &rxplen_value;
}

volatile uint32_t *fake_rxdata_reg(void)
{
	unsigned w = rx_words++;

	if (fake_setup_bytes == NULL) {
		rxdata_value = fake_rx_word;
	} else {
		rxdata_value = (uint32_t)fake_setup_bytes[w * 4 + 0]
		             | ((uint32_t)fake_setup_bytes[w * 4 + 1] << 8)
		             | ((uint32_t)fake_setup_bytes[w * 4 + 2] << 16)
		             | ((uint32_t)fake_setup_bytes[w * 4 + 3] << 24);
	}
	return &rxdata_value;
}

volatile uint32_t *fake_txdata(void)
{
	++tx_words;
	if (tx_wr_en_words != 0 && tx_words >= tx_wr_en_words)
		fake_ctrl &= ~(uint32_t)WR_EN;
	return &txdata_value;
}

static void fake_reset(void)
{
	sie_answers = 1;
	sie_fail_cmd = -1;
	devintst_events = 0;
	ep_pending = 0;
	ep_status_at_clear = -1;
	clear_answer_withheld = 0;
	clear_polls_left = 0;
	devint_latch = 0;
	staged_which = STAGED_NONE;
	staged_value = 0;
	fake_devintclr = 0;
	fake_epintclr = 0;
	fake_devintset = 0;
	packet_ready = 1;
	packet_valid = 1;
	packet_len = 8;
	tx_words = 0;
	tx_wr_en_words = 0;
	rx_words = 0;
	fake_setup_bytes = NULL;
	fake_rx_word = 0;
	fake_ctrl = 0;
	fake_cmddata = 0;
	fake_cmdcode = 0;
	usbhw_timeouts = 0;
}

/*
 * Did the last thing the firmware told the controller to do stall endpoint 0?
 *
 * There is no way to ask the fake directly: a stall is a protocol engine
 * command, and the last write of that sequence is the command's data phase.
 * USBHwEPStall(0x80, TRUE) ends in `USBCmdCode = 0x100 | (EP_ST << 16)`, so
 * that value in the command register is the stall, and unstalling or any other
 * command leaves something else there.
 */
static int last_command_stalled_ep0(void)
{
	return fake_cmdcode == (0x00000100u | ((uint32_t)EP_ST << 16));
}

/* ---- the protocol engine handshake ---- */

/*
 * USBHwEPGetStatus() is the shortest path to the handshake: one command, one
 * data read. handle_usb() calls it on every pass.
 */
MU_TEST(test_ep_status_returns_what_the_engine_reported)
{
	fake_reset();
	fake_cmddata = EPSTAT_B1FULL;

	mu_assert_int_eq(EPSTAT_B1FULL, USBHwEPGetStatus(0x82));
	mu_assert_int_eq(0, (int)usbhw_timeouts);
}

/*
 * The regression. An engine that stops reporting its commands complete used to
 * hang the caller in `while ((USBDevIntSt & dwIntr) != dwIntr);`, and with it
 * the loop that services USB. Now the wait gives up, counts it, and returns a
 * status of zero -- an endpoint that reads as neither full nor stalled, which
 * makes handle_usb() offer it another packet rather than do anything odd.
 */
MU_TEST(test_ep_status_gives_up_when_the_engine_stops_answering)
{
	fake_reset();
	fake_cmddata = EPSTAT_B1FULL;
	sie_answers = 0;

	mu_assert_int_eq(0, USBHwEPGetStatus(0x82));
	mu_assert(usbhw_timeouts > 0, "a wait that gave up must be counted");
}

/* Every command in a sequence is bounded, not just the first: stalling an
 * endpoint is a command plus a data write, and unstalling it the same. */
MU_TEST(test_stall_returns_when_the_engine_stops_answering)
{
	fake_reset();
	sie_answers = 0;

	USBHwEPStall(0x82, TRUE);
	USBHwEPStall(0x82, FALSE);

	mu_assert(usbhw_timeouts > 0, "a wait that gave up must be counted");
}

/* Nothing is counted on the normal path, so the counter means what it says. */
MU_TEST(test_working_engine_counts_no_timeouts)
{
	int i;

	fake_reset();
	for (i = 0; i < 50; ++i)
		(void)USBHwEPGetStatus(0x82);

	mu_assert_int_eq(0, (int)usbhw_timeouts);
}

/*
 * The point of a bound is that the firmware is still there afterwards. A
 * controller that stopped answering and then answers again must be usable
 * with no intervention: nothing is left half-selected, and no state has to be
 * reset by hand.
 */
MU_TEST(test_the_controller_is_usable_again_after_a_timeout)
{
	fake_reset();
	fake_cmddata = EPSTAT_B1FULL;

	sie_answers = 0;
	mu_assert_int_eq(0, USBHwEPGetStatus(0x82));
	mu_assert(usbhw_timeouts > 0, "a wait that gave up must be counted");

	sie_answers = 1;
	mu_assert_int_eq(EPSTAT_B1FULL, USBHwEPGetStatus(0x82));
}

/* ---- reading an endpoint ---- */

MU_TEST(test_ep_read_returns_the_packet)
{
	U8 buf[16];

	fake_reset();
	packet_len = 4;
	fake_rx_word = 0x44332211;
	memset(buf, 0, sizeof(buf));

	mu_assert_int_eq(4, USBHwEPRead(0x00, buf, sizeof(buf)));
	mu_assert_int_eq(0x11, buf[0]);
	mu_assert_int_eq(0x44, buf[3]);
	mu_assert_int_eq(0, (int)usbhw_timeouts);
}

/*
 * The regression on the control path. USBHandleControlTransfer() reads every
 * SETUP packet through here, and the length it polls only answers while this
 * endpoint is the one selected in the global USBCtrl register -- so a
 * controller that stopped answering left this polling forever. Now it gives up
 * and reports no packet. What the caller must then do is the subject of the
 * control transfer tests further down: before this change it ignored the
 * result, which is how a failed read ran the previous request.
 */
MU_TEST(test_ep_read_gives_up_when_no_packet_arrives)
{
	U8 buf[16];

	fake_reset();
	packet_ready = 0;

	mu_assert_int_eq(-1, USBHwEPRead(0x00, buf, sizeof(buf)));
	mu_assert(usbhw_timeouts > 0, "a wait that gave up must be counted");
}

/* A wait that gave up must not leave the endpoint selected for reading:
 * RD_EN left set in the global control register is what makes the *next*
 * transfer go wrong. */
MU_TEST(test_ep_read_clears_the_control_register_when_it_gives_up)
{
	U8 buf[16];

	fake_reset();
	packet_ready = 0;

	(void)USBHwEPRead(0x00, buf, sizeof(buf));
	mu_assert_int_eq(0, (int)fake_ctrl);
}

/*
 * The handshakes that come *after* the bytes have been copied: selecting the
 * endpoint and releasing its buffer, which is what tells the controller it
 * may receive into that buffer again.
 *
 * Either can give up on a controller that has stopped answering, and the
 * bytes are in the caller's buffer by then -- so this used to be reported as
 * a complete read while the hardware still held the packet and could present
 * it again. For a control transfer that is the stale-request hazard
 * USBHandleControlTransfer() exists to avoid; on a bulk OUT endpoint it is a
 * message the firmware acts on twice. Report no packet instead.
 */
MU_TEST(test_ep_read_reports_no_packet_when_the_select_gives_up)
{
	U8 buf[16];

	fake_reset();
	packet_len = 4;
	fake_rx_word = 0x44332211;
	sie_fail_cmd = CMD_EP_SELECT | 0;   /* endpoint 0 OUT is index 0 */

	mu_assert_int_eq(-1, USBHwEPRead(0x00, buf, sizeof(buf)));
	mu_assert(usbhw_timeouts > 0, "a wait that gave up must be counted");
}

MU_TEST(test_ep_read_reports_no_packet_when_the_buffer_release_gives_up)
{
	U8 buf[16];

	fake_reset();
	packet_len = 4;
	fake_rx_word = 0x44332211;
	sie_fail_cmd = CMD_EP_CLEAR_BUFFER;

	mu_assert_int_eq(-1, USBHwEPRead(0x00, buf, sizeof(buf)));
	mu_assert(usbhw_timeouts > 0, "a wait that gave up must be counted");
}

/* The same for a packet the controller reports as invalid: that path used to
 * return without clearing the register either. */
MU_TEST(test_ep_read_clears_the_control_register_on_an_invalid_packet)
{
	U8 buf[16];

	fake_reset();
	packet_valid = 0;

	mu_assert_int_eq(-1, USBHwEPRead(0x00, buf, sizeof(buf)));
	mu_assert_int_eq(0, (int)fake_ctrl);
	mu_assert_int_eq(0, (int)usbhw_timeouts);
}

/* ---- writing an endpoint ---- */

/* The normal path: one 64-byte packet is sixteen words, and the controller
 * clears WR_EN when it has had them all. */
MU_TEST(test_ep_write_writes_the_whole_packet)
{
	U8 buf[64];

	fake_reset();
	memset(buf, 0xA5, sizeof(buf));
	fake_ctrl = 0;
	tx_wr_en_words = 16;

	mu_assert_int_eq(64, USBHwEPWrite(0x82, buf, sizeof(buf)));
	mu_assert_int_eq(16, (int)tx_words);
}

/*
 * The regression on the bulk path. The loop used to be `while (USBCtrl &
 * WR_EN)` alone, trusting the controller to stop it. USBCtrl is one register
 * shared by every endpoint, so a USB interrupt taken in the middle of this can
 * leave WR_EN set with nothing draining the writes -- and the loop then walks
 * its source pointer through the whole of RAM and off the end of it. Counting
 * the bytes we were asked to write bounds it: with WR_EN stuck set it still
 * writes exactly the packet, and the canary after the buffer is untouched.
 */
MU_TEST(test_ep_write_stops_at_the_packet_length_with_wr_en_stuck)
{
	struct { U8 pkt[64]; U8 canary[64]; } s;

	fake_reset();
	memset(&s, 0, sizeof(s));
	memset(s.canary, 0x5A, sizeof(s.canary));
	fake_ctrl = WR_EN;
	tx_wr_en_words = 0; /* the controller never clears it */

	mu_assert_int_eq(64, USBHwEPWrite(0x82, s.pkt, sizeof(s.pkt)));
	mu_assert_int_eq(16, (int)tx_words);
	mu_assert_int_eq(0x5A, s.canary[0]);
	mu_assert_int_eq(0x5A, s.canary[63]);
}

/*
 * WR_EN clearing early ends the loop where it always did, and the packet that
 * came of it is not offered to the host: a short control packet is how the
 * host is told the data has ended, so sending one would turn a cut-off reply
 * into an apparently complete one. The caller is told how much was taken, and
 * the buffer is left unvalidated so the host retries.
 */
MU_TEST(test_ep_write_reports_a_short_write_and_does_not_validate_it)
{
	U8 buf[64];

	fake_reset();
	memset(buf, 0xA5, sizeof(buf));
	fake_ctrl = WR_EN;
	tx_wr_en_words = 4;

	mu_assert_int_eq(16, USBHwEPWrite(0x82, buf, sizeof(buf)));
	mu_assert_int_eq(4, (int)tx_words);
	mu_assert(fake_cmdcode == 0,
	          "a packet the controller would not take whole must not be validated");
	mu_assert(usbhw_timeouts > 0, "a short write must be counted");
}

/* A keep-alive is one byte. It must put out one word and no more -- the
 * length, not the hardware, is what ends the loop. */
MU_TEST(test_ep_write_of_one_byte_writes_one_word)
{
	U8 buf[4] = { 0xFF, 0, 0, 0 };

	fake_reset();
	fake_ctrl = WR_EN;
	tx_wr_en_words = 0;

	mu_assert_int_eq(1, USBHwEPWrite(0x82, buf, 1));
	mu_assert_int_eq(1, (int)tx_words);
}

/* A packet of no bytes that went through answers zero, and a write that could
 * not commit answers less than that: a control transfer's status stage is a
 * packet of no bytes, so a caller comparing the answer with the length it
 * asked for has to be able to tell the two apart. */
MU_TEST(test_a_zero_length_packet_that_went_through_is_not_a_failure)
{
	U8 buf[4] = { 0, 0, 0, 0 };

	fake_reset();
	mu_assert_int_eq(0, USBHwEPWrite(0x82, buf, 0));

	fake_reset();
	sie_fail_cmd = CMD_EP_VALIDATE_BUFFER;
	mu_assert_int_eq(-1, USBHwEPWrite(0x82, buf, 0));
}

/* A zero-length packet writes no data at all. */
MU_TEST(test_ep_write_of_zero_bytes_writes_nothing)
{
	U8 buf[4] = { 0, 0, 0, 0 };

	fake_reset();
	fake_ctrl = WR_EN;
	tx_wr_en_words = 0;

	mu_assert_int_eq(0, USBHwEPWrite(0x82, buf, 0));
	mu_assert_int_eq(0, (int)tx_words);
}

/*
 * A write whose handshake stops answering still returns, and says nothing was
 * taken -- as less than nothing, so a packet of no bytes that did go
 * through can still answer zero.
 *
 * Selecting the endpoint and validating its buffer is the step that offers the
 * packet to the host, and both are protocol engine commands that can give up.
 * Reporting a complete write then is the one answer the caller must not get:
 * for a control transfer DataIn() would advance past a packet the host can
 * never collect, and the reply would be lost with nothing left to send it.
 */
MU_TEST(test_ep_write_reports_nothing_taken_when_the_engine_stops_answering)
{
	U8 buf[64];

	fake_reset();
	memset(buf, 0xA5, sizeof(buf));
	fake_ctrl = WR_EN;
	tx_wr_en_words = 16;
	sie_answers = 0;

	mu_assert_int_eq(-1, USBHwEPWrite(0x82, buf, sizeof(buf)));
	mu_assert(usbhw_timeouts > 0, "a wait that gave up must be counted");
}

/*
 * And each of those two handshakes on its own, with the engine answering
 * everything else: the packet reached the controller, and then the step that
 * would have offered it to the host gave up. The caller has to be told
 * nothing was taken, whichever of the two it was.
 */
MU_TEST(test_ep_write_reports_nothing_taken_when_the_select_gives_up)
{
	U8 buf[64];

	fake_reset();
	memset(buf, 0xA5, sizeof(buf));
	fake_ctrl = WR_EN;
	tx_wr_en_words = 16;
	sie_fail_cmd = CMD_EP_SELECT | 5;   /* endpoint 0x82 is index 5 */

	mu_assert_int_eq(-1, USBHwEPWrite(0x82, buf, sizeof(buf)));
	mu_assert_int_eq(16, (int)tx_words);
	mu_assert(usbhw_timeouts > 0, "a wait that gave up must be counted");
}

MU_TEST(test_ep_write_reports_nothing_taken_when_the_validation_gives_up)
{
	U8 buf[64];

	fake_reset();
	memset(buf, 0xA5, sizeof(buf));
	fake_ctrl = WR_EN;
	tx_wr_en_words = 16;
	sie_fail_cmd = CMD_EP_VALIDATE_BUFFER;

	mu_assert_int_eq(-1, USBHwEPWrite(0x82, buf, sizeof(buf)));
	mu_assert_int_eq(16, (int)tx_words);
	mu_assert(usbhw_timeouts > 0, "a wait that gave up must be counted");
}

/* ---- the control transfer handler on top of a failed read ---- */

/*
 * What the bound on the read must not turn into: a setup packet that could not
 * be read, decoded as a request anyway.
 *
 * USBHandleControlTransfer() keeps the setup packet in a static, and used to
 * ignore what the read returned. A read that came back short -- which has
 * been recorded happening -- or one that reported no packet left the previous
 * request sitting there to be handled a second time. Whatever it was: a mode
 * change, a reset, a register write.
 */

static unsigned handler_calls;
static U8 handler_last_request;
static U8 handler_data[64];
/* How many bytes of reply the handler offers. Zero is a request with no data
 * stage; 64 is the shape of UBERTOOTH_POLL, which is how ubertooth-btle reads
 * every captured packet. */
static int handler_reply_len;

static BOOL fake_request_handler(TSetupPacket *pSetup, int *piLen,
                                 U8 **ppbData)
{
	(void)ppbData;
	++handler_calls;
	handler_last_request = pSetup->bRequest;
	*piLen = handler_reply_len;
	return TRUE;
}

/* usbcontrol.c calls this filter before the vendor handler; the real one
 * lives in usbstdreq.c, which this test does not need. */
BOOL USBFilterOsVendorMessage(TSetupPacket *pSetup, BOOL *pfSuccess,
                              int *piLen, U8 **ppbData)
{
	(void)pSetup; (void)pfSuccess; (void)piLen; (void)ppbData;
	return FALSE;
}

/* A vendor IN request asking for wLength bytes, as the host sends it. The
 * buffer is bigger than a setup packet so a read the firmware takes further
 * than eight bytes stays inside it. */
static void queue_setup_packet_len(U8 request, U16 wLength)
{
	static U8 setup[64];

	memset(setup, 0, sizeof(setup));
	setup[0] = 0xC0;     /* IN, vendor, device */
	setup[1] = request;
	/* wValue and wIndex zero */
	setup[6] = wLength & 0xFF;
	setup[7] = wLength >> 8;

	fake_setup_bytes = setup;
	packet_ready = 1;
	packet_valid = 1;
	packet_len = 8;
	rx_words = 0;
}

/* A vendor IN request with no data stage. */
static void queue_setup_packet(U8 request)
{
	queue_setup_packet_len(request, 0);
}

static void control_test_setup(void)
{
	fake_reset();
	handler_calls = 0;
	handler_last_request = 0;
	handler_reply_len = 0;
	USBRegisterRequestHandler(REQTYPE_TYPE_VENDOR, fake_request_handler,
	                          handler_data);
}

/* The normal path, so the test below means something: a setup packet that
 * reads cleanly reaches the handler. */
MU_TEST(test_a_readable_setup_packet_reaches_the_handler)
{
	control_test_setup();
	queue_setup_packet(0x21);

	USBHandleControlTransfer(0x00, EP_STATUS_SETUP);

	mu_assert_int_eq(1, (int)handler_calls);
	mu_assert_int_eq(0x21, handler_last_request);
}

/* The regression: a setup packet that cannot be read must not run the
 * previous request again, and must stall the pipe. */
MU_TEST(test_an_unreadable_setup_packet_runs_nothing)
{
	control_test_setup();
	queue_setup_packet(0x21);
	USBHandleControlTransfer(0x00, EP_STATUS_SETUP);
	mu_assert_int_eq(1, (int)handler_calls);

	/* now the controller reports no packet at all */
	packet_ready = 0;
	fake_cmdcode = 0;
	USBHandleControlTransfer(0x00, EP_STATUS_SETUP);

	mu_assert_int_eq(1, (int)handler_calls);
	mu_assert(last_command_stalled_ep0(), "a setup packet that could not be read must stall endpoint 0");
}

/* And the same for a setup packet the controller would not release: the eight
 * bytes arrived, but the hardware still holds the buffer and may present them
 * again, so running the request now could run it twice. */
MU_TEST(test_a_setup_packet_whose_buffer_release_gives_up_runs_nothing)
{
	control_test_setup();
	queue_setup_packet(0x21);
	USBHandleControlTransfer(0x00, EP_STATUS_SETUP);
	mu_assert_int_eq(1, (int)handler_calls);

	queue_setup_packet(0x23);
	sie_fail_cmd = CMD_EP_CLEAR_BUFFER;
	fake_cmdcode = 0;
	USBHandleControlTransfer(0x00, EP_STATUS_SETUP);

	mu_assert_int_eq(1, (int)handler_calls);
	mu_assert(last_command_stalled_ep0(),
	          "a setup packet the controller would not release must stall "
	          "endpoint 0");
}

/* The same for a short read: eight bytes or it is not a request. */
MU_TEST(test_a_short_setup_packet_runs_nothing)
{
	control_test_setup();
	queue_setup_packet(0x21);
	USBHandleControlTransfer(0x00, EP_STATUS_SETUP);
	mu_assert_int_eq(1, (int)handler_calls);

	packet_len = 4;
	fake_cmdcode = 0;
	USBHandleControlTransfer(0x00, EP_STATUS_SETUP);

	mu_assert_int_eq(1, (int)handler_calls);
	mu_assert(last_command_stalled_ep0(), "a setup packet that could not be read must stall endpoint 0");
}

/* And the state it leaves behind must be inert: a later data-stage callback
 * on the same endpoint must not resume the abandoned transfer. */
MU_TEST(test_a_failed_setup_leaves_no_transfer_to_resume)
{
	control_test_setup();
	queue_setup_packet(0x21);
	USBHandleControlTransfer(0x00, EP_STATUS_SETUP);

	packet_ready = 0;
	USBHandleControlTransfer(0x00, EP_STATUS_SETUP);
	handler_calls = 0;

	/* an OUT data stage arriving after that */
	packet_ready = 1;
	packet_len = 8;
	rx_words = 0;
	USBHandleControlTransfer(0x00, 0);

	mu_assert_int_eq(0, (int)handler_calls);
}

/* The same for the control path: a transfer that had to be refused must not
 * cost the next one. The stall ends that transfer, the host sends another
 * setup packet, and this one is handled. */
MU_TEST(test_a_control_transfer_works_after_a_failed_setup_read)
{
	control_test_setup();
	queue_setup_packet(0x21);
	USBHandleControlTransfer(0x00, EP_STATUS_SETUP);

	packet_ready = 0;
	USBHandleControlTransfer(0x00, EP_STATUS_SETUP);
	handler_calls = 0;

	queue_setup_packet(0x22);
	USBHandleControlTransfer(0x00, EP_STATUS_SETUP);

	mu_assert_int_eq(1, (int)handler_calls);
	mu_assert_int_eq(0x22, handler_last_request);
}

/* ---- how a reply's data stage ends ---- */

/*
 * What the endpoint 0 IN interrupt must and must not send.
 *
 * It used to send a packet every time, so every reply was followed by a
 * zero-length packet that was validated and left on the endpoint. The host of
 * that transfer never asks for it -- a reply shorter than the endpoint's
 * maximum has already ended its own data stage -- so it waited there until
 * some later request's IN token collected it, and that request came back with
 * no data, no stall and no error. It was measured on a Mac at two reads in
 * 2,350 during a mode.
 *
 * A zero-length packet is still needed where it means something: a reply
 * shorter than the host asked for whose bytes filled whole packets has
 * nothing in it to say the data ended, and a request with no data stage is
 * answered with one.
 */

#define VALIDATE_BUFFER_CMD (0x00000500u | ((uint32_t)CMD_EP_VALIDATE_BUFFER << 16))

static int last_command_validated_a_buffer(void)
{
	return fake_cmdcode == VALIDATE_BUFFER_CMD;
}

/* Did the firmware offer the host a packet of this many bytes? USBTxPLen
 * carries the length of the packet the controller was given. */
static int validated_a_packet_of(unsigned bytes)
{
	return last_command_validated_a_buffer() && fake_txplen == bytes;
}

/* A two-byte reply to a two-byte request: the shape of
 * UBERTOOTH_GET_CHANNEL, the request the zero-byte reply was measured with. */
MU_TEST(test_a_complete_reply_leaves_nothing_on_the_endpoint)
{
	control_test_setup();
	handler_reply_len = 2;
	queue_setup_packet_len(0x40, 2);
	tx_wr_en_words = 16;

	USBHandleControlTransfer(0x00, EP_STATUS_SETUP);
	mu_assert(validated_a_packet_of(2), "the two-byte reply must go to the host");

	/* the host collects it, which is the endpoint 0 IN event */
	tx_words = 0;
	fake_cmdcode = 0;
	USBHandleControlTransfer(0x80, 0);

	mu_assert_int_eq(0, (int)tx_words);
	mu_assert(!last_command_validated_a_buffer(),
	          "a reply the host already has must not leave a packet on the "
	          "endpoint for a later request's IN token to collect");
}

/* A reply as long as the host asked for ends when the host has counted its
 * bytes, so it needs no terminating packet either. This is the shape of
 * UBERTOOTH_POLL, which `ubertooth-btle` sends for every captured packet. */
MU_TEST(test_a_reply_as_long_as_asked_for_leaves_nothing_on_the_endpoint)
{
	control_test_setup();
	handler_reply_len = 64;
	queue_setup_packet_len(0x41, 64);
	tx_wr_en_words = 16;

	USBHandleControlTransfer(0x00, EP_STATUS_SETUP);
	mu_assert(validated_a_packet_of(64), "the reply must go to the host");

	tx_words = 0;
	fake_cmdcode = 0;
	USBHandleControlTransfer(0x80, 0);

	mu_assert_int_eq(0, (int)tx_words);
	mu_assert(!last_command_validated_a_buffer(),
	          "a reply as long as the host asked for needs no terminating "
	          "packet");
}

/* But a reply shorter than the host asked for, whose bytes filled whole
 * packets, does need one: nothing in it says the data has ended. */
MU_TEST(test_a_reply_that_fills_whole_packets_is_ended_with_a_zero_length_one)
{
	control_test_setup();
	handler_reply_len = 64;
	queue_setup_packet_len(0x42, 128);
	tx_wr_en_words = 16;

	USBHandleControlTransfer(0x00, EP_STATUS_SETUP);
	mu_assert(validated_a_packet_of(64), "the reply must go to the host");

	tx_words = 0;
	fake_cmdcode = 0;
	USBHandleControlTransfer(0x80, 0);
	mu_assert(validated_a_packet_of(0),
	          "a reply that filled whole packets must be ended with a "
	          "zero-length one, or the host waits for more");

	/* once, though */
	fake_cmdcode = 0;
	USBHandleControlTransfer(0x80, 0);
	mu_assert(!last_command_validated_a_buffer(),
	          "the terminating packet must be sent once");
}

/* A handler that offers nothing while the host asked for something: the
 * reply is empty, which is shorter than the host asked for and a multiple of
 * the endpoint's maximum, so the zero-length packet that ends the data stage
 * is the only packet of the transfer -- and it comes from the setup branch,
 * not the interrupt. */
MU_TEST(test_a_reply_of_nothing_to_a_host_that_asked_for_some)
{
	control_test_setup();
	handler_reply_len = 0;
	queue_setup_packet_len(0x44, 8);
	tx_wr_en_words = 16;

	USBHandleControlTransfer(0x00, EP_STATUS_SETUP);
	mu_assert(validated_a_packet_of(0),
	          "an empty reply must still end its data stage");

	fake_cmdcode = 0;
	USBHandleControlTransfer(0x80, 0);
	mu_assert(!last_command_validated_a_buffer(),
	          "and the transfer owes nothing after that");
}

/* A reply longer than one packet: each packet goes as the host collects the
 * one before it, and the last one is short, so nothing follows it. */
MU_TEST(test_a_reply_longer_than_one_packet_ends_with_its_short_packet)
{
	control_test_setup();
	handler_reply_len = 100;
	queue_setup_packet_len(0x45, 100);
	tx_wr_en_words = 64;

	USBHandleControlTransfer(0x00, EP_STATUS_SETUP);
	mu_assert(validated_a_packet_of(64), "the first packet is a full one");

	fake_cmdcode = 0;
	USBHandleControlTransfer(0x80, 0);
	mu_assert(validated_a_packet_of(36), "the rest follows");

	fake_cmdcode = 0;
	USBHandleControlTransfer(0x80, 0);
	mu_assert(!last_command_validated_a_buffer(),
	          "a short last packet ends the data stage on its own");
}

/* A request with no data stage is answered with one zero-length packet: that
 * packet is its status stage, and the host does collect it. */
MU_TEST(test_a_request_with_no_data_stage_is_answered_once)
{
	control_test_setup();
	handler_reply_len = 0;
	queue_setup_packet_len(0x43, 0);
	tx_wr_en_words = 16;

	USBHandleControlTransfer(0x00, EP_STATUS_SETUP);
	mu_assert(validated_a_packet_of(0),
	          "a request with no data stage is answered with a zero-length "
	          "packet");

	fake_cmdcode = 0;
	USBHandleControlTransfer(0x80, 0);
	mu_assert(!last_command_validated_a_buffer(),
	          "and with only one");
}

/* ---- a control reply the controller would not take ---- */

/*
 * A device-to-host control request is answered from one place: the endpoint 0
 * interrupt that says the last packet has reached the host. So a packet the
 * controller refuses used to end the transfer in silence -- nothing validated
 * for the host to collect, no interrupt to carry the transfer on, and the
 * request never answered. The host waits out its own timeout, one second for
 * the Ubertooth tools; `ubertooth-btle` reads every captured packet with a
 * control request, so one refused write ended a capture that had run for
 * minutes.
 *
 * The packet is now owed, and every USB service pass offers it again.
 *
 * `tx_words` counts words handed to the controller, so sixteen is one 64-byte
 * packet; the protocol engine command register holding a buffer validation is
 * what says the host can have it.
 */

/* Ask for a 64-byte reply with the controller refusing after four words. */
static void refused_control_in(U8 request)
{
	control_test_setup();
	handler_reply_len = 64;
	queue_setup_packet_len(request, 64);
	fake_ctrl = WR_EN;
	tx_wr_en_words = 4;
	USBHandleControlTransfer(0x00, EP_STATUS_SETUP);
}

/* The normal path, so the tests below mean something: a reply the controller
 * takes whole goes to the host, and no service pass sends it twice. */
MU_TEST(test_a_control_reply_the_controller_takes_is_sent_once)
{
	control_test_setup();
	handler_reply_len = 64;
	queue_setup_packet_len(0x30, 64);
	fake_ctrl = 0;
	tx_wr_en_words = 16;

	USBHandleControlTransfer(0x00, EP_STATUS_SETUP);
	mu_assert_int_eq(16, (int)tx_words);
	mu_assert(last_command_validated_a_buffer(),
	          "a whole packet must be offered to the host");

	/* a service pass with nothing owed */
	fake_cmdcode = 0;
	USBHwISR();
	mu_assert_int_eq(16, (int)tx_words);
	mu_assert(fake_cmdcode == 0, "a reply already sent must not be sent again");
}

/* The regression: a refused packet is offered again on the next service pass,
 * and reaches the host once the controller takes it. */
MU_TEST(test_a_refused_control_reply_is_offered_again)
{
	refused_control_in(0x31);
	mu_assert_int_eq(4, (int)tx_words);
	mu_assert(!last_command_validated_a_buffer(),
	          "a packet the controller would not take whole must not be offered");

	/* the controller takes it on the next pass */
	tx_words = 0;
	fake_ctrl = WR_EN;
	tx_wr_en_words = 16;
	USBHwISR();

	mu_assert_int_eq(16, (int)tx_words);
	mu_assert(last_command_validated_a_buffer(),
	          "the retry must offer the whole packet to the host");
}

/* The same when the packet was written but the handshake that offers it to
 * the host gave up: the host has nothing to collect, so the reply is still
 * owed. */
MU_TEST(test_a_reply_the_engine_would_not_offer_is_offered_again)
{
	control_test_setup();
	handler_reply_len = 64;
	queue_setup_packet_len(0x3a, 64);
	tx_wr_en_words = 16;
	/* The controller takes the words but will not answer the command that
	 * selects endpoint 0 IN -- the first half of offering the packet to the
	 * host. Only that command: an engine that answered nothing would not
	 * get as far as reading the setup packet. */
	sie_fail_cmd = CMD_EP_SELECT | 1;   /* endpoint 0x80 is index 1 */

	USBHandleControlTransfer(0x00, EP_STATUS_SETUP);
	mu_assert_int_eq(16, (int)tx_words);
	mu_assert(!last_command_validated_a_buffer(),
	          "a packet the engine would not select for is not the host's yet");

	/* and once it answers again the host gets the reply */
	sie_fail_cmd = -1;
	tx_words = 0;
	USBHwISR();
	mu_assert_int_eq(16, (int)tx_words);
	mu_assert(last_command_validated_a_buffer(),
	          "the retry must offer the packet once the engine answers");
}

/* It keeps trying while the controller keeps refusing, rather than giving up
 * after one attempt: a mode loop services USB thousands of times a second, so
 * a transient refusal is retried far inside the host's one-second timeout. */
MU_TEST(test_a_refused_control_reply_is_retried_on_every_pass)
{
	int i;

	refused_control_in(0x32);

	for (i = 0; i < 5; ++i) {
		tx_words = 0;
		USBHwISR();
		mu_assert_int_eq(4, (int)tx_words);
	}

	tx_words = 0;
	tx_wr_en_words = 16;
	USBHwISR();
	mu_assert_int_eq(16, (int)tx_words);
	mu_assert(last_command_validated_a_buffer(),
	          "the host must get its reply as soon as the controller takes it");
}

/* The retry must not reorder a reply longer than one packet: the refused
 * packet is sent again at the offset it was refused at, not the next one. */
MU_TEST(test_a_refused_control_reply_resumes_at_the_same_offset)
{
	unsigned i;

	control_test_setup();
	handler_reply_len = 64;
	for (i = 0; i < sizeof(handler_data); ++i)
		handler_data[i] = (U8)i;
	queue_setup_packet_len(0x33, 64);
	fake_ctrl = WR_EN;
	tx_wr_en_words = 2;

	USBHandleControlTransfer(0x00, EP_STATUS_SETUP);
	mu_assert_int_eq(2, (int)tx_words);

	/* The retry writes the packet from its start again: all sixteen words,
	 * ending at byte 63 of the reply. A transfer the refused write had
	 * advanced would resume sixteen bytes in, so it would write twelve
	 * words and end somewhere else. */
	tx_words = 0;
	tx_wr_en_words = 16;
	USBHwISR();
	mu_assert_int_eq(16, (int)tx_words);
	mu_assert_int_eq(0x3F3E3D3C, (int)txdata_value);
}

/* A new setup packet voids the debt: the host has moved on, and answering its
 * new request with the old one's data is worse than not answering at all. */
MU_TEST(test_a_new_setup_packet_voids_a_refused_control_reply)
{
	refused_control_in(0x34);

	/* the host gives up and asks for something else, with no data stage */
	handler_reply_len = 0;
	queue_setup_packet_len(0x35, 0);
	fake_ctrl = 0;
	tx_wr_en_words = 16;
	USBHandleControlTransfer(0x00, EP_STATUS_SETUP);
	mu_assert_int_eq(0x35, handler_last_request);

	tx_words = 0;
	USBHwISR();
	mu_assert_int_eq(0, (int)tx_words);
}

/* And so does a bus reset: the host has dropped the request, so offering the
 * packet again between the reset and its first setup packet would put a reply
 * on the endpoint that nothing is waiting for. */
MU_TEST(test_a_bus_reset_voids_a_refused_control_reply)
{
	refused_control_in(0x37);

	USBControlInRetryCancel();

	tx_words = 0;
	tx_wr_en_words = 16;
	USBHwISR();
	mu_assert_int_eq(0, (int)tx_words);
}

/*
 * The setup-path clear, on the one request shape where nothing else would do
 * it: a host-to-device request with a data stage. Its setup packet does not
 * call DataIn() at all -- the stack waits for the data first -- so without the
 * clear the owed packet from the transfer before would still be offered, and
 * the host would collect it as the reply to a request that asked for nothing.
 */
MU_TEST(test_a_setup_packet_with_an_out_data_stage_voids_a_refused_reply)
{
	static U8 setup[64];

	refused_control_in(0x3b);

	/* OUT, vendor, device, wLength 8: data from the host follows */
	memset(setup, 0, sizeof(setup));
	setup[0] = 0x40;
	setup[1] = 0x3c;
	setup[6] = 8;
	fake_setup_bytes = setup;
	packet_ready = 1;
	packet_valid = 1;
	packet_len = 8;
	rx_words = 0;
	USBHandleControlTransfer(0x00, EP_STATUS_SETUP);

	tx_words = 0;
	tx_wr_en_words = 16;
	USBHwISR();
	mu_assert_int_eq(0, (int)tx_words);
}

/* And so does an endpoint event the firmware had to give up on. Only endpoint
 * 0's interrupts are enabled, so the event lost is one of its own -- a setup
 * packet, most likely -- and the host would collect the owed reply as the data
 * stage of the request whose setup packet was dropped.
 *
 * On the deferral that comes first the event is still waiting, so there is
 * nothing to void yet; that is the test below this one. */
MU_TEST(test_a_given_up_endpoint_event_voids_a_refused_control_reply)
{
	refused_control_in(0x38);

	/* two passes in which the engine will not report the endpoint's status:
	 * the first defers the event, the second gives it up */
	fake_raise_ep_event(1 << 0);
	sie_answers = 0;
	USBHwISR();
	USBHwISR();

	/* the engine answers again, and the owed packet must be gone */
	sie_answers = 1;
	tx_words = 0;
	tx_wr_en_words = 16;
	USBHwISR();
	mu_assert_int_eq(0, (int)tx_words);
}

/* A deferred event is not a lost one, so the reply still owed is still owed:
 * voiding it there would throw away a reply the host is waiting for every
 * time the status read is merely late. */
MU_TEST(test_a_deferred_endpoint_event_keeps_a_refused_control_reply)
{
	refused_control_in(0x39);

	fake_raise_ep_event(1 << 0);
	sie_answers = 0;
	USBHwISR();

	sie_answers = 1;
	tx_words = 0;
	tx_wr_en_words = 16;
	USBHwISR();
	mu_assert_int_eq(16, (int)tx_words);
}

/* And so does a stall, which is how the firmware ends a transfer it cannot
 * serve. */
MU_TEST(test_a_stall_voids_a_refused_control_reply)
{
	refused_control_in(0x36);

	/* the next setup packet cannot be read, so the pipe is stalled */
	packet_ready = 0;
	USBHandleControlTransfer(0x00, EP_STATUS_SETUP);
	mu_assert(last_command_stalled_ep0(), "an unreadable setup packet must stall");

	tx_words = 0;
	tx_wr_en_words = 16;
	USBHwISR();
	mu_assert_int_eq(0, (int)tx_words);
}

/* ---- the interrupt handler ---- */

/*
 * USBHwISR() is where a bound turns into a decision: it asks the protocol
 * engine for an endpoint's status and for the device status, and both answers
 * arrive in one shared register. A read that gave up must not be acted on, and
 * must not leave its answer behind for the next one.
 */

static unsigned ep_handler_calls;
static U8 ep_handler_last_status;
static unsigned dev_handler_calls;

static void fake_ep_handler(U8 bEP, U8 bEPStatus)
{
	(void)bEP;
	++ep_handler_calls;
	ep_handler_last_status = bEPStatus;
}

static void fake_dev_handler(U8 bDevStatus)
{
	(void)bDevStatus;
	++dev_handler_calls;
}

static void isr_test_setup(void)
{
	fake_reset();
	ep_handler_calls = 0;
	ep_handler_last_status = 0;
	dev_handler_calls = 0;
	USBHwRegisterEPIntHandler(0x82, fake_ep_handler);
	USBHwRegisterDevIntHandler(fake_dev_handler);
	fake_reset();
}

/* Endpoint 0x82 is index 5; endpoint 0 OUT, the one that carries control
 * requests, is index 0. */
#define EP82_BIT (1u << 5)
#define EP0_BIT  (1u << 0)

/* The normal path, so the tests below mean something. */
MU_TEST(test_an_endpoint_event_reaches_its_handler)
{
	isr_test_setup();
	fake_raise_ep_event(EP82_BIT);
	fake_cmddata = EPSTAT_FE;

	USBHwISR();

	mu_assert_int_eq(1, (int)ep_handler_calls);
	mu_assert_int_eq(EP_STATUS_DATA, ep_handler_last_status);
}

/* An event the firmware handled is consumed, so the next pass does not
 * dispatch it a second time. This is what the fake has to get right for the
 * tests below to mean anything: a fake that kept the interrupt bit set could
 * not tell a recovered event from a lost one. */
MU_TEST(test_a_handled_endpoint_event_is_consumed)
{
	isr_test_setup();
	fake_raise_ep_event(EP82_BIT);
	fake_cmddata = EPSTAT_FE;

	USBHwISR();
	mu_assert_int_eq(1, (int)ep_handler_calls);
	mu_assert(!fake_ep_event_pending(EP82_BIT),
	          "a handled event must be cleared from USBEpIntSt");

	USBHwISR();
	mu_assert_int_eq(1, (int)ep_handler_calls);
}

/*
 * An event that arrives between the two commands must not be dropped in
 * favour of the older one.
 *
 * The status is read first so that a read which gives up loses nothing, but
 * the write that clears the interrupt consumes every event waiting on that
 * endpoint -- including one that arrived since the read. So the status that
 * write reports is the one to dispatch: on endpoint 0 the later event is a
 * fresh setup packet, and handling the earlier event's status instead would
 * feed those eight bytes to whatever transfer was in progress.
 */
MU_TEST(test_an_event_arriving_before_the_interrupt_is_cleared_is_the_one_handled)
{
	isr_test_setup();
	fake_raise_ep_event(EP82_BIT);
	fake_cmddata = EPSTAT_FE;       /* the first event: data available */
	ep_status_at_clear = EPSTAT_STP; /* a setup packet arrives in between */

	USBHwISR();

	mu_assert_int_eq(1, (int)ep_handler_calls);
	mu_assert_int_eq(EP_STATUS_SETUP, ep_handler_last_status);
}

/* And when that write's answer does not arrive, the status read before it is
 * dispatched rather than nothing: an older status for a real event beats an
 * event thrown away. */
MU_TEST(test_a_late_answer_from_the_clear_falls_back_to_the_status_read)
{
	isr_test_setup();
	fake_raise_ep_event(EP82_BIT);
	fake_cmddata = EPSTAT_FE;
	clear_answer_withheld = 1;
	ep_status_at_clear = EPSTAT_STP;  /* never read, because it is late */

	USBHwISR();

	mu_assert_int_eq(1, (int)ep_handler_calls);
	mu_assert_int_eq(EP_STATUS_DATA, ep_handler_last_status);
}

/*
 * The status read gave up. Acting on what is in the register would dispatch
 * another endpoint's status as this one's -- a setup packet or a stall that
 * never happened -- so the endpoint is skipped.
 */
MU_TEST(test_an_endpoint_whose_status_cannot_be_read_is_skipped)
{
	isr_test_setup();
	fake_raise_ep_event(EP82_BIT);
	fake_cmddata = EPSTAT_STP;      /* would decode as a setup packet */
	sie_answers = 0;

	USBHwISR();

	mu_assert_int_eq(0, (int)ep_handler_calls);
	mu_assert(usbhw_timeouts > 0, "a wait that gave up must be counted");
}

/*
 * The regression this file was extended for: skipping the endpoint must not
 * throw the event away.
 *
 * The status used to be read by writing USBEpIntClr, which clears the
 * interrupt and returns the answer in one step -- so a read that gave up left
 * no status to act on and no event either. On endpoint 0 the event is a setup
 * packet and nothing else says a control request is waiting, so the request
 * went unanswered until the host's own timeout: a second for the Ubertooth
 * tools, and the end of a `ubertooth-btle` capture, which reads every packet
 * with a control request.
 *
 * The status is now read with plain Select Endpoint, which leaves the
 * interrupt alone, so the event is still waiting on the next pass.
 */
MU_TEST(test_an_endpoint_event_whose_status_read_gives_up_is_not_lost)
{
	isr_test_setup();
	fake_cmddata = EPSTAT_STP;

	/* A clean pass first: the one-shot that limits the deferral to one is a
	 * static in the file under test, so this starts the test from a known
	 * state whatever ran before. */
	fake_raise_ep_event(EP82_BIT);
	USBHwISR();
	mu_assert_int_eq(1, (int)ep_handler_calls);
	ep_handler_calls = 0;

	fake_raise_ep_event(EP82_BIT);
	sie_answers = 0;

	USBHwISR();
	mu_assert_int_eq(0, (int)ep_handler_calls);
	mu_assert(fake_ep_event_pending(EP82_BIT),
	          "the event must still be waiting, not consumed by the failed read");
	mu_assert(fake_ep_slow_set(),
	          "EP_SLOW must be set, or no later pass looks at the endpoints");

	/* the engine answers again: the event arrives, with its own status */
	sie_answers = 1;
	USBHwISR();

	mu_assert_int_eq(1, (int)ep_handler_calls);
	mu_assert_int_eq(EP_STATUS_SETUP, ep_handler_last_status);
	mu_assert(!fake_ep_event_pending(EP82_BIT),
	          "the event must be consumed once it has been handled");
}

/*
 * And the event it recovers is its own. An engine that stops answering for
 * one endpoint and then recovers must not dispatch whatever answer was left
 * in the shared register: the bits the failed wait was watching are cleared
 * on the way out, so the next read is a read.
 */
MU_TEST(test_a_failed_status_read_does_not_answer_the_next_one)
{
	isr_test_setup();
	fake_raise_ep_event(EP82_BIT);
	fake_cmddata = EPSTAT_STP;
	sie_answers = 0;
	USBHwISR();
	mu_assert_int_eq(0, (int)ep_handler_calls);

	/* the engine answers again, with this endpoint's real status */
	sie_answers = 1;
	fake_cmddata = EPSTAT_FE;
	USBHwISR();

	mu_assert_int_eq(1, (int)ep_handler_calls);
	mu_assert_int_eq(EP_STATUS_DATA, ep_handler_last_status);
}

/*
 * The deferral is one pass, not forever. In the six modes that service USB
 * from the USB interrupt, putting EP_SLOW back re-enters this handler at
 * once, so an engine that has stopped answering for good would loop in here
 * rather than run the mode. The second failure gives the event up, which
 * leaves the firmware where it was before this change -- one unanswered
 * request -- instead of a board that does nothing.
 */
MU_TEST(test_an_endpoint_event_is_given_up_after_one_retry)
{
	isr_test_setup();
	fake_raise_ep_event(EP82_BIT);
	fake_cmddata = EPSTAT_FE;
	/* A clean pass first: the one-shot is a static in the file under test,
	 * so this starts the test from a known state whatever ran before. */
	USBHwISR();
	mu_assert_int_eq(1, (int)ep_handler_calls);

	fake_raise_ep_event(EP82_BIT);
	sie_answers = 0;
	USBHwISR();
	mu_assert(fake_ep_event_pending(EP82_BIT), "the first failure defers");

	USBHwISR();
	mu_assert(!fake_ep_event_pending(EP82_BIT),
	          "the second failure must consume the event, or the endpoint "
	          "loop never ends");
	mu_assert_int_eq(1, (int)ep_handler_calls);

	/* and the engine recovering after that is an ordinary pass */
	sie_answers = 1;
	fake_raise_ep_event(EP82_BIT);
	USBHwISR();
	mu_assert_int_eq(2, (int)ep_handler_calls);
}

/*
 * The one-shot is one retry per event, not one for the life of the firmware:
 * the same point as the device status test further down. An event the engine
 * never answers uses its retry up, and the next event must still get one --
 * on endpoint 0 that next event is a control request.
 */
MU_TEST(test_a_given_up_endpoint_event_leaves_the_next_one_its_retry)
{
	isr_test_setup();
	fake_cmddata = EPSTAT_FE;

	/* an event the engine never answers: deferred once, then given up */
	fake_raise_ep_event(EP82_BIT);
	sie_answers = 0;
	USBHwISR();
	USBHwISR();
	mu_assert(!fake_ep_event_pending(EP82_BIT), "the event must be given up");

	/* the next event's read fails once. It must still be deferred. */
	fake_raise_ep_event(EP82_BIT);
	USBHwISR();
	mu_assert(fake_ep_event_pending(EP82_BIT),
	          "an event after a given-up one got no retry, so its control "
	          "request was lost");

	sie_answers = 1;
	USBHwISR();
	mu_assert_int_eq(1, (int)ep_handler_calls);
}

/* A bus reset ends every transfer, so it arms every endpoint's deferral
 * again: an event deferred when the reset arrived is gone with it, and the
 * next event on that endpoint -- a control request, on endpoint 0 -- must
 * still get its second chance. */
MU_TEST(test_a_bus_reset_arms_every_endpoints_deferral_again)
{
	isr_test_setup();
	fake_cmddata = EPSTAT_FE;

	/* an event the engine never answers: deferred once, then given up */
	fake_raise_ep_event(EP82_BIT);
	sie_answers = 0;
	USBHwISR();
	USBHwISR();
	mu_assert(!fake_ep_event_pending(EP82_BIT), "the event must be given up");

	/* a bus reset, read cleanly */
	sie_answers = 1;
	devintst_events = DEV_STAT;
	fake_cmddata = RST | CON;
	USBHwISR();
	mu_assert_int_eq(1, (int)dev_handler_calls);
	devintst_events = 0;
	fake_cmddata = EPSTAT_FE;

	/* the next event's read fails once, and must be deferred */
	fake_raise_ep_event(EP82_BIT);
	sie_answers = 0;
	USBHwISR();
	mu_assert(fake_ep_event_pending(EP82_BIT),
	          "a bus reset must leave the next event its deferral");

	sie_answers = 1;
	USBHwISR();
	mu_assert_int_eq(1, (int)ep_handler_calls);
}

/*
 * Several endpoints, one failing. btbr enables interrupts on its bulk OUT
 * endpoint as well as endpoint 0, so the one-shot has to be per endpoint: a
 * flag shared between them would spend endpoint 0's retry on the other one's
 * failure and lose a control request.
 */
MU_TEST(test_each_endpoints_event_has_its_own_retry)
{
	isr_test_setup();
	fake_cmddata = EPSTAT_FE;

	/* a clean pass on each, to start from a known one-shot */
	fake_raise_ep_event(EP0_BIT | EP82_BIT);
	USBHwISR();

	/* both events, and an engine that answers neither */
	fake_raise_ep_event(EP0_BIT | EP82_BIT);
	sie_answers = 0;
	USBHwISR();
	mu_assert(fake_ep_event_pending(EP0_BIT), "endpoint 0's event is deferred");
	mu_assert(fake_ep_event_pending(EP82_BIT), "the other one's too");

	/* the engine answers again: both events arrive */
	sie_answers = 1;
	USBHwISR();
	mu_assert(!fake_ep_event_pending(EP0_BIT), "endpoint 0's event arrived");
	mu_assert(!fake_ep_event_pending(EP82_BIT), "the other one's too");
	mu_assert_int_eq(2, (int)ep_handler_calls);
}

/* A device status event whose read works reaches the handler. */
MU_TEST(test_a_device_status_event_reaches_its_handler)
{
	isr_test_setup();
	devintst_events = DEV_STAT;
	fake_cmddata = RST | CON;       /* a bus reset */

	USBHwISR();

	mu_assert_int_eq(1, (int)dev_handler_calls);
}

/*
 * A device status read that gave up must not drop the event: the interrupt was
 * cleared before the read, and a lost bus reset leaves the stack answering on
 * an address the host has taken away. It is put back for the next pass -- once,
 * so an engine that never answers degrades instead of looping for ever.
 */
MU_TEST(test_a_failed_device_status_read_is_retried_once)
{
	isr_test_setup();
	devintst_events = DEV_STAT;
	fake_cmddata = RST | CON;
	sie_answers = 0;

	USBHwISR();
	mu_assert_int_eq(0, (int)dev_handler_calls);
	mu_assert_int_eq(DEV_STAT, (int)fake_devintset_written());

	/* second pass, still no answer: the event is dropped rather than
	 * re-armed again */
	fake_forget_devintset();
	USBHwISR();
	mu_assert_int_eq(0, (int)dev_handler_calls);
	mu_assert_int_eq(0, (int)fake_devintset_written());

	/* and once the engine answers, a later event is handled normally */
	sie_answers = 1;
	USBHwISR();
	mu_assert_int_eq(1, (int)dev_handler_calls);
}

/*
 * The one-shot is one retry per event, not one for the life of the firmware.
 *
 * Dropping an event used to leave the flag set, so the next transient failure
 * got no retry. Device status events are rare -- a reset, a suspend, a resume,
 * not one per frame -- so that next failure can be minutes later, and the bus
 * reset it loses is the case the retry exists for.
 *
 * A successful read between the two would hide it, because success cleared the
 * flag even before the fix. So: an event the engine never answers, then an
 * isolated failure straight after it.
 */
MU_TEST(test_a_dropped_device_status_event_leaves_the_next_one_its_retry)
{
	isr_test_setup();
	devintst_events = DEV_STAT;
	fake_cmddata = RST | CON;

	/* A clean pass first: the flag is a static in the file under test, so
	 * this starts the test from a known one-shot whatever ran before. */
	USBHwISR();
	mu_assert_int_eq(1, (int)dev_handler_calls);

	/* An event the engine never answers: one retry, then dropped. */
	sie_answers = 0;
	USBHwISR();
	mu_assert_int_eq(DEV_STAT, (int)fake_devintset_written());
	fake_forget_devintset();
	USBHwISR();
	mu_assert_int_eq(0, (int)fake_devintset_written());
	mu_assert_int_eq(1, (int)dev_handler_calls);

	/* The next event's read fails once. It must still be put back. */
	USBHwISR();
	mu_assert(fake_devintset_written() == (uint32_t)DEV_STAT,
	          "a device status event after a dropped one got no retry, so "
	          "its bus reset was lost");

	/* And the retried read reaches the handler. */
	fake_forget_devintset();
	sie_answers = 1;
	USBHwISR();
	mu_assert_int_eq(2, (int)dev_handler_calls);
}

/* ---- source scans ---- */

/*
 * Two things this fix relies on cannot be reached from here: the waits that
 * are gone, and the interrupt handle_usb() holds off. Both are a shape the
 * source either has or does not, so they are scanned, the way
 * test_mode_dispatch.c scans the dispatch.
 */

/* Strip C comments from a line in place, carrying block state across lines.
 * The source explains all of this in prose, so a scan that read the comments
 * would find whatever it was looking for. */
static void strip_comments(char *line, int *in_block)
{
	char *r = line, *w = line;

	while (*r != '\0') {
		if (*in_block) {
			if (r[0] == '*' && r[1] == '/') {
				*in_block = 0;
				r += 2;
			} else {
				r++;
			}
			continue;
		}
		if (r[0] == '/' && r[1] == '*') {
			*in_block = 1;
			r += 2;
			continue;
		}
		if (r[0] == '/' && r[1] == '/')
			break;
		*w++ = *r++;
	}
	*w = '\0';
}

/* Count code lines in `path` that contain all of up to three needles. */
static int count_lines_with(const char *path, const char *a, const char *b,
                            const char *c)
{
	char line[1024];
	FILE *f = fopen(path, "r");
	int in_block = 0, hits = 0;

	if (f == NULL) {
		printf("  FAIL cannot open %s\n", path);
		return -1;
	}
	while (fgets(line, sizeof(line), f) != NULL) {
		strip_comments(line, &in_block);
		if (strstr(line, a) == NULL)
			continue;
		if (b != NULL && strstr(line, b) == NULL)
			continue;
		if (c != NULL && strstr(line, c) == NULL)
			continue;
		++hits;
	}
	fclose(f);
	return hits;
}

/*
 * The waits that hung the firmware were all `while` loops on a USB register.
 * There must be none left: each one is a loop a controller that stops
 * answering holds forever, and USB service is in the same thread as the mode
 * loops, so the board stops answering with it.
 */
MU_TEST(test_no_unbounded_waits_on_usb_registers_remain)
{
	const char *path = FW_LPCUSB_DIR "/usbhw_lpc.c";
	static const char *regs[] = {
		"USBDevIntSt", "USBRxPLen", "USBCtrl", "USBCmdData", "USBEpIntSt",
	};
	static const char *files[] = {
		FW_LPCUSB_DIR "/usbhw_lpc.c",
		FW_LPCUSB_DIR "/usbcontrol.c",
		FW_LPCUSB_DIR "/usbstdreq.c",
	};
	unsigned f, r;

	/* A loop on a USB register, by any of the three spellings C has for one.
	 * This is a line-based guard: a condition split across lines would get
	 * past it, which is the usual limit of scanning source as text. */
	for (f = 0; f < sizeof(files) / sizeof(files[0]); ++f) {
		for (r = 0; r < sizeof(regs) / sizeof(regs[0]); ++r) {
			mu_assert_int_eq(0, count_lines_with(files[f], "while", regs[r], NULL));
			mu_assert_int_eq(0, count_lines_with(files[f], "for (", regs[r], NULL));
			mu_assert_int_eq(0, count_lines_with(files[f], "for(", regs[r], NULL));
		}
	}
	/* The clock-enable waits at init are the exception the scan allows: they
	 * run once at boot, before the device is on the bus, so there is no host
	 * to stop answering. Four of them -- two for this part, two in the LPC23xx
	 * branch nothing here builds. Counted so they cannot grow; update the
	 * count if a part is added or removed. */
	mu_assert_int_eq(4, count_lines_with(path, "while", "USBClkSt", NULL));
}

/*
 * Two functions let a mode loop drive USB: handle_usb(), which also keeps the
 * bulk IN endpoint loaded, and handle_usb_control(), for a mode whose host
 * reads packets with poll requests instead. Each holds the USB interrupt off
 * while it runs, so the count below is one per function.
 *
 * On the modes that exist today this costs a register read and nothing else:
 * the six modes that service USB from the interrupt call neither, so the two
 * contexts never overlap. That is a property of those mode functions, though,
 * not of this code -- a mode that polled USB with the interrupt still enabled
 * would have two contexts driving one set of registers, and the loser of that
 * would wait on a handshake completion the winner consumed. This keeps the
 * guarantee here, where it can be checked.
 */
/*
 * The retry has to be reached from every path that drives USB: the main loop,
 * every polled mode, the bounded radio waits, the USB interrupt in the six
 * modes that use it, and both images. It is called from the end of USBHwISR()
 * for that reason -- one site that all of them already go through -- so the
 * count is one, in the file under test and nowhere else.
 */
MU_TEST(test_every_usb_service_pass_offers_an_owed_control_packet_again)
{
	mu_assert_int_eq(1, count_lines_with(FW_LPCUSB_DIR "/usbhw_lpc.c",
	                                     "USBControlInRetry();", NULL, NULL));
	mu_assert_int_eq(0, count_lines_with(FW_RXTX_DIR "/ubertooth_usb.c",
	                                     "USBControlInRetry", NULL, NULL));
	/* And the bus reset handler, which this test cannot reach because
	 * usbinit.c is not linked here, drops an owed packet. */
	mu_assert_int_eq(1, count_lines_with(FW_LPCUSB_DIR "/usbinit.c",
	                                     "USBControlInRetryCancel", NULL, NULL));
}

MU_TEST(test_handle_usb_holds_the_usb_interrupt_off)
{
	const char *path = FW_RXTX_DIR "/ubertooth_usb.c";

	mu_assert_int_eq(2, count_lines_with(path, "usb_irq_hold", NULL, NULL));
	mu_assert_int_eq(2, count_lines_with(path, "usb_irq_release", NULL, NULL));
}

/*
 * And the hold has to be the synchronising kind. A write to NVIC ICER0 is not
 * self-synchronising on Cortex-M3, so without the barriers an interrupt
 * already pending is taken after the mask -- inside the section being
 * protected, which is the whole point of it.
 */
MU_TEST(test_the_usb_interrupt_hold_synchronises)
{
	const char *path = FW_COMMON_DIR "/usb_irq.h";

	mu_assert_int_eq(1, count_lines_with(path, "ICER0", "ICER0_ICE_USB", NULL));
	mu_assert_int_eq(1, count_lines_with(path, "dsb", NULL, NULL));
	mu_assert_int_eq(1, count_lines_with(path, "isb", NULL, NULL));
	/* One definition, shared: each image's USB header includes it rather
	 * than carrying a copy that could drift from this one. */
	mu_assert_int_eq(1, count_lines_with(FW_RXTX_DIR "/ubertooth_usb.h",
	                                     "usb_irq.h", NULL, NULL));
	mu_assert_int_eq(1, count_lines_with(FW_BTBR_DIR "/include/ubtbr/ubertooth_usb.h",
	                                     "usb_irq.h", NULL, NULL));
}

int main(void)
{
	printf("USB hardware layer bounded wait tests\n");
	MU_RUN(test_ep_status_returns_what_the_engine_reported);
	MU_RUN(test_ep_status_gives_up_when_the_engine_stops_answering);
	MU_RUN(test_stall_returns_when_the_engine_stops_answering);
	MU_RUN(test_working_engine_counts_no_timeouts);
	MU_RUN(test_the_controller_is_usable_again_after_a_timeout);
	MU_RUN(test_ep_read_returns_the_packet);
	MU_RUN(test_ep_read_gives_up_when_no_packet_arrives);
	MU_RUN(test_ep_read_clears_the_control_register_when_it_gives_up);
	MU_RUN(test_ep_read_reports_no_packet_when_the_select_gives_up);
	MU_RUN(test_ep_read_reports_no_packet_when_the_buffer_release_gives_up);
	MU_RUN(test_ep_read_clears_the_control_register_on_an_invalid_packet);
	MU_RUN(test_ep_write_writes_the_whole_packet);
	MU_RUN(test_ep_write_stops_at_the_packet_length_with_wr_en_stuck);
	MU_RUN(test_ep_write_reports_a_short_write_and_does_not_validate_it);
	MU_RUN(test_ep_write_of_one_byte_writes_one_word);
	MU_RUN(test_ep_write_of_zero_bytes_writes_nothing);
	MU_RUN(test_a_zero_length_packet_that_went_through_is_not_a_failure);
	MU_RUN(test_ep_write_reports_nothing_taken_when_the_engine_stops_answering);
	MU_RUN(test_ep_write_reports_nothing_taken_when_the_select_gives_up);
	MU_RUN(test_ep_write_reports_nothing_taken_when_the_validation_gives_up);
	MU_RUN(test_a_readable_setup_packet_reaches_the_handler);
	MU_RUN(test_an_unreadable_setup_packet_runs_nothing);
	MU_RUN(test_a_setup_packet_whose_buffer_release_gives_up_runs_nothing);
	MU_RUN(test_a_short_setup_packet_runs_nothing);
	MU_RUN(test_a_failed_setup_leaves_no_transfer_to_resume);
	MU_RUN(test_a_control_transfer_works_after_a_failed_setup_read);
	MU_RUN(test_a_complete_reply_leaves_nothing_on_the_endpoint);
	MU_RUN(test_a_reply_as_long_as_asked_for_leaves_nothing_on_the_endpoint);
	MU_RUN(test_a_reply_that_fills_whole_packets_is_ended_with_a_zero_length_one);
	MU_RUN(test_a_reply_of_nothing_to_a_host_that_asked_for_some);
	MU_RUN(test_a_reply_longer_than_one_packet_ends_with_its_short_packet);
	MU_RUN(test_a_request_with_no_data_stage_is_answered_once);
	MU_RUN(test_a_control_reply_the_controller_takes_is_sent_once);
	MU_RUN(test_a_refused_control_reply_is_offered_again);
	MU_RUN(test_a_reply_the_engine_would_not_offer_is_offered_again);
	MU_RUN(test_a_refused_control_reply_is_retried_on_every_pass);
	MU_RUN(test_a_refused_control_reply_resumes_at_the_same_offset);
	MU_RUN(test_a_new_setup_packet_voids_a_refused_control_reply);
	MU_RUN(test_a_bus_reset_voids_a_refused_control_reply);
	MU_RUN(test_a_setup_packet_with_an_out_data_stage_voids_a_refused_reply);
	MU_RUN(test_a_given_up_endpoint_event_voids_a_refused_control_reply);
	MU_RUN(test_a_deferred_endpoint_event_keeps_a_refused_control_reply);
	MU_RUN(test_a_stall_voids_a_refused_control_reply);
	MU_RUN(test_an_endpoint_event_reaches_its_handler);
	MU_RUN(test_a_handled_endpoint_event_is_consumed);
	MU_RUN(test_an_event_arriving_before_the_interrupt_is_cleared_is_the_one_handled);
	MU_RUN(test_a_late_answer_from_the_clear_falls_back_to_the_status_read);
	MU_RUN(test_an_endpoint_whose_status_cannot_be_read_is_skipped);
	MU_RUN(test_an_endpoint_event_whose_status_read_gives_up_is_not_lost);
	MU_RUN(test_a_failed_status_read_does_not_answer_the_next_one);
	MU_RUN(test_an_endpoint_event_is_given_up_after_one_retry);
	MU_RUN(test_a_given_up_endpoint_event_leaves_the_next_one_its_retry);
	MU_RUN(test_a_bus_reset_arms_every_endpoints_deferral_again);
	MU_RUN(test_each_endpoints_event_has_its_own_retry);
	MU_RUN(test_a_device_status_event_reaches_its_handler);
	MU_RUN(test_a_failed_device_status_read_is_retried_once);
	MU_RUN(test_a_dropped_device_status_event_leaves_the_next_one_its_retry);
	MU_RUN(test_no_unbounded_waits_on_usb_registers_remain);
	MU_RUN(test_every_usb_service_pass_offers_an_owed_control_packet_again);
	MU_RUN(test_handle_usb_holds_the_usb_interrupt_off);
	MU_RUN(test_the_usb_interrupt_hold_synchronises);
	return mu_report();
}
