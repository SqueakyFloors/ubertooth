/*
 * Waiting for an Ubertooth to come back after a reset, for ubertooth-util -r.
 *
 * After a reset the firmware keeps the old USB device for a moment, then drops
 * off the bus and enumerates again; how long that takes depends on the host
 * (it is slower on macOS). Instead of sleeping a fixed time, the caller polls
 * for the selected device until a deadline. The clock, the sleep, the device
 * count and the selection are injected, so the timing can be tested without
 * hardware.
 */
#ifndef UBERTOOTH_UTIL_RESET_WAIT_H
#define UBERTOOTH_UTIL_RESET_WAIT_H

#include <stddef.h>
#include <stdint.h>

struct util_reset_wait_ops {
	/* Milliseconds from an arbitrary monotonic origin. */
	uint64_t (*now_ms)(void *context);
	void (*sleep_ms)(void *context, unsigned ms);
	/* Number of attached Ubertooth devices, or a negative value on error.
	 * Must not open a device. */
	int (*count)(void *context);
	/* Opens the selected device, or returns NULL if it is not available.
	 * Must not print errors or change the device state (no STOP). */
	void *(*try_select)(void *context);
};

struct util_reset_wait_params {
	unsigned initial_delay_ms; /* before the first check */
	unsigned poll_ms;          /* between checks */
	unsigned leave_ms;         /* how long to look for the old device leaving */
	unsigned deadline_ms;      /* give up this long after the wait started */
	/* Devices attached before the reset. A positive value enables looking for
	 * the reset device leaving (the count drops below it); zero or less (the
	 * count was unknown) skips that. */
	int count_before;
	/* Only try to select once count_before devices are attached again (or the
	 * count fails). Used for selection by index, where opening while the reset
	 * device is still missing could pick another board. */
	int select_needs_count;
};

/* Defaults: a short first delay, 100 ms polls, up to 3 s for the old device
 * to leave, and 10 s in total for it to come back. */
#define UTIL_RESET_WAIT_INITIAL_DELAY_MS 100u
#define UTIL_RESET_WAIT_POLL_MS 100u
#define UTIL_RESET_WAIT_LEAVE_MS 3000u
#define UTIL_RESET_WAIT_DEADLINE_MS 10000u

/*
 * Waits for the selected device after a reset: sleeps initial_delay_ms, waits
 * (up to leave_ms from the start) for the device count to drop below
 * count_before, then polls try_select() every poll_ms until it returns a
 * device or deadline_ms has passed since the start. One last attempt is made
 * at the deadline. Returns the selected device, or NULL on timeout.
 */
void *util_reset_wait(const struct util_reset_wait_ops *ops, void *context,
                      const struct util_reset_wait_params *params);

/* A serial number as the lowercase hex string -U matches: 16 bytes as two hex
 * digits each, plus the NUL. */
#define UTIL_RESET_SERIAL_STR_LEN 33

/* Reading the serial of the device about to be reset, injected so the choice
 * can be tested without hardware. */
struct util_reset_follow_ops {
	/* Writes the open device's serial as the lowercase hex string -U matches.
	 * Returns 0 on success, anything else on failure. */
	int (*get_serial)(void *context, char *serial, size_t size);
	/* Reports that the serial could not be read, so the selector is kept.
	 * u_arg is the -U argument, NULL if none was given. */
	void (*warn)(void *context, const char *u_arg);
};

/* What the wait after a reset looks for. */
struct util_reset_follow {
	/* The selection to wait for: the device's own serial when it could be
	 * read, otherwise the caller's -U argument (which may be NULL). */
	const char *selector;
	/* For util_reset_wait_params: set when the selection is by index. */
	int select_needs_count;
	/* Backs selector when the serial was read, empty otherwise. */
	char serial[UTIL_RESET_SERIAL_STR_LEN];
	/* Whether the board answered the serial read. A board that answers
	 * nothing refuses the reset request too, so the caller resets it at the
	 * USB level instead. */
	int answered;
};

/*
 * Chooses what the wait after a reset follows, from the still-open device
 * about to be reset. With several boards attached the enumeration order can
 * change across a reset, so -U<index> can come back pointing at another board
 * and later options in the same command would run on it. Following the
 * device's own serial keeps them on the board that was reset, whatever
 * selector was given.
 *
 * If the serial cannot be read, the caller's selector is kept (with the index
 * gate that goes with it) and ops->warn() is called. There is nothing to warn
 * about when the selector is already a serial, which follows the board on its
 * own, or when attached is 1: no other board can be opened instead. A negative
 * attached (the count failed) does warn, since it rules nothing out.
 */
void util_reset_follow_init(struct util_reset_follow *follow,
                            const struct util_reset_follow_ops *ops,
                            void *context, const char *u_arg, int attached);

#endif /* UBERTOOTH_UTIL_RESET_WAIT_H */
