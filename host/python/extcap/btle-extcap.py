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
from subprocess import Popen, DEVNULL, PIPE

child = None
# The signal that asked this script to stop, if one arrived. A capture that
# was asked to stop ended as asked, however the child reports it, and a
# capture asked to stop before the child started must not start one.
stop_signal = None

# The advertising channels ubertooth-btle will tune to, as the Bluetooth core
# specification numbers them. The first is the default.
# A call with no operation in it, bare or not.
NOTHING_TO_DO = ("error: nothing to do: give --extcap-interfaces,"
                 " --extcap-dlts, --extcap-config or --capture")

ADVERTISING_CHANNELS = ("37", "38", "39")

# Why no capture filter can be applied here. Wireshark shows this in the
# filter box when it verifies a filter, and a capture started with one anyway
# repeats it as a warning, so the user reads the same sentence either way.
NO_CAPTURE_FILTER = ("this interface does not apply capture filters;"
                     " use a display filter")

# Options Wireshark adds to the calls it makes, which this script accepts
# rather than refuse the call they came with. Only the ones Wireshark passes
# to every extcap program are here: --extcap-version carries the calling
# Wireshark's version, added to the --extcap-interfaces call by Wireshark 2.9
# and later, and nothing here behaves differently for different versions;
# --extcap-capture-filter carries a capture filter the user typed, which
# ubertooth-btle cannot apply: on its own it is Wireshark's verification call,
# answered with NO_CAPTURE_FILTER, and alongside --capture the capture starts
# and warns.
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

# Every long option this script takes, in getopt's spelling: a trailing "="
# marks one that carries a value. report_bad_option() reads this list to say
# what is wrong with a long option, so it is here rather than inline in the
# getopt call below.
LONG_OPTIONS = (
    "help",
    "extcap-interfaces",
    "extcap-dlts",
    "extcap-config",
    "capture",
    "extcap-interface=",
    "fifo=",
    "channel=",
) + TOLERATED_OPTIONS


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
    """Start one of the Ubertooth tools, or report why it could not run.

    Returns None when it could not, having written the reason. A tool that is
    not on the PATH Wireshark started this script with, or that cannot be
    executed, would otherwise end the script in a Python traceback. The
    reason has to arrive in the same one-line form as every other error here,
    since for a capture that is what Wireshark shows for the interface.

    The caller is left to end the call, rather than this ending it: a -V
    beside -h still has the usage line to print afterwards.
    """
    try:
        return Popen(command, **options)
    except OSError as err:
        print("error: could not run %s: %s"
              % (command[0], err.strerror or err), file=sys.stderr)
        return None


def report_versions():
    """Print the -V answer, and return the exit status it leaves behind.

    One function in libubertooth builds that answer and ubertooth-util prints
    it, so running it here passes the same lines on rather than writing a
    second version of them in Python, which would then have to be kept in
    step by hand. ubertooth-util is on the PATH this script is started with,
    as it is for the interface listing. ubertooth(7), COMMON OPTIONS, has the
    lines.

    The child writes to this script's own streams, so each line lands on the
    stream it belongs on and in the order the child wrote them, and a board
    it could not read is reported there exactly as the C tools report it.
    Every attached board is reported: -V ends the call before the interface
    is read, as it does before every other option.
    """
    # Nothing of this script's is waiting in the buffers here, and this keeps
    # it that way: a line written before the child starts must not land after
    # the child's.
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.flush()
        except (AttributeError, ValueError):
            # There is no such stream: a descriptor closed before the script
            # started leaves the interpreter without one, and nothing is
            # buffered for it either way.
            pass
    child = run_tool(['ubertooth-util', '-V'])
    if child is None:
        return 1
    status = child.wait()
    # ubertooth-util exits 0 with the whole answer and 1 when part of it could
    # not be read; a child killed by a signal (a negative status here)
    # answered nothing at all. Nothing finer is reported, as for every other
    # call here.
    return 0 if status == 0 else 1


def report_bad_option(refusal):
    """Report a command line getopt would not take, as the other tools do.

    One line on standard error naming what is wrong, and no usage text: the
    user can run -h. getopt's own wording ("option --bogus not recognized")
    names the option but not what the tools call the problem, so it is
    rewritten here to the line opt_report_bad_option() in opt_parse.c gives
    for the same mistake, which ubertooth-btbr also uses. The C tools add
    "the tools take single-letter options" to an unknown long option; here
    the long options are the normal spelling, Wireshark's own, so that part
    is left off. Two mistakes only a long option can make have no C
    counterpart and get a line of their own: a value given to an option that
    takes none, and an abbreviation short enough to mean either of two
    options.

    Which mistake it was comes from LONG_OPTIONS rather than from getopt's
    message, whose wording has changed between Python versions: Python 3.15
    reports an ambiguous abbreviation differently from 3.12, which read as an
    unknown option here until this was decided from the list instead.
    """
    # getopt reports the option without its dashes: the name it matched for
    # an option whose value is wrong, and the name as typed for one it could
    # not match or could match twice. Only its message says which form the
    # word had, so a single letter is read as the short form unless the
    # message calls it long: "-c" and "--c" arrive here as the same name.
    name = refusal.opt
    if not (refusal.msg.startswith("option --") or len(name) > 1):
        # The short options here take no value, so anything wrong with one is
        # an option this script does not have.
        print("error: unrecognized option '-%s'" % name, file=sys.stderr)
        sys.exit(1)

    # getopt accepts any abbreviation that names one option, so what the word
    # could have meant decides what was wrong with it. For a value that is
    # missing or not wanted the name is the whole option, so "--fi" is
    # reported as "--fifo": the option the user meant, which is the one place
    # here that does not quote the command line as it was typed. An empty
    # name is "--=value", which getopt splits at the "=".
    meanings = [option for option in LONG_OPTIONS
                if name and option.rstrip("=").startswith(name)]
    if len(meanings) > 1:
        message = "option '--%s' is short for more than one option" % name
    elif not meanings:
        message = "unrecognized option '--%s'" % name
    elif meanings[0].endswith("="):
        # It takes a value and getopt would not have refused it with one.
        message = "option '--%s' requires a value" % meanings[0].rstrip("=")
    else:
        message = "option '--%s' takes no value" % meanings[0]
    print("error: %s" % message, file=sys.stderr)
    sys.exit(1)


def verify_capture_filter(capture_filter):
    """Answer Wireshark's capture-filter verification call.

    Wireshark reads this call's standard output as the filter's problem and
    shows it in the capture filter box, in red; an empty answer means the
    filter is good (extcap.c, cb_verify_filter). The exit status is not part
    of the answer: a non-zero one makes Wireshark discard the output and say
    "Unable to check capture filter" instead, so this exits 0 whichever way
    it answers. No filter can be applied here, so any filter the user typed
    is reported, and the empty filter Wireshark verifies when the box is
    empty is good.
    """
    if capture_filter:
        print(NO_CAPTURE_FILTER)
    return 0


def sig_handler(sig, frame):
    global stop_signal
    stop_signal = sig
    # Whichever signal asked this script to stop, the child is asked with a
    # SIGTERM: that is the one ubertooth-btle's cleanup handler is for, and
    # Ctrl-C has already reached it through the process group anyway.
    #
    # The signal can arrive in the moment between this handler being set and
    # the child being there to stop. capture() looks at stop_signal for that,
    # so a capture that was stopped starts nothing.
    if child is not None:
        child.terminate()

def main():
    # A bare call has said nothing for a message about the command line to
    # answer, so the usage line is the answer, on standard error since it was
    # not asked for. Every other refusal is its one "error: " line alone
    # (.kiro/steering/cli-output.md).
    if not sys.argv[1:]:
        print(NOTHING_TO_DO, file=sys.stderr)
        usage(sys.stderr)
        sys.exit(1)

    try:
        opts, args = getopt.getopt(
            with_optional_values(sys.argv[1:]),
            "hV",
            list(LONG_OPTIONS))
    except getopt.GetoptError as err:
        # A usage error: the message goes to standard error and the exit
        # status is 1. The message says what was wrong, so the usage line
        # does not follow it (.kiro/steering/cli-output.md).
        report_bad_option(err)

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
    # None until --extcap-capture-filter arrives: the option with no operation
    # beside it is Wireshark's verification call, which an empty filter makes
    # too, so whether it was given has to be told from what it carried.
    capture_filter = None
    do_help = False
    do_version = False
    do_list_interfaces = False
    do_list_dlts = False
    do_config = False
    do_capture = False

    for o, a in opts:
        if o in ("-h", "--help"):
            # Both of these end the call, and both are answered below rather
            # than here, so whatever else the line asks for is answered in
            # the one order (see below) and not in the order it was typed.
            do_help = True
        elif o == "-V":
            do_version = True
        elif o == "--extcap-interfaces":
            do_list_interfaces = True
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

    # -V and -h both end the call, and both answer on standard output, so a
    # call with both prints both: the version first and the usage line last,
    # so the shorter answer is not scrolled off the screen by it, whichever
    # order they were written in. The status is the version's: -h has none of
    # its own. Both come after the whole command line has parsed, so a stray
    # word or an option this script does not take is still reported.
    if do_version or do_help:
        status = report_versions() if do_version else 0
        if do_help:
            usage()
        sys.exit(status)

    # The listing needs no interface, so it is answered before the interface
    # is read, and after -V and -h, which end the call ahead of every
    # operation.
    if do_list_interfaces:
        sys.exit(list_interfaces())

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
            print("warning: %s (got %s)" % (NO_CAPTURE_FILTER, capture_filter),
                  file=sys.stderr)
        # A stop is only a capture's to report, so the handlers go up here and
        # not at the top of the command: every other call keeps the default,
        # where a signal ends it rather than being noted and ignored.
        # Wireshark stops a capture with SIGTERM; tshark's Ctrl-C sends SIGINT
        # to the whole process group, and the script used to die of it with
        # nothing to report, so both arrive here.
        signal.signal(signal.SIGTERM, sig_handler)
        signal.signal(signal.SIGINT, sig_handler)
        sys.exit(capture(interface, fifo, channel))
    elif capture_filter is not None:
        # a capture filter and nothing to do with it: Wireshark verifying the
        # filter the user typed, before it starts a capture with it
        sys.exit(verify_capture_filter(capture_filter))
    else:
        # An interface and nothing to do with it: the call used to exit 0
        # having done nothing, which Wireshark reads as a call that worked.
        print(NOTHING_TO_DO, file=sys.stderr)
        sys.exit(1)


def usage(out=sys.stdout):
    print("Usage: %s --extcap-interfaces |"
          " --extcap-interface=ubertooth<serial>"
          " (--extcap-dlts | --extcap-config |"
          " --capture --fifo=PATH [--channel=37|38|39]) |"
          " -V | -h" % sys.argv[0],
          file=out)


# One "Serial No (<index>): <serial>" line per attached board, which is what
# "ubertooth-util -U all -s" prints for any number of boards. A board it could
# not read says "unavailable, <reason>" there instead, which this does not
# match.
INTERFACE_LINE = re.compile(r'Serial No \((\d+)\): ([0-9a-fA-F]+)$')

# What "ubertooth-util -U all -s" says when no board is attached. It exits 1
# for that as it does for a real failure, so this line is the only thing that
# tells the two apart, and an empty listing is the right answer to one of
# them. The host tools and this script are installed together, so the wording
# is pinned here rather than guessed at, and the test checks it against the
# built ubertooth-util.
NO_DEVICES = "error: no Ubertooth devices found"


def list_interfaces():
    """List one interface per attached board, and report a failed listing.

    Returns the exit status: 1 when ubertooth-util failed, 0 when it did not
    or when its only complaint was that no board is attached, which an empty
    listing answers. A failure with nothing to list used to come out as an
    empty listing and exit 0, which to Wireshark looks exactly like "no
    Ubertooth attached"; a failure that still named a board used to exit 0 as
    well, so Wireshark took the listing and the failure went unnoticed.
    """
    # -U all asks for the indexed listing explicitly, so one output form
    # covers one board and several. Each interface is named by the board's
    # serial, which --capture passes to "ubertooth-btle -U <serial>": an index
    # can change when boards are re-plugged between listing and capture, a
    # serial cannot. A board whose serial could not be read is shown by
    # ubertooth-util as "Serial No (<index>): unavailable, <reason>" and has
    # no serial to offer, so it is left out.
    proc = run_tool(['ubertooth-util', '-U', 'all', '-s'],
                    stdout=PIPE, stderr=PIPE, universal_newlines=True)
    if proc is None:
        # It said why; with nothing to list the call failed.
        return 1
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
    # listing. Where that listing goes with a non-zero status below, whoever
    # runs this call by hand still sees it, and Wireshark does not.
    for serial in serials:
        print("interface {value=ubertooth%s}{display=Ubertooth One %s}"
              % (serial, serial))

    # A partial failure is a failure, as it is for every other tool here: a
    # board that could not be opened exits 1 even though some were listed.
    # The cost is Wireshark's, and it is real: it throws away everything a
    # call that failed printed, so one unopenable board - another capture
    # already has it, say - leaves its capture list without the boards that
    # were fine, until that board is freed or unplugged. The reason is on
    # standard error either way. No board attached is not a failure of this
    # kind: the empty listing is the answer to it.
    if no_devices or proc.returncode == 0:
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
    """Capture on one board, and report whether the capture ended as asked.

    Returns 0 when it did and 1 when anything failed, including a child that
    failed or was killed. Wireshark reads a non-zero exit of the capture call
    as a failed capture and shows the error for it, so the child's failure has
    to reach it; it needs nothing finer than non-zero, so the child's own
    status is not passed through (.kiro/steering/cli-output.md).
    """
    global child
    if stop_signal is not None:
        # stopped before the capture began: there is nothing to start, and
        # nothing failed
        return 0
    # ubertooth-btle -f writes a text decode of every packet to its standard
    # output even with the PCAP data going to the fifo, which nobody asked
    # this call for: 8 seconds of ambient advertising is some 560 kB of it.
    # So it is dropped. Standard output here carries the listings alone, and
    # passing it on to standard error instead would crowd out the reason a
    # failed capture has to give there: Wireshark keeps only the first 1024
    # bytes of it (extcap.c, STDERR_BUFFER_SIZE). ubertooth-btle's own
    # failures go to its standard error, which is this script's, so nothing
    # that explains a failure is lost.
    #
    # A fifo of "-" is the exception: it means this script's own standard
    # output, which ubertooth-btle writes the capture to, so dropping that
    # output would throw the capture away. It prints no decode there either,
    # having taken the stream for the capture (capture_took_stdout in
    # ubertooth-btle.c). Wireshark always names a real fifo; "-" is for a
    # capture run by hand, where every file option here means standard output
    # by it (host/doc/ubertooth.md).
    child = run_tool([
        "ubertooth-btle", "-f",
        "-U", interface,
        "-q", fifo,
        "-A", channel,
    ], stdout=None if fifo == "-" else DEVNULL)
    if child is None:
        # It said why; no capture started, which Wireshark reads as a failed
        # capture and shows the reason for.
        return 1
    if stop_signal is not None:
        # the signal arrived while the child was starting, so the handler
        # may have run before this name held the child: pass it on here
        child.terminate()
    status = child.wait()
    # Two endings are the capture ending rather than failing, and Wireshark
    # ends a capture with both. This script was asked to stop and asked the
    # child to stop in turn, so the capture ended as asked however the child
    # reports it: ubertooth-btle catches the SIGTERM and leaves of its own
    # accord, and one that is killed by a signal died of the stop. Or the
    # child got SIGPIPE, which is what it gets when whoever was reading the
    # fifo stopped reading.
    if stop_signal is not None:
        return 0
    if status < 0 and -status == signal.SIGPIPE:
        return 0
    # Anything else that is not a clean exit failed: a child that failed or
    # was killed (reported as -N), reported to Wireshark as a failed capture.
    return 0 if status == 0 else 1

if __name__ == "__main__":
    main()
