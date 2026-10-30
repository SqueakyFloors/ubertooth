"""Where this tool's messages go: results, diagnostics, and a reader that left.

Standard output carries what the user asked for and standard error everything
else, as ubertooth(7) describes for every tool. These helpers are here rather
than in btctl.py so that ubertooth-btbr can report a bad command line, and
answer -V and -h, before it imports usb1: that import is what a host without
the Python libusb1 package cannot do, and -V is the call that has to work
there, since it is how a user says which tools they have.

Nothing here imports anything outside the standard library.
"""

import errno
import os
import sys
from threading import Lock
from time import strftime

def eprint(msg):
	sys.stderr.write(msg)
	sys.stderr.flush()

# Set once standard output has gone away, so the session can end with a
# status that says what was asked for was not all printed. A command runs on
# its own thread and the prompt on another, so either can find it first.
_output_gone = False
_output_lock = Lock()

def output_gone():
	"""True if standard output went away while the session was running."""
	return _output_gone

def point_at_the_bin(stream):
	"""Send a stream whose reader has gone to the bin, so writing it is safe.

	A write to a pipe nobody reads raises, and the interpreter flushes both
	streams as it exits, where no handler can reach the failure: that ends
	the tool in status 120 and a message of its own. Pointing the descriptor
	at the bin keeps whatever is written afterwards, including that last
	flush, from failing.
	"""
	try:
		nowhere = os.open(os.devnull, os.O_WRONLY)
		try:
			os.dup2(nowhere, stream.fileno())
		finally:
			os.close(nowhere)
	except (OSError, ValueError, AttributeError):
		# There is no such stream to point anywhere: it was closed before
		# the tool started. Nothing is written to it either way.
		pass

def output_lost():
	"""Note that standard output has gone away, and stop writing to it.

	Called from wherever a write to that stream failed. The descriptor is
	pointed at the bin, so nothing written afterwards fails either --
	including the flush the interpreter does as it exits, which no handler
	can reach and which would end the tool in status 120 and a message of
	its own. Said once, however many writes find the stream gone, and from
	whichever thread finds it first.
	"""
	global _output_gone
	with _output_lock:
		if _output_gone:
			return
		_output_gone = True
	point_at_the_bin(sys.stdout)
	try:
		error("standard output has gone away")
	except (BrokenPipeError, AttributeError, ValueError):
		# Standard error went the same way, which is what `2>&1` into a
		# reader that left leaves, or a descriptor closed before the tool
		# started. The exit status is then all there is to say it with.
		point_at_the_bin(sys.stderr)

def result(msg):
	"""Report what a command decoded: a packet, or an inquiry response.

	On standard output, where ubertooth(7) puts decoded packets, so a session
	can be piped or saved. Every line gets the time it was printed in front
	of it, `HH:MM:SS `, which is the only timing a session has; a message of
	several lines, such as an inquiry response and the EIR under it, is
	therefore still one timed line per line. Blank lines are dropped, so what
	a reader gets is packets and nothing else.
	"""
	now = strftime("%H:%M:%S")
	try:
		# Written in one go, so that a message of several lines reaches the
		# reader whole: an inquiry response and the EIR fields under it are
		# one message, and nothing should land between their lines.
		sys.stdout.write("".join("%s %s\n"%(now, line)
			for line in msg.splitlines() if line))
		sys.stdout.flush()
	except BrokenPipeError:
		# Whatever was reading the capture has stopped, which is what
		# `ubertooth-btbr | head -5` does.
		output_lost()
	except OSError as problem:
		# The stream is there and the descriptor under it refuses every
		# write with EBADF, which is what `ubertooth-btbr 1<&0` leaves. The
		# same loss, so it is reported the same way: anything let out of
		# here is on the thread the command runs on, where it ends the
		# capture and is reported as a failure of the command instead. Any
		# other OSError is a failure of its own and is left to be raised.
		if problem.errno != errno.EBADF:
			raise
		output_lost()

def warn(msg):
	"""Report something the user should know about: one `warning: ` line.

	On standard error, the way every other tool in this fork writes its
	warnings. btctl's logger carries the progress a command makes, and
	result() above carries what it decoded; a diagnostic about the tool
	itself is neither, so it goes through neither.
	"""
	eprint("warning: %s\n"%msg)

def error(msg):
	"""Report a failure, prefixed `error: `, on standard error.

	One line, unless the message itself carries a `note: ` line giving the
	usual cause -- btctl's open_failure_note() and unread_serials_note()
	build those. Used both for a command line the tool will not take and
	where there is nobody above to raise to: a command runs on its own
	thread, so anything let out of there is a traceback nobody can report.
	"""
	eprint("error: %s\n"%msg)
