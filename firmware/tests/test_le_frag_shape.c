/*
 * How each of the firmware's two low-energy capture loops gets a whole packet
 * and hands it over to be split across USB records.
 *
 * A packet of more than 50 bytes does not fit one record, so it goes out as
 * one LE_PACKET_START record and ordered LE_PACKET_CONT records. That split
 * lives in ubertooth_usb.c and runs for real in test_le_frag_enqueue. What
 * this test checks is the half above it, which cannot run here because it is
 * the capture loops themselves:
 *
 *   - le_phy.c, the connection follower behind ubertooth-btle -f and -n,
 *   - bluetooth_rxtx.c's bt_le_sync(), the follow half of -p.
 *
 * Both have to pass the whole captured packet to the one shared enqueue. The
 * second is the one that used to cut a packet at 50 bytes: it received into a
 * 48-byte buffer and read the PDU length through a 6-bit mask, so a Data
 * Length Extension packet was either cut or dropped for its length. Its
 * receive buffer now holds the longest packet there can be, and it reads the
 * whole length field, so there is no length to refuse.
 *
 * The two loops also share that buffer with the mode's other half, which is
 * only safe while they never run at the same time. The last cases pin that:
 * one half fills it with symbols and hands them to a data callback, and the
 * other has no data callback at all.
 *
 * These are text scans, the way test_le_phy_order.c and test_mode_dispatch.c
 * are. The round trip over real records is covered on hardware.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "minunit.h"
#include "source_scan.h"
#include "ubertooth_le_frag.h"
/* For le_length_is_possible() and the two payload maximums, which are pure
 * logic and run here. stubs/ubertooth.h stands in for the hardware header
 * this one includes. */
#include "bluetooth_le.h"

#ifndef FW_RXTX_DIR
#error "FW_RXTX_DIR must name firmware/bluetooth_rxtx"
#endif

/* Each file, comments and string literals removed, spaces collapsed. */
static char le_phy[400000];
static char rxtx[600000];
static char dma[60000];
/* Only the declarations near the top; the whitening tables below them are
 * long and nothing here reads them. */
static char le_h[20000];

static void load(const char *name, char *out, size_t size)
{
	char path[512];
	char line[4096];
	char clean[4096];
	FILE *f;
	int in_block = 0;
	size_t used = 0;

	snprintf(path, sizeof(path), "%s/%s", FW_RXTX_DIR, name);
	f = fopen(path, "r");
	if (f == NULL) {
		/* Every check below reads this text, so a file that cannot be read
		 * must stop the test rather than leave it checking nothing. */
		fprintf(stderr, "cannot open %s\n", path);
		exit(1);
	}
	while (fgets(line, sizeof(line), f) != NULL) {
		strip_comments(line, &in_block);
		strip_string_literals(line);
		collapse_spaces(line, clean, sizeof(clean));
		if (clean[0] == '\0')
			continue;
		used += (size_t) snprintf(out + used, size - used, "%s ", clean);
		if (used >= size - 1)
			break;
	}
	fclose(f);
	/* A file that outgrew the buffer would be scanned only in part, which
	 * would pass checks on the text that was cut off. */
	mu_assert(used < size - 1, "the source did not fit the scan buffer");
}

/* ------------------------------------------------------------------ *
 * Both loops queue packets through the one shared enqueue
 * ------------------------------------------------------------------ */

/* One fragmenting scheme, not one per capture path: a packet long enough to
 * need splitting takes the same shape whichever loop captured it. */
MU_TEST(both_capture_loops_hand_their_packets_to_the_shared_enqueue)
{
	mu_assert(strstr(le_phy, "usb_enqueue_le_packet(") != NULL,
	          "the connection follower does not use the shared enqueue");
	mu_assert(strstr(rxtx, "usb_enqueue_le_packet(") != NULL,
	          "the promiscuous follow half does not use the shared enqueue");
	/* Nothing writes a record's fragment metadata anywhere else, which is
	 * what a second scheme would look like. */
	mu_assert(strstr(le_phy, "LE_PACKET_START") == NULL,
	          "the connection follower builds fragment records of its own");
	mu_assert(strstr(rxtx, "LE_PACKET_START") == NULL,
	          "the promiscuous follow half builds fragment records of its own");
}

/* The whole captured packet, not the 50 bytes a record happens to hold. */
MU_TEST(the_promiscuous_loop_queues_the_whole_packet)
{
	mu_assert(strstr(rxtx, "unsigned pdu_bytes = len + 3;") != NULL,
	          "the captured length is not the PDU and its CRC");
	mu_assert(strstr(rxtx,
	                 "usb_enqueue_le_packet(&meta, p, p + 4, pdu_bytes)")
	          != NULL,
	          "the promiscuous follow half does not queue the whole packet");
	/* The fixed 50-byte copy this path used to make is gone. */
	mu_assert(strstr(rxtx, "enqueue(LE_PACKET, (uint8_t *)packet)") == NULL,
	          "the promiscuous follow half still makes a fixed-size copy");
}

/* ------------------------------------------------------------------ *
 * The promiscuous loop's receive buffer and length field
 * ------------------------------------------------------------------ */

/* The length field is eight bits wide. Reading six of them turned a Data
 * Length Extension length into a smaller one, so the packet was cut and
 * mislabelled, and the lengths the mask did not mangle were refused instead.
 */
MU_TEST(the_promiscuous_loop_reads_the_whole_length_field)
{
	mu_assert(strstr(rxtx, "unsigned len = p[5] + 2;") != NULL,
	          "the PDU length is not read as a whole byte");
	mu_assert(strstr(rxtx, "p[5] & 0x3f") == NULL,
	          "the PDU length is still read through a 6-bit mask");
	mu_assert(strstr(rxtx, "if (len > 39)") == NULL,
	          "a long packet is still refused for its length");
}

/* Nothing can arrive that the buffer cannot hold, which is why no length has
 * to be refused: the buffer is as long as the host's own, and the radio is
 * told to put at most that much in it. */
MU_TEST(the_promiscuous_buffers_hold_the_longest_packet)
{
	mu_assert(strstr(rxtx, "uint8_t packet[LE_MAX_USB_PACKET_SIZE];") != NULL,
	          "the receive buffer is not sized for the longest packet");
	mu_assert(strstr(rxtx,
	                 "#define LE_RX_PDU_MAX (LE_MAX_USB_PACKET_SIZE - 4)")
	          != NULL,
	          "the received bytes are not the buffer less the access address");
	mu_assert(strstr(rxtx, "dma_init_le(le_rx.packet + 4, LE_RX_PDU_MAX)")
	          != NULL,
	          "the receive DMA is not pointed at the whole buffer");
	/* The longest packet the length field can ask for, and the buffer, are
	 * the same size: the access address, the header, 255 payload bytes and
	 * the CRC. */
	mu_assert_int_eq(4 + 2 + 255 + 3, LE_MAX_USB_PACKET_SIZE);
}

/* The whitening table covers only the front of a PDU, so the rest of a long
 * packet is dewhitened without it, carrying on from where the table stopped.
 */
MU_TEST(a_long_promiscuous_packet_is_dewhitened_past_the_table)
{
	mu_assert(strstr(rxtx,
	                 "if (pdu_bytes > LE_WHITENING_WORD_BYTES)"
	                 " le_dewhiten_from(p + 4 + LE_WHITENING_WORD_BYTES,"
	                 " pdu_bytes - LE_WHITENING_WORD_BYTES, channel,"
	                 " LE_WHITENING_WORD_BYTES);") != NULL,
	          "the bytes past the whitening table are not dewhitened");
}

/* ------------------------------------------------------------------ *
 * The two halves of the mode share one receive buffer
 * ------------------------------------------------------------------ */

/* The halves run one after the other, so the buffer they share is only ever
 * in one of its two shapes. bt_generic_le() is the half that searches
 * symbols, and it is the only one with a data callback. */
MU_TEST(only_one_half_of_the_mode_reads_symbols)
{
	static char body[200000];
	const char *generic = strstr(rxtx, "void bt_generic_le(u8 active_mode)");
	const char *sync = strstr(rxtx, "void bt_le_sync(u8 active_mode)");
	/* The next function after bt_le_sync(), which bounds its body. */
	const char *end = strstr(rxtx, "int cb_follow_le(");
	size_t len;

	mu_assert(generic != NULL, "bt_generic_le() is not there to be found");
	mu_assert(sync != NULL, "bt_le_sync() is not there to be found");
	mu_assert(end != NULL, "cb_follow_le() is not there to be found");
	mu_assert(generic < sync && sync < end,
	          "the two halves moved; this scan assumes their order");

	len = (size_t)(end - sync);
	mu_assert(len < sizeof(body), "bt_le_sync() did not fit the scan buffer");
	memcpy(body, sync, len);
	body[len] = '\0';

	/* Within bt_le_sync(), nothing calls or assigns the data callback, and
	 * nothing touches the symbol half of the buffer. */
	mu_assert(strstr(body, "data_cb") == NULL,
	          "the follow half uses the symbol half's callback");
	mu_assert(strstr(body, "le_rx.symbols") == NULL,
	          "the follow half reads the symbol half's buffer");

	/* And the search half hands over the symbols, not the packet. */
	mu_assert(strstr(rxtx, "data_cb(le_rx.symbols)") != NULL,
	          "the search half does not hand the symbols to its callback");
}

/* The search half slides a pattern over the whole buffer from its first pass,
 * including the half it has not filled yet. That half holds whatever the
 * follow half left there, so it is cleared on the way in. */
MU_TEST(the_shared_buffer_is_cleared_before_symbols_are_searched)
{
	mu_assert(strstr(rxtx,
	                 "memset(le_rx.symbols, 0, sizeof(le_rx.symbols));")
	          != NULL,
	          "the symbol buffer is not cleared when the search half starts");
}

/* ------------------------------------------------------------------ *
 * The receive DMA covers a whole packet in one arming
 * ------------------------------------------------------------------ */

/* The loop reads the PDU length out of the first four bytes and then waits
 * for the rest, which means the first four have to arrive on their own. */
MU_TEST(the_loop_waits_for_the_length_byte_and_then_for_the_packet)
{
	mu_assert(strstr(rxtx, "unsigned want = 4 * ((pdu_bytes + 3) / 4);")
	          != NULL,
	          "the loop does not wait for this packet's bytes");
	mu_assert(strstr(rxtx,
	                 "while (DMACC0DestAddr < (uint32_t)(p + 4) + want")
	          != NULL,
	          "the loop does not follow the transfer by destination address");
	/* A length field corrupted by noise can ask for the longest packet
	 * there is, so the wait has to end on a mode change rather than hold
	 * the mode open for bytes the radio has stopped sending. */
	mu_assert(strstr(rxtx, "&& requested_mode == active_mode) ; ") != NULL,
	          "a stopped mode does not end the wait for a packet's bytes");
	mu_assert(strstr(rxtx, "if (requested_mode != active_mode) goto cleanup;")
	          != NULL,
	          "a stopped mode does not end the mode after the wait");
}

/* The receive buffer is reused, so a packet whose bytes did not all arrive
 * would be handed over padded with the previous packet's. It is dropped
 * instead. */
MU_TEST(a_packet_whose_bytes_did_not_all_arrive_is_dropped)
{
	mu_assert(strstr(rxtx,
	                 "int whole = rx_err == 0 && DMACC0DestAddr >="
	                 " (uint32_t)(p + 4) + want;") != NULL,
	          "whether the whole packet arrived is not worked out");
	mu_assert(strstr(rxtx, "if (!whole) goto rx_flush;") != NULL,
	          "a packet that arrived in part is still handed over");
	/* And the host hears about the error on the next record. */
	mu_assert(strstr(rxtx, "if (rx_err) status |= DMA_ERROR;") != NULL,
	          "a transfer error during a packet is not reported");
}

/* Waiting for a long packet costs time the mode could spend on the next
 * connection event, and the radio synchronizes on 32 bits, so noise does look
 * like a packet sometimes. A length the specification does not allow, and any
 * long length before there is a CRC to check it against, is refused. */
MU_TEST(an_impossible_length_is_not_waited_for)
{
	mu_assert(strstr(rxtx,
	                 "if (!le_length_is_possible(p[5], le.crc_verify))"
	                 " goto rx_flush;") != NULL,
	          "an impossible PDU length is waited for and sent on");

	/* The decision itself is pure logic, so it runs here. Before the
	 * connection's CRCInit is known, only a length that existed before Data
	 * Length Extension is believed; after, the specification's maximum. */
	mu_assert(le_length_is_possible(0, 0), "an empty PDU is refused");
	mu_assert(le_length_is_possible(LE_MAX_PAYLOAD_LEGACY, 0),
	          "the longest pre-extension length is refused without a CRC");
	mu_assert(!le_length_is_possible(LE_MAX_PAYLOAD_LEGACY + 1, 0),
	          "a long length is believed before there is a CRC to check it");
	mu_assert(le_length_is_possible(LE_MAX_PAYLOAD_LEGACY + 1, 1),
	          "a long length is refused even once the CRC can check it");
	mu_assert(le_length_is_possible(LE_MAX_PAYLOAD, 1),
	          "the specification's longest payload is refused");
	mu_assert(!le_length_is_possible(LE_MAX_PAYLOAD + 1, 1),
	          "a length the specification does not allow is believed");
	mu_assert(!le_length_is_possible(255, 1),
	          "the largest the length field can hold is believed");

	/* The values themselves: 251 from Data Length Extension, and 37 from an
	 * advertising PDU's 6-bit length field -- not a data channel PDU's lower
	 * legacy maximum, because a mode following a host-supplied target starts
	 * on the advertising access address. */
	mu_assert_int_eq(251, LE_MAX_PAYLOAD);
	mu_assert_int_eq(37, LE_MAX_PAYLOAD_LEGACY);
	/* The larger still fits a capture, so refusing a length is about the
	 * time it costs and not about running past the buffer. */
	mu_assert(4 + 2 + LE_MAX_PAYLOAD + 3 <= LE_MAX_USB_PACKET_SIZE,
	          "the longest allowed packet does not fit a capture");
}

/* ------------------------------------------------------------------ *
 * What the receive DMA is armed to do
 * ------------------------------------------------------------------ */

/* Exactly one item interrupts, which is what makes the terminal count mean
 * "a packet has started" rather than "another four bytes arrived". The loop's
 * counter has to be cleared before the controller is armed, or that one count
 * can be lost. */
MU_TEST(only_the_first_dma_item_interrupts)
{
	const char *clear = strstr(dma, "rx_tc = 0; rx_err = 0;");
	const char *arm = strstr(dma, "DMACC0SrcAddr = le_dma_lli[0].src;");

	mu_assert(strstr(dma, "(i == 0 ? DMACCxControl_I : 0);") != NULL,
	          "more than the first DMA item raises a terminal count");
	mu_assert(strstr(dma, "const unsigned size[2] = { LE_DMA_HEAD,"
	                      " len - LE_DMA_HEAD };") != NULL,
	          "the chain is not the first bytes and then the rest");

	/* Clearing the count belongs to arming the channel, so every caller
	 * gets it -- including the one that arms it on the way into the mode,
	 * where the count left over from the half that ran before would
	 * otherwise be read as a packet. */
	mu_assert(clear != NULL,
	          "arming the low-energy receive does not clear the count");
	mu_assert(arm != NULL, "the channel setup in dma_init_le() moved");
	mu_assert(clear < arm,
	          "the count is cleared after the channel is armed");
	mu_assert(strstr(rxtx, "rx_tc = 0; rx_err = 0;") == NULL ||
	          strstr(rxtx, "void bt_le_sync(u8 active_mode)") >
	          strstr(rxtx, "rx_tc = 0; rx_err = 0;"),
	          "the follow half still clears the count itself");
}

/* The buffer is written by the controller, not by code the compiler can see,
 * so reads of it are kept after the waits that end when the bytes are there.
 */
MU_TEST(reads_of_the_receive_buffer_are_kept_after_the_waits)
{
	/* The scan strips string literals, so the barrier's operands are gone
	 * from this text and only its shape is left to look for. */
	mu_assert(strstr(rxtx, "static inline void le_rx_barrier(void) { "
	                       "__asm__ volatile (") != NULL,
	          "there is no barrier between the wait and reading the buffer");
	mu_assert(strstr(rxtx, "le_rx_barrier(); memcpy(p, &le.access_address, 4);")
	          != NULL,
	          "the first bytes are read without a barrier after the wait");
	mu_assert(strstr(rxtx, "if (!whole) goto rx_flush; le_rx_barrier();")
	          != NULL,
	          "the rest of the packet is read without a barrier after the wait");
}

int main(void)
{
	printf("LE capture shape tests\n");
	load("le_phy.c", le_phy, sizeof(le_phy));
	load("bluetooth_rxtx.c", rxtx, sizeof(rxtx));
	load("ubertooth_dma.c", dma, sizeof(dma));
	load("bluetooth_le.h", le_h, sizeof(le_h));
	MU_RUN(both_capture_loops_hand_their_packets_to_the_shared_enqueue);
	MU_RUN(the_promiscuous_loop_queues_the_whole_packet);
	MU_RUN(the_promiscuous_loop_reads_the_whole_length_field);
	MU_RUN(the_promiscuous_buffers_hold_the_longest_packet);
	MU_RUN(a_long_promiscuous_packet_is_dewhitened_past_the_table);
	MU_RUN(only_one_half_of_the_mode_reads_symbols);
	MU_RUN(the_shared_buffer_is_cleared_before_symbols_are_searched);
	MU_RUN(the_loop_waits_for_the_length_byte_and_then_for_the_packet);
	MU_RUN(a_packet_whose_bytes_did_not_all_arrive_is_dropped);
	MU_RUN(an_impossible_length_is_not_waited_for);
	MU_RUN(only_the_first_dma_item_interrupts);
	MU_RUN(reads_of_the_receive_buffer_are_kept_after_the_waits);
	return mu_report();
}
