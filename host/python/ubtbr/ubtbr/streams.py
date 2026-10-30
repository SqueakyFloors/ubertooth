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
	"""Write a diagnostic to standard error, or drop it if it cannot go there.

	Every error, warning and note the tool gives goes through here, so that
	losing the stream costs the lines and nothing else: a write that fails
	must not raise in the middle of a session and end a capture that was
	running.

	The state this is for is a pipe whose reader has left, which
	can_be_written() cannot see at startup on Linux. Any other failure of the
	stream is taken the same way, a full disk among them: there is nowhere
	left to report the loss, so the line goes and the exit status is the
	command's own. The stream is pointed at the bin so the writes to come are
	safe.
	"""
	try:
		sys.stderr.write(msg)
		sys.stderr.flush()
	except (OSError, ValueError, AttributeError):
		point_at_the_bin(sys.stderr)

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

def can_be_written(stream):
	"""Say whether a stream's descriptor would take what is written to it.

	A write of nothing asks the descriptor that question and puts no text on
	it: one opened for reading only refuses it, whatever the length. A flush
	cannot ask, because an empty buffer flushes whichever state the
	descriptor is in.

	It is not the whole answer on Linux, where a pipe whose reader has left
	takes a write of nothing and refuses the next real one (fs/pipe.c,
	pipe_write(), "Null write succeeds"); macOS refuses both. So a true here
	means the descriptor is open for writing, not that a reader is still
	listening, and flush_the_streams() below is what covers the difference.
	"""
	try:
		os.write(stream.fileno(), b"")
		return True
	except (OSError, ValueError, AttributeError):
		return False

def bin_an_unusable_standard_error():
	"""Keep this tool's diagnostics out of the capture and out of its status.

	With no such stream, eprint() has nothing to write to and every
	diagnostic the tool gives ends in a traceback. Descriptor 2 is pointed at
	the bin instead, so a child -- ubertooth-util, for the -V answer --
	inherits the bin rather than a closed descriptor whose number its next
	open() would take. Taking a lower descriptor would be worse still:
	standard output may be closed too, and a child handed a writable bin for
	it would throw away what it was asked to write.

	A descriptor opened for reading only leaves a stream whose every write
	fails, including the flush the interpreter does as it exits, where no
	handler can reach it: the bin takes those lines too, so the command ends
	with its own exit status rather than status 120.
	"""
	if sys.stderr is None:
		nowhere = os.open(os.devnull, os.O_WRONLY)
		if nowhere == 2:
			# The bin is standard error itself, and os.open gives a
			# descriptor no child inherits (PEP 446), so it is marked
			# as one a child does: handed back the closed descriptor,
			# the first file the child opened would take that number.
			os.set_inheritable(2, True)
		else:
			# dup2 marks what it writes to as inheritable, so the
			# child gets the bin here too.
			os.dup2(nowhere, 2)
			os.close(nowhere)
		sys.stderr = open(2, "w", closefd=False)
	elif not can_be_written(sys.stderr):
		point_at_the_bin(sys.stderr)

def flush_the_streams():
	"""Empty both standard streams while a failure can still be handled.

	Registered with atexit, which the interpreter runs before the flush it
	does as it exits. That last flush is outside every handler: one that
	fails there ends the tool in status 120 and a message of its own,
	whatever the session made of its work. So a stream that refuses the
	flush here is pointed at the bin and flushed again, which empties the
	buffer and leaves the interpreter's flush nothing to fail at. The exit
	status is then the command's own, as it is for every other lost line.

	This is the state can_be_written() cannot see: a pipe whose reader has
	left, which on Linux passes for writable at startup and is found only by
	a real write.
	"""
	for stream in (sys.stdout, sys.stderr):
		try:
			stream.flush()
		except (OSError, ValueError, AttributeError):
			# A failed flush leaves what it could not write in the
			# buffer, so the bin has to be given it rather than only the
			# writes to come.
			point_at_the_bin(stream)
			try:
				stream.flush()
			except (OSError, ValueError, AttributeError):
				# There is no such stream to empty, or none the bin could
				# be put under. Nothing more can be done here, and the
				# exit status is what reports the loss.
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
	# Standard error may have gone the same way, which is what `2>&1` into a
	# reader that left leaves, or a descriptor closed before the tool
	# started. eprint() drops the line in that case, and the exit status is
	# then all there is to say it with.
	error("standard output has gone away")

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
		# capture and is reported as a failure of the command instead.
		#
		# Only that errno, which is the stream being unusable from the
		# start. A write that failed for a reason of its own -- no space
		# left on the device it was redirected to, a terminal that went
		# away -- is worth naming, and ends the command carrying its own
		# message rather than this one.
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
