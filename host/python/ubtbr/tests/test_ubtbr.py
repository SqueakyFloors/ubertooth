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
    waiting for good. Whoever then stopped it joined that thread with no
    timeout either, which is why the tool could not be interrupted: Ctrl-C at
    the prompt runs the same stop, so the key meant to recover the tool was
    what wedged it.

  * the receive thread ending on a packet that does not fit the framing says
    so instead of dying on a bare assert, and a close() afterwards still
    returns and closes the device.

  * a close() while the receive loop is stuck inside libusb returns rather
    than joining for good, and leaves the device open: closing the handle
    under a read still running there turns an exit into a crash.

  * the tool itself, driven through its prompt: "q" exits 0, where it used to
    raise TypeError because the handler for it was called without the line
    the user typed. A Ctrl-C with a command running returns to the prompt,
    having asked the device to go idle, and the tool then exits 0 too. Both
    cases hung before, since nothing let the interpreter exit while the
    command and receive threads were alive.
"""

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
draining the endpoint, and UBTBR_FAKE_SETUP_DELAY how long claiming the
interface takes, which holds the tool short of its prompt.
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
        time.sleep(float(os.environ.get("UBTBR_FAKE_SETUP_DELAY", "0")))

    def bulkWrite(self, endpoint, data, timeout=0):
        self.writes += 1
        path = os.environ.get("UBTBR_FAKE_WRITES")
        if path:
            with open(path, "a") as writes:
                writes.write("%s %d\\n" % (bytes(data).hex(), timeout))
        taken = os.environ.get("UBTBR_FAKE_WRITE_FAILS_AFTER")
        if taken is not None and self.writes > int(taken):
            raise USBErrorTimeout("timeout")
        return len(data)

    def bulkRead(self, endpoint, length, timeout=0):
        time.sleep(timeout / 1000.0)
        raise USBErrorTimeout("timeout")

    def close(self):
        pass


class USBContext:
    def openByVendorIDAndProductID(self, vendor, product,
                                   skip_on_error=False):
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

    def register_msg_handler(self, handler):
        assert self.handler is None
        self.handler = handler

    def unregister_msg_handler(self, handler):
        # Compared the way BTCtl compares it: each read of a bound method
        # gives a new object, so "is" would never match.
        assert self.handler == handler
        self.handler = None

    def send_idle_cmd(self):
        self.idle_requests += 1

    def send_inquiry_cmd(self):
        self.inquiry_requests += 1


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

# ---- a command of a super command that will not stop ----------------------

# Driven through the pieces a super command uses on itself: the thread it
# would run them from is what the case is about.
class WedgedCmd(btctl.BTCtlCmd):
    """A command whose loop does not look at the stop."""

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

# ---- the tool, driven through its prompt ----------------------------------

ENVIRONMENT = dict(
    os.environ,
    PYTHONPATH=os.pathsep.join((WORK.name, MODULE_DIR)),
    PYTHONDONTWRITEBYTECODE="1",
    UBTBR_FAKE_WRITES=WRITES_FILE,
)


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
# error and exits 1, where the write used to wait for good
running = tool(dict(ENVIRONMENT, UBTBR_FAKE_WRITE_FAILS_AFTER="0"))
output, errors = running.communicate("", timeout=DEADLINE * 2)
assert running.returncode == 1, (running.returncode, output, errors)
assert output == "", output
assert "error: could not set up the Ubertooth" in errors, errors
assert "Traceback" not in errors, errors
print("ubtbr: a device that takes no request is reported, not waited on")

# a device that stops taking requests once the prompt is up: that command
# fails, the prompt stays, and the tool still exits 0. The three writes taken
# are the ones the tool sends before its prompt.
running = tool(dict(ENVIRONMENT, UBTBR_FAKE_WRITE_FAILS_AFTER="3"))
output, errors = running.communicate("bdaddr 11:22:33:44:55:66\nq\n",
                                     timeout=DEADLINE * 2)
assert running.returncode == 0, (running.returncode, output, errors)
assert "error: timeout" in errors, errors
assert "Traceback" not in errors, errors
print("ubtbr: a request the device will not take ends the command, not the tool")

# an interrupt before the prompt is up, which is the window the hang shortly
# after start lives in: the tool ends, with no traceback and nothing on
# standard output
running = tool(dict(ENVIRONMENT, UBTBR_FAKE_SETUP_DELAY=str(DEADLINE)))
time.sleep(0.5)
running.send_signal(signal.SIGINT)
output, errors = running.communicate("", timeout=DEADLINE * 2)
assert running.returncode == 1, (running.returncode, output, errors)
assert output == "", output
assert "Traceback" not in errors, errors
print("ubtbr: an interrupt before the prompt ends the tool")

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
