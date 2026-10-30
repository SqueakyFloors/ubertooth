/*
 * What a capture record says about the noise power of the packet it holds.
 *
 * Every record of both capture formats carries a signal figure, a noise
 * figure, and a flag for each saying whether that figure is there at all. A
 * reader that finds the noise flag clear does not take the byte for a reading:
 * Wireshark labels it `Invalid Noise Power` on a Basic Rate record and
 * `Unused signed byte` on a low-energy one. A reader that finds the flag set
 * shows the figure, and computes a signal-to-noise ratio from the pair.
 *
 * The background figure is the board's estimate, and not every mode has one. A
 * record says whether it does, so the flag follows that: set where the board
 * estimated the background, clear where it had nothing to estimate it from.
 * Earlier releases had no figure for the background at all, and the records
 * went out claiming a noise power of -128 dBm on the low-energy path and the
 * packet's own average on the Basic Rate one, both with the flag set, which a
 * reader could not tell from a figure for the channel.
 *
 * libbtbb assembles the flags itself, from the figures it is handed: it sets
 * the noise-power-valid flag when the noise figure is below the signal figure.
 * That is why these cases read the flags back out of written files rather than
 * checking what the callbacks passed. Both formats are read for both paths,
 * because libbtbb assembles each one's flags separately.
 *
 * The real ubertooth_callback.c runs, with real libbtbb decoding and real
 * capture files. The fifo calls are the test's own, so no board and no 72 MB
 * ring buffer is involved.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pthread.h>

#include <btbb.h>
#include <libusb.h>

#include "../../../firmware/tests/minunit.h"
#include "ubertooth.h"
#include "ubertooth_callback.h"
#include "ubertooth_fifo.h"

/* ---- the record fields these cases read ---------------------------------
 *
 * Both link types start their record with the channel, the signal figure and
 * the noise figure, so those three offsets are shared; the flags sit further
 * along in the Basic Rate record, which carries more fields ahead of them.
 * The flag values are libbtbb's own.
 */
#define SIGNAL_POWER_AT 1
#define NOISE_POWER_AT  2
#define LE_FLAGS_AT     8
#define BREDR_FLAGS_AT  20

#define LE_SIGPOWER_VALID      0x0002
#define LE_NOISEPOWER_VALID    0x0004
#define BREDR_SIGPOWER_VALID   0x0002
#define BREDR_NOISEPOWER_VALID 0x0004

/* Where a pcap file's first record body begins: a 24-byte file header and a
 * 16-byte record header, both of which libbtbb writes as structs. */
#define PCAP_FIRST_RECORD_AT 40

/* A pcapng enhanced packet block and where its captured bytes begin: the
 * block type and length, the interface, two words of time, the captured
 * length and the length the packet had on air. */
#define PCAPNG_ENHANCED_PACKET 6
#define PCAPNG_BLOCK_PAYLOAD_AT 28

/* ---- fifo fake ----------------------------------------------------------
 * A ring of a few slots: a case pushes the records it wants and then calls
 * the callback once per record. The real ubertooth_fifo.c is not linked
 * because its ring is 72 MB.
 */
#define FAKE_SLOTS 4

static struct {
	usb_pkt_rx slots[FAKE_SLOTS];
	size_t write_ptr;
	size_t read_ptr;
} ring;

fifo_t* fifo_init(void)
{
	memset(&ring, 0, sizeof(ring));
	return (fifo_t*) malloc(1);
}

usb_pkt_rx* fifo_get_write_element(fifo_t* fifo)
{
	(void) fifo;
	return &ring.slots[ring.write_ptr];
}

void fifo_inc_write_ptr(fifo_t* fifo, uint64_t arrival_ns)
{
	(void) fifo;
	(void) arrival_ns;
	ring.write_ptr = (ring.write_ptr + 1) % FAKE_SLOTS;
}

void fifo_push(fifo_t* fifo, const usb_pkt_rx* packet, uint64_t arrival_ns)
{
	ring.slots[ring.write_ptr] = *packet;
	fifo_inc_write_ptr(fifo, arrival_ns);
}

uint8_t fifo_empty(fifo_t* fifo)
{
	(void) fifo;
	return ring.read_ptr == ring.write_ptr;
}

uint64_t fifo_popped_arrival_ns(const fifo_t* fifo) { (void) fifo; return 0; }

usb_pkt_rx fifo_pop(fifo_t* fifo)
{
	size_t selected = ring.read_ptr;

	(void) fifo;
	ring.read_ptr = (ring.read_ptr + 1) % FAKE_SLOTS;
	return ring.slots[selected];
}

/* ---- the records the cases capture -------------------------------------- */

/* The RSSI readings every record below carries. A real board sends readings
 * like these: the average sits between the minimum and the maximum, and well
 * above the floor, which is exactly what made it look like a noise
 * measurement worth recording. */
#define RECORD_RSSI_MAX (-40)
#define RECORD_RSSI_AVG (-43)
#define RECORD_RSSI_MIN (-50)

/* The background the board reports, and whether it says it has one. The
 * cases set these before building a record. */
static uint8_t record_noise_raw;
static uint8_t record_has_noise;

/* A background well below the packet, as a quiet channel reads. */
#define RECORD_RSSI_NOISE ((uint8_t)(int8_t) -45)

/* A low-energy advertising packet the decoder takes: the advertising access
 * address, little endian, then an ADV_IND header whose length matches the six
 * bytes of payload after it. */
static void advertising_record(usb_pkt_rx* rx)
{
	memset(rx, 0, sizeof(*rx));
	rx->pkt_type = LE_PACKET;
	rx->channel = 0;
	rx->clk100ns = htole32(1000000u);
	rx->rssi_max = RECORD_RSSI_MAX;
	rx->rssi_min = RECORD_RSSI_MIN;
	rx->rssi_avg = RECORD_RSSI_AVG;
	rx->rssi_count = 1;
	rx->rssi_noise = record_noise_raw;
	rx->status |= SIGNAL_VALID;
	if (record_has_noise)
		rx->status |= NOISE_ESTIMATE;
	rx->data[0] = 0xd6;
	rx->data[1] = 0xbe;
	rx->data[2] = 0x89;
	rx->data[3] = 0x8e;
	rx->data[4] = 0x00;
	rx->data[5] = 6;
}

/* The LAP the Basic Rate record below carries, the 64-bit sequence its sync
 * word is scrambled with, the (64,30) expurgated BCH generator, and the
 * Barker sequence the LAP's top bit chooses. */
#define BASIC_RATE_LAP     0x5a3c96u
#define SYNC_WORD_SCRAMBLE 0x83848d96bbcc54fcull
#define SYNC_WORD_BCH      0260534236651ull
#define SYNC_WORD_BARKER_0 0x2cu
#define SYNC_WORD_BARKER_1 0x13u

/* The 64-bit sync word of a LAP, built from the Bluetooth Core specification
 * (Vol 2, Part B 6.3.3): the LAP and its Barker sequence, scrambled, then the
 * BCH parity of that. Bit i is the i-th symbol on air. Building it here is
 * what lets the Basic Rate case run with no board and no saved capture. */
static uint64_t sync_word(uint32_t lap)
{
	uint64_t information, scrambled, parity;
	int bit;

	lap &= 0xffffffu;
	information = lap | ((uint64_t) ((lap >> 23) ? SYNC_WORD_BARKER_1 :
	                                               SYNC_WORD_BARKER_0) << 24);
	scrambled = information ^ (SYNC_WORD_SCRAMBLE >> 34);
	parity = scrambled << 34;
	for (bit = 63; bit >= 34; bit--) {
		if ((parity >> bit) & 1)
			parity ^= SYNC_WORD_BCH << (bit - 34);
	}
	return ((scrambled << 34) | parity) ^ SYNC_WORD_SCRAMBLE;
}

/* A Basic Rate packet the decoder finds: silence, then the preamble and sync
 * word of one LAP, then silence again, packed most significant bit first the
 * way the board sends symbols. */
static void basic_rate_record(usb_pkt_rx* rx)
{
	/* Where the access code starts, far enough in that the decoder has
	 * room to search before it. */
	enum { PREAMBLE_AT = 16 };
	uint8_t symbols[DMA_SIZE * 8] = { 0 };
	uint64_t word = sync_word(BASIC_RATE_LAP);
	int i, j;

	memset(rx, 0, sizeof(*rx));
	rx->pkt_type = BR_PACKET;
	rx->channel = 0;
	rx->clk100ns = htole32(1000000u);
	rx->rssi_max = RECORD_RSSI_MAX;
	rx->rssi_min = RECORD_RSSI_MIN;
	rx->rssi_avg = RECORD_RSSI_AVG;
	rx->rssi_count = 1;
	rx->rssi_noise = record_noise_raw;
	rx->status |= SIGNAL_VALID;
	if (record_has_noise)
		rx->status |= NOISE_ESTIMATE;

	/* The preamble is the two symbols that lead into the sync word's first
	 * bit, so which pair it is follows that bit. */
	symbols[PREAMBLE_AT + 0] = (word & 1) ? 1 : 0;
	symbols[PREAMBLE_AT + 1] = (word & 1) ? 0 : 1;
	symbols[PREAMBLE_AT + 2] = (word & 1) ? 1 : 0;
	symbols[PREAMBLE_AT + 3] = (word & 1) ? 0 : 1;
	for (i = 0; i < 64; i++)
		symbols[PREAMBLE_AT + 4 + i] = (word >> i) & 1;

	for (i = 0; i < DMA_SIZE; i++) {
		for (j = 0; j < 8; j++)
			rx->data[i] |= symbols[i * 8 + j] << (7 - j);
	}
}

/* ---- reading the files back --------------------------------------------- */

/* What one record says about its own signal and noise, and whether there was a
 * record to read at all: a capture that wrote none leaves `read` clear, so a
 * run that captured nothing cannot pass by default. */
struct record_powers {
	int read;
	int8_t signal;
	int8_t noise;
	uint16_t flags;
};

/* The two powers are single bytes, and libbtbb writes the flags beside them
 * little endian, which is how they are read back here. */
static void read_powers(const uint8_t* body, int flags_at,
                        struct record_powers* out)
{
	out->signal = (int8_t) body[SIGNAL_POWER_AT];
	out->noise = (int8_t) body[NOISE_POWER_AT];
	out->flags = (uint16_t) (body[flags_at] |
	                         ((uint16_t) body[flags_at + 1] << 8));
}

/* The whole of a file, read into a buffer the caller frees. 0 and a NULL
 * buffer where the file cannot be read. */
static size_t slurp(const char* path, uint8_t** out)
{
	FILE* fp = fopen(path, "rb");
	long length;
	uint8_t* buffer;

	*out = NULL;
	if (fp == NULL)
		return 0;
	if (fseek(fp, 0, SEEK_END) != 0 || (length = ftell(fp)) < 0) {
		fclose(fp);
		return 0;
	}
	rewind(fp);
	buffer = (uint8_t*) malloc((size_t) length + 1);
	if (buffer == NULL) {
		fclose(fp);
		return 0;
	}
	if (fread(buffer, 1, (size_t) length, fp) != (size_t) length) {
		free(buffer);
		fclose(fp);
		return 0;
	}
	fclose(fp);
	*out = buffer;
	return (size_t) length;
}

/* What the first record of a pcap says. The case decides what to make of a
 * file that holds none, once it has tidied its files away. */
static void first_pcap_record(const char* path, int flags_at,
                              struct record_powers* out)
{
	uint8_t* data;
	size_t length = slurp(path, &data);

	out->read = length >= PCAP_FIRST_RECORD_AT + (size_t) flags_at + 2;
	if (out->read)
		read_powers(data + PCAP_FIRST_RECORD_AT, flags_at, out);
	free(data);
}

/* The same, from the first enhanced packet block of a pcapng. */
static void first_pcapng_record(const char* path, int flags_at,
                                struct record_powers* out)
{
	uint8_t* data;
	size_t length = slurp(path, &data);
	size_t offset = 0;
	int found = 0;

	while (offset + 12 <= length) {
		uint32_t block, block_length;

		/* libbtbb writes a block's type and length as struct fields,
		 * so they are in this machine's own byte order. */
		memcpy(&block, data + offset, sizeof(block));
		memcpy(&block_length, data + offset + 4, sizeof(block_length));
		if (block_length < 12 || offset + block_length > length)
			break;
		if (block == PCAPNG_ENHANCED_PACKET &&
		    block_length >= PCAPNG_BLOCK_PAYLOAD_AT +
		                    (uint32_t) flags_at + 2) {
			read_powers(data + offset + PCAPNG_BLOCK_PAYLOAD_AT,
			            flags_at, out);
			found = 1;
			break;
		}
		offset += block_length;
	}
	free(data);
	out->read = found;
}

/* ---- running a capture -------------------------------------------------- */

/* The files one capture writes, in a directory of its own so that a file left
 * behind by an earlier run cannot be read back here by mistake. */
struct capture_files {
	char dir[sizeof("/tmp/ubertooth-noise-XXXXXX")];
	char pcap[sizeof("/tmp/ubertooth-noise-XXXXXX") + 16];
	char pcapng[sizeof("/tmp/ubertooth-noise-XXXXXX") + 18];
	int saved_stdout;
};

/* The packet lines the callbacks print go to a file, so a run of the suite
 * shows the cases rather than the packets. */
static void take_stdout(struct capture_files* files)
{
	char path[sizeof(files->dir) + 16];
	int output;

	snprintf(path, sizeof(path), "%s/output", files->dir);
	fflush(stdout);
	files->saved_stdout = dup(fileno(stdout));
	mu_assert_fatal(files->saved_stdout >= 0, "cannot set the output aside");
	output = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	mu_assert_fatal(output >= 0, "cannot open a file for the output");
	dup2(output, fileno(stdout));
	close(output);
}

static void give_stdout_back(struct capture_files* files)
{
	char path[sizeof(files->dir) + 16];

	snprintf(path, sizeof(path), "%s/output", files->dir);
	fflush(stdout);
	dup2(files->saved_stdout, fileno(stdout));
	close(files->saved_stdout);
	unlink(path);
}

static void open_capture_files(struct capture_files* files)
{
	snprintf(files->dir, sizeof(files->dir), "/tmp/ubertooth-noise-XXXXXX");
	mu_assert_fatal(mkdtemp(files->dir) != NULL,
	                "cannot make a directory to write in");
	snprintf(files->pcap, sizeof(files->pcap), "%s/capture.pcap",
	         files->dir);
	snprintf(files->pcapng, sizeof(files->pcapng), "%s/capture.pcapng",
	         files->dir);
}

static void remove_capture_files(struct capture_files* files)
{
	unlink(files->pcap);
	unlink(files->pcapng);
	rmdir(files->dir);
}

/* One low-energy packet captured into both formats, and what each record says
 * about its signal and noise. */
static void capture_le_packet(struct record_powers* pcap_record,
                              struct record_powers* pcapng_record)
{
	struct capture_files files;
	usb_pkt_rx rx;
	ubertooth_t ut;

	memset(&ut, 0, sizeof(ut));
	ut.clk100ns_period = CLK100NS_TIMER1_PERIOD;
	ut.fifo = fifo_init();
	mu_assert_fatal(ut.fifo != NULL, "cannot allocate the ring buffer");
	open_capture_files(&files);
	mu_assert_fatal(lell_pcap_create_file(files.pcap, &ut.h_pcap_le) == 0,
	                "cannot open a pcap to write");
	mu_assert_fatal(lell_pcapng_create_file(files.pcapng, "noise",
	                                        &ut.h_pcapng_le) == 0,
	                "cannot open a pcapng to write");

	advertising_record(&rx);
	fifo_push(ut.fifo, &rx, 0);
	take_stdout(&files);
	cb_btle(&ut, NULL);
	give_stdout_back(&files);

	lell_pcap_close(ut.h_pcap_le);
	lell_pcapng_close(ut.h_pcapng_le);
	first_pcap_record(files.pcap, LE_FLAGS_AT, pcap_record);
	first_pcapng_record(files.pcapng, LE_FLAGS_AT, pcapng_record);
	remove_capture_files(&files);
	free(ut.fifo);
}

/* The same for one Basic Rate packet. The capture is a replay, which is what
 * infile says: a live one would trim the board's clock, and there is no board
 * here to trim. */
static void capture_bredr_packet(struct record_powers* pcap_record,
                                 struct record_powers* pcapng_record)
{
	struct capture_files files;
	usb_pkt_rx rx;
	ubertooth_t ut;
	FILE* replay = tmpfile();

	mu_assert_fatal(replay != NULL, "cannot open a file to replay from");
	memset(&ut, 0, sizeof(ut));
	ut.clk100ns_period = CLK100NS_PERIOD;
	ut.fifo = fifo_init();
	mu_assert_fatal(ut.fifo != NULL, "cannot allocate the ring buffer");
	open_capture_files(&files);
	mu_assert_fatal(btbb_pcap_create_file(files.pcap,
	                                      &ut.h_pcap_bredr) == 0,
	                "cannot open a pcap to write");
	mu_assert_fatal(btbb_pcapng_create_file(files.pcapng, "noise",
	                                        &ut.h_pcapng_bredr) == 0,
	                "cannot open a pcapng to write");

	basic_rate_record(&rx);
	fifo_push(ut.fifo, &rx, 0);
	infile = replay;
	take_stdout(&files);
	cb_rx(&ut, NULL);
	give_stdout_back(&files);
	infile = NULL;
	fclose(replay);

	btbb_pcap_close(ut.h_pcap_bredr);
	btbb_pcapng_close(ut.h_pcapng_bredr);
	first_pcap_record(files.pcap, BREDR_FLAGS_AT, pcap_record);
	first_pcapng_record(files.pcapng, BREDR_FLAGS_AT, pcapng_record);
	remove_capture_files(&files);
	free(ut.fifo);
}

/* The signal figure both paths record: the packet's own power as the board
 * averaged it, converted to dBm. */
#define RECORDED_SIGNAL_DBM (-94)

/* The background the board estimated, in dBm. */
#define RECORDED_NOISE_DBM (-102)

static void check_noise_is_absent(const char* name,
                                  const struct record_powers* record,
                                  uint16_t signal_valid,
                                  uint16_t noise_valid)
{
	printf("%s: signal=%d noise=%d flags=0x%04x\n", name, record->signal,
	       record->noise, record->flags);

	/* The signal figure is a measurement and says so: the strongest RSSI
	 * the board read while the packet was arriving. */
	mu_assert(record->flags & signal_valid,
	          "the signal power is recorded as not measured");
	mu_assert_int_eq(RECORDED_SIGNAL_DBM, record->signal);

	/* There is no background figure, and the record now says so. A reader
	 * that finds this flag clear does not take the byte for a reading, and
	 * computes no signal-to-noise ratio from it. */
	mu_assert(!(record->flags & noise_valid),
	          "the noise power is recorded as present");

	/* Why it is clear: libbtbb sets the flag when the noise figure it is
	 * handed is below that record's signal figure, so a missing figure is
	 * one no signal can reach. */
	mu_assert(record->noise >= record->signal,
	          "the recorded noise power is below the signal power");

	/* Whatever stands in the field, it is neither of the two figures that
	 * used to be claimed there as readings of the channel. */
	mu_assert(record->noise != INT8_MIN,
	          "the record still carries -128 dBm as its noise power");
}

static void check_noise_is_recorded(const char* name,
                                    const struct record_powers* record,
                                    uint16_t signal_valid,
                                    uint16_t noise_valid)
{
	printf("%s: signal=%d noise=%d flags=0x%04x\n", name, record->signal,
	       record->noise, record->flags);

	mu_assert(record->flags & signal_valid,
	          "the signal power is recorded as not measured");
	mu_assert_int_eq(RECORDED_SIGNAL_DBM, record->signal);

	/* The board estimated the background, so the record says so and carries
	 * the figure, and a reader computes a signal-to-noise ratio from the
	 * pair. */
	mu_assert(record->flags & noise_valid,
	          "the noise power is recorded as absent");
	mu_assert_int_eq(RECORDED_NOISE_DBM, record->noise);
}

MU_TEST(a_low_energy_record_claims_no_noise_estimate)
{
	struct record_powers pcap_record, pcapng_record;

	record_noise_raw = RECORD_RSSI_NOISE;
	record_has_noise = 0;
	memset(&pcap_record, 0, sizeof(pcap_record));
	memset(&pcapng_record, 0, sizeof(pcapng_record));
	capture_le_packet(&pcap_record, &pcapng_record);
	mu_assert_fatal(pcap_record.read && pcapng_record.read,
	                "the capture holds no low-energy record to read");
	check_noise_is_absent("low-energy pcap", &pcap_record,
	                      LE_SIGPOWER_VALID, LE_NOISEPOWER_VALID);
	check_noise_is_absent("low-energy pcapng", &pcapng_record,
	                      LE_SIGPOWER_VALID, LE_NOISEPOWER_VALID);
}

MU_TEST(a_basic_rate_record_claims_no_noise_estimate)
{
	struct record_powers pcap_record, pcapng_record;

	record_noise_raw = RECORD_RSSI_NOISE;
	record_has_noise = 0;
	memset(&pcap_record, 0, sizeof(pcap_record));
	memset(&pcapng_record, 0, sizeof(pcapng_record));
	capture_bredr_packet(&pcap_record, &pcapng_record);
	mu_assert_fatal(pcap_record.read && pcapng_record.read,
	                "the capture holds no basic-rate record to read");
	check_noise_is_absent("basic-rate pcap", &pcap_record,
	                      BREDR_SIGPOWER_VALID, BREDR_NOISEPOWER_VALID);
	check_noise_is_absent("basic-rate pcapng", &pcapng_record,
	                      BREDR_SIGPOWER_VALID, BREDR_NOISEPOWER_VALID);
}

MU_TEST(a_low_energy_record_carries_its_noise_estimate)
{
	struct record_powers pcap_record, pcapng_record;

	record_noise_raw = RECORD_RSSI_NOISE;
	record_has_noise = 1;
	memset(&pcap_record, 0, sizeof(pcap_record));
	memset(&pcapng_record, 0, sizeof(pcapng_record));
	capture_le_packet(&pcap_record, &pcapng_record);
	mu_assert_fatal(pcap_record.read && pcapng_record.read,
	                "the capture holds no low-energy record to read");
	check_noise_is_recorded("low-energy pcap", &pcap_record,
	                        LE_SIGPOWER_VALID, LE_NOISEPOWER_VALID);
	check_noise_is_recorded("low-energy pcapng", &pcapng_record,
	                        LE_SIGPOWER_VALID, LE_NOISEPOWER_VALID);
}

MU_TEST(a_basic_rate_record_carries_its_noise_estimate)
{
	struct record_powers pcap_record, pcapng_record;

	record_noise_raw = RECORD_RSSI_NOISE;
	record_has_noise = 1;
	memset(&pcap_record, 0, sizeof(pcap_record));
	memset(&pcapng_record, 0, sizeof(pcapng_record));
	capture_bredr_packet(&pcap_record, &pcapng_record);
	mu_assert_fatal(pcap_record.read && pcapng_record.read,
	                "the capture holds no Basic Rate record to read");
	check_noise_is_recorded("Basic Rate pcap", &pcap_record,
	                        BREDR_SIGPOWER_VALID, BREDR_NOISEPOWER_VALID);
	check_noise_is_recorded("Basic Rate pcapng", &pcapng_record,
	                        BREDR_SIGPOWER_VALID, BREDR_NOISEPOWER_VALID);
}

int main(void)
{
	/* Nothing here should look for a board; make sure of it. */
	setenv("UBERTOOTH_TEST_NO_DEVICES", "1", 1);

	MU_RUN(a_low_energy_record_claims_no_noise_estimate);
	MU_RUN(a_basic_rate_record_claims_no_noise_estimate);
	MU_RUN(a_low_energy_record_carries_its_noise_estimate);
	MU_RUN(a_basic_rate_record_carries_its_noise_estimate);
	return mu_report();
}
