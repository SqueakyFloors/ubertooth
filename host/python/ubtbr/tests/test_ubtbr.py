#!/usr/bin/env python3
"""ubertooth-btbr and the ubtbr modules ending when asked to, without a board.

Run as: test_ubtbr.py <path to the configured ubertooth-btbr>

A fake usb1 module stands in for the real one, so no device is opened: its
handle answers every read with a timeout, which is what a device that is up
and silent looks like, and records every write.

What is checked:

  * a command the device never answers still stops. The message loop used to
    wait for a message with no timeout and leave only when a state response
    said so, so a request the device never answered left the command thread
    waiting forever. Whoever then stopped it joined that thread with no
    timeout either, which is why the tool could not be interrupted: Ctrl-C at
    the prompt runs the same stop, so the key meant to recover the tool was
    what wedged it.

  * the receive thread ending on a packet that does not fit the framing says
    so instead of dying on a bare assert, and a close() afterwards still
    returns and closes the device.

  * a close() while the receive loop is stuck inside libusb returns rather
    than joining forever, and leaves the device open: closing the handle
    under a read still running there turns an exit into a crash.

  * a Ctrl-C in the instant a command starts leaves the prompt up with the
    board released. It used to leave the prompt holding a thread that never
    ran, and joining that thread ended the tool in a RuntimeError traceback
    with the board still claimed.

  * a command whose thread the interpreter refuses is reported on standard
    error and leaves the prompt able to take the next one. The thread is
    what releases the board the command claimed as it was built, so a
    refused thread used to leave the board claimed with nothing holding it:
    every later command was refused as still running, and the tool ended in
    a traceback.

  * a board that takes none of the requests the session opens with is reported
    with the firmware that behaves that way named, whichever of those requests
    it stops taking at, and a setup failure of another kind is not.

  * a packet a command decodes goes to standard output, with the time it was
    printed in front of it, and the progress a command makes stays on
    standard error as plain lines. Both used to be log lines on standard
    error, so piping the tool into anything got nothing.

  * the tool itself, driven through its prompt: "q" exits 0, where it used to
    raise TypeError because the handler for it was called without the line
    the user typed. A Ctrl-C with a command running returns to the prompt,
    having asked the device to go idle, and the tool then exits 0 too. Both
    cases hung before, since nothing let the interpreter exit while the
    command and receive threads were alive.
"""

import contextlib
import importlib.machinery
import importlib.util
import io
import logging
import os
import re
import signal
import struct
import subprocess
import sys
import tempfile
import time

WRAPPER = sys.argv[1]
# The ubtbr package sits one directory above this test.
MODULE_DIR = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))
# The deadline every wait here shares. Each case is bounded well below it, so
# a case that regresses fails on its own assertion instead of on the CTest
# timeout.
DEADLINE = 10

# Stands in for usb1. Only the pieces ubtbr uses are here, and none of them
# touch a device.
FAKE_USB1 = '''"""A fake usb1 module: no device is opened.

UBTBR_FAKE_WRITES names a file every bulk write is appended to, as its bytes
in hex and the timeout it was given, so a test can tell what the tool sent and
how long it was prepared to wait. UBTBR_FAKE_WRITE_FAILS_AFTER is how many
writes are taken before the rest time out, standing in for a device that stops
draining the endpoint, and UBTBR_FAKE_WRITE_ERROR makes those writes fail
some other way instead. UBTBR_FAKE_SETUP_DELAY is how long claiming the
interface takes, which holds the tool short of its prompt, with
UBTBR_FAKE_CLAIMED a file created once it is in there.
UBTBR_FAKE_CLAIM_ERROR makes claiming the interface fail instead: "busy" and
"access" are the two codes a board another program holds gives, on Linux and
on macOS, and anything else is some other claim failure. UBTBR_FAKE_WRITE_DELAY
holds each write, which keeps the tool short of its prompt with the receive
thread already running. UBTBR_FAKE_NO_DEVICE makes the context find nothing
to open, and UBTBR_FAKE_CLOSED names a file created when the handle is closed.

UBTBR_FAKE_SERIALS is the boards on the bus, in enumeration order, as
comma-separated serial numbers; "?" stands for a board whose serial cannot be
read, and "!" for one that answers neither the serial read nor an open, which
is what a board another program holds looks like. An entry may name its USB
ids as "<serial>/vvvv:pppp"; without them it is an Ubertooth One. Unset means
one board.
UBTBR_FAKE_OPENED names a file the serial of the board that was opened is
written to, which is how a test tells which board a selection reached.
UBTBR_FAKE_SERIAL_READS and UBTBR_FAKE_OPENS name files every serial read and
every open is appended to, which is how a test tells how many boards a
selection touched.

UBTBR_FAKE_LIST_ERROR makes listing the bus raise something the tool does not
expect, which is how a test reaches the way out of a failure nobody handled.
That is a bug's way out, so the tool is allowed its traceback there; what a
test asks is what else comes with it.

UBTBR_FAKE_READS names a file of packets the device sends, one per line in
hex: each is handed to the next read, and the reads after them time out as
usual. UBTBR_FAKE_READS_AFTER holds them back until that many writes have
gone out, which is how a test has a packet arrive while a command is
running rather than before the prompt is up, and UBTBR_FAKE_READ_DELAY holds
each of them, which spreads them over a session instead of handing them all
over at once.

UBTBR_FAKE_READ_FAILS makes the read raise USBErrorPipe instead of timing
out, once UBTBR_FAKE_READS_AFTER writes have gone out: that is a board that
left the USB bus with the session still up, which is what a power-cycled hub
port does to a running command.
"""

import os
import time


class USBError(Exception):
    # The libusb code, which usb1 keeps on the class as this does.
    value = -1


class USBErrorTimeout(USBError):
    value = -7


class USBErrorAccess(USBError):
    value = -3


class USBErrorPipe(USBError):
    pass


class USBErrorBusy(USBError):
    value = -6


class _Handle:
    def __init__(self):
        self.writes = 0
        self.reads = 0

    def claimInterface(self, interface):
        failure = os.environ.get("UBTBR_FAKE_CLAIM_ERROR")
        if failure == "busy":
            raise USBErrorBusy()
        if failure == "access":
            raise USBErrorAccess()
        if failure:
            raise USBError()
        marker = os.environ.get("UBTBR_FAKE_CLAIMED")
        if marker:
            open(marker, "w").close()
        time.sleep(float(os.environ.get("UBTBR_FAKE_SETUP_DELAY", "0")))

    def bulkWrite(self, endpoint, data, timeout=0):
        self.writes += 1
        path = os.environ.get("UBTBR_FAKE_WRITES")
        if path:
            with open(path, "a") as writes:
                writes.write("%s %d\\n" % (bytes(data).hex(), timeout))
        time.sleep(float(os.environ.get("UBTBR_FAKE_WRITE_DELAY", "0")))
        taken = os.environ.get("UBTBR_FAKE_WRITE_FAILS_AFTER")
        if taken is not None and self.writes > int(taken):
            if os.environ.get("UBTBR_FAKE_WRITE_ERROR"):
                raise USBError("failed")
            raise USBErrorTimeout("timeout")
        return len(data)

    def bulkRead(self, endpoint, length, timeout=0):
        canned = os.environ.get("UBTBR_FAKE_READS")
        after = int(os.environ.get("UBTBR_FAKE_READS_AFTER", "0"))
        if os.environ.get("UBTBR_FAKE_READ_FAILS") and self.writes >= after:
            raise USBErrorPipe("LIBUSB_ERROR_PIPE [-9]")
        if canned and self.writes >= after:
            with open(canned) as pending:
                packets = pending.read().split()
            if self.reads < len(packets):
                data = bytes.fromhex(packets[self.reads])
                self.reads += 1
                time.sleep(float(os.environ.get("UBTBR_FAKE_READ_DELAY", "0")))
                return data
        time.sleep(timeout / 1000.0)
        raise USBErrorTimeout("timeout")

    def close(self):
        marker = os.environ.get("UBTBR_FAKE_CLOSED")
        if marker:
            open(marker, "w").close()


class _Device:
    """One device on the bus, an Ubertooth One unless the test says otherwise."""

    def __init__(self, serial, vendor=0x1d50, product=0x6002):
        self._serial = serial
        self._vendor = vendor
        self._product = product

    def getVendorID(self):
        return self._vendor

    def getProductID(self):
        return self._product

    def getSerialNumber(self):
        # Reading the serial opens the board, so it is recorded as an open
        # too: a test can then tell that a selection by index touched no
        # board it was not asked about.
        self._note("UBTBR_FAKE_SERIAL_READS")
        self._note("UBTBR_FAKE_OPENS")
        # A board another program holds answers neither this nor an open.
        if self._serial in ("?", "!"):
            raise USBError("serial unreadable")
        return self._serial

    def open(self):
        self._note("UBTBR_FAKE_OPENS")
        if self._serial == "!":
            raise USBErrorAccess("access denied")
        marker = os.environ.get("UBTBR_FAKE_OPENED")
        if marker:
            with open(marker, "w") as opened:
                opened.write(self._serial)
        return _Handle()

    def _note(self, variable):
        path = os.environ.get(variable)
        if path:
            with open(path, "a") as noted:
                noted.write("%s\\n" % self._serial)


def _described(entry):
    """One UBTBR_FAKE_SERIALS entry: "<serial>" or "<serial>/vvvv:pppp"."""
    serial, _, ids = entry.partition("/")
    if not ids:
        return _Device(serial)
    vendor, _, product = ids.partition(":")
    return _Device(serial, int(vendor, 16), int(product, 16))


class USBContext:
    def getDeviceIterator(self, skip_on_error=False):
        if os.environ.get("UBTBR_FAKE_LIST_ERROR"):
            raise RuntimeError("listing the bus failed")
        if os.environ.get("UBTBR_FAKE_NO_DEVICE"):
            return iter(())
        serials = os.environ.get("UBTBR_FAKE_SERIALS", ONE_BOARD)
        return iter([_described(entry) for entry in serials.split(",")])
'''

# The one board the fake puts on the bus unless a test says otherwise.
ONE_BOARD = "1cc001038088cb956c43ad61c52000f5"
FAKE_USB1 = FAKE_USB1.replace("ONE_BOARD", repr(ONE_BOARD))

# Kept in a name of its own, so its finalizer removes the directory however
# the test ends, including on a failed assertion.
WORK = tempfile.TemporaryDirectory()
with open(os.path.join(WORK.name, "usb1.py"), "w") as fake:
    fake.write(FAKE_USB1)
WRITES_FILE = os.path.join(WORK.name, "writes")

# The modules live in the source tree, and a test must not leave a __pycache__
# behind in it.
sys.dont_write_bytecode = True
sys.path.insert(0, MODULE_DIR)
sys.path.insert(0, WORK.name)
import usb1
assert os.path.dirname(usb1.__file__) == WORK.name, usb1.__file__
from ubtbr import btctl, lmp


def request_packet(code):
    """The USB packet the device gets for a request that carries no payload."""
    payload = struct.pack("<I", code)
    return (b"S\x00" + struct.pack("<H", len(payload)) + payload).hex()


IDLE_PACKET = request_packet(btctl.BTCTL_IDLE_REQ)
INQUIRY_PACKET = request_packet(btctl.BTCTL_INQUIRY_REQ)
INQUIRY_SCAN_PACKET = request_packet(btctl.BTCTL_INQUIRY_SCAN_REQ)
# What a command says when the device did not answer the idle request, which
# is how the two ways of ending a stop are told apart below.
NO_ANSWER = "did not go idle"


class Recorder(logging.Handler):
    """Keeps what ubtbr reported, so a test can tell how a command ended.

    Two channels reach it. The progress a command makes is a log line, so it
    is a logging handler; a warning or a failure is one `warning: ` or
    `error: ` line written straight to standard error, so it also stands in
    for that stream. Both end up in the same list, since what a test asks is
    what was said and not how. What a command decodes goes to standard
    output instead, and is checked through the tool itself further down.

    Standing in for the stream is what pins these messages to standard error:
    a line that went to standard output instead would not be here. The real
    stream is written to as well, so a failing run still shows everything the
    test saw, and the logging handler ubtbr installs holds the original
    stream, so log lines are unaffected by this.
    """

    def __init__(self, stream):
        super().__init__()
        self.messages = []
        self._stream = stream

    def emit(self, record):
        self.messages.append(record.getMessage())

    def write(self, text):
        self.messages.append(text)
        return self._stream.write(text)

    def flush(self):
        self._stream.flush()

    def said(self, text):
        return any(text in message for message in self.messages)


records = Recorder(sys.stderr)
btctl.log.addHandler(records)
sys.stderr = records


def recorded_writes():
    """Every packet sent so far with the timeout it got, oldest first."""
    if not os.path.exists(WRITES_FILE):
        return []
    with open(WRITES_FILE) as recorded:
        return [line.split() for line in recorded.read().splitlines()]


def writes():
    """Every packet sent so far, oldest first."""
    return [packet for packet, timeout in recorded_writes()]


def wait_until(predicate):
    """True once the predicate holds, False if the deadline passes first."""
    limit = time.monotonic() + DEADLINE
    while time.monotonic() < limit:
        if predicate():
            return True
        time.sleep(0.05)
    return predicate()


# ---- a command the device never answers still stops ------------------------

class SilentDevice:
    """A BTCtl that takes requests and answers nothing."""

    def __init__(self):
        self.handler = None
        self.idle_requests = 0
        self.inquiry_requests = 0
        self.closed = False

    def register_msg_handler(self, handler):
        # Refused the way BTCtl refuses it, so what a caller sees here is
        # what it sees against a real device.
        if self.handler is not None:
            raise btctl.BTCtlBusyError("the previous command is still running")
        self.handler = handler

    def unregister_msg_handler(self, handler):
        # Compared the way BTCtl compares it: each read of a bound method
        # gives a new object, so "is" would never match.
        if self.handler != handler:
            raise btctl.BTCtlBusyError("another command holds it")
        self.handler = None

    def send_idle_cmd(self):
        self.idle_requests += 1

    def send_inquiry_cmd(self):
        self.inquiry_requests += 1

    def close(self):
        self.closed = True


device = SilentDevice()
command = btctl.BTCtlInquiryCmd(device)
assert command.daemon, "a command must not hold up an interpreter exiting"
command.start()
assert wait_until(lambda: device.inquiry_requests == 1), device.inquiry_requests
started = time.monotonic()
command.stop()
command.join(DEADLINE)
assert not command.is_alive(), "a command the device ignores must still stop"
took = time.monotonic() - started
assert took < DEADLINE, took
# the stop still asks the device to go idle
assert device.idle_requests == 1, device.idle_requests
# and the command reports itself over and lets go of the message handler, so
# the next command can be started
assert command.done(), "a command whose loop has ended must report it"
assert device.handler is None, "a command that ended must release the handler"
# and it says why it ended, rather than looking like a clean stop
assert records.said(NO_ANSWER), records.messages
print("ubtbr: a command the device never answers stops in %.2fs" % took)

# a command that is answered ends on the answer, not on the stop running out
records.messages.clear()
device = SilentDevice()
command = btctl.BTCtlInquiryCmd(device)
command.start()
started = time.monotonic()
command.stop()
device.handler(btctl.BTCTL_STATE_RESP,
               (btctl.BTCTL_STATE_STANDBY, btctl.BTCTL_REASON_SUCCESS))
command.join(DEADLINE)
assert not command.is_alive(), "an answered stop must end the command"
took = time.monotonic() - started
assert took < DEADLINE, took
assert records.said("Inquiry done"), records.messages
assert not records.said(NO_ANSWER), records.messages
print("ubtbr: an answered stop ends the command in %.2fs" % took)

# a stop the device does not even take the request for: asking for it must not
# raise, since what asks is the keyboard interrupt handler
class RefusingDevice(SilentDevice):
    def send_idle_cmd(self):
        super().send_idle_cmd()
        raise usb1.USBErrorTimeout("timeout")


device = RefusingDevice()
command = btctl.BTCtlInquiryCmd(device)
command.start()
command.stop()
command.join(DEADLINE)
assert not command.is_alive(), "a stop that could not be sent must still stop"
assert device.idle_requests == 1, device.idle_requests
print("ubtbr: a stop the device will not take still ends the command")

# a command whose opening request the device will not take either. It runs on
# its own thread, so an error let out of there is a thread traceback nobody
# can report.
records.messages.clear()


class RefusingStartDevice(SilentDevice):
    def send_inquiry_cmd(self):
        super().send_inquiry_cmd()
        raise usb1.USBErrorTimeout("timeout")


device = RefusingStartDevice()
command = btctl.BTCtlInquiryCmd(device)
command.start()
command.join(DEADLINE)
assert not command.is_alive(), "a command that could not start must end"
assert command.done(), "a command whose loop never ran must still report it"
assert device.handler is None, "a command that ended must release the handler"
assert records.said("BTCtlInquiryCmd stopped: USBErrorTimeout: timeout"), \
    records.messages
print("ubtbr: a command the device will not start is reported, not raised")

# ---- a diagnostic is one line on standard error ---------------------------

# The module used to report its own warnings and failures as log lines,
# "HH:MM:SS | WARNING | ...", where every other tool in the tree writes one
# "warning: " or "error: " line. Both are pinned here: the prefix, the
# stream, and that nothing goes to standard output.
device = SilentDevice()
command = btctl.BTCtlInquiryCmd(device)
command.start()
assert wait_until(lambda: device.inquiry_requests == 1), device.inquiry_requests
warned, said = io.StringIO(), io.StringIO()
with contextlib.redirect_stderr(warned), contextlib.redirect_stdout(said):
    command.stop()
    command.join(DEADLINE)
assert not command.is_alive(), "the command must still stop"
assert warned.getvalue() == ("warning: BTCtlInquiryCmd: the device did not go "
                             "idle, stopping anyway\n"), repr(warned.getvalue())
assert said.getvalue() == "", said.getvalue()

device = RefusingStartDevice()
command = btctl.BTCtlInquiryCmd(device)
failed, said = io.StringIO(), io.StringIO()
with contextlib.redirect_stderr(failed), contextlib.redirect_stdout(said):
    command.start()
    command.join(DEADLINE)
assert not command.is_alive(), "a command that could not start must end"
assert failed.getvalue() == ("error: BTCtlInquiryCmd stopped: "
                             "USBErrorTimeout: timeout\n"), \
    repr(failed.getvalue())
assert said.getvalue() == "", said.getvalue()

# and the LMP decoder's one warning, which reaches warn() through an import
# inside the function: btctl imports lmp, so lmp cannot import it back at
# module scope, and a test is what catches that import going wrong.
link_manager = lmp.LMPMaster(None)
unknown, said = io.StringIO(), io.StringIO()
with contextlib.redirect_stderr(unknown), contextlib.redirect_stdout(said):
    # Opcode 0, which no state handles.
    assert link_manager.receive(0, b"\x00") is False
assert unknown.getvalue() == \
    "warning: LMP opcode 0x0 is not handled in state 1\n", \
    repr(unknown.getvalue())
assert said.getvalue() == "", said.getvalue()
print("ubtbr: a warning and a failure are each one line on standard error")

# ---- a command of a super command that will not stop ----------------------

# Driven through the pieces a super command uses on itself: the thread it
# would run them from is what the case is about.
class WedgedCmd(btctl.BTCtlCmd):
    """A command whose loop does not look at the stop."""

    # Its own allowance: what is being tested is giving up, not how long the
    # tool waits before it does.
    JOIN_TIMEOUT = 0.5

    def _run_loop(self):
        time.sleep(DEADLINE * 10)


class BareSuperCmd(btctl.BTCtlSuperCmd):
    def run_once(self):
        self._wait(DEADLINE)


device = SilentDevice()
super_command = BareSuperCmd(device)
assert super_command._start_cmd(WedgedCmd) is not None
super_command._stop_cmd()
# the wedged command still owns the message handler, so the handle on it is
# kept rather than dropped and tripped over by the next one
assert super_command._cmd is not None, "a command that did not stop must be kept"
assert device.handler is not None, device.handler
assert super_command._start_cmd(WedgedCmd) is None, \
    "no second command while the first still holds the handler"
assert super_command.stopped() and super_command.done(), \
    "a super command that cannot stop its command is over"
print("ubtbr: a command that will not stop does not take the next one with it")

# ---- the prompt, with a command that will not stop ------------------------

# The prompt lives in the tool, which only runs main() when run as a program.
loader = importlib.machinery.SourceFileLoader("ubertooth_btbr", WRAPPER)
tool_module = importlib.util.module_from_spec(
    importlib.util.spec_from_loader(loader.name, loader))
loader.exec_module(tool_module)


class WedgedSuperCmd(btctl.BTCtlSuperCmd):
    """A super command whose thread does not look at the stop either."""

    JOIN_TIMEOUT = 0.5

    def run(self):
        self._start_cmd(WedgedCmd)
        time.sleep(DEADLINE * 10)


device = SilentDevice()
prompt = tool_module.BTCtlPrompt(device)
assert prompt.start_cmd(WedgedSuperCmd) is not None
assert wait_until(lambda: device.handler is not None), "no command started"
prompt.stop_cmd()
assert prompt._cmd is not None, "a command that did not stop must be kept"
# typing another command must not build one behind the one still holding the
# message handler, whatever the kept one reports about being done
assert prompt.start_cmd(btctl.BTCtlInquiryCmd) is None, \
    "no second command while the first still holds the handler"
assert device.inquiry_requests == 0, device.inquiry_requests
# and leaving must not take the device from under it either
prompt.do_exit("q")
assert not device.closed, \
    "the device must stay open while a command can still write to it"
# and a session that ends with the board still held did not do all it was
# asked, so it must not report success
assert prompt.abandoned(), "leaving the device claimed is not a clean exit"
print("ubtbr: the prompt stays usable with a command that will not stop")

# ---- a super command that gives up and then ends cleanly ------------------

# The harder shape of the case above. A super command that gives up on a
# child stops itself, so its own thread ends and the prompt's join succeeds:
# the prompt rightly lets go of it, while the child it left behind still owns
# the device's one message handler. The next command typed therefore has to
# be refused by the device, not by the prompt, and the refusal has to come
# back as an error rather than out of the prompt as a traceback.
class GivingUpSuperCmd(btctl.BTCtlSuperCmd):
    """A super command running one child that does not look at its stop."""

    def run_once(self):
        self._start_cmd(WedgedCmd)
        self._wait(0.2)
        self._stop_cmd()


device = SilentDevice()
prompt = tool_module.BTCtlPrompt(device)
assert prompt.start_cmd(GivingUpSuperCmd) is not None
assert wait_until(lambda: device.handler is not None), "no child started"
assert wait_until(lambda: not prompt._cmd.is_alive()), \
    "a super command that gave up must end"
prompt.stop_cmd()
assert prompt._cmd is None, "a super command that ended must be let go of"
assert device.handler is not None, "the child still holds the handler"
refused, said = io.StringIO(), io.StringIO()
with contextlib.redirect_stderr(refused), contextlib.redirect_stdout(said):
    assert prompt.onecmd("inquiry") is None, "a refused command must not exit"
assert device.inquiry_requests == 0, device.inquiry_requests
assert refused.getvalue() == \
    "error: inquiry: the previous command is still running\n", \
    repr(refused.getvalue())
assert said.getvalue() == "", said.getvalue()
print("ubtbr: a command left behind by a super command is reported, not raised")

# the refusal can also land on a command's own thread, where a traceback is
# nobody's to report: a command that runs commands of its own starts them
# from there
device = SilentDevice()
orphan = WedgedCmd(device)
orphan.start()
assert wait_until(lambda: device.handler is not None), "no command started"
thrown = io.StringIO()
with contextlib.redirect_stderr(thrown):
    refused_super = GivingUpSuperCmd(device)
    refused_super.start()
    refused_super.join(DEADLINE)
assert not refused_super.is_alive(), "a refused super command must end"
# One error line and nothing else: a traceback from in there is nobody's to
# report, and the refusal is what the user has to be told.
assert thrown.getvalue() == ("error: GivingUpSuperCmd stopped: BTCtlBusyError: "
                             "the previous command is still running\n"), \
    repr(thrown.getvalue())
print("ubtbr: a refusal on a command's own thread is reported, not a traceback")



# ---- a packet that does not fit the framing -------------------------------

class BadFramingDevice:
    """A USB handle that answers reads with packets the device never sends."""

    def __init__(self, packets):
        self.packets = list(packets)
        self.closed = False

    def claimInterface(self, interface):
        pass

    def bulkWrite(self, endpoint, data, timeout=0):
        return len(data)

    def bulkRead(self, endpoint, length, timeout=0):
        if not self.packets:
            time.sleep(timeout / 1000.0)
            raise usb1.USBErrorTimeout("timeout")
        return self.packets.pop(0)

    def close(self):
        self.closed = True


# a continuation packet where a message must start, and a start packet whose
# length does not match the packet it came in
for packets in ([b"C" + bytes(63)],
                [b"S\x00" + struct.pack("<H", 120) + bytes(8)],
                [b""]):
    handle = BadFramingDevice(packets)
    bt = btctl.BTCtl(handle)
    bt.connect()
    assert wait_until(lambda: bt.rx_error() is not None), \
        ("the receive thread must report what stopped it", packets)
    assert isinstance(bt.rx_error(), btctl.BTCtlFramingError), bt.rx_error()
    started = time.monotonic()
    bt.close()
    took = time.monotonic() - started
    assert took < DEADLINE, took
    assert handle.closed, "close() must still close the device"
print("ubtbr: a malformed packet stops the receive thread with a reason")

# ---- a receive loop stuck inside libusb ------------------------------------

class StuckDevice(BadFramingDevice):
    def bulkRead(self, endpoint, length, timeout=0):
        time.sleep(DEADLINE * 10)


handle = StuckDevice([])
bt = btctl.BTCtl(handle)
bt.connect()
started = time.monotonic()
bt.close()
took = time.monotonic() - started
assert took < DEADLINE, took
assert not handle.closed, \
    "the device must stay open while a read is still running in libusb"
print("ubtbr: close() returns in %.2fs with the receive loop stuck" % took)

# ---- exiting with a command the prompt has lost track of ------------------

# The device itself is the only thing that knows a command still owns the
# messages, so it is where the refusal to close belongs: the prompt rightly
# let go of the super command once it ended, and closing the handle under the
# child it left behind is how a clean exit turns into a crash.
handle = BadFramingDevice([])
bt = btctl.BTCtl(handle)
bt.connect()
prompt = tool_module.BTCtlPrompt(bt)
assert prompt.start_cmd(GivingUpSuperCmd) is not None
assert wait_until(lambda: not prompt._cmd.is_alive()), \
    "a super command that gave up must end"
prompt.stop_cmd()
assert prompt._cmd is None, "a super command that ended must be let go of"
assert bt._msg_handler is not None, "the child still owns the messages"
with contextlib.redirect_stderr(io.StringIO()):
    prompt.do_exit("q")
assert not handle.closed, \
    "the device must stay open while a command can still write to it"
# only close() knows it kept the device open here, since the prompt had
# already let go of the command: it has to say so, or the session reports
# success having abandoned a claimed board
assert prompt.abandoned(), "an exit that left the device claimed is not clean"
print("ubtbr: exiting leaves the device open for a command nobody holds")

# ---- a message that stopped part way --------------------------------------

# A message longer than one packet goes out as a start packet and
# continuations. The device keeps what it has and takes only the rest: a
# fresh start packet is a framing error its firmware does not come back
# from. So a message that stopped part way has to end the sending, not free
# the connection for the next command to frame a new one.
class StallingHandle:
    """A USB handle that takes the first write of each message and no more."""

    def __init__(self):
        self.sent = []
        self.closed = False

    def claimInterface(self, interface):
        pass

    def bulkWrite(self, endpoint, data, timeout=0):
        self.sent.append(bytes(data)[:1])
        if len(self.sent) > 1:
            raise usb1.USBErrorTimeout("timeout")
        return len(data)

    def bulkRead(self, endpoint, length, timeout=0):
        time.sleep(timeout / 1000.0)
        raise usb1.USBErrorTimeout("timeout")

    def close(self):
        self.closed = True


handle = StallingHandle()
bt = btctl.BTCtl(handle)
long_message = bytes(200)
try:
    bt._send_usb(long_message)
except usb1.USBError:
    pass
else:
    raise AssertionError("the continuation did not fail")
assert handle.sent == [b"S", b"C"], handle.sent
# nothing else may be framed onto a device waiting for the rest of that one
for again in (long_message, bytes(4)):
    try:
        bt._send_usb(again)
    except btctl.BTCtlFramingError:
        pass
    else:
        raise AssertionError("a second message was framed anyway")
assert handle.sent == [b"S", b"C"], handle.sent
# and a stop asked for in that state is reported, not raised: what asks is
# the keyboard interrupt handler
command = btctl.BTCtlInquiryCmd(bt)
command.stop()
print("ubtbr: a message that stopped part way ends the sending")

# ---- a command that ends after the prompt gave up on it -------------------

# Giving up has to be a pause, not the end of the session: a command that
# ends later releases the handler, and the prompt must notice rather than
# refusing every command for as long as the tool runs.
class SlowCmd(btctl.BTCtlCmd):
    """A command that ignores its stop, then ends on its own."""

    JOIN_TIMEOUT = 0.2
    LINGER = 1

    def _run_loop(self):
        time.sleep(self.LINGER)


device = SilentDevice()
prompt = tool_module.BTCtlPrompt(device)
assert prompt.start_cmd(SlowCmd) is not None
prompt.stop_cmd()
assert prompt._cmd is not None, "a command that outlived its stop is kept"
assert prompt.start_cmd(btctl.BTCtlInquiryCmd) is None, "must refuse while held"
assert wait_until(lambda: device.handler is None), "the command never ended"
assert prompt.start_cmd(btctl.BTCtlInquiryCmd) is not None, \
    "the prompt must take commands again once the handler is free"
assert wait_until(lambda: device.inquiry_requests == 1), device.inquiry_requests
prompt.stop_cmd()
assert prompt._cmd is None, prompt._cmd
print("ubtbr: the prompt recovers when a command it gave up on ends")

# ---- a Ctrl-C in the instant a command starts -----------------------------

# Starting a command claims the device, starts the thread that will release
# it again, and hands the prompt the handle its stop needs. A Ctrl-C landing
# between any two of those left the prompt holding nothing while the device
# stayed claimed, or holding a thread that never ran, which cannot be joined:
# that RuntimeError came out of main() as a traceback. The signal below is a
# real one, sent from inside the command being started, so it arrives in that
# window the way a key press does.
class InterruptedBeforeThreadCmd(btctl.BTCtlInquiryCmd):
    """Interrupted once it is built, before its thread runs."""

    def start(self):
        os.kill(os.getpid(), signal.SIGINT)
        super().start()


class InterruptedWhileClaimingCmd(btctl.BTCtlInquiryCmd):
    """Interrupted as it claims the device's messages."""

    def __init__(self, bt):
        super().__init__(bt)
        os.kill(os.getpid(), signal.SIGINT)


class InterruptedAfterThreadCmd(btctl.BTCtlInquiryCmd):
    """Interrupted once its thread runs, before the prompt is given it."""

    def start(self):
        super().start()
        os.kill(os.getpid(), signal.SIGINT)


for interrupted_cls in (InterruptedBeforeThreadCmd,
                        InterruptedWhileClaimingCmd,
                        InterruptedAfterThreadCmd):
    name = interrupted_cls.__name__
    device = SilentDevice()
    prompt = tool_module.BTCtlPrompt(device)
    arrived = False
    try:
        prompt.start_cmd(interrupted_cls)
    except KeyboardInterrupt:
        arrived = True
        # What the prompt's own handler runs for a Ctrl-C.
        try:
            prompt.do_idle()
        except RuntimeError as e:
            raise AssertionError("%s: the interrupt handler raised: %s"
                                 % (name, e))
    assert arrived, (name, "the interrupt never arrived")
    assert prompt._cmd is None, (name, prompt._cmd)
    # and the board is let go of, rather than left claimed by a command
    # nothing holds
    assert device.handler is None, (name, "the board is still claimed")
    assert device.idle_requests >= 1, (name, device.idle_requests)
    # the prompt is where the user is left: it takes a command again
    inquiries = device.inquiry_requests
    assert prompt.start_cmd(btctl.BTCtlInquiryCmd) is not None, name
    assert wait_until(lambda: device.inquiry_requests == inquiries + 1), \
        (name, device.inquiry_requests)
    prompt.stop_cmd()
    assert prompt._cmd is None, (name, prompt._cmd)
print("ubtbr: a Ctrl-C as a command starts returns to the prompt, board free")

# and it comes out of the typed command the way a key press does: onecmd
# catches what a command can raise, which the interrupt is not.
device = SilentDevice()
prompt = tool_module.BTCtlPrompt(device)
typed = tool_module.BTCtlInquiryCmd
tool_module.BTCtlInquiryCmd = InterruptedBeforeThreadCmd
arrived = False
try:
    prompt.onecmd("inquiry")
except KeyboardInterrupt:
    arrived = True
    prompt.do_idle()
finally:
    tool_module.BTCtlInquiryCmd = typed
assert arrived, "onecmd swallowed the interrupt"
assert prompt._cmd is None, prompt._cmd
assert device.handler is None, "the board is still claimed"
print("ubtbr: the interrupt comes out of the command that was typed")

# ---- a command whose thread the interpreter refuses -----------------------

# Building the command claims the device's one message handler, and its
# thread is what gives that back. A thread the interpreter refuses to start
# therefore left the board claimed with nothing holding it: the prompt held
# no command, every later command was refused as still running, and the
# RuntimeError came out of main() as a traceback.
class RefusedThreadCmd(btctl.BTCtlInquiryCmd):
    """A command the interpreter will not give a thread to."""

    def start(self):
        # Worded as the interpreter words it when it is out of threads.
        raise RuntimeError("can't start new thread")


class RefusedThreadSuperCmd(btctl.BTCtlSuperCmd):
    """A super command the interpreter will not give a thread to.

    It claims nothing of its own, so there is nothing to give back here: the
    prompt still has to report it rather than let the RuntimeError out.
    """

    def start(self):
        raise RuntimeError("can't start new thread")

    def run_once(self):
        self._wait(DEADLINE)


for refused_cls in (RefusedThreadCmd, RefusedThreadSuperCmd):
    name = refused_cls.__name__
    device = SilentDevice()
    prompt = tool_module.BTCtlPrompt(device)
    try:
        prompt.start_cmd(refused_cls)
    except btctl.BTCtlError as e:
        assert str(e) == "could not start the command: can't start new thread", \
            (name, str(e))
    except RuntimeError as e:
        raise AssertionError("%s: raised past the prompt: %s" % (name, e))
    else:
        raise AssertionError("%s: the refused thread was not reported" % name)
    assert prompt._cmd is None, (name, prompt._cmd)
    # the board is let go of, rather than left claimed by a command nothing
    # holds
    assert device.handler is None, (name, "the board is still claimed")
    # and the prompt is where the user is left: it takes a command again
    assert prompt.start_cmd(btctl.BTCtlInquiryCmd) is not None, name
    assert wait_until(lambda: device.inquiry_requests == 1), \
        (name, device.inquiry_requests)
    prompt.stop_cmd()
    assert prompt._cmd is None, (name, prompt._cmd)
print("ubtbr: a command whose thread is refused leaves the board free")

# and the user gets one error line on standard error, with the prompt left
# up: the refusal is reported the way any other command that could not run is
device = SilentDevice()
prompt = tool_module.BTCtlPrompt(device)
typed = tool_module.BTCtlInquiryCmd
tool_module.BTCtlInquiryCmd = RefusedThreadCmd
refused, said = io.StringIO(), io.StringIO()
try:
    with contextlib.redirect_stderr(refused), contextlib.redirect_stdout(said):
        assert prompt.onecmd("inquiry") is None, "a refused command must not exit"
finally:
    tool_module.BTCtlInquiryCmd = typed
assert refused.getvalue() == \
    "error: inquiry: could not start the command: can't start new thread\n", \
    repr(refused.getvalue())
assert said.getvalue() == "", said.getvalue()
assert prompt._cmd is None, prompt._cmd
assert device.handler is None, "the board is still claimed"
# the next command typed runs, rather than being refused as still running
assert prompt.start_cmd(btctl.BTCtlInquiryCmd) is not None
assert wait_until(lambda: device.inquiry_requests == 1), device.inquiry_requests
prompt.stop_cmd()
print("ubtbr: a refused thread is one error line, and the prompt stays up")

# the same from a command that runs commands of its own, where the refusal
# lands on that command's thread: a traceback from in there is nobody's to
# report, and the child's claim still has to go back
class RefusedChildSuperCmd(btctl.BTCtlSuperCmd):
    """A super command whose child will not get a thread."""

    def run_once(self):
        self._start_cmd(RefusedThreadCmd)
        self._wait(DEADLINE)


device = SilentDevice()
super_command = RefusedChildSuperCmd(device)
try:
    super_command._start_cmd(RefusedThreadCmd)
except RuntimeError:
    pass
else:
    raise AssertionError("the refused thread was not reported")
assert super_command._cmd is None, super_command._cmd
assert device.handler is None, "the board is still claimed"

device = SilentDevice()
thrown = io.StringIO()
with contextlib.redirect_stderr(thrown):
    super_command = RefusedChildSuperCmd(device)
    super_command.start()
    super_command.join(DEADLINE)
assert not super_command.is_alive(), "a super command with no child must end"
assert thrown.getvalue() == ("error: RefusedChildSuperCmd stopped: "
                             "RuntimeError: can't start new thread\n"), \
    repr(thrown.getvalue())
assert device.handler is None, "the board is still claimed"
print("ubtbr: a child whose thread is refused is reported, not a traceback")

# ---- the tool, driven through its prompt ----------------------------------

ENVIRONMENT = dict(
    os.environ,
    PYTHONPATH=os.pathsep.join((WORK.name, MODULE_DIR)),
    PYTHONDONTWRITEBYTECODE="1",
    UBTBR_FAKE_WRITES=WRITES_FILE,
)


# The wrapper puts its own module directory ahead of PYTHONPATH, so check
# that the fake still wins where the tool itself looks, before any case runs
# the tool. Otherwise these cases would quietly open whatever board is
# plugged in.
found = subprocess.run(
    [sys.executable, "-c",
     "import sys; sys.path.insert(0, %r); import usb1; print(usb1.__file__)"
     % MODULE_DIR],
    capture_output=True, text=True, env=ENVIRONMENT, timeout=DEADLINE)
assert os.path.dirname(found.stdout.strip()) == WORK.name, \
    ("the tool would import the real usb1 and open a board", found.stdout,
     found.stderr)


def tool(environment=None, arguments=()):
    if os.path.exists(WRITES_FILE):
        os.remove(WRITES_FILE)
    return subprocess.Popen(
        [sys.executable, WRAPPER] + list(arguments),
        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, text=True,
        env=environment or ENVIRONMENT)


def ran(arguments=(), typed="", **fakes):
    """Run the tool once and return (exit status, standard out, standard err)."""
    running = tool(dict(ENVIRONMENT, **fakes), arguments)
    output, errors = running.communicate(typed, timeout=DEADLINE * 2)
    return running.returncode, output, errors


# "q" at the prompt exits 0. It used to call the handler for it without the
# line the user typed, which the handler takes, so the tool ended in a
# TypeError, and then hung: the receive thread kept the interpreter alive.
running = tool()
output, errors = running.communicate("q\n", timeout=DEADLINE * 2)
assert running.returncode == 0, (running.returncode, output, errors)
assert "Traceback" not in errors, errors
assert "Type ? to list commands" in errors, errors
assert IDLE_PACKET in writes(), writes()
# and every request was given a timeout. Without one, a device that takes
# none of them leaves the tool inside a libusb call no interrupt can reach.
assert recorded_writes(), recorded_writes()
for packet, timeout in recorded_writes():
    assert int(timeout) > 0, (packet, timeout)
print("ubtbr: q at the prompt exits 0")

# a device that takes none of the requests: the tool says so on standard
# error and exits 1, where the write used to wait forever. It also names the
# firmware that behaves that way, since a board carrying bluetooth_rxtx offers
# the endpoint this tool writes to but never reads it, and that is what every
# report of the tool hanging shortly after start has turned out to be.
WRONG_FIRMWARE = "note: a board carrying bluetooth_rxtx never reads these"
running = tool(dict(ENVIRONMENT, UBTBR_FAKE_WRITE_FAILS_AFTER="0"))
output, errors = running.communicate("", timeout=DEADLINE * 2)
assert running.returncode == 1, (running.returncode, output, errors)
assert output == "", output
assert "error: could not set up the Ubertooth" in errors, errors
assert WRONG_FIRMWARE in errors, errors
assert "Traceback" not in errors, errors
# The way out closes the device, and the board will not take the idle request
# either: that warning is one "warning: " line, not a log line, which is what
# the whole run is checked for here.
assert ("warning: could not ask the device to go idle while closing it: "
        "timeout\n") in errors, errors
assert "| WARNING |" not in errors and "| ERROR |" not in errors, errors
print("ubtbr: a device that takes no request is reported, not waited on")

# and the same when the requests stop being taken part way through the three
# the session opens with, which is the shape a real board gives: the endpoint
# nothing reads holds the first two, and the third, the one that fills it,
# waits.
running = tool(dict(ENVIRONMENT, UBTBR_FAKE_WRITE_FAILS_AFTER="2"))
output, errors = running.communicate("", timeout=DEADLINE * 2)
assert running.returncode == 1, (running.returncode, output, errors)
assert output == "", output
assert "error: could not set up the Ubertooth" in errors, errors
assert WRONG_FIRMWARE in errors, errors
assert "Traceback" not in errors, errors
print("ubtbr: the request that fills the endpoint is reported the same way")

# a setup request that failed some other way is not a board to reflash, so it
# gets the error without that note
running = tool(dict(ENVIRONMENT, UBTBR_FAKE_WRITE_FAILS_AFTER="0",
                    UBTBR_FAKE_WRITE_ERROR="1"))
output, errors = running.communicate("", timeout=DEADLINE * 2)
assert running.returncode == 1, (running.returncode, output, errors)
assert output == "", output
assert "error: could not set up the Ubertooth" in errors, errors
assert "bluetooth_rxtx" not in errors, errors
assert "Traceback" not in errors, errors
print("ubtbr: a setup failure that is not a silent endpoint says only that")

# a device that stops taking requests once the prompt is up: that command
# fails, the prompt stays, and the tool still exits 0. The three writes taken
# are the ones the tool sends before its prompt.
running = tool(dict(ENVIRONMENT, UBTBR_FAKE_WRITE_FAILS_AFTER="3"))
# the first of these is sent from the command's thread, the second from the
# prompt's, and neither may come out as a traceback
output, errors = running.communicate("inquiry\nbdaddr 11:22:33:44:55:66\nq\n",
                                     timeout=DEADLINE * 2)
assert running.returncode == 0, (running.returncode, output, errors)
assert "error: bdaddr 11:22:33:44:55:66: timeout" in errors, errors
assert "Traceback" not in errors, errors
assert "Exception in thread" not in errors, errors
print("ubtbr: a request the device will not take ends the command, not the tool")

# a board that left the USB bus with the session still up: the thread reading
# it ends on the USB failure, nothing restarts it, and no message arrives for
# the rest of the session. The session therefore did not do what it was asked,
# and exits 1 -- it used to report every step correctly and then exit 0, so a
# script could not tell this from a run where every command worked. The reads
# fail only once the three setup writes have gone out, so the failure lands
# where a hub port power-cycled under a running command puts it.
running = tool(dict(ENVIRONMENT, UBTBR_FAKE_READ_FAILS="1",
                    UBTBR_FAKE_READS_AFTER="3"))
output, errors = running.communicate("inquiry\nq\n", timeout=DEADLINE * 2)
assert running.returncode == 1, (running.returncode, output, errors)
assert "error: the receive thread stopped: USBErrorPipe" in errors, errors
assert "Traceback" not in errors, errors
assert "Exception in thread" not in errors, errors
print("ubtbr: a board that left the bus under the session exits 1")

# an interrupt while a command that runs commands of its own is going: the
# prompt joins whatever it started, and a super command is not a command.
# A command is then typed, because running one after the advertised
# "discoverable" is what the report is about: the interrupt has to have left
# the message handler free, not just returned to the prompt.
running = tool()
running.stdin.write("discoverable\n")
running.stdin.flush()
assert wait_until(lambda: INQUIRY_SCAN_PACKET in writes()), writes()
sent = writes()
# The command below is typed only once the interrupt has been handled, which
# is what the prompt the tool writes for each line it reads says: the third
# one is written for the read that takes that command, and the prompt only
# gets there by way of the KeyboardInterrupt the signal raised. A line
# written straight after the signal instead races it, and can be taken off
# the pipe by the very read the signal interrupts: the interrupt then comes
# out of that read in place of the line, which loses it the way Ctrl-C loses
# the line being typed. The device cannot stand in for this, because the
# idle request the interrupt sends is the same one "discoverable" sends
# every time it swaps its two scans.
seen = bytearray()


def prompts_printed():
    """How many lines the tool has asked for, as far as it has run.

    The prompt is on standard error, so that is the stream read here; the
    progress lines come with it, which is what the count skips past.
    """
    try:
        seen.extend(os.read(running.stderr.fileno(), 4096))
    except BlockingIOError:
        pass
    return seen.count(tool_module.BTCtlPrompt.PROMPT.encode())


def what_the_tool_did():
    """What to report if a wait below runs out.

    Neither wait reaches communicate(), so nothing else would drain standard
    output. Only called from a failing assert, where consuming it costs
    nothing.
    """
    os.set_blocking(running.stdout.fileno(), False)
    try:
        output = os.read(running.stdout.fileno(), 1 << 16)
    except BlockingIOError:
        output = b""
    finally:
        os.set_blocking(running.stdout.fileno(), True)
    return ("stdout: %r, stderr: %r, exit: %s"
            % (output, bytes(seen), running.poll()))


os.set_blocking(running.stderr.fileno(), False)
assert wait_until(lambda: prompts_printed() >= 2), what_the_tool_did()
running.send_signal(signal.SIGINT)
assert wait_until(lambda: prompts_printed() >= 3), what_the_tool_did()
os.set_blocking(running.stderr.fileno(), True)
output, errors = running.communicate("inquiry\nq\n", timeout=DEADLINE * 2)
# whatever was read above to see the prompts, so a failure below still shows
# everything the tool printed. Decoded leniently because these are raw pipe
# bytes, and a failure must not turn into a decode error.
errors = seen.decode(errors="replace") + errors
assert running.returncode == 0, (running.returncode, output, errors)
assert "Traceback" not in errors, errors
assert "Exception in thread" not in errors, errors
assert "still running" not in errors, errors
# the prompt took the command rather than refusing it, which is what it does
# when the interrupt left the stopped command holding the message handler.
# Checked separately, so that failure cannot be mistaken for a lost line.
assert "Not idle" not in errors, errors
assert INQUIRY_PACKET in writes()[len(sent):], (sent, writes())
print("ubtbr: a command runs after Ctrl-C ended a discoverable")

# ---- what goes to which stream --------------------------------------------
#
# A packet a command decodes used to be a log line on standard error, of the
# form "HH:MM:SS | INFO | RX ...", so a session piped into anything got
# nothing. It now goes to standard output with the time in front of it, and
# the progress a command makes stays on standard error without one.

READS_FILE = os.path.join(WORK.name, "reads")
# The address the fake device reports, as the FHS packet carries it: the
# lowest 24 bits are the LAP, then the UAP, then the NAP.
FAKE_BDADDR = 0x112233445566


def framed(code, body):
    """One message from the device, as the packet carrying it, in hex."""
    message = struct.pack("<I", code) + bytes(body)
    return (b"S\x00" + struct.pack("<H", len(message)) + message).hex()


def state_message(state, reason=0):
    """The packet a device sends to report the state it is now in."""
    return framed(btctl.BTCTL_STATE_RESP, (state, reason))


def received_message(flags, bb_hdr, body):
    """The packet a device sends for one packet it received.

    btctl_rx_pkt_t: the clock and channel it was received on, the decode
    flags, the size of what follows, the baseband header, then the packet.
    """
    received = (struct.pack("<IBBH", 0, 39, flags, len(body))
                + bytes(bb_hdr) + bytes(body))
    return framed(btctl.BTCTL_RX_PKT, received)


def acl_message(llid, data):
    """The packet a device sends for one received ACL packet of good CRC."""
    # The payload header a DM1 carries: the length, the flow bit, the LLID.
    header = bytes(((len(data) << 3) | (1 << 2) | llid,))
    return received_message(1 << btctl.BBPKT_F_GOOD_CRC,
                            (1, btctl.DM1, 0, 0), header + bytes(data))


def fhs_message(bdaddr):
    """The packet a device sends for one received FHS packet, in hex."""
    body = bytearray(18)
    lap = bdaddr & 0xffffff
    body[4] = (lap & 0x3f) << 2
    body[5] = (lap >> 6) & 0xff
    body[6] = (lap >> 14) & 0xff
    body[7] = (lap >> 22) & 0x03
    body[8] = (bdaddr >> 24) & 0xff
    body[9] = (bdaddr >> 32) & 0xff
    body[10] = (bdaddr >> 40) & 0xff
    return received_message(0, (0, btctl.FHS, 0, 0), body)


def lmp_message(opcode):
    """The packet a device sends for one received LMP PDU, 17 bytes long."""
    return acl_message(btctl.LLID_LMP, bytes((opcode << 1,)) + bytes(16))


def canned_session(typed, packets, until, then=""):
    """Drive one session that is handed packets, and return the two streams.

    The packets are held back until the four writes a session opens with and
    the command typed here have gone out, so they arrive while that command
    is running and holds the message handler, as those from a real board do.
    "q" is typed only once `until` has been printed, so the stop cannot race
    the last of them; `then` is typed just before it, for commands that are
    about what the session has already printed.
    """
    with open(READS_FILE, "w") as canned:
        canned.write("".join(packet + "\n" for packet in packets))
    running = tool(dict(ENVIRONMENT, UBTBR_FAKE_READS=READS_FILE,
                        UBTBR_FAKE_READS_AFTER="4"))
    running.stdin.write(typed + "\n")
    running.stdin.flush()
    printed = bytearray()

    def arrived():
        try:
            printed.extend(os.read(running.stdout.fileno(), 4096))
        except BlockingIOError:
            pass
        return until.encode() in printed

    os.set_blocking(running.stdout.fileno(), False)
    waited = wait_until(arrived)
    os.set_blocking(running.stdout.fileno(), True)
    output, errors = running.communicate(then + "q\n", timeout=DEADLINE * 2)
    # Decoded leniently because the part read above is raw pipe bytes, and a
    # failure must not turn into a decode error.
    output = printed.decode(errors="replace") + output
    assert waited, (output, errors, running.poll())
    assert running.returncode == 0, (running.returncode, output, errors)
    return output, errors


def timed_lines(output, text):
    """The lines of standard output holding the text.

    Nothing has to be cut off them: the prompt and the intro are on standard
    error, so every line of this stream is a packet with the time in front
    of it.
    """
    return [line for line in output.splitlines() if text in line]


def time_prefixed(errors):
    """The lines of standard error that start with a time of day.

    None should: the time belongs to the packets on standard output.
    """
    return [line for line in errors.splitlines()
            if re.match(r"\d\d:\d\d:\d\d ", line)]


# An inquiry: the state response is what has a command take packets at all,
# then an FHS packet and a response carrying an EIR arrive. The response is
# two lines, and each of them is timed.
EIR_NAME = bytes((10, 9)) + b"Ubertooth"
output, errors = canned_session("inquiry", [
    state_message(btctl.BTCTL_STATE_INQUIRY),
    fhs_message(FAKE_BDADDR),
    acl_message(btctl.LLID_L2CAP_START, EIR_NAME),
], until="COMPLETE_LOCAL_NAME")
assert len(timed_lines(output, "RX FHS")) == 1, output
assert re.fullmatch(r"\d\d:\d\d:\d\d RX FHS: FHS: bdaddr=112233445566"
                    r" parity=0 cls=0 ltaddr=0 clk=0",
                    timed_lines(output, "RX FHS")[0]), output
assert re.fullmatch(r"\d\d:\d\d:\d\d ACL\(llid=L2CAP_START, flow=1, len=11\):"
                    r" 0a 09 55 62 65 72 74 6f 6f 74 68",
                    timed_lines(output, "ACL(")[0]), output
assert re.fullmatch(r"\d\d:\d\d:\d\d COMPLETE_LOCAL_NAME: 'Ubertooth'",
                    timed_lines(output, "COMPLETE_LOCAL_NAME")[0]), output
# nothing of the packets on standard error, and the progress there carries
# neither a time nor a level
assert "RX FHS" not in errors and "COMPLETE_LOCAL_NAME" not in errors, errors
assert "Inquiry state Ready\n" in errors, errors
assert "Starting BTCtlInquiryCmd\n" in errors, errors
assert "| INFO |" not in errors and "| DEBUG |" not in errors, errors
assert time_prefixed(errors) == [], errors
print("ubtbr: a decoded packet goes to standard output, progress to standard error")

# and every line of that stream is a packet, so a reader can take them by
# their time alone. The prompt and the intro used to be there too, and the
# prompt carries no newline of its own, so the first packet after each one
# landed on its end ("bt> 12:14:53 RX FHS: ..."). One prompt per command the
# session read, on standard error, and none after the command that ended it.
PROMPT = tool_module.BTCtlPrompt.PROMPT
INTRO = tool_module.BTCtlPrompt.intro
assert all(re.match(r"\d\d:\d\d:\d\d ", line)
           for line in output.splitlines()), output
assert PROMPT not in output and INTRO not in output, output
assert INTRO + "\n" in errors, errors
assert errors.count(PROMPT) == 2, errors
print("ubtbr: the prompt and the intro are on standard error")

# and so is what `?` and `? <command>` print. Those are the tool describing
# itself, not the capture data a command was asked for, and they carry no
# time: typed during a piped capture they used to put untimed lines into it,
# which is what made the stream no longer readable by time alone. A command
# there is no help for is a request the tool cannot answer, so it reads like
# the prompt's other refusals rather than Cmd's "*** No help on". Both
# spellings of asking about one command are typed, the `? page` the man page
# gives and the `help page` Cmd's own text points at.
output, errors = canned_session("inquiry", [
    state_message(btctl.BTCTL_STATE_INQUIRY),
    fhs_message(FAKE_BDADDR),
], until="RX FHS", then="?\n? page\n? nosuch\nhelp page\n")
assert all(re.match(r"\d\d:\d\d:\d\d ", line)
           for line in output.splitlines()), output
assert "Documented commands" not in output, output
assert "inquiry_scan" not in output and "BD_ADDR" not in output, output
assert "Documented commands (type help <topic>):" in errors, errors
assert "inquiry_scan" in errors, errors
# both spellings of the one command, which Cmd takes to the same method
assert errors.count("page <BD_ADDR>: Establish connection with given"
                    " device\n") == 2, errors
assert "error: no help on 'nosuch'\n" in errors, errors
assert "***" not in errors and "***" not in output, (output, errors)
assert time_prefixed(errors) == [], errors
print("ubtbr: what `?` prints is on standard error, so a capture stays readable")

# a word the prompt does not know is refused there too, in the same wording.
# It used to be taken silently: the handler that reads "x" and "q" as ways of
# leaving returned nothing for everything else, so the user was handed the
# next prompt as though the command had run. A command name is not a command
# line, so the session carries on and its exit status is unchanged, and the
# refusal is a diagnostic rather than capture data, so standard output stays
# empty.
status, output, errors = ran(typed="bogus\nbogus 11:22:33:44:55:66\nq\n")
assert status == 0, (status, output, errors)
assert output == "", output
# the name is what does not exist, so that is what the line quotes, whatever
# was typed after it: both lines above name the same command
assert errors.count("error: no such command 'bogus'\n") == 2, errors
assert "***" not in errors, errors
assert "Traceback" not in errors, errors
# and the prompt was read again after each refusal: one per line typed, and
# none after the one that ended the session
assert errors.count(PROMPT) == 3, errors
print("ubtbr: an unknown command at the prompt is one error line, not silence")

# and the two short spellings of leaving are read the same way, so `x` and
# `q` with a word after them leave as `exit <word>` does rather than being
# refused as commands that do not exist
for typed in ("x\n", "q\n", "x now\n", "q now\n"):
    status, output, errors = ran(typed=typed)
    assert status == 0, (typed, status, output, errors)
    assert "no such command" not in errors, (typed, errors)
    assert "Traceback" not in errors, (typed, errors)
print("ubtbr: `x` and `q` leave whatever follows them")


# A session at a terminal leaves the prompt to Cmd, which hands it to the
# terminal's line editor: the editor draws it and counts its width while it
# redraws the line being edited, so one written behind its back would put the
# cursor four columns out on Ctrl-A. It only runs with a terminal on both
# standard input and standard output, and then standard error is the same
# terminal, so the user sees the prompt either way. Checked on the class
# rather than through a pseudo-terminal, because what is being pinned is
# which of the two writes it.
class FakeStream:
    def __init__(self, terminal):
        self._terminal = terminal

    def isatty(self):
        return self._terminal


def prompt_with_streams(stdin_is_terminal, stdout_is_terminal):
    """The prompt Cmd is left holding for streams of the given kind."""
    saved = sys.stdin, sys.stdout
    sys.stdin = FakeStream(stdin_is_terminal)
    sys.stdout = FakeStream(stdout_is_terminal)
    try:
        return tool_module.BTCtlPrompt(None).prompt
    finally:
        sys.stdin, sys.stdout = saved


assert prompt_with_streams(True, True) == PROMPT
# a capture on standard output, however the commands arrive, and a terminal
# that is only being typed at: Cmd is left with no prompt, and say_prompt()
# writes it to standard error.
assert prompt_with_streams(True, False) == ""
assert prompt_with_streams(False, True) == ""
assert prompt_with_streams(False, False) == ""
print("ubtbr: a terminal session leaves the prompt to the line editor")

# a session holding a connection reports the LMP packets it decodes the same
# way, named and in hex, so a capture of a connection reads on its own. The
# link manager traces the same exchange on standard error, which is a
# diagnostic and was for a while the only place those packets appeared at
# all. Both commands that hold a connection are checked, because each has
# its own handler.
CONNECTED_LT_ADDR_1 = (btctl.BTCTL_STATE_CONNECTED,
                       (1 << 5) | btctl.BTCTL_REASON_SUCCESS)
NAME_REQ = (r"\d\d:\d\d:\d\d RX LMP: BBHdr\(lt_addr=1, type=DM1, flags=\)"
            r" \| \(tid 0\) LMP_NAME_REQ +\| 02(00){16}")
for typed in ("page_scan", "page 11:22:33:44:55:66"):
    output, errors = canned_session(typed, [
        state_message(*CONNECTED_LT_ADDR_1),
        lmp_message(btctl.LMP_NAME_REQ),
    ], until="RX LMP")
    assert re.fullmatch(NAME_REQ, timed_lines(output, "RX LMP")[0]), output
    # and in plain text: a capture carries no terminal color
    assert "\x1b" not in output, output
    assert "RX LMP" not in errors, errors
    assert "Connection ready, lt_addr=1\n" in errors, errors
    assert "lmp_rx" in errors, errors
    assert time_prefixed(errors) == [], errors
print("ubtbr: a connection's LMP packets go to standard output as well")

# What the tool says when standard output has gone away, whichever write
# found it gone.
OUTPUT_GONE = "error: standard output has gone away\n"

# standard output going away part way through a capture: reported once, the
# prompt still ends the session, and the status says the capture is not all
# there. The flush the interpreter does on the way out used to fail as well,
# which ended the tool in status 120 and a message of its own.
with open(READS_FILE, "w") as canned:
    canned.write(state_message(btctl.BTCTL_STATE_INQUIRY) + "\n")
    for _ in range(100):
        canned.write(fhs_message(FAKE_BDADDR) + "\n")
running = tool(dict(ENVIRONMENT, UBTBR_FAKE_READS=READS_FILE,
                    UBTBR_FAKE_READS_AFTER="4", UBTBR_FAKE_READ_DELAY="0.05"))
running.stdin.write("inquiry\n")
running.stdin.flush()
read = bytearray()


def capture_started():
    """True once packets are being printed, so there is one to lose."""
    try:
        read.extend(os.read(running.stdout.fileno(), 4096))
    except BlockingIOError:
        pass
    return read.count(b"RX FHS") >= 2


os.set_blocking(running.stdout.fileno(), False)
assert wait_until(capture_started), (bytes(read), running.poll())
# What `ubertooth-btbr | head -2` does: the reader stops reading and goes.
running.stdout.close()
running.stdin.write("q\n")
running.stdin.flush()
running.stdin.close()
errors = running.stderr.read()
assert running.wait(timeout=DEADLINE) == 1, (running.returncode, errors)
assert errors.count(OUTPUT_GONE) == 1, errors
assert "Exception ignored" not in errors, errors
assert "Traceback" not in errors, errors
print("ubtbr: a capture whose reader went away is reported, and exits 1")

# a reader that leaves before any packet is printed costs the session
# nothing, because nothing the prompt writes is on that stream: not the
# intro, not the prompt, and not what `?` prints, which is typed here to
# cover it. Nothing asked for was lost, so the session ends cleanly. It used
# to report the stream gone and exit 1, the prompt having been written into
# it.
running = tool()
running.stdout.close()
running.stdin.write("?\nq\n")
running.stdin.flush()
running.stdin.close()
errors = running.stderr.read()
assert running.wait(timeout=DEADLINE) == 0, (running.returncode, errors)
assert OUTPUT_GONE not in errors, errors
assert "Exception ignored" not in errors, errors
assert "Traceback" not in errors, errors
print("ubtbr: a reader that leaves before any packet costs the session nothing")

# standard error closed before the tool started, which is `2>&-`: nothing
# can be said, and the session fails. That is the interpreter's own rule and
# not this tool's -- input() wants all three streams and raises "lost
# sys.stderr" without one -- so the prompt never gets as far as being read.
# Nothing on standard output and exit 1, which is all that is left to say it
# with.
running = subprocess.Popen([sys.executable, WRAPPER], stdin=subprocess.PIPE,
                           stdout=subprocess.PIPE, text=True,
                           env=ENVIRONMENT, preexec_fn=lambda: os.close(2))
output, _ = running.communicate("q\n", timeout=DEADLINE * 2)
assert running.returncode == 1, (running.returncode, output)
assert output == "", output
print("ubtbr: a session with standard error closed fails, saying nothing")

# the usage text has nowhere to go either: one line, and the 0 that -h would
# have exited with becomes 1, because what was asked for was not printed
running = tool(arguments=("-h",))
running.stdout.close()
errors = running.stderr.read()
assert running.wait(timeout=DEADLINE) == 1, (running.returncode, errors)
assert errors.count(OUTPUT_GONE) == 1, errors
assert "Exception ignored" not in errors, errors
assert "Traceback" not in errors, errors
print("ubtbr: -h whose usage text could not be printed exits 1")

# and with that stream closed before the tool started, which is `-h >&-`:
# there is no such stream at all rather than a reader who left, and the write
# used to end the tool in an AttributeError traceback. The text was asked for
# and has nowhere to go, so it is reported as the same loss, with the same
# status.
running = subprocess.Popen([sys.executable, WRAPPER, "-h"],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           text=True, env=ENVIRONMENT,
                           preexec_fn=lambda: os.close(1))
output, errors = running.communicate(timeout=DEADLINE)
assert running.returncode == 1, (running.returncode, output, errors)
assert output == "", output
assert errors.count(OUTPUT_GONE) == 1, errors
assert "Traceback" not in errors, errors
print("ubtbr: -h with no standard output at all is reported, not a traceback")


def read_only_stdout():
    """Give the child a standard output that cannot be written to."""
    nowhere = os.open(os.devnull, os.O_RDONLY)
    os.dup2(nowhere, 1)
    os.close(nowhere)


# and the other face of the same closed descriptor: most interpreters leave
# the tool without the stream, some leave the stream and a descriptor whose
# every write fails with EBADF. The second is what `ubertooth-btbr -h 1<&0`
# gives on any of them, which is how it is reached here. The usage text
# reaches the buffer and the flush is where it fails, so the handler that
# catches it is not the one above; the line and the status are the same.
running = subprocess.Popen([sys.executable, WRAPPER, "-h"],
                           stderr=subprocess.PIPE, text=True, env=ENVIRONMENT,
                           preexec_fn=read_only_stdout)
_, errors = running.communicate(timeout=DEADLINE)
assert running.returncode == 1, (running.returncode, errors)
assert errors.count(OUTPUT_GONE) == 1, errors
assert "Traceback" not in errors, errors
assert "Exception ignored" not in errors, errors
print("ubtbr: -h on a standard output that cannot be written is reported too")

# and with both streams the same pipe, which is `2>&1 | head -1`, the status
# is all that is left: the error line has nowhere to go either, and saying so
# must not become an unhandled failure of its own.
running = subprocess.Popen([sys.executable, WRAPPER, "-h"],
                           stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True,
                           env=ENVIRONMENT)
running.stdout.close()
running.stdin.close()
assert running.wait(timeout=DEADLINE) == 1, running.returncode
print("ubtbr: both streams gone leaves the exit status to say it")

# a failure nobody handled, leaving past a standard output that has gone
# away, ends in the traceback that failure deserves and nothing else: the
# last flush is taken whichever way the session ends, so the interpreter is
# never the one to find the stream gone. Nothing had been printed by then, so
# there is nothing to report lost; what this pins is that the way out is the
# tool's and not status 120 with a message of the interpreter's own.
running = tool(dict(ENVIRONMENT, UBTBR_FAKE_LIST_ERROR="1"))
running.stdout.close()
running.stdin.close()
errors = running.stderr.read()
assert running.wait(timeout=DEADLINE) == 1, (running.returncode, errors)
assert "RuntimeError: listing the bus failed" in errors, errors
assert "Exception ignored" not in errors, errors
print("ubtbr: an unhandled failure does not leave the last flush to the exit")

# no Ubertooth at all: one error line, nothing on standard output, exit 1,
# where the tool used to end in a traceback
running = tool(dict(ENVIRONMENT, UBTBR_FAKE_NO_DEVICE="1"))
output, errors = running.communicate("", timeout=DEADLINE * 2)
assert running.returncode == 1, (running.returncode, output, errors)
assert output == "", output
assert errors == "error: no Ubertooth devices found\n", errors
print("ubtbr: no Ubertooth is one error line and exit 1")

# ---- which board the session gets -----------------------------------------
#
# The tool used to open whichever board libusb handed over first, with no way
# to say which one to use. On a host with several boards it therefore reached
# an arbitrary one: a user who had flashed btbr onto one board got a write
# timeout from a different board still carrying bluetooth_rxtx, and a note
# blaming the firmware of the board they had just flashed.

OTHER_BOARD = "346001058088cb95345bad61c52000f5"
TWO_BOARDS = ",".join((ONE_BOARD, OTHER_BOARD))
OPENED_FILE = os.path.join(WORK.name, "opened")
SERIAL_READS_FILE = os.path.join(WORK.name, "serial-reads")
OPENS_FILE = os.path.join(WORK.name, "opens")


def opened():
    """The serial of the board the last run opened, or None if it opened none."""
    if not os.path.exists(OPENED_FILE):
        return None
    with open(OPENED_FILE) as marker:
        return marker.read()


def touched(path):
    """Every board the last run read a serial from, or opened, oldest first."""
    if not os.path.exists(path):
        return []
    with open(path) as noted:
        return noted.read().splitlines()


def selecting(arguments, typed="", serials=TWO_BOARDS, **fakes):
    for path in (OPENED_FILE, SERIAL_READS_FILE, OPENS_FILE):
        if os.path.exists(path):
            os.remove(path)
    return ran(arguments, typed, UBTBR_FAKE_SERIALS=serials,
               UBTBR_FAKE_OPENED=OPENED_FILE,
               UBTBR_FAKE_SERIAL_READS=SERIAL_READS_FILE,
               UBTBR_FAKE_OPENS=OPENS_FILE, **fakes)


# several boards and no -U: refused, with the boards listed, and no board
# opened. This is the case that made the tool unusable on a host with more
# than one board.
status, output, errors = selecting(())
assert status == 1, (status, output, errors)
assert output == "", output
assert errors == ("error: multiple Ubertooth devices found; select one with "
                  "-U <index|serial>\n"
                  "  Device 0: %s\n"
                  "  Device 1: %s\n" % (ONE_BOARD, OTHER_BOARD)), errors
assert opened() is None, "a refused selection must not open a board"
assert not writes(), writes()
print("ubtbr: several boards with no -U are listed, not guessed between")

# a serial reaches that board whatever its place in the enumeration order,
# and the session then runs as it does with one board attached
status, output, errors = selecting(("-U", OTHER_BOARD), "q\n")
assert status == 0, (status, output, errors)
assert opened() == OTHER_BOARD, opened()
assert IDLE_PACKET in writes(), writes()
print("ubtbr: -U <serial> reaches that board")

# the first characters of a serial are enough, in either case. They have to
# include a non-digit: an all-digits argument is an index, which is the rule
# ubertooth(7) states and every tool follows.
for prefix in ("1cc0", "1CC0", ONE_BOARD.upper()):
    status, output, errors = selecting(("-U", prefix), "q\n")
    assert status == 0, (prefix, status, output, errors)
    assert opened() == ONE_BOARD, (prefix, opened())
print("ubtbr: a serial prefix selects a board, in either case")

# a prefix that starts both serials names neither
status, output, errors = selecting(("-U", "1cc"),
                                   serials=",".join((ONE_BOARD,
                                                     "1cc0ffffffffffff")))
assert status == 1, (status, output, errors)
assert output == "", output
assert errors == ("error: '1cc' matches 2 Ubertooth devices: %s, "
                  "1cc0ffffffffffff\n"
                  "select one of them by a longer prefix of its serial "
                  "number\n" % ONE_BOARD), errors
assert opened() is None, opened()
print("ubtbr: a prefix matching two boards selects neither")

# a serial nothing on the bus starts with
status, output, errors = selecting(("-U", "zz"))
assert status == 1, (status, output, errors)
assert output == "", output
assert errors == "error: no Ubertooth found with serial matching 'zz'\n", errors
print("ubtbr: an unmatched serial is one error line")

# an all-digits argument is an index into the enumeration order, and reaches
# that board without reading any serial: reading one opens the board, and
# ubertooth(7) promises the index path disturbs nothing.
status, output, errors = selecting(("-U", "1"), "q\n")
assert status == 0, (status, output, errors)
assert opened() == OTHER_BOARD, opened()
assert touched(SERIAL_READS_FILE) == [], touched(SERIAL_READS_FILE)
assert touched(OPENS_FILE) == [OTHER_BOARD], touched(OPENS_FILE)
status, output, errors = selecting(("-U", "0"), "q\n")
assert status == 0, (status, output, errors)
assert opened() == ONE_BOARD, opened()
assert touched(SERIAL_READS_FILE) == [], touched(SERIAL_READS_FILE)
print("ubtbr: -U <index> is a position in the list, and reads no serial")

# an empty -U is no selection at all, as it is in the other tools
status, output, errors = selecting(("-U", ""), "q\n", serials=ONE_BOARD)
assert status == 0, (status, output, errors)
assert opened() == ONE_BOARD, opened()
assert touched(SERIAL_READS_FILE) == [], touched(SERIAL_READS_FILE)
print("ubtbr: an empty -U takes the only attached board")

# an index no board has, including a nonzero index with one board attached:
# opening the only board for any index would hide that indexes renumber. The
# last is past what a C int holds, which libubertooth has a case of its own
# for because atoi() would wrap it into a real index.
for serials, selector, attached in ((TWO_BOARDS, "9", 2),
                                    (ONE_BOARD, "1", 1),
                                    (TWO_BOARDS, "4294967296", 2),
                                    (TWO_BOARDS, "9" * 5000, 2)):
    status, output, errors = selecting(("-U", selector), serials=serials)
    assert status == 1, (selector, status, output, errors)
    assert output == "", (selector, output)
    assert errors == ("error: no Ubertooth at index %s (%d attached)\n"
                      "indexes change when boards are added or removed; "
                      "select a board by serial with -U<serial>\n"
                      % (selector, attached)), errors
    assert opened() is None, opened()
print("ubtbr: an index no board has is refused, and names the count")

# a board whose serial cannot be read keeps its place in the list, so it can
# still be chosen by index, and is listed as unreadable rather than left out
status, output, errors = selecting(("-U", "1"), "q\n",
                                   serials=",".join((ONE_BOARD, "?")))
assert status == 0, (status, output, errors)
assert opened() == "?", opened()
status, output, errors = selecting((), serials=",".join((ONE_BOARD, "?")))
assert status == 1, (status, output, errors)
assert "  Device 1: serial unreadable\n" in errors, errors
print("ubtbr: a board with no readable serial is still listed and indexable")

# and a serial that matches nothing while a board stayed quiet says so: the
# string the user gave may well be that board's serial
status, output, errors = selecting(("-U", "abcd"),
                                   serials=",".join((ONE_BOARD, "?")))
assert status == 1, (status, output, errors)
UNREAD_NOTE = ("1 attached Ubertooth device did not answer the serial read\n"
               "note: a board nobody may open, or one that has left the bus, "
               "reads as having no serial; select it by index with "
               "-U<index>\n")
assert errors == ("error: no Ubertooth found with serial matching 'abcd'\n"
                  + UNREAD_NOTE), errors
# and so does a prefix that matched more than one, for the same reason: the
# quiet board's serial could have been the one the user meant
status, output, errors = selecting(("-U", "1cc"),
                                   serials=",".join((ONE_BOARD,
                                                     "1cc0ffffffffffff",
                                                     "?")))
assert status == 1, (status, output, errors)
assert errors.endswith(UNREAD_NOTE), errors
print("ubtbr: a serial refusal says when a board did not answer the read")

# boards reporting one serial cannot be told apart by a longer prefix, so
# that refusal says to use an index instead
status, output, errors = selecting(("-U", "1cc0"),
                                   serials=",".join((ONE_BOARD, ONE_BOARD)))
assert status == 1, (status, output, errors)
assert errors == ("error: '1cc0' matches 2 Ubertooth devices: %s, %s\n"
                  "these Ubertooth devices report the same serial number; "
                  "select one by index with -U<index>\n"
                  % (ONE_BOARD, ONE_BOARD)), errors
print("ubtbr: boards sharing a serial are selected by index, not a prefix")

# every board the C tools recognize is listed, not only the Ubertooth One.
# The btbr firmware builds for the Ubertooth Zero too, and a board left out
# of the list would not be listed here, so a bare command would call the One
# the only board and open it -- the fault this whole section is about.
ZERO_AND_ONE = ",".join(("0b700017490c2cafa039ee5ac52000f5/1d50:6000",
                         ONE_BOARD))
status, output, errors = selecting((), serials=ZERO_AND_ONE)
assert status == 1, (status, output, errors)
assert errors == ("error: multiple Ubertooth devices found; select one with "
                  "-U <index|serial>\n"
                  "  Device 0: 0b700017490c2cafa039ee5ac52000f5\n"
                  "  Device 1: %s\n" % ONE_BOARD), errors
status, output, errors = selecting(("-U", "0b70"), "q\n", serials=ZERO_AND_ONE)
assert status == 0, (status, output, errors)
assert opened() == "0b700017490c2cafa039ee5ac52000f5", opened()
# and something else of the same vendor is not an Ubertooth
status, output, errors = selecting(("-U", "0b70"),
                                   serials=",".join(
                                       ("0b700017490c2cafa039ee5ac52000f5"
                                        "/1d50:1234", ONE_BOARD)))
assert status == 1, (status, output, errors)
assert errors == "error: no Ubertooth found with serial matching '0b70'\n", \
    errors
print("ubtbr: every board the C tools recognize is listed, and nothing else")

# a board that cannot be opened at all, which is what another program holding
# it looks like: reported as that board, in the words libubertooth uses for
# the same two libusb codes, not as a board that is not there
status, output, errors = selecting(("-U", "0"), serials="!")
assert status == 1, (status, output, errors)
assert output == "", output
assert errors == ("error: could not open Ubertooth at index 0: access "
                  "denied\n"
                  "note: a board that cannot be opened is usually a "
                  "permissions problem; on Linux install the udev rules "
                  "shipped with this package\n"), errors
print("ubtbr: a board that cannot be opened is reported as that board")

# the board a failed setup was talking to is named, so the wrong board is
# told apart from a bad one: by serial when one was read, by index when the
# selection never needed one
status, output, errors = selecting(("-U", OTHER_BOARD),
                                   UBTBR_FAKE_WRITE_FAILS_AFTER="0")
assert status == 1, (status, output, errors)
assert ("error: could not set up Ubertooth %s: " % OTHER_BOARD) in errors, \
    errors
status, output, errors = selecting(("-U", "1"),
                                   UBTBR_FAKE_WRITE_FAILS_AFTER="0")
assert status == 1, (status, output, errors)
assert "error: could not set up Ubertooth at index 1: " in errors, errors
print("ubtbr: a setup that failed names the board it was talking to")

# a board another program is using opens and answers the serial read, and
# fails when its interface is claimed: reported in libubertooth's words for
# that, not as a board that took none of the session's requests. BUSY is what
# Linux gives and ACCESS what macOS gives, and neither is a permissions
# problem here, because a board nobody may open never reaches the claim.
for failure, code in (("busy", -6), ("access", -3)):
    status, output, errors = selecting(("-U", ONE_BOARD),
                                       UBTBR_FAKE_CLAIM_ERROR=failure)
    assert status == 1, (failure, status, output, errors)
    assert output == "", output
    assert errors == ("error: Ubertooth %s is in use by another program "
                      "(libusb %d)\n" % (ONE_BOARD, code)), errors
    assert not writes(), writes()
# any other claim failure is reported as itself
status, output, errors = selecting(("-U", ONE_BOARD),
                                   UBTBR_FAKE_CLAIM_ERROR="other")
assert status == 1, (status, output, errors)
assert errors == ("error: claiming the USB interface of Ubertooth %s failed "
                  "(libusb -1)\n" % ONE_BOARD), errors
print("ubtbr: a board another program is using is reported as in use")

# ---- the command line ------------------------------------------------------

# -h prints the usage text on standard output and exits 0, having opened no
# board. It used to be a parse failure: usage on standard output, exit 1.
status, output, errors = selecting(("-h",))
assert status == 0, (status, output, errors)
assert output.startswith("Usage: ubertooth-btbr "), output
assert "-U <index|serial>" in output, output
assert errors == "", errors
assert opened() is None, "-h must not open a board"
print("ubtbr: -h prints the usage text on standard output and exits 0")

# a command line the tool will not take: the one line that says what is wrong
# goes to standard error, standard output stays empty, and no usage text
# follows it
for arguments, said in (
        (("-z",), "error: unrecognized option '-z'\n"),
        (("--bogus",), "error: unrecognized option '--bogus': the tools take "
                       "single-letter options\n"),
        (("-f",), "error: option '-f' requires a value\n"),
        (("-U",), "error: option '-U' requires a value\n"),
        (("stray",), "error: unexpected argument 'stray'\n"),
        (("-U", "0", "-U", "1"), "error: option '-U' given more than once\n"),
        (("-f", "1", "-f", "2"), "error: option '-f' given more than once\n"),
        (("-b", "1", "-b", "2"), "error: option '-b' given more than once\n")):
    status, output, errors = selecting(arguments)
    assert status == 1, (arguments, status, output, errors)
    assert output == "", (arguments, output)
    assert errors == said, (arguments, errors)
    assert opened() is None, (arguments, opened())
print("ubtbr: a command line the tool will not take is one error line")

# -h ends the command only once the whole line has parsed, as in the other
# tools, so a bad value or a stray word is reported even alongside it
for arguments, said in ((("-h", "-f", "nine"),
                         "error: -f must be 0-65535 (got nine)\n"),
                        (("-h", "stray"), "error: unexpected argument "
                                          "'stray'\n"),
                        (("-U", "0", "-U", "1", "-h"),
                         "error: option '-U' given more than once\n")):
    status, output, errors = selecting(arguments)
    assert status == 1, (arguments, status, output, errors)
    assert output == "", (arguments, output)
    assert errors == said, (arguments, errors)
print("ubtbr: a bad command line is reported even alongside -h")

# the cases below run against one board, as the ones above this section did
for selection_marker in (OPENED_FILE, SERIAL_READS_FILE, OPENS_FILE):
    if os.path.exists(selection_marker):
        os.remove(selection_marker)

# an option value that is not a number: one error line, not the traceback
# out of int(). Refused before a board is opened, so a wrong command line
# touches nothing and says what is wrong with it rather than reporting
# whatever the board did first: there is no device here to find, and the
# answer is still about the option.
# "nine" is not a number; -5 and 70000 do not fit the 16 bits the request
# packs, and used to reach the packing and raise from there, which left the
# device open and the receive thread inside libusb.
for bad in ("nine", "-5", "70000"):
    running = subprocess.Popen([sys.executable, WRAPPER, "-f", bad],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True,
                               env=dict(ENVIRONMENT, UBTBR_FAKE_NO_DEVICE="1"))
    output, errors = running.communicate("", timeout=DEADLINE * 2)
    assert running.returncode == 1, (bad, running.returncode, output, errors)
    assert output == "", (bad, output)
    assert errors == "error: -f must be 0-65535 (got %s)\n" % bad, repr(errors)
    assert not writes(), (bad, writes())
print("ubtbr: a bad option value is refused before a board is opened")

# an interrupt before the prompt is up, which is the window the hang shortly
# after start lives in: the tool ends, with no traceback and nothing on
# standard output
claimed = os.path.join(WORK.name, "claimed")
running = tool(dict(ENVIRONMENT, UBTBR_FAKE_SETUP_DELAY=str(DEADLINE),
                    UBTBR_FAKE_CLAIMED=claimed))
# interrupted inside claiming the interface, not somewhere in the imports
assert wait_until(lambda: os.path.exists(claimed)), claimed
running.send_signal(signal.SIGINT)
output, errors = running.communicate("", timeout=DEADLINE * 2)
assert running.returncode == 1, (running.returncode, output, errors)
assert output == "", output
assert "Traceback" not in errors, errors
print("ubtbr: an interrupt before the prompt ends the tool")

# the same, but interrupted once the receive thread is running, which is the
# window the setup requests sit in. The thread has to be given its stop
# before the interpreter goes: one still inside libusb when the library is
# taken down turns the exit into an abort, which is what a board dropping off
# USB mid-run produced on hardware.
closed = os.path.join(WORK.name, "closed")
# Long enough to be interrupted inside a request, short enough that the idle
# request the way out sends is not what this case spends its time on.
running = tool(dict(ENVIRONMENT, UBTBR_FAKE_WRITE_DELAY="2",
                    UBTBR_FAKE_CLOSED=closed))
# interrupted inside a setup request, so the device is open and being read
assert wait_until(lambda: writes()), "no request was sent"
running.send_signal(signal.SIGINT)
output, errors = running.communicate("", timeout=DEADLINE * 2)
assert running.returncode == 1, (running.returncode, output, errors)
assert output == "", output
assert "Traceback" not in errors, errors
assert os.path.exists(closed), \
    "the device was not closed, so a thread was left inside libusb"
print("ubtbr: an interrupt mid-request stops the receive thread before exiting")

# Ctrl-C with a command running returns to the prompt, and the tool then
# exits 0. The command is one the fake device never answers, which is the
# case that used to hang.
running = tool()
running.stdin.write("inquiry\n")
running.stdin.flush()
assert wait_until(lambda: INQUIRY_PACKET in writes()), writes()
sent = writes()
running.send_signal(signal.SIGINT)
# the stop asks the device to go idle, which is how the interrupt is seen
# from outside before anything more is typed
assert wait_until(lambda: IDLE_PACKET in writes()[len(sent):]), writes()
output, errors = running.communicate("q\n", timeout=DEADLINE * 2)
assert running.returncode == 0, (running.returncode, output, errors)
assert "Traceback" not in errors, errors
print("ubtbr: Ctrl-C with a command running returns to the prompt")

# ---- -V, which the tool answers by running ubertooth-util -----------------
#
# A fake ubertooth-util stands in for the real one: it records the arguments
# it was called with, prints the answer the test gives it and exits with the
# status the test gives it. So no board is involved, and what is checked is
# that the tool passes ubertooth-util's answer through rather than writing a
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
VERSION_ANSWER = ("the first line ubertooth-util printed\n"
                  "the second line ubertooth-util printed\n"
                  "the third line ubertooth-util printed\n")

FAKE_UTIL_DIR = os.path.join(WORK.name, "bin")
os.mkdir(FAKE_UTIL_DIR)
FAKE_UTIL_PATH = os.path.join(FAKE_UTIL_DIR, "ubertooth-util")
with open(FAKE_UTIL_PATH, "w") as fake:
    fake.write(FAKE_UTIL % sys.executable)
os.chmod(FAKE_UTIL_PATH, 0o755)
VERSION_ARGS_FILE = os.path.join(WORK.name, "util-args")
# An empty directory, so a run given this PATH can run no ubertooth-util at
# all. The tool itself is started by path, so that costs it nothing.
NO_UTIL_DIR = os.path.join(WORK.name, "empty")
os.mkdir(NO_UTIL_DIR)


def version_ran(arguments, answer=VERSION_ANSWER, status=0,
                path=FAKE_UTIL_DIR, **fakes):
    if os.path.exists(VERSION_ARGS_FILE):
        os.remove(VERSION_ARGS_FILE)
    return ran(arguments, PATH=path, FAKE_UTIL_ARGS=VERSION_ARGS_FILE,
               FAKE_UTIL_ANSWER=answer, FAKE_UTIL_STATUS=str(status),
               **fakes)


def version_arguments():
    with open(VERSION_ARGS_FILE) as record:
        return record.read().split("\n")


# -V on its own: the answer, unchanged, on standard output, and exit 0. It
# asks ubertooth-util for the version and nothing else, and opens no board of
# its own, so it answers whatever firmware the board is carrying.
status, output, errors = version_ran(("-V",))
assert status == 0, (status, output, errors)
assert output == VERSION_ANSWER, output
assert errors == "", errors
assert version_arguments() == ["-V"], version_arguments()
assert not writes(), writes()

# -U names the board, and is passed on as ubertooth-util's own -U
status, output, errors = version_ran(("-V", "-U", "05f"))
assert status == 0, (status, output, errors)
assert version_arguments() == ["-V", "-U", "05f"], version_arguments()

# a board that is attached and could not be read exits 1 there, and so here:
# its line says why, and the answer is still printed
status, output, errors = version_ran(
    ("-V",), answer="Firmware version: read failed\n", status=1)
assert status == 1, (status, output, errors)
assert output == "Firmware version: read failed\n", output

# -V beside -h prints both, the version first, and exits 0
status, output, errors = version_ran(("-h", "-V"))
assert status == 0, (status, output, errors)
assert output.startswith(VERSION_ANSWER), output
assert "Usage: ubertooth-btbr" in output, output
assert errors == "", errors

# a bad option value is still reported beside -V, and no version is printed:
# the command failed before it got that far
status, output, errors = version_ran(("-V", "-f", "bad"))
assert status == 1, (status, output, errors)
assert output == "", output
assert errors == "error: -f must be 0-65535 (got bad)\n", errors

# ubertooth-util not on the PATH: one error line, not a traceback
status, output, errors = version_ran(("-V",), path=NO_UTIL_DIR)
assert status == 1, (status, output, errors)
assert output == "", output
assert "Traceback" not in errors, errors
assert errors == \
    "error: could not run ubertooth-util: No such file or directory\n", errors

# and the usage text beside it is still printed, since -h was asked for too:
# a version that could not be read does not take the rest of the answer with
# it. The status is the version's.
status, output, errors = version_ran(("-h", "-V"), path=NO_UTIL_DIR)
assert status == 1, (status, output, errors)
assert output.startswith("Usage: ubertooth-btbr"), output
assert errors == \
    "error: could not run ubertooth-util: No such file or directory\n", errors

print("ubtbr: -V passes ubertooth-util's answer through")

# -V with standard output closed before the tool started, which is `-V >&-`:
# the flush that orders this tool's lines before the child's has no stream to
# flush, where it used to end the tool in an AttributeError traceback. The
# answer cannot be printed, so it is reported as the same loss as a reader
# who left, and the command exits 1.
#
# ubertooth-util is on the PATH here and must not be run at all: started with
# that descriptor closed, the first file it opens takes its number, and the
# answer would be written into whatever that turned out to be -- for a tool
# that opens USB device nodes, one of those.
if os.path.exists(VERSION_ARGS_FILE):
    os.remove(VERSION_ARGS_FILE)
running = subprocess.Popen([sys.executable, WRAPPER, "-V"],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           text=True,
                           env=dict(ENVIRONMENT, PATH=FAKE_UTIL_DIR,
                                    FAKE_UTIL_ARGS=VERSION_ARGS_FILE,
                                    FAKE_UTIL_ANSWER=VERSION_ANSWER,
                                    FAKE_UTIL_STATUS="0"),
                           preexec_fn=lambda: os.close(1))
output, errors = running.communicate(timeout=DEADLINE)
assert running.returncode == 1, (running.returncode, output, errors)
assert output == "", output
assert errors.count(OUTPUT_GONE) == 1, errors
assert "Traceback" not in errors, errors
assert not os.path.exists(VERSION_ARGS_FILE), "ubertooth-util must not run"
print("ubtbr: -V with no standard output at all is reported, not a traceback")


# ---- and -V and -h need no Python libusb1 package -------------------------
#
# A session needs the usb1 module, which that package ships. -V does not: it
# is how a user says which tools they have, and a package that is not
# installed is exactly when they would ask. Here the module is shadowed by
# one that raises what the interpreter raises where the package is missing,
# and the fake usb1 is left off the path, so nothing can satisfy the import.

WITHOUT_USB1 = os.path.join(WORK.name, "no-usb1")
os.mkdir(WITHOUT_USB1)
with open(os.path.join(WITHOUT_USB1, "usb1.py"), "w") as refused:
    # name= is part of what the interpreter raises for a module that is not
    # there, and the wrapper reads it to decide whether the install note
    # applies, so the stand-in carries it too.
    refused.write(
        'raise ModuleNotFoundError("No module named \'usb1\'",'
        ' name="usb1")\n')
# The ubtbr package is still here; the fake usb1 in WORK.name is not.
NO_USB1 = os.pathsep.join((WITHOUT_USB1, MODULE_DIR))

# -V is answered, unchanged, and exits 0
status, output, errors = version_ran(("-V",), PYTHONPATH=NO_USB1)
assert status == 0, (status, output, errors)
assert output == VERSION_ANSWER, output
assert errors == "", errors

# and so is -h
status, output, errors = version_ran(("-h",), PYTHONPATH=NO_USB1)
assert status == 0, (status, output, errors)
assert output == tool_module.USAGE, output
assert errors == "", errors

# a command line the tool will not take is still refused there, in the one
# line it gives anywhere else
status, output, errors = version_ran(("-f", "bad"), PYTHONPATH=NO_USB1)
assert status == 1, (status, output, errors)
assert output == "", output
assert errors == "error: -f must be 0-65535 (got bad)\n", errors

# A call that does open a board needs the package, which is what says it was
# really out of reach above. That is one error line naming what is missing
# and a note saying how to install it, not a traceback.
status, output, errors = version_ran((), PYTHONPATH=NO_USB1)
assert status == 1, (status, output, errors)
assert output == "", output
assert "Traceback" not in errors, errors
assert errors == (
    "error: could not start the session: No module named 'usb1'\n"
    "note: a session needs the Python libusb1 package, which ships the usb1 "
    "module; install it with \"pip install libusb1\"\n"), errors

# The install note belongs to the one module a user installs a package for.
# Something else missing from the session's imports is a broken install, so
# it is reported without being told to install libusb1.
BROKEN_INSTALL = os.path.join(WORK.name, "broken-install")
os.mkdir(BROKEN_INSTALL)
with open(os.path.join(BROKEN_INSTALL, "usb1.py"), "w") as elsewhere:
    elsewhere.write(
        'raise ModuleNotFoundError("No module named \'nosuchthing\'",'
        ' name="nosuchthing")\n')
status, output, errors = version_ran(
    (), PYTHONPATH=os.pathsep.join((BROKEN_INSTALL, MODULE_DIR)))
assert status == 1, (status, output, errors)
assert output == "", output
assert "Traceback" not in errors, errors
assert errors == (
    "error: could not start the session: No module named 'nosuchthing'\n"
), errors

print("ubtbr: -V and -h need no Python libusb1 package")

WORK.cleanup()
