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
UBTBR_FAKE_CLAIMED a file created once it is in there. UBTBR_FAKE_WRITE_DELAY
holds each write, which keeps the tool short of its prompt with the receive
thread already running. UBTBR_FAKE_NO_DEVICE makes the context find nothing
to open, and UBTBR_FAKE_CLOSED names a file created when the handle is closed.
"""

import os
import time


class USBError(Exception):
    pass


class USBErrorTimeout(USBError):
    pass


class _Handle:
    def __init__(self):
        self.writes = 0

    def claimInterface(self, interface):
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
        time.sleep(timeout / 1000.0)
        raise USBErrorTimeout("timeout")

    def close(self):
        marker = os.environ.get("UBTBR_FAKE_CLOSED")
        if marker:
            open(marker, "w").close()


class USBContext:
    def openByVendorIDAndProductID(self, vendor, product,
                                   skip_on_error=False):
        if os.environ.get("UBTBR_FAKE_NO_DEVICE"):
            return None
        return _Handle()
'''

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
from ubtbr import btctl


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
    """Keeps what ubtbr logged, so a test can tell how a command ended."""

    def __init__(self):
        super().__init__()
        self.messages = []

    def emit(self, record):
        self.messages.append(record.getMessage())

    def said(self, text):
        return any(text in message for message in self.messages)


records = Recorder()
btctl.log.addHandler(records)


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
records.messages.clear()
thrown = io.StringIO()
with contextlib.redirect_stderr(thrown):
    refused_super = GivingUpSuperCmd(device)
    refused_super.start()
    refused_super.join(DEADLINE)
assert not refused_super.is_alive(), "a refused super command must end"
assert "Exception in thread" not in thrown.getvalue(), thrown.getvalue()
assert "Traceback" not in thrown.getvalue(), thrown.getvalue()
assert records.said("the previous command is still running"), records.messages
print("ubtbr: a refusal on a command's own thread is logged, not a traceback")



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

records.messages.clear()
device = SilentDevice()
thrown = io.StringIO()
with contextlib.redirect_stderr(thrown):
    super_command = RefusedChildSuperCmd(device)
    super_command.start()
    super_command.join(DEADLINE)
assert not super_command.is_alive(), "a super command with no child must end"
assert "Exception in thread" not in thrown.getvalue(), thrown.getvalue()
assert "Traceback" not in thrown.getvalue(), thrown.getvalue()
assert records.said("RuntimeError: can't start new thread"), records.messages
assert device.handler is None, "the board is still claimed"
print("ubtbr: a child whose thread is refused is logged, not a traceback")

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


def tool(environment=None):
    if os.path.exists(WRITES_FILE):
        os.remove(WRITES_FILE)
    return subprocess.Popen(
        [sys.executable, WRAPPER],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, text=True,
        env=environment or ENVIRONMENT)


# "q" at the prompt exits 0. It used to call the handler for it without the
# line the user typed, which the handler takes, so the tool ended in a
# TypeError, and then hung: the receive thread kept the interpreter alive.
running = tool()
output, errors = running.communicate("q\n", timeout=DEADLINE * 2)
assert running.returncode == 0, (running.returncode, output, errors)
assert "Traceback" not in errors, errors
assert "Type ? to list commands" in output, output
assert IDLE_PACKET in writes(), writes()
# and every request was given a timeout. Without one, a device that takes
# none of them leaves the tool inside a libusb call no interrupt can reach.
assert recorded_writes(), recorded_writes()
for packet, timeout in recorded_writes():
    assert int(timeout) > 0, (packet, timeout)
print("ubtbr: q at the prompt exits 0")

# a device that takes none of the requests: the tool says so on standard
# error and exits 1, where the write used to wait forever. It also names the
# firmware to flash, since a board carrying bluetooth_rxtx offers the endpoint
# this tool writes to but never reads it, and that is what every report of the
# tool hanging shortly after start has turned out to be.
running = tool(dict(ENVIRONMENT, UBTBR_FAKE_WRITE_FAILS_AFTER="0"))
output, errors = running.communicate("", timeout=DEADLINE * 2)
assert running.returncode == 1, (running.returncode, output, errors)
assert output == "", output
assert "error: could not set up the Ubertooth" in errors, errors
assert "note: ubertooth-btbr needs the btbr firmware" in errors, errors
assert "Traceback" not in errors, errors
print("ubtbr: a device that takes no request is reported, not waited on")

# a setup request that failed some other way is not a board waiting to be
# reflashed, so it gets the error without that note
running = tool(dict(ENVIRONMENT, UBTBR_FAKE_WRITE_FAILS_AFTER="0",
                    UBTBR_FAKE_WRITE_ERROR="1"))
output, errors = running.communicate("", timeout=DEADLINE * 2)
assert running.returncode == 1, (running.returncode, output, errors)
assert output == "", output
assert "error: could not set up the Ubertooth" in errors, errors
assert "btbr firmware" not in errors, errors
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
# is what the prompt the tool prints before each line it reads says: the
# third one is printed by the read that takes that command, and the prompt
# only gets there by way of the KeyboardInterrupt the signal raised. A line
# written straight after the signal instead races it, and can be taken off
# the pipe by the very read the signal interrupts: the interrupt then comes
# out of that read in place of the line, which loses it the way Ctrl-C loses
# the line being typed. The device cannot stand in for this, because the
# idle request the interrupt sends is the same one "discoverable" sends
# every time it swaps its two scans.
seen = bytearray()


def prompts_printed():
    """How many lines the tool has asked for, as far as it has run."""
    try:
        seen.extend(os.read(running.stdout.fileno(), 4096))
    except BlockingIOError:
        pass
    return seen.count(tool_module.BTCtlPrompt.prompt.encode())


def what_the_tool_did():
    """What to report if a wait below runs out.

    Neither wait reaches communicate(), so nothing else would drain standard
    error, and the prompts alone say almost nothing. Only called from a
    failing assert, where consuming standard error costs nothing.
    """
    os.set_blocking(running.stderr.fileno(), False)
    try:
        errors = os.read(running.stderr.fileno(), 1 << 16)
    except BlockingIOError:
        errors = b""
    finally:
        os.set_blocking(running.stderr.fileno(), True)
    return ("stdout: %r, stderr: %r, exit: %s"
            % (bytes(seen), errors, running.poll()))


os.set_blocking(running.stdout.fileno(), False)
assert wait_until(lambda: prompts_printed() >= 2), what_the_tool_did()
running.send_signal(signal.SIGINT)
assert wait_until(lambda: prompts_printed() >= 3), what_the_tool_did()
os.set_blocking(running.stdout.fileno(), True)
output, errors = running.communicate("inquiry\nq\n", timeout=DEADLINE * 2)
# whatever was read above to see the prompts, so a failure below still shows
# everything the tool printed. Decoded leniently because these are raw pipe
# bytes, and a failure must not turn into a decode error.
output = seen.decode(errors="replace") + output
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

# no Ubertooth at all: one error line, nothing on standard output, exit 1,
# where the tool used to end in a traceback
running = tool(dict(ENVIRONMENT, UBTBR_FAKE_NO_DEVICE="1"))
output, errors = running.communicate("", timeout=DEADLINE * 2)
assert running.returncode == 1, (running.returncode, output, errors)
assert output == "", output
assert errors == "error: no Ubertooth devices found\n", errors
print("ubtbr: no Ubertooth is one error line and exit 1")

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

WORK.cleanup()
