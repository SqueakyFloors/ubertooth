"""The frequency and Wi-Fi channel checks ubertooth-specan-ui makes.

They live here rather than in the ubertooth-specan-ui script so that a bad
range is refused by code that does not need a Qt binding or numpy, and so the
refusals can be tested anywhere the Python tools are built. Each one writes a
single line starting "error: " to standard error and raises ValueError; the
caller exits 1 without opening a window or a device.
"""

import sys


# The range ubertooth-specan accepts (specan_options.h in the host tools).
MIN_FREQ = 2402
MAX_FREQ = 2480

# Wi-Fi channels in the 2.4 GHz band.
MIN_WIFI_CHANNEL = 1
MAX_WIFI_CHANNEL = 14


def _refuse(message):
    print("error: " + message, file=sys.stderr)
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
