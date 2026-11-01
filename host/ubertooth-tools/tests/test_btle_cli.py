#!/usr/bin/env python3
"""Verify ubertooth-btle refuses bad -a values, get+set and a command line
with no work in it before USB access, and that "-q -" refuses a terminal.

UBERTOOTH_TEST_NO_DEVICES=1 hides attached boards from the tool, so a case
that reached device selection reports no device instead of opening a board
this machine is using.
"""

import os
import subprocess
import sys

try:
    import pty
except ImportError:  # no pty module on this platform
    pty = None


BINARY = sys.argv[1]
ENVIRONMENT = dict(os.environ, UBERTOOTH_TEST_NO_DEVICES="1")
# How long one ubertooth-btle run is given. The whole test takes about 1.3 s on
# an idle machine and 2.2 s on one busy with parallel builds, over some eighty
# short runs, where the cost that grows is starting a process rather than the
# work a run does. Twenty seconds is therefore many times what any one run
# needs, with room for a four-core machine slower again than this one.
RUN_TIMEOUT_S = 20


def reject(arguments, expected):
    result = subprocess.run(
        [BINARY, "-U", "no-such-board", *arguments],
        capture_output=True,
        text=True,
        timeout=RUN_TIMEOUT_S,
        check=False,
        env=ENVIRONMENT,
    )
    assert result.returncode == 1, result
    # the refusal is on standard error, with nothing on standard output
    assert expected in result.stderr, result
    assert result.stdout == "", result
    # refused before any device lookup
    assert "no ubertooth found" not in result.stderr.lower(), result


def accepted_command_line(arguments):
    """Runs the command line past every check, so the device is what it
    complains about and not the combination of options."""
    result = subprocess.run(
        [BINARY, "-U", "ffffffffffffffffffffffffffffffff", *arguments],
        capture_output=True,
        text=True,
        errors="replace",
        timeout=RUN_TIMEOUT_S,
        check=False,
        env=ENVIRONMENT,
    )
    output = result.stdout + result.stderr
    assert result.returncode == 1, (arguments, result)
    assert "error: -" not in result.stderr, (arguments, result)
    assert "no ubertooth found" in output.lower(), (arguments, result)


BAD = "error: access address must be 1-8 hex digits"
reject(("-azzzz",), BAD + " (got zzzz)")
reject(("-a", "zzzz"), BAD + " (got zzzz)")
reject(("-a123456789",), BAD + " (got 123456789)")
reject(("-a0x8e89bed6",), BAD + " (got 0x8e89bed6)")
BOTH = "error: -a cannot both read and set the access address"
reject(("-a8e89bed6", "-a"), BOTH)
reject(("-a", "-a11223344"), BOTH)
reject(("-a", "11223344", "-a"), BOTH)

# A command line with nothing to act on: refused before the device is opened,
# where it used to open the board, send nothing and exit 0 after printing the
# usage text on stdout.
NOTHING = "error: nothing to do: choose a major mode (-f, -n or -p)," \
    " or -a, -k, -s or -t"
reject((), NOTHING)
# the options that only shape work asked for by another option
reject(("-A", "39"), NOTHING)
reject(("-x", "0"), NOTHING)

# Interference acts on a connection, so it needs the mode that has one: the
# usage text and the man page have always scoped -i/-I to -f and -p. With -n,
# which only logs advertising packets, the jam mode was set on the board all
# the same. -j/-J are the older spellings of -i/-I.
for spelling in ("i", "I", "j", "J"):
    NEEDS_MODE = "error: -%s needs -f or -p: there is no connection to" \
        " interfere with otherwise" % spelling
    # on its own too: the man page promises this message for "-i or -I
    # without -f or -p", and it says more than listing every major mode
    reject(("-" + spelling,), NEEDS_MODE)
    reject(("-n", "-" + spelling), NEEDS_MODE)
    # not only with -n: any command line without -f or -p
    reject(("-a", "-" + spelling), NEEDS_MODE)
    reject(("-k", "-" + spelling), NEEDS_MODE)
    reject(("-tnone", "-" + spelling), NEEDS_MODE)
# -f and -p take it
for arguments in (("-f", "-i"), ("-f", "-I"), ("-p", "-j"), ("-p", "-J")):
    accepted_command_line(arguments)
print("ubertooth-btle: -i/-I need -f or -p")

# A sniffing mode runs until it is interrupted, so the access address and
# faux peripheral mode blocks after it used to send their control requests on the
# handle the mode had already closed.
AFTER_A_MODE = "error: -a and -s do not go with -f, -n or -p:" \
    " a sniffing mode runs until it is interrupted"
for mode in ("-f", "-n", "-p"):
    for option in ("-a", "-a8e89bed6", "-s22:44:66:88:aa:cc"):
        reject((mode, option), AFTER_A_MODE)
    # -k runs once the mode has started, so it goes with any of them
    accepted_command_line((mode, "-k"))
    accepted_command_line((mode, "-k1"))
print("ubertooth-btle: -a and -s are refused alongside a sniffing mode")

# -t is sent before the mode starts, which is what the follow modes want. A
# connection that is already running names no device, so a target has nothing
# to match there, and the board read "a target was set" as "the host gave us
# an access address" and followed the advertising one.
NO_TARGET_WITH_PROMISC = "error: -t does not go with -p: promiscuous mode" \
    " has no device address to match a target against"
for mode in ("-f", "-n"):
    accepted_command_line((mode, "-t22:44:66:88:aa:cc"))
    accepted_command_line((mode, "-tnone"))
for target in ("-t22:44:66:88:aa:cc", "-tnone"):
    reject(("-p", target), NO_TARGET_WITH_PROMISC)
    reject((target, "-p"), NO_TARGET_WITH_PROMISC)
print("ubertooth-btle: -t is refused alongside -p")

# a bare command line is the one refusal the usage text follows, on standard
# error with it, and nothing is printed on standard output: the usage text
# used to land there
result = subprocess.run(
    [BINARY],
    capture_output=True,
    text=True,
    timeout=RUN_TIMEOUT_S,
    check=False,
    env=ENVIRONMENT,
)
assert result.returncode == 1, result
assert result.stdout == "", result
assert result.stderr.startswith(NOTHING + "\n"), result
assert "Usage:" in result.stderr and "Major modes:" in result.stderr, result

# the same nothing to do, from a command line that did say something: the one
# line, and no usage text after it
result = subprocess.run(
    [BINARY, "-A", "39"],
    capture_output=True,
    text=True,
    timeout=RUN_TIMEOUT_S,
    check=False,
    env=ENVIRONMENT,
)
assert result.returncode == 1, result
assert result.stdout == "", result
assert result.stderr == NOTHING + "\n", result
# each of the options that do have work in it gets past this check: it is the
# device the tool then complains about, not the command line
for arguments in (("-f",), ("-n",), ("-p",), ("-a",), ("-a8e89bed6",),
                  ("-k",), ("-k1",), ("-s22:44:66:88:aa:cc",),
                  ("-t22:44:66:88:aa:cc",), ("-tnone",)):
    result = subprocess.run(
        [BINARY, "-U", "ffffffffffffffffffffffffffffffff", *arguments],
        capture_output=True,
        text=True,
        errors="replace",
        timeout=RUN_TIMEOUT_S,
        check=False,
        env=ENVIRONMENT,
    )
    output = result.stdout + result.stderr
    assert result.returncode == 1, (arguments, result)
    assert "nothing to do" not in output, (arguments, result)
    assert "no ubertooth found" in output.lower(), (arguments, result)
print("ubertooth-btle: a command line with no work in it is refused")

# "-q -" streams binary PCAP, so a terminal is refused with the same message
# ubertooth-specan prints for "-d -", from the shared check. A mode goes with
# it, since the capture file is opened only once the command line is accepted
# and a capture option on its own has no work in it.
if pty is None:
    pty_fd = None
else:
    try:
        pty_fd, tty_fd = pty.openpty()
    except OSError:  # a sandbox with no pty to allocate
        pty_fd = None

if pty_fd is None:
    print("ubertooth-btle: skipping the tty case, no pty available")
else:
    try:
        result = subprocess.run(
            [BINARY, "-n", "-q", "-"],
            stdin=subprocess.DEVNULL,
            stdout=tty_fd,
            stderr=subprocess.PIPE,
            text=True,
            timeout=RUN_TIMEOUT_S,
            check=False,
            env=ENVIRONMENT,
        )
    finally:
        os.close(tty_fd)
        os.close(pty_fd)
    assert result.returncode == 1, result
    assert "stdout is a tty, refusing to write binary capture data" \
        in result.stderr, result
    # refused before the device was opened, which is where this message would
    # come from
    assert "could not open" not in result.stderr.lower(), result
    print("ubertooth-btle: -q - refuses a tty")

# The bad-address lines are diagnostics, so they go to standard error with
# nothing on standard output: standard output can be a capture stream.
for arguments, expected in (
        (("-s", "00:11"),
         "error: faux peripheral address must be xx:xx:xx:xx:xx:xx (got 00:11)\n"),
        (("-t", "zz:zz:zz:zz:zz:zz"),
         "error: target address must be xx:xx:xx:xx:xx:xx"
         " (got zz:zz:zz:zz:zz:zz)\n"),
        # the mask is named after the option's own address
        (("-t", "11:22:33:44:55:66/99"),
         "error: target mask must be 0-48 (got 99)\n"),
        # -s has no mask and no "none": each is refused as an address
        (("-s", "11:22:33:44:55:66/99"),
         "error: faux peripheral address must be xx:xx:xx:xx:xx:xx"
         " (got 11:22:33:44:55:66/99)\n"),
        (("-s", "none"),
         "error: faux peripheral address must be xx:xx:xx:xx:xx:xx (got none)\n"),
):
    result = subprocess.run([BINARY, *arguments], capture_output=True,
                            text=True, timeout=RUN_TIMEOUT_S, check=False,
                            env=ENVIRONMENT)
    assert result.returncode == 1, (arguments, result)
    assert result.stdout == "", (arguments, result)
    assert result.stderr == expected, (arguments, result)
print("ubertooth-btle: the bad-address lines are on standard error")

# Usage text streams: -h asked for it, so it is the command's output, it goes
# to standard output and the command succeeds. An unknown option is reported
# on standard error and no usage text follows it.
result = subprocess.run([BINARY, "-h"], capture_output=True, text=True,
                        timeout=RUN_TIMEOUT_S, check=False, env=ENVIRONMENT)
# -h did what was asked, so the status says so: it used to be 1, which a
# script checking "ubertooth-btle -h" read as a failure.
assert result.returncode == 0, result
assert "Usage:" in result.stdout, result
# The one capability limit a user has to know before choosing -p: it takes an
# algorithm-2 connection too, but without the channel map, so the capture has
# gaps that -f would not leave.
assert "an algorithm 2 connection takes longer and is followed" \
    in result.stdout, result
assert "without its channel map; -f captures all of one" in result.stdout, \
    result
assert result.stderr == "", result
result = subprocess.run([BINARY, "-Z"], capture_output=True, text=True,
                        timeout=RUN_TIMEOUT_S, check=False, env=ENVIRONMENT)
assert result.returncode == 1, result
assert result.stdout == "", result
assert result.stderr == "error: unrecognized option '-Z'\n", result
print("ubertooth-btle: usage on stdout for -h, and not after a bad option")
