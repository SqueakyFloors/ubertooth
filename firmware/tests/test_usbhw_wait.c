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

volatile uint32_t fake_devintclr, fake_devintset, fake_devinten, fake_devintpri;
volatile uint32_t fake_epintst, fake_epinten, fake_epintclr, fake_epintpri;
volatile uint32_t fake_reep, fake_epin, fake_maxpsize;
volatile uint32_t fake_txplen, fake_ctrl;
volatile uint32_t fake_cmdcode, fake_cmddata;
volatile uint32_t fake_epdmaen, fake_epdmadis, fake_udcah;
volatile uint32_t fake_clkctrl, fake_clkst, fake_pconp;
volatile uint32_t fake_pinsel1, fake_pinsel3, fake_pinsel4;

/* Does the protocol engine answer commands? */
static int sie_answers;
/* Event bits the interrupt handler should see (FRAME, DEV_STAT, EP_SLOW). */
static uint32_t devintst_events;
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
	devintst_value = devintst_events
	               | (sie_answers ? (CCEMTY | CDFULL | EP_RLZED) : 0u);
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
	devintst_events = 0;
	fake_epintst = 0;
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

/* A write whose handshake stops answering still returns. */
MU_TEST(test_ep_write_returns_when_the_engine_stops_answering)
{
	U8 buf[64];

	fake_reset();
	memset(buf, 0xA5, sizeof(buf));
	fake_ctrl = WR_EN;
	tx_wr_en_words = 16;
	sie_answers = 0;

	mu_assert_int_eq(64, USBHwEPWrite(0x82, buf, sizeof(buf)));
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

static BOOL fake_request_handler(TSetupPacket *pSetup, int *piLen,
                                 U8 **ppbData)
{
	(void)ppbData;
	++handler_calls;
	handler_last_request = pSetup->bRequest;
	*piLen = 0;
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

/* A vendor IN request with no data stage, as the host sends it. Bigger than a
 * setup packet so a read the firmware takes further than eight bytes stays
 * inside the buffer. */
static void queue_setup_packet(U8 request)
{
	static U8 setup[64];

	memset(setup, 0, sizeof(setup));
	setup[0] = 0xC0;     /* IN, vendor, device */
	setup[1] = request;
	/* wValue, wIndex and wLength all zero */

	fake_setup_bytes = setup;
	packet_ready = 1;
	packet_valid = 1;
	packet_len = 8;
	rx_words = 0;
}

static void control_test_setup(void)
{
	fake_reset();
	handler_calls = 0;
	handler_last_request = 0;
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

/* The normal path, so the tests below mean something. */
MU_TEST(test_an_endpoint_event_reaches_its_handler)
{
	isr_test_setup();
	devintst_events = EP_SLOW;
	fake_epintst = 1 << 5;          /* endpoint 0x82 */
	fake_cmddata = EPSTAT_FE;

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
	devintst_events = EP_SLOW;
	fake_epintst = 1 << 5;
	fake_cmddata = EPSTAT_STP;      /* would decode as a setup packet */
	sie_answers = 0;

	USBHwISR();

	mu_assert_int_eq(0, (int)ep_handler_calls);
	mu_assert(usbhw_timeouts > 0, "a wait that gave up must be counted");
}

/*
 * And it must not leave the answer behind. The engine stops answering for one
 * endpoint and recovers; the bits the failed wait was watching have been
 * cleared, so the next endpoint's status is its own and not the first one's.
 */
MU_TEST(test_a_failed_status_read_does_not_answer_the_next_one)
{
	isr_test_setup();
	devintst_events = EP_SLOW;
	fake_epintst = 1 << 5;
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
	mu_assert_int_eq(DEV_STAT, (int)fake_devintset);

	/* second pass, still no answer: the event is dropped rather than
	 * re-armed again */
	fake_devintset = 0;
	USBHwISR();
	mu_assert_int_eq(0, (int)dev_handler_calls);
	mu_assert_int_eq(0, (int)fake_devintset);

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
	mu_assert_int_eq(DEV_STAT, (int)fake_devintset);
	fake_devintset = 0;
	USBHwISR();
	mu_assert_int_eq(0, (int)fake_devintset);
	mu_assert_int_eq(1, (int)dev_handler_calls);

	/* The next event's read fails once. It must still be put back. */
	USBHwISR();
	mu_assert(fake_devintset == (uint32_t)DEV_STAT,
	          "a device status event after a dropped one got no retry, so "
	          "its bus reset was lost");

	/* And the retried read reaches the handler. */
	fake_devintset = 0;
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
	MU_RUN(test_ep_read_clears_the_control_register_on_an_invalid_packet);
	MU_RUN(test_ep_write_writes_the_whole_packet);
	MU_RUN(test_ep_write_stops_at_the_packet_length_with_wr_en_stuck);
	MU_RUN(test_ep_write_reports_a_short_write_and_does_not_validate_it);
	MU_RUN(test_ep_write_of_one_byte_writes_one_word);
	MU_RUN(test_ep_write_of_zero_bytes_writes_nothing);
	MU_RUN(test_ep_write_returns_when_the_engine_stops_answering);
	MU_RUN(test_a_readable_setup_packet_reaches_the_handler);
	MU_RUN(test_an_unreadable_setup_packet_runs_nothing);
	MU_RUN(test_a_short_setup_packet_runs_nothing);
	MU_RUN(test_a_failed_setup_leaves_no_transfer_to_resume);
	MU_RUN(test_a_control_transfer_works_after_a_failed_setup_read);
	MU_RUN(test_an_endpoint_event_reaches_its_handler);
	MU_RUN(test_an_endpoint_whose_status_cannot_be_read_is_skipped);
	MU_RUN(test_a_failed_status_read_does_not_answer_the_next_one);
	MU_RUN(test_a_device_status_event_reaches_its_handler);
	MU_RUN(test_a_failed_device_status_read_is_retried_once);
	MU_RUN(test_a_dropped_device_status_event_leaves_the_next_one_its_retry);
	MU_RUN(test_no_unbounded_waits_on_usb_registers_remain);
	MU_RUN(test_handle_usb_holds_the_usb_interrupt_off);
	MU_RUN(test_the_usb_interrupt_hold_synchronises);
	return mu_report();
}
