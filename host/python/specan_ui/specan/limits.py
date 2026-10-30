"""The frequency and Wi-Fi channel checks ubertooth-specan-ui makes.

They live here rather than in the ubertooth-specan-ui script so that a bad
range is refused by code that does not need a Qt binding or numpy, and so the
refusals can be tested anywhere the Python tools are built. Each one writes a
single line starting "error: " to standard error and raises ValueError; the
caller exits 1 without opening a window or a device.

eprint() is here for the same reason, and the rest of the package writes its
diagnostics through it: this module is the one the command line can rely on.
"""

import os
import sys


# The range ubertooth-specan accepts (specan_options.h in the host tools).
MIN_FREQ = 2402
MAX_FREQ = 2480

# Wi-Fi channels in the 2.4 GHz band.
MIN_WIFI_CHANNEL = 1
MAX_WIFI_CHANNEL = 14


def point_at_the_bin(stream):
    """Send a stream that cannot be written to the bin, so writing it is safe.

    The interpreter flushes both streams as it exits, where no handler can
    reach a failure: a write that fails there ends the tool in status 120 and
    a message of its own. ubtbr/streams.py does the same for ubertooth-btbr,
    and has the longer note.
    """
    try:
        nowhere = os.open(os.devnull, os.O_WRONLY)
        try:
            os.dup2(nowhere, stream.fileno())
        finally:
            os.close(nowhere)
    except (OSError, ValueError, AttributeError):
        # There is no such stream to point anywhere: it was closed before the
        # tool started. Nothing is written to it either way.
        pass


def eprint(text):
    """Write a diagnostic to standard error, or drop it if it cannot go there.

    Every error, warning and note this tool writes goes through here, the
    argument parser's own refusals among them: the script gives the parser
    an error() that writes through this, because before Python 3.11 argparse
    writes its messages without catching anything and one of them could
    otherwise raise. Losing that stream must cost the lines and nothing
    else: a write that fails must not raise where it was written from and
    end a command, or a capture thread, that was otherwise working.

    The state this is for is a pipe whose reader has left. The tool's startup
    check cannot see one on Linux, where a write of nothing to such a pipe
    succeeds and only a real write finds the reader gone. Any other failure
    of the stream is taken the same way, a full disk among them: there is
    nowhere left to report the loss, so the line goes and the exit status is
    the command's own. The descriptor is pointed at the bin so the writes to
    come are safe.
    """
    try:
        sys.stderr.write(text)
        sys.stderr.flush()
    except (OSError, ValueError, AttributeError):
        point_at_the_bin(sys.stderr)


def _refuse(message):
    eprint("error: " + message + "\n")
    raise ValueError(message)


def convert_wifi(channel):
    """The centre frequency of a 2.4 GHz Wi-Fi channel, in MHz."""
    if channel < MIN_WIFI_CHANNEL or channel > MAX_WIFI_CHANNEL:
        _refuse("channel " + str(channel) + " is not a valid wifi channel")

    # Channels 1-13 are spaced 5 MHz apart. Channel 14 is the exception: it
    # sits 12 MHz above channel 13's 2472 MHz, not 5.
    if channel == MAX_WIFI_CHANNEL:
        return 2484
    return channel * 5 + 2407


def parse_wifi_range(value):
    """The (lower, upper) channels of a --wifi value: "6" or "1-11"."""
    parts = value.split("-")
    try:
        if len(parts) == 1:
            lower = upper = int(parts[0])
        elif len(parts) == 2:
            lower = int(parts[0])
            upper = int(parts[1])
        else:
            raise ValueError()
    except ValueError:
        _refuse("invalid channel range: " + value)
    return lower, upper


def check_freq(freq):
    if freq < MIN_FREQ:
        _refuse("frequency of " + str(freq) + " MHz is below minimum "
                "frequency of " + str(MIN_FREQ))
    if freq > MAX_FREQ:
        _refuse("frequency of " + str(freq) + " MHz is above maximum "
                "frequency of " + str(MAX_FREQ))


def check_freq_pair(freq1, freq2):
    check_freq(freq1)
    check_freq(freq2)

    if freq1 > freq2:
        _refuse("lower frequency of " + str(freq1) + " MHz is above upper "
                "frequency of " + str(freq2) + " MHz")
