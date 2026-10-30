#!/usr/bin/env python3
"""ubertooth-specan-ui refuses a bad range on standard error, with exit 1.

It also pins the frequency each Wi-Fi channel number converts to, since that
arithmetic lives in the same module as the refusals.

Run as: test_specan_ui_streams.py <path to the specan package's directory>
                                 [<path to ubertooth-specan-ui>]

The checks live in specan/limits.py, which imports nothing outside the
standard library, so these cases run wherever the Python tools are built --
no Qt binding, no numpy, no window and no board. Each refusal writes one line
starting "error: " to standard error, nothing to standard output, and raises
ValueError, which the script turns into exit 1.

With the script named as well, the same cases are run through it end to end,
along with -h and -V. The script reads its whole command line before the
window's own imports, so none of those calls needs numpy or a Qt binding, and
one case shadows both with modules that refuse to import to prove it. Only
opening the window needs them.
"""

import contextlib
import io
import os
import subprocess
import sys
import tempfile


PACKAGE_DIR = sys.argv[1]
SCRIPT = sys.argv[2] if len(sys.argv) > 2 else None

sys.path.insert(0, PACKAGE_DIR)
from specan import limits  # noqa: E402  (after the path is set up)


# Each case: the arguments the script would be given, the call the script
# makes for them, and the whole line it must write to standard error.
CASES = (
    (("--wifi", "nonsense"), lambda: limits.parse_wifi_range("nonsense"),
     "error: invalid channel range: nonsense\n"),
    (("--wifi", "1-2-3"), lambda: limits.parse_wifi_range("1-2-3"),
     "error: invalid channel range: 1-2-3\n"),
    (("--wifi", "15"), lambda: limits.convert_wifi(15),
     "error: channel 15 is not a valid wifi channel\n"),
    (("--wifi", "0"), lambda: limits.convert_wifi(0),
     "error: channel 0 is not a valid wifi channel\n"),
    (("-l", "2000"), lambda: limits.check_freq_pair(2000, limits.MAX_FREQ),
     "error: frequency of 2000 MHz is below minimum frequency of 2402\n"),
    (("-u", "3000"), lambda: limits.check_freq_pair(limits.MIN_FREQ, 3000),
     "error: frequency of 3000 MHz is above maximum frequency of 2480\n"),
    (("-l", "2470", "-u", "2410"), lambda: limits.check_freq_pair(2470, 2410),
     "error: lower frequency of 2470 MHz is above upper frequency of 2410 MHz\n"),
    # Channel 14 is centred above the range the board tunes, so a padding
    # under 4 asks for a band that lies entirely above it.
    (("--wifi", "14", "--padding", "3"),
     lambda: limits.check_freq_pair(2481, limits.MAX_FREQ),
     "error: frequency of 2481 MHz is above maximum frequency of 2480\n"),
)


# ---- the checks themselves -------------------------------------------------

for _arguments, call, expected in CASES:
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        try:
            call()
        except ValueError:
            pass
        else:
            raise AssertionError("%s was accepted" % expected.strip())
    # nothing on standard output: these used to print "ERROR: ..." there
    assert out.getvalue() == "", (expected, out.getvalue())
    assert err.getvalue() == expected, (expected, err.getvalue())

# A good value is accepted and says nothing at all.
for call in (lambda: limits.parse_wifi_range("1-11"),
             lambda: limits.convert_wifi(6),
             lambda: limits.check_freq_pair(2410, 2470)):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        call()
    assert out.getvalue() == "" and err.getvalue() == "", (out.getvalue(),
                                                           err.getvalue())

print("specan/limits.py: %d refusals, each one line on stderr" % len(CASES))


# A refusal with nowhere to write it is still a refusal, and not a failure of
# its own: the ValueError the caller turns into exit 1 comes out, where the
# write used to raise BrokenPipeError straight past it and end the tool in a
# traceback. The exit status was 1 either way, so this is pinned on the
# exception rather than the status.
#
# Arranged in a child, because the state is a real descriptor: standard error
# is a pipe whose reader has already left. Unlike the script's own cases
# further down, this one bites on macOS too -- nothing has had the chance to
# point the stream at the bin first.
REFUSAL_IN_A_CHILD = """
import os, sys
sys.path.insert(0, PACKAGE)
reading, writing = os.pipe()
os.close(reading)
os.dup2(writing, 2)
os.close(writing)
from specan import limits
try:
    limits.check_freq_pair(2000, limits.MAX_FREQ)
except ValueError:
    print("refused with ValueError, as the caller expects")
except BaseException as problem:
    print("raised " + type(problem).__name__ + " instead")
else:
    print("accepted a frequency below the minimum")
""".replace("PACKAGE", repr(PACKAGE_DIR))

refused = subprocess.run([sys.executable, "-c", REFUSAL_IN_A_CHILD],
                         stdout=subprocess.PIPE, text=True, timeout=60)
assert refused.returncode == 0, refused
assert refused.stdout == "refused with ValueError, as the caller expects\n", \
    refused.stdout

print("specan/limits.py: a refusal it cannot write is still a refusal")


# The centre frequency of each channel, with both ends of the evenly spaced
# run and the one channel that breaks the spacing: 14 is 12 MHz above 13.
CENTRES = ((1, 2412), (6, 2437), (11, 2462), (13, 2472), (14, 2484))
for channel, centre in CENTRES:
    assert limits.convert_wifi(channel) == centre, (channel, centre,
                                                    limits.convert_wifi(channel))

print("specan/limits.py: %d channel centre frequencies" % len(CENTRES))


# ---- and the same cases through the script, where it can run --------------

if SCRIPT is None:
    sys.exit(0)

# The script never opens a device for these cases, but if a later change makes
# it try, this keeps it off whatever board is attached.
ENVIRONMENT = dict(os.environ, UBERTOOTH_TEST_NO_DEVICES="1",
                   PYTHONPATH=os.pathsep.join(
                       [PACKAGE_DIR] + [os.environ["PYTHONPATH"]]
                       if "PYTHONPATH" in os.environ else [PACKAGE_DIR]))


def run(arguments, environment=None):
    return subprocess.run([sys.executable, SCRIPT] + list(arguments),
                          env=environment or ENVIRONMENT,
                          stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, universal_newlines=True,
                          timeout=60)


# -h prints the usage text on standard output and exits 0. The script reads
# its whole command line before the window's own imports, so this runs on a
# machine with no numpy and no Qt binding, as every case below does.
result = run(["-h"])
assert result.returncode == 0, result
assert result.stdout.startswith("usage:"), result
assert result.stderr == "", result
# --help is the same option
assert run(["--help"]).stdout == result.stdout, result

for arguments, _call, expected in CASES:
    result = run(arguments)
    assert result.returncode == 1, (arguments, result)
    assert result.stdout == "", (arguments, result)
    assert result.stderr == expected, (arguments, expected, result)

print("ubertooth-specan-ui: the same %d refusals through the script"
      % len(CASES))


# ---- -V, which the script answers by running ubertooth-util ---------------
#
# A fake ubertooth-util stands in for the real one: it records the arguments it
# was called with, prints the answer the test gives it and exits with the
# status the test gives it. So no board is involved, and what is checked is
# that the script passes ubertooth-util's answer through rather than writing a
# version of its own.

FAKE_UTIL = """#!%s
import os
import sys

with open(os.environ["FAKE_UTIL_ARGS"], "w") as record:
    record.write("\\n".join(sys.argv[1:]))
sys.stdout.write(os.environ["FAKE_UTIL_ANSWER"])
sys.exit(int(os.environ["FAKE_UTIL_STATUS"]))
"""

# What the fake answers with. Deliberately not the real lines, which
# ubertooth(7), COMMON OPTIONS, describes: those are the C side's words, held
# to the C tests by host/ubertooth-tools/tests/test_doc_example_lines.py, and
# a copy of them here would let that check pass against this fake instead.
# What this pins is that every line arrives unchanged, whatever it says.
ANSWER = ("the first line ubertooth-util printed\n"
          "the second line ubertooth-util printed\n"
          "the third line ubertooth-util printed\n")

VERSION_WORK = tempfile.TemporaryDirectory()
FAKE_DIRECTORY = os.path.join(VERSION_WORK.name, "bin")
os.mkdir(FAKE_DIRECTORY)
FAKE_PATH = os.path.join(FAKE_DIRECTORY, "ubertooth-util")
with open(FAKE_PATH, "w") as fake:
    fake.write(FAKE_UTIL % sys.executable)
os.chmod(FAKE_PATH, 0o755)
VERSION_ARGS_FILE = os.path.join(VERSION_WORK.name, "args")
# An empty directory, so a run given this PATH can run no ubertooth-util at
# all. The script itself is started by path, so that costs it nothing.
EMPTY_DIRECTORY = os.path.join(VERSION_WORK.name, "empty")
os.mkdir(EMPTY_DIRECTORY)


def version_run(arguments, answer=ANSWER, status=0, path=FAKE_DIRECTORY):
    if os.path.exists(VERSION_ARGS_FILE):
        os.remove(VERSION_ARGS_FILE)
    return run(arguments, dict(ENVIRONMENT, PATH=path,
                               FAKE_UTIL_ARGS=VERSION_ARGS_FILE,
                               FAKE_UTIL_ANSWER=answer,
                               FAKE_UTIL_STATUS=str(status)))


def version_arguments():
    with open(VERSION_ARGS_FILE) as record:
        return record.read().split("\n")


# -V on its own: the answer, unchanged, on standard output, and exit 0. It
# asks ubertooth-util for the version and nothing else.
result = version_run(["-V"])
assert result.returncode == 0, result
assert result.stdout == ANSWER, result
assert result.stderr == "", result
assert version_arguments() == ["-V"], version_arguments()

# -U names the board, and is passed on as ubertooth-util's own -U
result = version_run(["-V", "-U", "05f"])
assert result.returncode == 0, result
assert version_arguments() == ["-V", "-U", "05f"], version_arguments()

# a board that is attached and could not be read exits 1 there, and so here:
# its line says why, and the answer is still printed
result = version_run(["-V"], answer="Firmware version: read failed\n",
                     status=1)
assert result.returncode == 1, result
assert result.stdout == "Firmware version: read failed\n", result

# -V beside -h prints both, the version first, and exits 0
result = version_run(["-h", "-V"])
assert result.returncode == 0, result
assert result.stdout.startswith(ANSWER), result
assert "usage:" in result.stdout, result
assert result.stderr == "", result

# a range the tool will not take is still reported beside -V, and no version
# is printed: the command failed before it got that far
result = version_run(["-V", "-l", "2000"])
assert result.returncode == 1, result
assert result.stdout == "", result
assert result.stderr == \
    "error: frequency of 2000 MHz is below minimum frequency of 2402\n", result

# ubertooth-util not on the PATH: one error line, not a traceback
result = version_run(["-V"], path=EMPTY_DIRECTORY)
assert result.returncode == 1, result
assert result.stdout == "", result
assert "Traceback" not in result.stderr, result
assert result.stderr == \
    "error: could not run ubertooth-util: No such file or directory\n", result

# and the usage text beside it is still printed, since -h was asked for too:
# a version that could not be read does not take the rest of the answer with
# it. The status is the version's.
result = version_run(["-h", "-V"], path=EMPTY_DIRECTORY)
assert result.returncode == 1, result
assert result.stdout.startswith("usage:"), result
assert result.stderr == \
    "error: could not run ubertooth-util: No such file or directory\n", result

print("ubertooth-specan-ui: -V passes ubertooth-util's answer through")

# -V with standard output closed before the tool started, which is `-V >&-`:
# the interpreter is left without that stream, and the flush that orders this
# tool's lines before the child's used to end the command in an AttributeError
# traceback. The answer cannot be printed, so it is said in one line and the
# command exits 1.
#
# ubertooth-util is on the PATH here and must not be run at all: started with
# that descriptor closed, the first file it opens takes its number, and the
# answer would be written into whatever that turned out to be -- for a tool
# that opens USB device nodes, one of those.
if os.path.exists(VERSION_ARGS_FILE):
    os.remove(VERSION_ARGS_FILE)
closed = subprocess.Popen(
    [sys.executable, SCRIPT, "-V"], stdout=subprocess.PIPE,
    stderr=subprocess.PIPE, text=True,
    env=dict(ENVIRONMENT, PATH=FAKE_DIRECTORY,
             FAKE_UTIL_ARGS=VERSION_ARGS_FILE, FAKE_UTIL_ANSWER=ANSWER,
             FAKE_UTIL_STATUS="0"),
    preexec_fn=lambda: os.close(1))
_, closed_errors = closed.communicate(timeout=60)
assert closed.returncode == 1, (closed.returncode, closed_errors)
assert "Traceback" not in closed_errors, closed_errors
assert closed_errors == "error: standard output has gone away\n", closed_errors
assert not os.path.exists(VERSION_ARGS_FILE), "ubertooth-util must not run"
print("ubertooth-specan-ui: -V with no standard output at all is reported,"
      " not a traceback")


# ---- a standard stream the shell closed before the tool started -----------
#
# The three states a redirection leaves are each a different thing to the
# interpreter: no stream at all, where the argument parser writes the usage
# text to standard error instead, which is not where the man page says it
# goes; a stream over a descriptor that cannot be written, which takes the
# text into the buffer and fails at the flush, where the interpreter meets it
# as it exits and ends the tool in status 120; and no standard error, where
# print(..., file=sys.stderr) writes to standard output instead, so a
# refusal lands among what the user asked for.
#
# The state has to be arranged between fork and exec, since it is about the
# descriptors the tool is started with.

OUTPUT_GONE = "error: standard output has gone away\n"


def read_only(*numbers):
    """Leave each of these descriptors open for reading only, which is `1<&0`
    or `2<&0`. Returns what Popen runs between fork and exec."""
    def prepare():
        for number in numbers:
            nowhere = os.open(os.devnull, os.O_RDONLY)
            os.dup2(nowhere, number)
            os.close(nowhere)
    return prepare


def ran_with(arguments, prepare, environment=None):
    """Run the tool with one of its streams as a shell could have left it."""
    running = subprocess.Popen(
        [sys.executable, SCRIPT] + list(arguments),
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        universal_newlines=True, env=environment or ENVIRONMENT,
        preexec_fn=prepare)
    output, errors = running.communicate(timeout=60)
    return running.returncode, output, errors


# -h with no standard output, which is `-h >&-`: the usage text was asked for
# and cannot be printed, so the loss is reported and the command fails. It
# used to print the text on standard error and exit 0, which says both that
# the text was delivered and that it went where a reader of the man page
# would not look for it.
status, output, errors = ran_with(("-h",), lambda: os.close(1))
assert status == 1, (status, output, errors)
assert output == "", output
assert errors == OUTPUT_GONE, errors
assert "usage:" not in errors, errors
assert "Traceback" not in errors, errors

# -h on a standard output that cannot be written, which is `-h 1<&0`: the
# text reaches the buffer and the flush is where it fails, so a different
# handler catches it; the line and the status are the same. The descriptor is
# pointed at the bin, so the flush the interpreter does as it exits -- which
# no handler can reach -- does not end the tool in status 120 instead.
status, output, errors = ran_with(("-h",), read_only(1))
assert status == 1, (status, output, errors)
assert output == "", output
assert errors == OUTPUT_GONE, errors
assert "Traceback" not in errors, errors
assert "Exception ignored" not in errors, errors

# -V and -h together, where both halves of the answer are lost: the line is
# said once, by whichever found the stream gone first, since there is nothing
# left to print after it. ubertooth-util is on the PATH here and must not be
# run, for the reason -V alone does not run it.
if os.path.exists(VERSION_ARGS_FILE):
    os.remove(VERSION_ARGS_FILE)
status, output, errors = ran_with(("-V", "-h"), lambda: os.close(1),
                                 environment=dict(
                                     ENVIRONMENT, PATH=FAKE_DIRECTORY,
                                     FAKE_UTIL_ARGS=VERSION_ARGS_FILE,
                                     FAKE_UTIL_ANSWER=ANSWER,
                                     FAKE_UTIL_STATUS="0"))
assert status == 1, (status, output, errors)
assert errors == OUTPUT_GONE, errors
assert not os.path.exists(VERSION_ARGS_FILE), "ubertooth-util must not run"

# a refused range with no standard error, which is `-l 2000 2>&-`: the line
# is lost, and what matters is where it does not go. It used to land on
# standard output, among what the user asked for.
status, output, errors = ran_with(("-l", "2000"), lambda: os.close(2))
assert status == 1, (status, output, errors)
assert output == "", output
assert errors == "", errors

# and that stream going does not cost the answer: -h still prints the usage
# text, on standard output, and exits 0
status, output, errors = ran_with(("-h",), lambda: os.close(2))
assert status == 0, (status, output, errors)
assert output.startswith("usage:"), output


def close_both():
    """Leave neither stream, which is `>&- 2>&-`."""
    os.close(1)
    os.close(2)


# with neither stream, which is `-h >&- 2>&-`, the status is all that is
# left: the usage text is written nowhere, and the bin this tool opens for
# its own diagnostics is not somewhere the user asked for it either.
status, output, errors = ran_with(("-h",), close_both)
assert status == 1, (status, output, errors)
assert output == "" and errors == "", (output, errors)

print("ubertooth-specan-ui: a standard stream the shell closed is reported,"
      " not a traceback")


# ---- and a descriptor the shell left open for reading only ----------------
#
# A stream is there and every write to it fails, including the flush the
# interpreter does as it exits, where no handler can reach it: the command
# used to end in status 120 and a message printed nowhere.

VERSION_ENVIRONMENT = dict(ENVIRONMENT, PATH=FAKE_DIRECTORY,
                           FAKE_UTIL_ARGS=VERSION_ARGS_FILE,
                           FAKE_UTIL_ANSWER=ANSWER,
                           FAKE_UTIL_STATUS="0")

# -V on a standard output that cannot be written, which is `-V 1<&0`: the
# answer is the child's to print, and the flush that orders this tool's lines
# before the child's has an empty buffer, so it succeeds and used to leave the
# loss to the child -- whose only trace of it is an exit status ubertooth-util
# does not fail for. So the descriptor is asked here, the loss is reported,
# and no child is started.
if os.path.exists(VERSION_ARGS_FILE):
    os.remove(VERSION_ARGS_FILE)
status, output, errors = ran_with(("-V",), read_only(1),
                                 environment=VERSION_ENVIRONMENT)
assert status == 1, (status, output, errors)
assert output == "", output
assert errors == OUTPUT_GONE, errors
assert not os.path.exists(VERSION_ARGS_FILE), "ubertooth-util must not run"

# A refused range with a standard error that cannot be written, which is
# `-l 2000 2<&0`: the line is lost as a closed stream's is, and the status is
# the tool's own. That stream is no longer this test's pipe here, so the
# status is what the case pins; what matters beside it is that standard
# output is left alone.
status, output, errors = ran_with(("-l", "2000"), read_only(2))
assert status == 1, (status, output, errors)
assert output == "", output

# and that stream going does not cost the answer: -h still prints the usage
# text, on standard output, and exits 0
status, output, errors = ran_with(("-h",), read_only(2))
assert status == 0, (status, output, errors)
assert output.startswith("usage:"), output

# with neither stream writable at once, which is `-h 1<&0 2<&0`, the status
# is all that is left
status, output, errors = ran_with(("-h",), read_only(1, 2))
assert status == 1, (status, output, errors)

print("ubertooth-specan-ui: a descriptor open for reading only costs the"
      " command neither its status nor its answer")


# ---- and a pipe whose reader has left -------------------------------------
#
# The third state, and the one the startup guard cannot see: on Linux a write
# of nothing to a pipe with no reader succeeds (fs/pipe.c, pipe_write(),
# "Null write succeeds"), so the descriptor passes for writable there and
# only a real write finds the reader gone. macOS refuses both. The flush the
# interpreter does as it exits then failed where no handler could reach it,
# which ended the command in status 120 on Linux and left the same command
# line with a different status on the two platforms. So it is pinned here
# rather than left to the read-only case above, which looks the same on both.

def broken_pipe(*numbers):
    """Point these descriptors at one pipe whose reader has already left,
    which is `2>&1 | <a reader that stops>` when both are named. Returns what
    Popen runs between fork and exec."""
    def prepare():
        reading, writing = os.pipe()
        os.close(reading)
        for number in numbers:
            os.dup2(writing, number)
        os.close(writing)
    return prepare


# a refused range, which is `-l 2000 2> <a reader that stops>`: the line is
# lost and the status is the command's own 1, not 120
status, output, errors = ran_with(("-l", "2000"), broken_pipe(2))
assert status == 1, (status, output, errors)
assert output == "", output

# and that stream going does not cost the answer: -h still prints the usage
# text, on standard output, and exits 0
status, output, errors = ran_with(("-h",), broken_pipe(2))
assert status == 0, (status, output, errors)
assert output.startswith("usage:"), output

# both streams the same pipe, which is `-h 2>&1 | <a reader that stops>`:
# the usage text and the line reporting its loss both go nowhere, and the
# status is all that is left
status, output, errors = ran_with(("-h",), broken_pipe(1, 2))
assert status == 1, (status, output, errors)

print("ubertooth-specan-ui: a standard error whose reader has left leaves"
      " the command its own exit status")


# ---- and none of that needs numpy or a Qt binding -------------------------
#
# The window does. The command line does not, and -V least of all: it is how a
# user says which tools they have, and a Qt binding that is missing or broken
# is exactly when they would be asked. Both modules are shadowed here by ones
# that refuse to import, which is what a machine without them looks like to
# the script.

WITHOUT_GUI = os.path.join(VERSION_WORK.name, "no-gui")
os.mkdir(WITHOUT_GUI)
with open(os.path.join(WITHOUT_GUI, "numpy.py"), "w") as refused:
    refused.write('raise ImportError("numpy is not installed here")\n')
os.mkdir(os.path.join(WITHOUT_GUI, "qtpy"))
with open(os.path.join(WITHOUT_GUI, "qtpy", "__init__.py"), "w") as refused:
    refused.write('raise ImportError("no Qt binding is installed here")\n')

no_gui = dict(ENVIRONMENT,
              PYTHONPATH=os.pathsep.join([WITHOUT_GUI,
                                          ENVIRONMENT["PYTHONPATH"]]),
              PATH=FAKE_DIRECTORY,
              FAKE_UTIL_ARGS=VERSION_ARGS_FILE,
              FAKE_UTIL_ANSWER=ANSWER,
              FAKE_UTIL_STATUS="0")

result = run(["-V"], no_gui)
assert result.returncode == 0, result
assert result.stdout == ANSWER, result
assert "ImportError" not in result.stderr, result

result = run(["-h"], no_gui)
assert result.returncode == 0, result
assert result.stdout.startswith("usage:"), result
assert result.stderr == "", result

result = run(["-l", "2000"], no_gui)
assert result.returncode == 1, result
assert result.stdout == "", result
assert result.stderr == \
    "error: frequency of 2000 MHz is below minimum frequency of 2402\n", result

# and a run that does open the window still needs them, which is what says
# the modules were really out of reach above
result = run([], no_gui)
assert result.returncode != 0, result
assert "numpy is not installed here" in result.stderr, result

# The same with numpy left alone and only the Qt binding shadowed, which is
# the case the design is for: a Qt binding that is missing or broken is what
# a user would be asking -V about. numpy is imported first, so the case
# above never reaches the Qt binding at all.
WITHOUT_QT = os.path.join(VERSION_WORK.name, "no-qt")
os.mkdir(WITHOUT_QT)
os.mkdir(os.path.join(WITHOUT_QT, "qtpy"))
with open(os.path.join(WITHOUT_QT, "qtpy", "__init__.py"), "w") as refused:
    refused.write('raise ImportError("no Qt binding is installed here")\n')

no_qt = dict(no_gui,
             PYTHONPATH=os.pathsep.join([WITHOUT_QT,
                                         ENVIRONMENT["PYTHONPATH"]]))

result = run(["-V"], no_qt)
assert result.returncode == 0, result
assert result.stdout == ANSWER, result
assert "ImportError" not in result.stderr, result

# The window still needs the Qt binding, which is what says it was really out
# of reach above. Only where numpy is installed: without it the window case
# stops at numpy, which is imported first, and this half would fail for a
# reason that has nothing to do with Qt.
numpy_installed = subprocess.run(
    [sys.executable, "-c", "import numpy"], env=ENVIRONMENT,
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0
if numpy_installed:
    result = run([], no_qt)
    assert result.returncode != 0, result
    assert "no Qt binding is installed here" in result.stderr, result
else:
    print("ubertooth-specan-ui: numpy is not installed, so the window's own "
          "need for a Qt binding is not checked")

print("ubertooth-specan-ui: -V, -h and the refusals need no Qt binding")
