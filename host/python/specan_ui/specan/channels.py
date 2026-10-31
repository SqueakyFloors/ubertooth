# Copyright 2017 Emmanuel Blot
#
# This file is part of Project Ubertooth.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2, or (at your option)
# any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; see the file COPYING.  If not, write to
# the Free Software Foundation, Inc., 51 Franklin Street,
# Boston, MA 02110-1301, USA.

"""BLE channel numbers for the frequencies ubertooth-specan-ui plots.

The window can label its axis and its markers with Bluetooth LE channel
numbers instead of megahertz. The arithmetic and the labels it produces live
here rather than in the window so they can be tested wherever the Python
tools are built: this module imports nothing outside the standard library, so
it needs no Qt binding and no numpy, and it draws nothing.

Ported from Emmanuel Blot's "Show BLE channels" patch
(https://github.com/eblot/ubertooth/commit/91a56841), which labeled the axis
and markers in channels in place of megahertz. Here it is a mode the window
switches into and out of, so the frequency labels are still there.
"""

import math


# Bluetooth LE uses 40 radio frequencies, 2 MHz apart, from 2402 to 2480 MHz.
BLE_LOW_FREQ = 2402
BLE_CHANNEL_SPACING = 2
BLE_FREQUENCY_COUNT = 40

# Which of those frequencies carry the advertising channels, by their position
# in the run: 2402 MHz is channel 37, 2426 MHz is 38, and 2480 MHz is 39. The
# data channels 0 to 36 fill the rest in frequency order, so a data channel's
# number is its frequency's position less the advertising channels below it
# (Bluetooth Core specification, Volume 6, Part B, section 1.4.1).
_ADVERTISING = {0: 37, 12: 38, 39: 39}

# How many channels the axis names at most. Nine of them across the whole band
# works out at about one every 10 MHz, which is as close together as the
# labels can be read at the window's usual width, and is what the frequency
# axis beside it has always drawn. A plot with no more channels in it than
# that names every one of them.
MOST_CHANNEL_MARKS = 9


def channel_at(position):
    """The BLE channel on the frequency at this position in the run.

    The position is counted from 2402 MHz, so position 1 is 2404 MHz.
    """
    if position in _ADVERTISING:
        return _ADVERTISING[position]
    return position - sum(1 for taken in _ADVERTISING if taken < position)


def channels():
    """Every BLE frequency in MHz with its channel, in frequency order."""
    return tuple((BLE_LOW_FREQ + position * BLE_CHANNEL_SPACING,
                  channel_at(position))
                 for position in range(BLE_FREQUENCY_COUNT))


def nearest_channel(mhz):
    """The BLE channel nearest a frequency in MHz, or None if there is none.

    A frequency between two channels takes the nearer of them, and the higher
    one when it lies exactly between: the plot's bins are 1 MHz apart where
    the channels are 2 MHz apart, so half the bins fall between two channels,
    and so does any point the user clicks. Beyond 1 MHz outside the band
    there is no channel to name, which is the plot's margin either side.
    """
    offset = mhz - BLE_LOW_FREQ
    position = int(math.floor(offset / BLE_CHANNEL_SPACING + 0.5))
    position = min(max(position, 0), BLE_FREQUENCY_COUNT - 1)
    if abs(offset - position * BLE_CHANNEL_SPACING) > BLE_CHANNEL_SPACING / 2.0:
        return None
    return channel_at(position)


def frequency_label(mhz, ble_channels):
    """The label for one point of the plot: its frequency, or its BLE channel.

    A point with no channel keeps its frequency even in channel mode, so a
    label is never left blank: the plot is drawn a little wider than the band
    BLE uses, and its margins have no channel to name.
    """
    if ble_channels:
        channel = nearest_channel(mhz)
        if channel is not None:
            return 'ch %d' % channel
    return '%.0f MHz' % mhz


def difference_label(mhz, ble_channels):
    """The label for the gap between two points, in MHz in either mode.

    A difference between channel numbers would say nothing about the gap on
    the air, because the advertising channels sit among the data channels
    rather than after them: channels 10 and 11 are 4 MHz apart where 9 and 10
    are 2. The unit is spelled out in channel mode, where no label beside it
    is in megahertz.
    """
    if ble_channels:
        return '(%+.0f MHz)' % mhz
    return '(%+.0f)' % mhz


def axis_ticks(low_mhz, high_mhz):
    """Where to mark the axis with channel numbers, and with which numbers.

    Returns (frequency in MHz, label) pairs for a plot running from low_mhz
    up to, but not including, high_mhz: that last megahertz is the right edge
    of the window, where a mark would be drawn off it.

    The marks are spread as widely as MOST_CHANNEL_MARKS allows, and the top
    channel of the plot is named as well as the bottom one, so both ends of
    the band are labeled. A plot zoomed in on a few channels names every one
    of them rather than leaving the axis blank.
    """
    inside = [(mhz, '%d' % channel) for mhz, channel in channels()
              if low_mhz <= mhz < high_mhz]
    if not inside:
        return []
    step = int(math.ceil(len(inside) / float(MOST_CHANNEL_MARKS)))
    marks = inside[::step]
    if marks[-1] != inside[-1]:
        marks.append(inside[-1])
    return marks
