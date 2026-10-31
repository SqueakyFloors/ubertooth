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
advertising channels 37, 38 and 39, and the data channels 0 to 36 filling the
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
    assert channels.nearest_channel(mhz) == expected, (mhz, expected,
                                                       channels.nearest_channel(mhz))

print("specan/channels.py: %d bins of the swept band" % len(range(2402, 2481)))


# The plot is drawn a little wider than the band, and there is no channel out
# there to name: a point within 1 MHz of the edge channel still takes it,
# because that is the half of its 2 MHz the window draws.
for mhz, expected in ((2399, None), (2400, None), (2401, 37),
                      (2481, 39), (2482, None), (2500, None)):
    assert channels.nearest_channel(mhz) == expected, (mhz, expected,
                                                       channels.nearest_channel(mhz))

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

# The gap between two markers is in MHz in either mode, because a difference
# between channel numbers would say nothing about the gap on the air. The
# unit is spelled out where nothing beside it is in MHz.
assert channels.difference_label(-4, False) == '(-4)'
assert channels.difference_label(-4, True) == '(-4 MHz)'
assert channels.difference_label(6, False) == '(+6)'
assert channels.difference_label(6, True) == '(+6 MHz)'

print("specan/channels.py: both sets of labels, and the switch between them")


# The axis. In MHz it is marked every tenth megahertz from the low end of the
# plot, as it always has been; in channels the marks sit on channels, so a
# line on the graph lines up with the number under it.
assert channels.axis_ticks(2400, 2482, False) == [
    (2400, '2400'), (2410, '2410'), (2420, '2420'), (2430, '2430'),
    (2440, '2440'), (2450, '2450'), (2460, '2460'), (2470, '2470'),
    (2480, '2480'),
], channels.axis_ticks(2400, 2482, False)

marks = channels.axis_ticks(2400, 2482, True)
assert marks == [
    (2402, '37'), (2412, '4'), (2422, '9'), (2432, '13'), (2442, '18'),
    (2452, '23'), (2462, '28'), (2472, '33'), (2480, '39'),
], marks
# Every mark names the channel that is really on that frequency, and both
# ends of the band are marked.
for frequency, text in marks:
    assert int(text) == CHANNEL_OF_FREQUENCY[frequency], (frequency, text)
assert marks[0][0] == 2402 and marks[-1][0] == 2480, marks

# A narrower plot is marked only where it reaches: -l 2440 -u 2460 draws
# 2438 to 2462.
narrow = channels.axis_ticks(2438, 2462, True)
assert narrow == [(2442, '18'), (2452, '23'), (2462, '28')], narrow
assert channels.axis_ticks(2438, 2462, False) == [(2438, '2438'),
                                                  (2448, '2448'),
                                                  (2458, '2458')]

print("specan/channels.py: the axis marks in both modes")
