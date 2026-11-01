#!/usr/bin/env python3
"""The BLE channel ubertooth-specan-ui labels each frequency with.

Run as: test_specan_channels.py <path to the specan package's directory>

specan/channels.py imports nothing outside the standard library and draws
nothing, so these cases run wherever the Python tools are built -- no Qt
binding, no numpy, no window and no board. Pressing the key that switches the
labels, and what the window then draws, is checked by hand at a desktop.

The table below is written out rather than worked out, so that it is a second
opinion on the module's arithmetic rather than the same arithmetic twice. It
is the mapping in the Bluetooth Core specification, Volume 6, Part B, section
1.4.1: 40 frequencies 2 MHz apart from 2402 MHz, three of them carrying the
advertising channels 37, 38, and 39, and the data channels 0 to 36 filling the
rest in frequency order.
"""

import sys


PACKAGE_DIR = sys.argv[1]

sys.path.insert(0, PACKAGE_DIR)
from specan import channels  # noqa: E402  (after the path is set up)


CHANNEL_OF_FREQUENCY = {
    2402: 37, 2404: 0, 2406: 1, 2408: 2, 2410: 3, 2412: 4, 2414: 5,
    2416: 6, 2418: 7, 2420: 8, 2422: 9, 2424: 10, 2426: 38, 2428: 11,
    2430: 12, 2432: 13, 2434: 14, 2436: 15, 2438: 16, 2440: 17, 2442: 18,
    2444: 19, 2446: 20, 2448: 21, 2450: 22, 2452: 23, 2454: 24, 2456: 25,
    2458: 26, 2460: 27, 2462: 28, 2464: 29, 2466: 30, 2468: 31, 2470: 32,
    2472: 33, 2474: 34, 2476: 35, 2478: 36, 2480: 39,
}

ADVERTISING = {2402: 37, 2426: 38, 2480: 39}


# ---- the mapping itself ----------------------------------------------------

listed = channels.channels()
assert dict(listed) == CHANNEL_OF_FREQUENCY, listed
# In frequency order, so the window can mark its axis from one end to the
# other, and every channel named once.
assert [frequency for frequency, _ in listed] == sorted(CHANNEL_OF_FREQUENCY)
assert sorted(channel for _, channel in listed) == list(range(40)), listed

for frequency, channel in ADVERTISING.items():
    assert channels.nearest_channel(frequency) == channel, frequency

print("specan/channels.py: %d frequencies, each with its channel"
      % len(listed))


# Every bin the plot can show. ubertooth-specan sweeps in steps of 1 MHz
# where the channels are 2 MHz apart, so half the bins fall between two
# channels and take the nearer one -- the one above, for a bin exactly
# between.
for mhz in range(2402, 2481):
    expected = CHANNEL_OF_FREQUENCY[mhz if mhz % 2 == 0 else mhz + 1]
    got = channels.nearest_channel(mhz)
    assert got == expected, (mhz, expected, got)

print("specan/channels.py: %d bins of the swept band" % len(range(2402, 2481)))


# The plot is drawn a little wider than the band, and there is no channel out
# there to name: a point within 1 MHz of the edge channel still takes it,
# because that is the half of its 2 MHz the window draws.
for mhz, expected in ((2399, None), (2400, None), (2401, 37),
                      (2481, 39), (2482, None), (2500, None)):
    got = channels.nearest_channel(mhz)
    assert got == expected, (mhz, expected, got)

print("specan/channels.py: the plot's margins have no channel")


# ---- and the labels the window draws with it -------------------------------

# Switching to channels and back leaves the MHz labels as they were: the
# channel numbers are a mode the window can be in, not a replacement for the
# frequencies.
for frequency in sorted(CHANNEL_OF_FREQUENCY):
    in_mhz = '%d MHz' % frequency
    in_channels = 'ch %d' % CHANNEL_OF_FREQUENCY[frequency]
    assert channels.frequency_label(frequency, False) == in_mhz, frequency
    assert channels.frequency_label(frequency, True) == in_channels, frequency
    # And back again, from the same call the window makes on every repaint.
    assert channels.frequency_label(frequency, False) == in_mhz, frequency

# A point with no channel keeps its frequency rather than going blank.
assert channels.frequency_label(2400, True) == '2400 MHz'
assert channels.frequency_label(2483, True) == '2483 MHz'

# The gap between two markers is in MHz in both modes, and says so in both,
# because a difference between channel numbers would say nothing about the gap
# on the air. It is the one figure on screen that is never a channel number.
assert channels.difference_label(-4, False) == '(-4 MHz)'
assert channels.difference_label(-4, True) == '(-4 MHz)'
assert channels.difference_label(6, False) == '(+6 MHz)'
assert channels.difference_label(6, True) == '(+6 MHz)'

print("specan/channels.py: both sets of labels, and the switch between them")


# The axis marks of the whole band: nine of them, every fifth channel, with
# both ends of the band named.
marks = channels.axis_ticks(2400, 2482)
assert marks == [
    (2402, '37'), (2412, '4'), (2422, '9'), (2426, '38'), (2432, '13'),
    (2442, '18'), (2452, '23'), (2462, '28'), (2472, '33'), (2480, '39'),
], marks
assert marks[0][0] == 2402 and marks[-1][0] == 2480, marks

# Channel 38 is named wherever it is in the plot, which an even spacing reaches
# only by chance. It is the tenth mark of the whole band, one past the spacing
# cap, and the gap either side of it is uneven: both are the price of naming
# the advertising channel a sweep is most often watched for. 37 and 39 are the
# band's ends and an even spacing names them anyway.
for low in range(2398, 2427):
    for high in range(2427, 2485):
        named = [text for _, text in channels.axis_ticks(low, high)]
        assert '38' in named, (low, high, named)

# A narrower plot is marked only where it reaches, and a plot of a few
# channels names every one of them rather than leaving the axis blank or
# labeling only its ends: -l 2426 -u 2426 draws 2424 to 2428, where the whole
# point of the mode is to see which of those is channel 38.
for low, high, expected in (
        # -l 2440 -u 2460, so the plot runs 2438 to 2462: twelve channels,
        # named every other one. The top channel takes the place of the mark
        # it would otherwise have been squeezed in beside.
        (2438, 2462, [(2438, '16'), (2442, '18'), (2446, '20'), (2450, '22'),
                      (2454, '24'), (2460, '27')]),
        (2424, 2428, [(2424, '10'), (2426, '38')]),
        (2420, 2432, [(2420, '8'), (2422, '9'), (2424, '10'), (2426, '38'),
                      (2428, '11'), (2430, '12')]),
        # 25 channels, so every third one and then the top.
        (2402, 2452, [(2402, '37'), (2408, '2'), (2414, '5'), (2420, '8'),
                      (2426, '38'), (2432, '13'), (2438, '16'), (2444, '19'),
                      (2450, '22')]),
        # Nothing of the band in view at all. No command line reaches this --
        # the tool will not tune below 2402 MHz -- so it is here to say the
        # answer is an empty axis rather than a failure.
        (2300, 2350, []),
):
    got = channels.axis_ticks(low, high)
    assert got == expected, (low, high, got)
    for frequency, text in got:
        assert int(text) == CHANNEL_OF_FREQUENCY[frequency], (frequency, text)

# However narrow or wide the plot, the axis is never crowded past what can be
# read, never blank while a channel is in view, and never has one mark pushed
# up against another: the lowest and highest channel of the plot are both
# named, and no two marks are closer than the channels themselves.
#
# The even spacing keeps its gaps within a factor of two of each other, so the
# mark at the top is not squeezed in beside its neighbor. A plot holding
# channel 38 is exempt from that one: 38 is added to the spacing rather than
# chosen by it, so it can be one mark past the cap and can leave an uneven gap
# either side. That is the price of always naming it.
for low in range(2398, 2484):
    for high in range(low + 1, 2485):
        got = channels.axis_ticks(low, high)
        assert len(got) <= channels.MOST_CHANNEL_MARKS + 1, (low, high, got)
        in_view = [mhz for mhz in CHANNEL_OF_FREQUENCY if low <= mhz < high]
        assert bool(got) == bool(in_view), (low, high, got)
        if not got:
            continue
        assert got[0][0] == min(in_view), (low, high, got)
        assert got[-1][0] == max(in_view), (low, high, got)
        gaps = [second[0] - first[0] for first, second in zip(got, got[1:])]
        if not gaps:
            continue
        # No mark is ever closer to its neighbor than the channels themselves.
        assert min(gaps) >= 2, (low, high, got)
        if '38' not in [text for _, text in got]:
            assert max(gaps) <= 2 * min(gaps), (low, high, got)

print("specan/channels.py: the channel marks of the axis")
