#ifndef UBERTOOTH_DFU_RECOVERY_H
#define UBERTOOTH_DFU_RECOVERY_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "dfu_transition.h"

/*
 * Waiting for the bootloader to reach DFU idle. Every wait is bounded: a
 * failed state query ends it at once, and the states that only resolve with
 * time are polled a fixed number of times, so a bootloader that stops
 * answering ends the command instead of spinning.
 */
enum dfu_idle_result {
	DFU_IDLE_OK = 0,
	DFU_IDLE_INVALID_ARGUMENT = -1,
	DFU_IDLE_QUERY_FAILED = -2,      /* the state could not be read */
	DFU_IDLE_ABORT_FAILED = -3,
	DFU_IDLE_CLEAR_FAILED = -4,
	DFU_IDLE_NOT_REACHED = -5,       /* still not idle after every poll */
	DFU_IDLE_UNEXPECTED_STATE = -6,  /* a state this host cannot act on */
};

/* The wait is bounded by max_polls iterations. Only the states that pass with
 * time are slept on, so poll_interval_ms sets the ceiling rather than the
 * duration: at most ten seconds of waiting, far longer than the bootloader's
 * own 20 ms poll timeout, and no wait at all for the states that are acted on. */
#define DFU_IDLE_DEFAULT_MAX_POLLS 40
#define DFU_IDLE_DEFAULT_POLL_INTERVAL_MS 250

struct dfu_idle_config {
	unsigned max_polls;
	unsigned poll_interval_ms;
};

struct dfu_idle_report {
	enum dfu_idle_result result;
	unsigned polls;
	int operation_error;  /* the transport error, when there was one */
	int last_state;       /* the last state read, or -1 if none was */
};

struct dfu_idle_ops {
	/* Return the DFU state (>= 0) or a negative transport error. */
	int (*get_state)(void *context, void *handle);
	int (*abort)(void *context, void *handle);
	int (*clear_status)(void *context, void *handle);
	void (*sleep_ms)(void *context, unsigned milliseconds);
};

/* Drive the bootloader to STATE_DFU_IDLE, or say why that did not happen.
 * Returns DFU_IDLE_OK or a negative enum dfu_idle_result, also in
 * report->result. */
int dfu_reach_idle(const struct dfu_idle_ops *ops,
                   void *context,
                   void *handle,
                   const struct dfu_idle_config *config,
                   struct dfu_idle_report *report);

/* One phrase naming the outcome, for the message a caller prints. */
const char *dfu_idle_result_string(enum dfu_idle_result result);

enum dfu_transfer_result {
	DFU_TRANSFER_OK = 0,
	DFU_TRANSFER_INVALID_ARGUMENT = -1,
	DFU_TRANSFER_REOPEN_FAILED = -2,
	DFU_TRANSFER_REOPEN_TIMEOUT = -3,
	DFU_TRANSFER_CLAIM_FAILED = -4,
	DFU_TRANSFER_RETRY_EXHAUSTED = -5,
};

struct dfu_transfer_config {
	unsigned max_attempts;
	unsigned reopen_polls;
	unsigned poll_interval_ms;
};

struct dfu_transfer_report {
	enum dfu_transfer_result result;
	unsigned attempts;
	unsigned reopen_polls;
	int transfer_error;
	int operation_error;
};

struct dfu_transfer_ops {
	/* One complete transfer in either direction, from its first block. */
	int (*transfer)(void *context, void *handle);
	void (*close_dfu)(void *context, void *handle, int claimed);
	/* Return 1 when found, 0 when absent, and a negative value on error. */
	int (*reopen_matching_dfu)(void *context,
	                           const struct dfu_usb_identity *identity,
	                           void **handle);
	int (*claim_dfu)(void *context, void *handle);
	void (*sleep_ms)(void *context, unsigned milliseconds);
};

int dfu_transfer_run(const struct dfu_transfer_ops *ops,
                     void *context,
                     const struct dfu_usb_identity *identity,
                     const struct dfu_transfer_config *config,
                     void **handle,
                     struct dfu_transfer_report *report);

enum dfu_verify_result {
	DFU_VERIFY_OK = 0,
	DFU_VERIFY_INVALID_ARGUMENT = -1,
	DFU_VERIFY_APP_TIMEOUT = -2,
	DFU_VERIFY_IDENTITY_MISMATCH = -3,
	DFU_VERIFY_REVISION_FAILED = -4,
	DFU_VERIFY_REVISION_MISMATCH = -5,
};

struct dfu_verify_config {
	unsigned polls;
	unsigned poll_interval_ms;
};

struct dfu_verify_report {
	enum dfu_verify_result result;
	unsigned polls;
	int operation_error;
	char actual_revision[64];
};

struct dfu_verify_ops {
	int (*open_application)(void *context, const char *selector,
	                        void **handle,
	                        struct dfu_usb_identity *identity);
	int (*get_revision)(void *context, void *handle,
	                    char *revision, size_t revision_size);
	void (*close_application)(void *context, void *handle);
	void (*sleep_ms)(void *context, unsigned milliseconds);
};

int dfu_verify_application(const struct dfu_verify_ops *ops,
                           void *context,
                           const char *selector,
                           const struct dfu_usb_identity *expected_identity,
                           const char *expected_revision,
                           const struct dfu_verify_config *config,
                           struct dfu_verify_report *report);

/*
 * Is this a file that can be read into memory whole?
 *
 * ubertooth-dfu reads a .dfu, and a .bin it is asked to sign, in one
 * allocation sized from the file itself. So the file is measured first: a
 * huge or sparse one would otherwise be allocated before anything looked at
 * it, and a directory, a FIFO or a device node has no length that says what a
 * read would return at all. One guard for every whole-file read in the tool.
 */
enum dfu_file_size_result {
	DFU_FILE_SIZE_OK = 0,
	DFU_FILE_SIZE_INVALID_ARGUMENT = -1,
	DFU_FILE_SIZE_UNKNOWN = -2,      /* the size could not be measured */
	DFU_FILE_SIZE_NOT_REGULAR = -3,  /* a directory, FIFO, device, socket */
	DFU_FILE_SIZE_TOO_LARGE = -4,    /* past the maximum the caller gave */
};

/* Measure file against maximum bytes. The size is left in *size when it could
 * be measured and 0 otherwise, so a caller can name it in its message; size
 * may be NULL. Reads nothing and leaves the file position alone. */
enum dfu_file_size_result dfu_image_size_check(FILE *file,
                                               unsigned long maximum,
                                               long long *size);

/* The largest image this host will read: the flash above the bootloader on
 * the largest part the table holds, plus the DFU suffix the file carries.
 * Taken from the part table and the bootloader's own address limit rather
 * than stated, so a part added to the table moves it. An unsigned .bin is
 * capped at this less the suffix that signing adds, so what -s writes is a
 * file -d still accepts. */
unsigned long dfu_max_image_size(void);

int dfu_extract_firmware_revision(FILE *image, char *revision,
                                  size_t revision_size);

/* The result of checking a firmware image before it is flashed: OK, or the
 * reason the image is refused. The checks run in the order listed, so the
 * first problem found is the one reported, except that what the file is and
 * how big it is (DFU_IMAGE_NOT_REGULAR, DFU_IMAGE_TOO_LARGE, the last two
 * here) is settled first of all: the rest needs the file in memory. */
enum dfu_image_result {
	DFU_IMAGE_OK = 0,
	DFU_IMAGE_INVALID_ARGUMENT = -1,
	DFU_IMAGE_TOO_SHORT = -2,       /* too short to hold a suffix at all */
	DFU_IMAGE_SUFFIX_LENGTH = -3,   /* bLength is not 16: no DFU suffix here */
	DFU_IMAGE_DFU_VERSION = -4,
	DFU_IMAGE_SIGNATURE = -5,
	DFU_IMAGE_NO_FIRMWARE = -6,     /* a suffix and nothing to write */
	DFU_IMAGE_CRC = -7,
	DFU_IMAGE_VENDOR = -8,
	DFU_IMAGE_PRODUCT = -9,
	DFU_IMAGE_UNREADABLE = -10,     /* the file would not read */
	DFU_IMAGE_OUT_OF_MEMORY = -11,
	DFU_IMAGE_TOO_LARGE = -12,      /* past dfu_max_image_size() */
	DFU_IMAGE_NOT_REGULAR = -13,    /* a directory, FIFO, device or socket */
};

struct dfu_image_report {
	enum dfu_image_result result;
	long long image_size;
	uint8_t suffix_length;
	uint16_t bcd_dfu;
	uint16_t id_vendor;
	uint16_t id_product;
	uint16_t expected_vendor;
	uint16_t expected_product;
	uint32_t crc_calculated;
	uint32_t crc_found;
};

/* The DFU suffix CRC32, over length bytes of data. */
uint32_t dfu_image_crc32(const uint8_t *data, size_t length);

/* Check that image is a DFU file for this device: a regular file no larger
 * than dfu_max_image_size() (checked before anything is allocated), then the
 * suffix length, DFU version,
 * signature, a non-empty payload, the suffix CRC, and idVendor/idProduct
 * (id_vendor/id_product, or the 0xffff wildcard). Reads the whole file and
 * restores its position. Returns DFU_IMAGE_OK or a negative
 * enum dfu_image_result, also left in report->result. */
int dfu_image_check(FILE *image, uint16_t id_vendor, uint16_t id_product,
                    struct dfu_image_report *report);

/* One line saying what is wrong with the image described by report, for a
 * result other than DFU_IMAGE_OK. */
void dfu_image_describe_failure(const struct dfu_image_report *report,
                                char *message, size_t message_size);

/*
 * How much flash there is to read back.
 *
 * A read with no flash behind it is a bus fault that takes the bootloader
 * down for good, so the host must not ask for more than the part has.
 *
 * A bootloader built from this tree refuses such a request itself, bounded by
 * the same figures (firmware/bootloader/flash.h). But a board carries
 * whatever bootloader was flashed on it, and older ones answer REQ_UPLOAD
 * from any address below 512 KiB, whatever part they run on. For those
 * boards, the host asking for no more than the part has is the only
 * protection there is.
 *
 * The part identification number says how much that is. It is an
 * In-Application Programming value the application reports
 * (UBERTOOTH_GET_PARTNUM); the bootloader has no such request, so a board
 * already sitting in DFU cannot be asked.
 */
struct dfu_part {
	uint32_t part_id;
	const char *name;      /* "LPC1754" */
	uint32_t flash_size;   /* total flash on the part, in bytes */
};

/* The part behind part_id, or NULL for one this host has no entry for. */
const struct dfu_part *dfu_part_lookup(uint32_t part_id);

/* The table entry at index, or NULL past the last one, so the table itself
 * can be walked and checked. */
const struct dfu_part *dfu_part_at(size_t index);

/* The part the application is linked for
 * (firmware/common/LPC17xx_Linker_Script_with_bootloader.ld gives the
 * application 128K - 16384 bytes), and so the size assumed for a part that
 * cannot be identified.
 *
 * This is the one remaining way to ask past the end of flash, and it is the
 * common path: a board already in the bootloader always takes it. It holds
 * for the parts the Ubertooth One is built from, which have at least this
 * much, but it is an assumption and not a floor: the family table does list
 * smaller parts, a 31 KiB image would fit one of them, and such a board in
 * the bootloader would still be read past the end of its flash. A bootloader
 * built from this tree refuses the request, so what is left exposed is a
 * board carrying an older one. */
#define DFU_ASSUMED_FLASH_SIZE (128u * 1024u)

/* How many bytes above the bootloader a read-back covers on a part with
 * flash_size bytes of flash, or with DFU_ASSUMED_FLASH_SIZE when flash_size
 * is 0 (the part is unknown). Returns 0 when there is no application region
 * at all. */
uint32_t dfu_upload_length(uint32_t flash_size);

/* How much flash a download of an image_size-byte .dfu covers: the whole
 * file, the DFU suffix included, rounded up to a whole block. The tool sends
 * every byte of the file from 0x4000 upwards and pads the last block, so this
 * is what the write really reaches, and the figure to compare with
 * dfu_upload_length() for the attached part. */
unsigned long dfu_download_length(unsigned long image_size);

#endif /* UBERTOOTH_DFU_RECOVERY_H */
