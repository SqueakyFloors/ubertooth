"""Verify that ubertooth-follow refuses two capture options given one file.

``-r``, ``-q`` and ``-d`` each open their own destination for writing, which
truncates whatever another has put there, so a run given one name for two of
them would keep one format and lose the rest. The run is refused instead, with
one ``error:`` line naming both options, before any destination is opened: the
file named is checked here to be byte for byte what it was.

Two different names still work, one capture option on its own still works, and
two spellings of standard output still get the one-capture-on-standard-output
refusal rather than this one.

Everything here is a refused or accepted command line, so no board is
involved. ubertooth-follow needs BlueZ, so this test runs only where the tool
is built.
"""

import os
import subprocess
import sys
from pathlib import Path
import tempfile

FOLLOW = sys.argv[1]

# A piconet to follow, so the command line is complete; nothing is opened,
# because every check below is made before a device is selected. LC_ALL, so a
# refusal that quotes strerror() is in the words this test reads.
TARGET = ["-l", "5a3c96", "-u", "0f"]
ENVIRONMENT = dict(os.environ, UBERTOOTH_TEST_NO_DEVICES="1", LC_ALL="C")
RUN_TIMEOUT_S = 30
KNOWN = b"neither capture wrote over this\n"

failures = []


def check(condition, what, detail=""):
    if not condition:
        failures.append("FAIL %s%s" % (what, detail and ": " + detail))


def run(arguments, work):
    result = subprocess.run([FOLLOW] + TARGET + arguments, cwd=work,
                            env=ENVIRONMENT, stdin=subprocess.DEVNULL,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            timeout=RUN_TIMEOUT_S, check=False)
    return result.returncode, result.stdout, result.stderr.decode()


with tempfile.TemporaryDirectory() as work:
    work = Path(work)

    # Each colliding pair, with the file already holding something known: a
    # refusal that opened either destination would have emptied it.
    for first, second, name in (("-q", "-d", "coll.bin"),
                                ("-r", "-q", "coll.bin"),
                                ("-r", "-d", "coll.bin"),
                                ("-q", "-d", "./coll.bin")):
        existing = work / "coll.bin"
        existing.write_bytes(KNOWN)
        status, output, errors = run([first, name, second, name], work)
        pair = "%s %s %s %s" % (first, name, second, name)
        check(status == 1, "%s exits 1" % pair, str(status))
        check(errors == "error: %s and %s cannot both write to %s\n"
              % (first, second, name),
              "%s is refused, naming both options" % pair, errors)
        check(output == b"", "%s writes nothing to standard output" % pair,
              repr(output[:60]))
        check(existing.read_bytes() == KNOWN,
              "%s leaves the file as it was" % pair,
              repr(existing.read_bytes()[:60]))

    # Two spellings of one path are one file too, and the line names each as
    # the user wrote it.
    existing = work / "spelled.bin"
    existing.write_bytes(KNOWN)
    status, output, errors = run(["-q", "spelled.bin", "-d", "./spelled.bin"],
                                 work)
    check(status == 1, "-q spelled.bin -d ./spelled.bin exits 1", str(status))
    check(errors == "error: -q and -d cannot both write to one file"
          " (spelled.bin and ./spelled.bin)\n",
          "two spellings of one path are refused, naming both", errors)
    check(existing.read_bytes() == KNOWN,
          "two spellings of one path leave the file as it was")

    # A name that does not exist yet is not created by a refused run either.
    status, output, errors = run(["-q", "fresh.bin", "-d", "fresh.bin"], work)
    check(status == 1, "a colliding pair of new names exits 1", str(status))
    check(not (work / "fresh.bin").exists(),
          "a refused run creates no file")

    # What must keep working. These command lines are accepted, so they go on
    # to look for a board and fail there; what matters is that the refusal
    # above is not what they get. The null device is in the list because
    # nothing is lost by writing both captures there: that is what the command
    # line asks for.
    for arguments in (["-q", "cap.pcap", "-d", "dump.bin"],
                      ["-r", "cap.pcapng", "-q", "cap.pcap",
                       "-d", "dump.bin"],
                      ["-d", "dump.bin"],
                      ["-q", "cap.pcap"],
                      ["-q", "/dev/null", "-d", "/dev/null"]):
        written = " ".join(arguments)
        status, output, errors = run(arguments, work)
        check("cannot both write to" not in errors,
              "%s is not refused as a collision" % written, errors)

    # A capture on standard output is a destination like any other once the
    # shell has pointed the stream at a file another option also names.
    redirected = work / "redirected.bin"
    redirected.write_bytes(KNOWN)
    with redirected.open("ab") as stream:
        result = subprocess.run(
            [FOLLOW] + TARGET + ["-q", "-", "-d", "redirected.bin"],
            cwd=work, env=ENVIRONMENT, stdin=subprocess.DEVNULL,
            stdout=stream, stderr=subprocess.PIPE,
            timeout=RUN_TIMEOUT_S, check=False)
    errors = result.stderr.decode()
    check(result.returncode == 1, "-q - -d <the redirect's file> exits 1",
          str(result.returncode))
    check(errors == "error: -q and -d cannot both write to one file"
          " (standard output and redirected.bin)\n",
          "a capture on a redirected standard output is compared too", errors)
    check(redirected.read_bytes() == KNOWN,
          "the redirect's file is left as it was",
          repr(redirected.read_bytes()[:60]))

    # Two captures asking for standard output are the
    # one-capture-on-standard-output rule's, however the two are spelled, so
    # that refusal is what a user still gets.
    for first, second in (("-", "-"), ("/dev/stdout", "-"),
                          ("-", "/dev/fd/1")):
        written = "-q %s -d %s" % (first, second)
        status, output, errors = run(["-q", first, "-d", second], work)
        check(status == 1, "%s exits 1" % written, str(status))
        check(errors ==
              "error: -q and -d cannot both write to standard output\n",
              "%s keeps the standard output refusal" % written, errors)
        check(output == b"", "%s writes nothing to standard output" % written,
              repr(output[:60]))

for line in failures:
    print(line)
if failures:
    sys.exit("%d checks failed" % len(failures))
print("ubertooth-follow capture destinations: all checks passed")
