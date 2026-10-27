/*
 * How firmware/bluetooth_rxtx/le_phy.c splits a long low-energy packet across
 * USB records, checked against the format the host reads.
 *
 * A packet of more than 50 bytes goes out as one LE_PACKET_START record and
 * ordered LE_PACKET_CONT records, each carrying the whole packet's length and
 * its own position in the two spare bytes of the record. The host's
 * ubertooth_le_frag.c reads them back. The two halves have to pack those
 * bytes the same way, and the sender has to ask the ring for room for every
 * record before it adds the first one, or a nearly full ring takes half a
 * packet and the host throws the whole thing away.
 *
 * usb_enqueue_le() is hardware code -- it runs inside the capture loop and
 * writes into the ring the USB interrupt drains -- so it cannot run here. This
 * test scans the source instead, the way test_le_phy_order.c and
 * test_mode_dispatch.c do, and checks the expressions the host-side test
 * (host/ubertooth-tools/tests/test_le_frag.c) builds its records from are the
 * ones the firmware writes.
 *
 * The round trip over real records is covered on hardware, by a capture
 * comparison against a build without this change.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "minunit.h"
#include "source_scan.h"
#include "ubertooth_le_frag.h"

#ifndef FW_RXTX_DIR
#error "FW_RXTX_DIR must name firmware/bluetooth_rxtx"
#endif

/* The whole file, comments and string literals removed, spaces collapsed. */
static char source[400000];

static void load_le_phy(void)
{
	char path[512];
	char line[4096];
	char clean[4096];
	FILE *f;
	int in_block = 0;
	size_t used = 0;

	snprintf(path, sizeof(path), "%s/le_phy.c", FW_RXTX_DIR);
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
		used += (size_t) snprintf(source + used, sizeof(source) - used,
		                          "%s ", clean);
		if (used >= sizeof(source) - 1)
			break;
	}
	fclose(f);
}

/* Both halves have to agree on where a fragment's length and position live,
 * and the host's constants are the ones the firmware writes with. */
MU_TEST(the_metadata_bytes_are_packed_the_way_the_host_reads_them)
{
	mu_assert(strstr(source, "f->reserved[0] = total & 0xff;") != NULL,
	          "the low byte of the length is not written to reserved[0]");
	mu_assert(strstr(source,
	                 "f->reserved[1] = ((total >> 8) & LE_FRAG_TOTAL_HIGH_MASK) |"
	                 " ((offset / DMA_SIZE) << LE_FRAG_INDEX_SHIFT);") != NULL,
	          "reserved[1] is not the length's 9th bit and the fragment index");
	/* The host reads the same two constants, so a change to either has to
	 * be made in the shared header and not here. */
	mu_assert_int_eq(0x01, LE_FRAG_TOTAL_HIGH_MASK);
	mu_assert_int_eq(1, LE_FRAG_INDEX_SHIFT);
	/* Six records is the most a packet needs, so the index fits the seven
	 * bits left in reserved[1]. */
	mu_assert((LE_MAX_USB_PACKET_SIZE + DMA_SIZE - 1) / DMA_SIZE
	          <= (0xff >> LE_FRAG_INDEX_SHIFT),
	          "a fragment index could not fit reserved[1]");
	/* And the length needs nine bits, which is what the mask leaves. */
	mu_assert(LE_MAX_USB_PACKET_SIZE <= 0x1ff,
	          "a packet length could not fit reserved[]");
}

/* A chain the host cannot complete is worse than a dropped packet, so the
 * room for every record is checked before the first one is added. */
MU_TEST(the_ring_is_asked_for_room_before_any_record_is_added)
{
	const char *space = strstr(source, "usb_queue_space()");
	const char *loop = strstr(source, "for (offset = 0; offset < total;");

	mu_assert(space != NULL, "the fragmenting path does not ask for room");
	mu_assert(loop != NULL, "the record loop is not there to be found");
	mu_assert(space < loop,
	          "room is asked for after records have started going in");
	mu_assert(strstr(source, "if (usb_queue_space() < fragments) return 0;")
	          != NULL,
	          "a ring without room for every record does not refuse the packet");
	mu_assert(strstr(source, "fragments = (total + DMA_SIZE - 1) / DMA_SIZE;")
	          != NULL,
	          "the record count is not the packet rounded up to whole records");
}

/* Records are reused, so the bytes a short last record does not fill have to
 * be cleared, or they carry the previous packet's. */
MU_TEST(the_last_record_is_cleared_past_the_packet)
{
	mu_assert(strstr(source,
	                 "if (chunk < DMA_SIZE) memset(f->data + chunk, 0,"
	                 " DMA_SIZE - chunk);") != NULL,
	          "a record is not cleared past the bytes it carries");
}

/* Where each record's bytes come from. The captured packet is the access
 * address followed by packet->data, so a record starting at `offset` takes
 * whatever of the address is still to come and then continues 4 bytes behind
 * itself in packet->data. These are the two copies the host-side test
 * synthesizes its records with, so a change to either expression has to be
 * made in both places. */
MU_TEST(each_record_takes_its_own_part_of_the_packet)
{
	mu_assert(strstr(source,
	                 "memcpy(f->data, (const uint8_t *) &packet->access_address"
	                 " + offset, from_address);") != NULL,
	          "the first record does not take the access address from offset");
	mu_assert(strstr(source,
	                 "memcpy(f->data + from_address, packet->data + offset +"
	                 " from_address - 4, chunk - from_address);") != NULL,
	          "a record does not continue 4 bytes behind itself in the capture");
	mu_assert(strstr(source, "if (offset < 4) { from_address = 4 - offset;")
	          != NULL,
	          "only the first record straddles the address and the capture");
	mu_assert(strstr(source,
	                 "if (from_address > chunk) from_address = chunk;") != NULL,
	          "a record shorter than the access address is not clamped");
}

/* The two record types the host distinguishes, used the way it expects: the
 * first record only at offset zero. */
MU_TEST(the_first_record_is_the_only_start_record)
{
	mu_assert(strstr(source,
	                 "usb_fill_le_metadata(f, packet, (offset == 0) ?"
	                 " LE_PACKET_START : LE_PACKET_CONT);") != NULL,
	          "the record type does not follow the offset");
	/* A packet that fits one record keeps the plain type, and says nothing
	 * about fragments. */
	mu_assert(strstr(source, "if (total <= DMA_SIZE) {") != NULL,
	          "a short packet no longer takes the single-record path");
	mu_assert(strstr(source, "usb_fill_le_metadata(f, packet, LE_PACKET);")
	          != NULL,
	          "a short packet is not sent as a plain record");
}

/* Nothing longer than the host can hold is sent at all. */
MU_TEST(an_overlong_packet_is_refused)
{
	mu_assert(strstr(source, "if (total > LE_MAX_USB_PACKET_SIZE) return 0;")
	          != NULL,
	          "a packet longer than the host's buffer is not refused");
	mu_assert(strstr(source, "unsigned total = 4 + packet->size;") != NULL,
	          "the packet length is not the access address plus the capture");
}

int main(void)
{
	printf("LE fragment format tests\n");
	load_le_phy();
	MU_RUN(the_metadata_bytes_are_packed_the_way_the_host_reads_them);
	MU_RUN(the_ring_is_asked_for_room_before_any_record_is_added);
	MU_RUN(the_last_record_is_cleared_past_the_packet);
	MU_RUN(each_record_takes_its_own_part_of_the_packet);
	MU_RUN(the_first_record_is_the_only_start_record);
	MU_RUN(an_overlong_packet_is_refused);
	return mu_report();
}
