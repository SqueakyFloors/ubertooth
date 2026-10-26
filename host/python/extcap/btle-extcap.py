#!/usr/bin/env python3

# Copyright 2013, 2023 Mike Ryan
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

import getopt
import re
import signal
import sys
from subprocess import Popen, PIPE

child = None
# The signal that asked this script to stop, if one arrived. A capture that
# was asked to stop ended as asked, however the child reports it, and a
# capture asked to stop before the child started must not start one.
stop_signal = None

# The advertising channels ubertooth-btle will tune to, as the Bluetooth core
# specification numbers them. The first is the default.
ADVERTISING_CHANNELS = ("37", "38", "39")

# Options Wireshark adds to the calls it makes, which this script accepts
# rather than refuse the call they came with. Only the ones Wireshark passes
# to every extcap program are here: --extcap-version carries the calling
# Wireshark's version, added to the --extcap-interfaces call by Wireshark 2.9
# and later, and nothing here behaves differently for different versions;
# --extcap-capture-filter carries a capture filter the user typed, which
# ubertooth-btle cannot apply, so the capture starts and says so.
#
# The rest of what Wireshark can pass (--extcap-reload-option,
# --extcap-control-in, --extcap-control-out, --extcap-config-option-name and
# --extcap-config-option-value) it passes only to a program whose listing
# advertises the feature behind it: a reloadable argument, a toolbar control,
# a configurable table value. This one advertises none of them, so those stay
# unrecognized and are refused like any other unknown option.
TOLERATED_OPTIONS = ("extcap-version=", "extcap-capture-filter=")

# --extcap-version is documented as carrying a value and is seen without one,
# and getopt has no long option whose value is optional. with_optional_values
# below lets one entry above accept both forms.
OPTIONAL_VALUE = ("--extcap-version",)


def with_optional_values(argv):
    """Give the bare form of an optional-value option an empty value.

    getopt has no long option whose value is optional, so the entry for one
    asks for a value and the bare form is given an empty one here. The form
    whose value is the next word is left alone, for getopt to read it the
    usual way: appending "=" to that one left the value behind as a stray
    word, which stopped getopt and dropped every option after it. A next
    word starting with "-" is the next option, never a value, as in every
    other tool here.
    """
    rewritten = []
    for position, word in enumerate(argv):
        if word == "--":
            # getopt stops here, so nothing after it is an option to rewrite
            rewritten.extend(argv[position:])
            break
        if word in OPTIONAL_VALUE:
            following = argv[position + 1] if position + 1 < len(argv) else None
            if following is None or following.startswith("-"):
                word += "="
        rewritten.append(word)
    return rewritten


def run_tool(command, **options):
    """Start one of the Ubertooth tools, or end with an error: line.

    A tool that is not on the PATH Wireshark started this script with, or
    that cannot be executed, would otherwise end the script in a Python
    traceback. The reason has to arrive in the same one-line form as every
    other error here, since for a capture that is what Wireshark shows for
    the interface.
    """
    try:
        return Popen(command, **options)
    except OSError as err:
        print("error: could not run %s: %s"
              % (command[0], err.strerror or err), file=sys.stderr)
        sys.exit(1)


def sig_handler(sig, frame):
    global stop_signal
    stop_signal = sig
    # The signal can arrive before there is a child to pass it on to, while
    # the command line is still being read. capture() looks at stop_signal
    # for that, so a capture that was stopped starts nothing.
    if child is not None:
        child.terminate()

def main():
    signal.signal(signal.SIGTERM, sig_handler)

    try:
        opts, args = getopt.getopt(
            with_optional_values(sys.argv[1:]),
            "h",
            [
                "help",
                "extcap-interfaces",
                "extcap-dlts",
                "extcap-config",
                "capture",
                "extcap-interface=",
                "fifo=",
                "channel=",
                *TOLERATED_OPTIONS,
            ])
    except getopt.GetoptError as err:
        # A usage error: the message and the usage line go to standard error
        # and the exit status is 1 (.kiro/steering/cli-output.md).
        print("error: %s" % err, file=sys.stderr)
        usage(sys.stderr)
        sys.exit(1)

    # No call here takes an operand, so a leftover word is a mistake, refused
    # before anything runs and before -h ends the command, the way the other
    # tools here refuse one (opt_check_no_operands in opt_parse.c).
    if args:
        # getopt read the rewritten words, so the user's own are quoted back:
        # the two lists have the same length and order, and what getopt did
        # not take is the tail of both.
        typed = sys.argv[1:][len(sys.argv[1:]) - len(args):]
        print("error: unexpected argument '%s'" % typed[0], file=sys.stderr)
        sys.exit(1)

    interface = ''
    fifo = None
    channel = ADVERTISING_CHANNELS[0]
    capture_filter = ''
    do_list_dlts = False
    do_config = False
    do_capture = False

    for o, a in opts:
        if o in ("-h", "--help"):
            usage()
            sys.exit(0)
        elif o == "--extcap-interfaces":
            sys.exit(list_interfaces())
        elif o == "--extcap-dlts":
            do_list_dlts = True
        elif o == "--extcap-config":
            do_config = True
        elif o == "--capture":
            do_capture = True
        elif o == "--extcap-interface":
            interface = a
        elif o == "--fifo":
            fifo = a
        elif o == "--channel":
            channel = a
        elif o == "--extcap-version":
            # the calling Wireshark's version: nothing here depends on it
            pass
        elif o == "--extcap-capture-filter":
            capture_filter = a
        else:
            assert False, "unhandled option"

    # every operation down here depends on having an interface. Its value is
    # "ubertooth<serial>", as --extcap-interfaces lists it; an older
    # "ubertooth<index>" still works, because ubertooth-btle -U reads an
    # all-digit value as an index.
    m = re.match(r'ubertooth([0-9a-fA-F]+)$', interface)
    if not m:
        print("error: --extcap-interface must be ubertooth<serial>, as "
              "--extcap-interfaces lists it", file=sys.stderr)
        sys.exit(1)
    interface = m.group(1)

    if do_list_dlts:
        list_dlts()
    elif do_config:
        config()
    elif do_capture:
        if fifo is None:
            print("error: --capture needs --fifo=PATH", file=sys.stderr)
            sys.exit(1)
        # Checked here rather than left to ubertooth-btle, so a refused
        # command line starts no child and writes nothing to the fifo. The
        # wording is ubertooth-btle's, so one message covers both.
        if channel not in ADVERTISING_CHANNELS:
            print("error: advertising channel index must be %s-%s (got %s)"
                  % (ADVERTISING_CHANNELS[0], ADVERTISING_CHANNELS[-1],
                     channel), file=sys.stderr)
            sys.exit(1)
        if capture_filter:
            print("warning: this interface applies no capture filter; "
                  "filter the packets in Wireshark instead (got %s)"
                  % capture_filter, file=sys.stderr)
        sys.exit(capture(interface, fifo, channel))


def usage(out=sys.stdout):
    print("Usage: %s --extcap-interfaces |"
          " --extcap-interface=ubertooth<serial>"
          " (--extcap-dlts | --extcap-config |"
          " --capture --fifo=PATH [--channel=37|38|39])" % sys.argv[0],
          file=out)


# One "<index>: <serial>" line per attached board, which is what
# "ubertooth-util -U all -s" prints for any number of boards.
INTERFACE_LINE = re.compile(r'(\d+): ([0-9a-fA-F]+)$')

# What "ubertooth-util -U all -s" says when no board is attached. It exits 1
# for that as it does for a real failure, so this line is the only thing that
# tells the two apart, and an empty listing is the right answer to one of
# them. The host tools and this script are installed together, so the wording
# is pinned here rather than guessed at, and the test checks it against the
# built ubertooth-util.
NO_DEVICES = "error: no Ubertooth devices found"


def list_interfaces():
    """List one interface per attached board, and report a failed listing.

    Returns the exit status: 1 when ubertooth-util failed and there is
    nothing to list, 0 otherwise. An empty listing used to be the answer to
    a failure too, which to Wireshark looks exactly like "no Ubertooth
    attached".
    """
    # -U all asks for the indexed listing explicitly, so one output form
    # covers one board and several. Each interface is named by the board's
    # serial, which --capture passes to "ubertooth-btle -U <serial>": an index
    # can change when boards are re-plugged between listing and capture, a
    # serial cannot. A board whose serial could not be read is shown by
    # ubertooth-util as "<index>: <unavailable>" and has no serial to offer,
    # so it is left out.
    proc = run_tool(['ubertooth-util', '-U', 'all', '-s'],
                    stdout=PIPE, stderr=PIPE, universal_newlines=True)
    out, errors = proc.communicate()

    serials = [m.group(2) for m in
               (INTERFACE_LINE.match(line.strip()) for line in out.split('\n'))
               if m]

    # Nothing failed if the only error ubertooth-util reported is that there
    # is no board: the empty listing says that already, so that line is not
    # passed on. Only the "error: " lines are weighed, because libubertooth
    # writes some of its failures without a prefix, and such a line arriving
    # beside this one would still mean no board was found. Everything it wrote
    # is passed on, which is the only place the reason can appear: Wireshark
    # reads this call's standard output and leaves its standard error to its
    # own, and someone running the call by hand sees it there too.
    reported = [line for line in errors.splitlines()
                if line.startswith("error: ")]
    no_devices = not serials and reported == [NO_DEVICES]
    for line in errors.splitlines():
        if not (no_devices and line == NO_DEVICES):
            print(line, file=sys.stderr)

    # The boards that could be named are listed even when another one could
    # not be, which is what ubertooth-util itself does with a partial
    # listing.
    for serial in serials:
        print("interface {value=ubertooth%s}{display=Ubertooth One %s}"
              % (serial, serial))

    # Wireshark throws away everything this call printed when it exits
    # non-zero, so a board that could not be opened must not take the boards
    # that could with it: a listing that named a board is a listing, whatever
    # else went wrong. An empty one is a failure, unless ubertooth-util's only
    # complaint was that there is no board.
    if serials or no_devices or proc.returncode == 0:
        return 0
    # Nothing was suppressed on this path, so whatever ubertooth-util wrote,
    # the user saw. Without a line that says something went wrong, the failure
    # said nothing of itself and its status is all there is to report: a note,
    # a warning or a blank line is not a reason. An unprefixed diagnosis is
    # one, since libubertooth writes some of its failures without a prefix.
    diagnosed = [line for line in errors.splitlines()
                 if line.strip() and
                 not line.startswith(("note: ", "warning: "))]
    if not diagnosed:
        print("error: ubertooth-util failed (%s)" % (
            "killed by signal %d" % -proc.returncode if proc.returncode < 0
            else "exit status %d" % proc.returncode), file=sys.stderr)
    return 1


def list_dlts():
    print("dlt {number=256}{name=DLT_BLUETOOTH_LE_LL_WITH_PHDR}{display=Bluetooth LE}\n")


def config():
    args = []
    args.append((0, '--channel', 'Advertising Channel', 'selector'))

    # The first channel is the one --capture uses without --channel, so it is
    # the one Wireshark's selector offers first.
    values = [(0, ch, ch, "true" if ch == ADVERTISING_CHANNELS[0] else "false")
              for ch in ADVERTISING_CHANNELS]

    for arg in args:
        print("arg {number=%d}{call=%s}{display=%s}{type=%s}" % arg)

    for value in values:
        print("value {arg=%d}{value=%s}{display=%s}{default=%s}" % value)


def capture(interface, fifo, channel):
    """Capture on one board, and report how ubertooth-btle ended.

    Wireshark reads a non-zero exit of the capture call as a failed capture
    and shows the error for it, so the child's failure has to reach it.
    """
    global child
    if stop_signal is not None:
        # stopped before the capture began: there is nothing to start, and
        # nothing failed
        return 0
    child = run_tool([
        "ubertooth-btle", "-f",
        "-U", interface,
        "-q", fifo,
        "-A", channel,
    ])
    if stop_signal is not None:
        # the signal arrived while the child was starting, so the handler
        # may have run before this name held the child: pass it on here
        child.terminate()
    status = child.wait()
    if status < 0:
        # Killed by a signal, reported as -N. Two cases mean the capture
        # ended rather than failed, and Wireshark ends a capture with both:
        # this script was asked to stop and passed the signal on, so the
        # child's death is that stop whichever signal finished it off; or
        # the child got SIGPIPE because whoever was reading the fifo stopped
        # reading it. Any other signal is a failure, reported the way a
        # shell reports it.
        if stop_signal is not None or -status == signal.SIGPIPE:
            return 0
        return 128 - status
    return status

if __name__ == "__main__":
    main()
