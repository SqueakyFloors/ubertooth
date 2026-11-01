from time import time
import logging
from binascii import unhexlify, hexlify
from struct import pack, unpack
# One spelling of a `warning: ` line in the package. btctl imports this
# module, so its own warn() could only be reached from inside a function;
# streams.py imports nothing of the package, so there is no cycle here.
from ubtbr.streams import data_to_str, warn
log = logging.getLogger("btctl")
LLID_LMP = 3

EXT_OPCODE = 0x100

# LMP opcodes
LMP_NAME_REQ                     = 1
LMP_NAME_RES                     = 2
LMP_ACCEPTED                     = 3
LMP_NOT_ACCEPTED                 = 4
LMP_CLKOFFSET_REQ                = 5
LMP_CLKOFFSET_RES                = 6
LMP_DETACH                       = 7
LMP_IN_RAND                      = 8
LMP_COMB_KEY                     = 9
LMP_UNIT_KEY                     = 10
LMP_AU_RAND                      = 11
LMP_SRES                         = 12
LMP_TEMP_RAND                    = 13
LMP_TEMP_KEY                     = 14
LMP_ENCRYPTION_MODE_REQ          = 15
LMP_ENCRYPTION_KEY_SIZE_REQ      = 16
LMP_START_ENCRYPTION_REQ         = 17
LMP_STOP_ENCRYPTION_REQ          = 18
LMP_SWITCH_REQ                   = 19
LMP_HOLD                         = 20
LMP_HOLD_REQ                     = 21
LMP_SNIFF                        = 22
LMP_SNIFF_REQ                    = 23
LMP_UNSNIFF_REQ                  = 24
LMP_PARK_REQ                     = 25
LMP_SET_BROADCAST_SCAN_WINDOW    = 27
LMP_MODIFY_BEACON                = 28
LMP_UNPARK_BD_ADDR_REQ           = 29
LMP_UNPARK_PM_ADDR_REQ           = 30
LMP_INCR_POWER_REQ               = 31
LMP_DECR_POWER_REQ               = 32
LMP_MAX_POWER                    = 33
LMP_MIN_POWER                    = 34
LMP_AUTO_RATE                    = 35
LMP_PREFERRED_RATE               = 36
LMP_VERSION_REQ                  = 37
LMP_VERSION_RES                  = 38
LMP_FEATURES_REQ                 = 39
LMP_FEATURES_RES                 = 40
LMP_QUALITY_OF_SERVICE           = 41
LMP_QUALITY_OF_SERVICE_REQ       = 42
LMP_SCO_LINK_REQ                 = 43
LMP_REMOVE_SCO_LINK_REQ          = 44
LMP_MAX_SLOT                     = 45
LMP_MAX_SLOT_REQ                 = 46
LMP_TIMING_ACCURACY_REQ          = 47
LMP_TIMING_ACCURACY_RES          = 48
LMP_SETUP_COMPLETE               = 49
LMP_USE_SEMI_PERMANENT_KEY       = 50
LMP_HOST_CONNECTION_REQ          = 51
LMP_SLOT_OFFSET                  = 52
LMP_PAGE_MODE_REQ                = 53
LMP_PAGE_SCAN_MODE_REQ           = 54
LMP_SUPERVISION_TIMEOUT          = 55
LMP_TEST_ACTIVATE                = 56
LMP_TEST_CONTROL                 = 57
LMP_ENCRYPTION_KEY_SIZE_MASK_REQ = 58
LMP_ENCRYPTION_KEY_SIZE_MASK_RES = 59
LMP_SET_AFH                      = 60
LMP_ENCAPSULATED_HEADER          = 61
LMP_ENCAPSULATED_PAYLOAD         = 62
LMP_SIMPLE_PAIRING_CONFIRM       = 63
LMP_SIMPLE_PAIRING_NUMBER        = 64
LMP_DHKEY_CHECK                  = 65
LMP_ESCAPE_1                     = 124
LMP_ESCAPE_2                     = 125
LMP_ESCAPE_3                     = 126
LMP_ESCAPE_4                     = 127

# LMP extended opcodes
LMP_ACCEPTED_EXT                = 1
LMP_NOT_ACCEPTED_EXT            = 2
LMP_FEATURES_REQ_EXT            = 3
LMP_FEATURES_RES_EXT            = 4
LMP_CLK_ADJ                     = 5
LMP_PACKET_TYPE_TABLE_REQ       = 11
LMP_ESCO_LINK_REQ               = 12
LMP_REMOVE_ESCO_LINK_REQ        = 13
LMP_CHANNEL_CLASSIFICATION_REQ  = 16
LMP_CHANNEL_CLASSIFICATION      = 17
LMP_SNIFF_SUBRATING_REQ         = 21
LMP_SNIFF_SUBRATING_RES         = 22
LMP_PAUSE_ENCRYPTION_REQ        = 23
LMP_RESUME_ENCRYPTION_REQ       = 24
LMP_IO_CAPABILITY_REQ           = 25
LMP_IO_CAPABILITY_RES           = 26
LMP_NUMERIC_COMPARISON_FAILED   = 27
LMP_PASSKEY_FAILED              = 28
LMP_OOB_FAILED                  = 29
LMP_KEYPRESS_NOTIFICATION       = 30
LMP_POWER_CONTROL_REQ           = 31
LMP_POWER_CONTROL_RES           = 32

# Error codes a refusal carries (Core v5.4 | Vol 1, Part F, 1.3).
LMP_ERR_UNKNOWN_LMP_PDU         = 0x19
LMP_ERR_UNSUPPORTED_LMP_FEATURE = 0x1a

# Why a peer says it is hanging up: the reasons a detach is seen carrying, as
# Core v5.4 | Vol 1, Part F, 1.3 names them. Any other code is reported as
# itself.
LMP_DETACH_REASON2STR = {
	0x05: "authentication failure",
	0x13: "the peer's user ended the connection",
	0x14: "the peer is low on resources",
	0x15: "the peer is about to power off",
	0x16: "the peer's own host ended the connection",
	0x1a: "unsupported remote feature",
	0x22: "LMP response timeout",
	0x29: "pairing with a unit key is not supported",
}

# What LMP_SET_AFH carries: a 4-byte instant, a 1-byte mode, and a 10-byte
# channel map, one bit per channel (Core v5.4 | Vol 2, Part C, 5.2).
AFH_PARAMETERS_LEN = 15

# The reasons an LMP_NOT_ACCEPTED carries here
# (Core v5.4 | Vol 1, Part F, 1.3)
LMP_ERR_UNSUPPORTED_REMOTE_FEATURE	= 0x1a
LMP_ERR_INVALID_LMP_PARAMETERS		= 0x1e
LMP_ERR_UNSUPPORTED_PARAM_VALUE		= 0x20

# The same codes in words, for the line a peer's refusal gets. Any other code
# is reported as itself.
LMP_ERR2STR = {
	LMP_ERR_UNKNOWN_LMP_PDU: "unknown LMP PDU",
	# The same words the detach table above gives this code.
	LMP_ERR_UNSUPPORTED_LMP_FEATURE: "unsupported remote feature",
	LMP_ERR_INVALID_LMP_PARAMETERS: "invalid LMP parameters",
	LMP_ERR_UNSUPPORTED_PARAM_VALUE: "unsupported parameter value",
}

LMP_OP2STR = {
	LMP_NAME_REQ: "LMP_NAME_REQ",
	LMP_NAME_RES: "LMP_NAME_RES",
	LMP_ACCEPTED: "LMP_ACCEPTED",
	LMP_NOT_ACCEPTED: "LMP_NOT_ACCEPTED",
	LMP_CLKOFFSET_REQ: "LMP_CLKOFFSET_REQ",
	LMP_CLKOFFSET_RES: "LMP_CLKOFFSET_RES",
	LMP_DETACH: "LMP_DETACH",
	LMP_IN_RAND: "LMP_IN_RAND",
	LMP_COMB_KEY: "LMP_COMB_KEY",
	LMP_UNIT_KEY: "LMP_UNIT_KEY",
	LMP_AU_RAND: "LMP_AU_RAND",
	LMP_SRES: "LMP_SRES",
	LMP_TEMP_RAND: "LMP_TEMP_RAND",
	LMP_TEMP_KEY: "LMP_TEMP_KEY",
	LMP_ENCRYPTION_MODE_REQ: "LMP_ENCRYPTION_MODE_REQ",
	LMP_ENCRYPTION_KEY_SIZE_REQ: "LMP_ENCRYPTION_KEY_SIZE_REQ",
	LMP_START_ENCRYPTION_REQ: "LMP_START_ENCRYPTION_REQ",
	LMP_STOP_ENCRYPTION_REQ: "LMP_STOP_ENCRYPTION_REQ",
	LMP_SWITCH_REQ: "LMP_SWITCH_REQ",
	LMP_HOLD: "LMP_HOLD",
	LMP_HOLD_REQ: "LMP_HOLD_REQ",
	LMP_SNIFF: "LMP_SNIFF",
	LMP_SNIFF_REQ: "LMP_SNIFF_REQ",
	LMP_UNSNIFF_REQ: "LMP_UNSNIFF_REQ",
	LMP_PARK_REQ: "LMP_PARK_REQ",
	LMP_SET_BROADCAST_SCAN_WINDOW: "LMP_SET_BROADCAST_SCAN_WINDOW",
	LMP_MODIFY_BEACON: "LMP_MODIFY_BEACON",
	LMP_UNPARK_BD_ADDR_REQ: "LMP_UNPARK_BD_ADDR_REQ",
	LMP_UNPARK_PM_ADDR_REQ: "LMP_UNPARK_PM_ADDR_REQ",
	LMP_INCR_POWER_REQ: "LMP_INCR_POWER_REQ",
	LMP_DECR_POWER_REQ: "LMP_DECR_POWER_REQ",
	LMP_MAX_POWER: "LMP_MAX_POWER",
	LMP_MIN_POWER: "LMP_MIN_POWER",
	LMP_AUTO_RATE: "LMP_AUTO_RATE",
	LMP_PREFERRED_RATE: "LMP_PREFERRED_RATE",
	LMP_VERSION_REQ: "LMP_VERSION_REQ",
	LMP_VERSION_RES: "LMP_VERSION_RES",
	LMP_FEATURES_REQ: "LMP_FEATURES_REQ",
	LMP_FEATURES_RES: "LMP_FEATURES_RES",
	LMP_QUALITY_OF_SERVICE: "LMP_QUALITY_OF_SERVICE",
	LMP_QUALITY_OF_SERVICE_REQ: "LMP_QUALITY_OF_SERVICE_REQ",
	LMP_SCO_LINK_REQ: "LMP_SCO_LINK_REQ",
	LMP_REMOVE_SCO_LINK_REQ: "LMP_REMOVE_SCO_LINK_REQ",
	LMP_MAX_SLOT: "LMP_MAX_SLOT",
	LMP_MAX_SLOT_REQ: "LMP_MAX_SLOT_REQ",
	LMP_TIMING_ACCURACY_REQ: "LMP_TIMING_ACCURACY_REQ",
	LMP_TIMING_ACCURACY_RES: "LMP_TIMING_ACCURACY_RES",
	LMP_SETUP_COMPLETE: "LMP_SETUP_COMPLETE",
	LMP_USE_SEMI_PERMANENT_KEY: "LMP_USE_SEMI_PERMANENT_KEY",
	LMP_HOST_CONNECTION_REQ: "LMP_HOST_CONNECTION_REQ",
	LMP_SLOT_OFFSET: "LMP_SLOT_OFFSET",
	LMP_PAGE_MODE_REQ: "LMP_PAGE_MODE_REQ",
	LMP_PAGE_SCAN_MODE_REQ: "LMP_PAGE_SCAN_MODE_REQ",
	LMP_SUPERVISION_TIMEOUT: "LMP_SUPERVISION_TIMEOUT",
	LMP_TEST_ACTIVATE: "LMP_TEST_ACTIVATE",
	LMP_TEST_CONTROL: "LMP_TEST_CONTROL",
	LMP_ENCRYPTION_KEY_SIZE_MASK_REQ: "LMP_ENCRYPTION_KEY_SIZE_MASK_REQ",
	LMP_ENCRYPTION_KEY_SIZE_MASK_RES: "LMP_ENCRYPTION_KEY_SIZE_MASK_RES",
	LMP_SET_AFH: "LMP_SET_AFH",
	LMP_ENCAPSULATED_HEADER: "LMP_ENCAPSULATED_HEADER",
	LMP_ENCAPSULATED_PAYLOAD: "LMP_ENCAPSULATED_PAYLOAD",
	LMP_SIMPLE_PAIRING_CONFIRM: "LMP_SIMPLE_PAIRING_CONFIRM",
	LMP_SIMPLE_PAIRING_NUMBER: "LMP_SIMPLE_PAIRING_NUMBER",
	LMP_DHKEY_CHECK: "LMP_DHKEY_CHECK",
	LMP_ESCAPE_1: "LMP_ESCAPE_1",
	LMP_ESCAPE_2: "LMP_ESCAPE_2",
	LMP_ESCAPE_3: "LMP_ESCAPE_3",
	LMP_ESCAPE_4: "LMP_ESCAPE_4",
}

LMP_OPEXT2STR = {
	LMP_ACCEPTED_EXT: "LMP_ACCEPTED_EXT",
	LMP_NOT_ACCEPTED_EXT: "LMP_NOT_ACCEPTED_EXT",
	LMP_FEATURES_REQ_EXT: "LMP_FEATURES_REQ_EXT",
	LMP_FEATURES_RES_EXT: "LMP_FEATURES_RES_EXT",
	LMP_CLK_ADJ: "LMP_CLK_ADJ",
	LMP_PACKET_TYPE_TABLE_REQ: "LMP_PACKET_TYPE_TABLE_REQ",
	LMP_ESCO_LINK_REQ: "LMP_ESCO_LINK_REQ",
	LMP_REMOVE_ESCO_LINK_REQ: "LMP_REMOVE_ESCO_LINK_REQ",
	LMP_CHANNEL_CLASSIFICATION_REQ: "LMP_CHANNEL_CLASSIFICATION_REQ",
	LMP_CHANNEL_CLASSIFICATION: "LMP_CHANNEL_CLASSIFICATION",
	LMP_SNIFF_SUBRATING_REQ: "LMP_SNIFF_SUBRATING_REQ",
	LMP_SNIFF_SUBRATING_RES: "LMP_SNIFF_SUBRATING_RES",
	LMP_PAUSE_ENCRYPTION_REQ: "LMP_PAUSE_ENCRYPTION_REQ",
	LMP_RESUME_ENCRYPTION_REQ: "LMP_RESUME_ENCRYPTION_REQ",
	LMP_IO_CAPABILITY_REQ: "LMP_IO_CAPABILITY_REQ",
	LMP_IO_CAPABILITY_RES: "LMP_IO_CAPABILITY_RES",
	LMP_NUMERIC_COMPARISON_FAILED: "LMP_NUMERIC_COMPARISON_FAILED",
	LMP_PASSKEY_FAILED: "LMP_PASSKEY_FAILED",
	LMP_OOB_FAILED: "LMP_OOB_FAILED",
	LMP_KEYPRESS_NOTIFICATION: "LMP_KEYPRESS_NOTIFICATION",
	LMP_POWER_CONTROL_REQ: "LMP_POWER_CONTROL_REQ",
	LMP_POWER_CONTROL_RES: "LMP_POWER_CONTROL_RES",
}

# PDUs that normally expect no reply: responses and the notifications this
# end has no handler for. Vol 2,
# Part C, 2.5 asks the LM to ignore such a PDU rather than refuse it, so none
# of these puts anything on the air. Anything else with no handler is refused,
# which is the safe way round: a PDU missing from here costs one needless
# refusal, where one wrongly listed would cost the link.
#
# The power control requests are not here on purpose. They too are answered
# only at the ends of the range, but 4.1.3 requires a device that does not
# support power control, as this one does not, to answer them anyway: with
# LMP_MAX_POWER or LMP_MIN_POWER, or with Unsupported LMP Feature, which is
# the honest one here.
LMP_NO_REPLY_EXPECTED = frozenset((
	# with a 7-bit opcode
	LMP_NAME_RES, LMP_ACCEPTED, LMP_NOT_ACCEPTED, LMP_CLKOFFSET_RES,
	LMP_SRES, LMP_TEMP_KEY, LMP_HOLD,
	LMP_SET_BROADCAST_SCAN_WINDOW, LMP_MODIFY_BEACON,
	LMP_MAX_POWER, LMP_MIN_POWER,
	LMP_AUTO_RATE, LMP_PREFERRED_RATE, LMP_VERSION_RES, LMP_FEATURES_RES,
	LMP_QUALITY_OF_SERVICE, LMP_MAX_SLOT, LMP_TIMING_ACCURACY_RES,
	LMP_SLOT_OFFSET, LMP_SUPERVISION_TIMEOUT,
	LMP_ENCRYPTION_KEY_SIZE_MASK_RES,
	# and with a 15-bit one
	EXT_OPCODE|LMP_ACCEPTED_EXT, EXT_OPCODE|LMP_NOT_ACCEPTED_EXT,
	EXT_OPCODE|LMP_FEATURES_RES_EXT,
	EXT_OPCODE|LMP_CHANNEL_CLASSIFICATION, EXT_OPCODE|LMP_SNIFF_SUBRATING_RES,
	EXT_OPCODE|LMP_IO_CAPABILITY_RES, EXT_OPCODE|LMP_NUMERIC_COMPARISON_FAILED,
	EXT_OPCODE|LMP_PASSKEY_FAILED, EXT_OPCODE|LMP_OOB_FAILED,
	EXT_OPCODE|LMP_KEYPRESS_NOTIFICATION, EXT_OPCODE|LMP_POWER_CONTROL_RES,
))

def u8(d):	return d
def u16(d):
	return unpack("<H", d)[0]
def u32(d):
	return unpack("<I", d)[0]
def p8(n):	return pack("<B", n)
def p16(n):	return pack("<H", n)
def p32(n):	return pack("<I", n)
def p64(n):	return pack("<Q", n)

def opcode2str(op):
	"""An opcode as table 5.1 writes it: 0x20, or 127/11 for a 15-bit one."""
	if op & EXT_OPCODE:
		return "%d/%d"%(LMP_ESCAPE_4, op & ~EXT_OPCODE)
	return "0x%x"%op

def op2str(op):
	"""An opcode's name, plain or extended, or its number where it has none.

	The one place an opcode is named, so a capture and a warning about the
	same PDU call it the same thing.
	"""
	if op & EXT_OPCODE:
		ext = op & ~EXT_OPCODE
		return LMP_OPEXT2STR.get(ext, "LMP extended opcode %d"%ext)
	return LMP_OP2STR.get(op, "LMP opcode %d"%op)

def pdu2str(pdu):
	# A PDU of no bytes, which the ACL payload header can say the length of
	# and a capture should still show as what arrived.
	if not pdu:
		return "(no tid) %-40s |"%"LMP PDU of no bytes"

	opcode = u8(pdu[0])
	tid = opcode & 1
	opcode >>= 1

	# An opcode with no name is reported as its number: a capture of a PDU
	# this tool does not know should still say which one it was.
	if opcode == LMP_ESCAPE_4:
		if len(pdu) < 2:
			# The escape opcode says the real one is in the second byte,
			# and this PDU has no second byte. Reading it anyway used to
			# end the session in an IndexError.
			opstr = "%s with no extended opcode"%op2str(LMP_ESCAPE_4)
		else:
			opstr = op2str(u8(pdu[1]) | EXT_OPCODE)
	else:
		opstr = op2str(opcode)
	# Plain text, no terminal color: this goes into the packets a capture
	# holds on standard output, where escapes are something for a reader of
	# that capture to strip.
	return ("(tid %d) %-40s | %s"%(tid, opstr, data_to_str(pdu)))

class LMP:
	# Features supported (Core v5.2 | Vol 2, Part C, table 3.2 p585)
	# Byte 0's two lowest bits, 3-slot and 5-slot packets, stay clear: a
	# peer's LMP_MAX_SLOT_REQ for more than one slot is refused, and the
	# mask has to say what this end will really do.
	FEATURES = b"\x00\x00\x00\x00\x00\x00\x19\x00" # No AFH

	def __init__(self, con, debug=True):
		self._debug = debug
		self._con = con
		self._start_time = time()
		self._clkn = 0
		# The opcodes already reported as unhandled, so each is said once for
		# the life of the connection rather than once per PDU.
		self._unhandled = set()
		self._rx_tid = 0

	def time(self):
		return time()-self._start_time

	# FSM per LMP_state
	def receive(self, clkn, pdu):
		self._clkn = clkn
		if self._debug:
			log.info("%d|%.2f sec | <<< lmp_rx (state=%d): %s"%(clkn,self.time(),self._state, pdu2str(pdu)))

		# A PDU with no opcode to act on, which is a message from the board
		# that ended early: what goes over the air is padded to the 17 bytes
		# a DM1 carries. There is no opcode to name in a refusal either
		# (Vol 2, Part C, 2.5), so the packet is reported and the connection
		# goes on.
		if not pdu:
			warn("an LMP PDU of no bytes arrived, so there is nothing to "
				"answer")
			return False

		# Parse lmp message
		opcode = u8(pdu[0])
		tid = opcode & 1
		opcode >>= 1

		# An answer belongs to the transaction that asked for it, so the
		# handlers below send it back with this PDU's transaction ID: 0 for
		# one the central started, 1 for the peripheral's
		# (Core v5.4 | Vol 2, Part C, 2.4). A peer cannot match an answer
		# carrying the other one, and goes on waiting.
		self._rx_tid = tid

		if opcode == LMP_ESCAPE_4:
			if len(pdu) < 2:
				# The escape says the opcode is 15 bits and this PDU holds
				# only the first 7 of them, so which PDU it is cannot be
				# known: a refusal has to name the extended opcode it
				# refuses (2.7) and there is none here. Reading that byte
				# anyway used to end the session in an IndexError.
				warn("an LMP PDU of one byte under %s arrived, carrying no "
					"extended opcode"%LMP_OP2STR[LMP_ESCAPE_4])
				return False
			op = u8(pdu[1]) | EXT_OPCODE
			data = pdu[2:]
		else:
			op = opcode
			data = pdu[1:]

		fsm = self._FSM[self._state]
		reg = fsm.get(op)
		if reg is None:
			# A request this link manager does not handle still has to be
			# answered: silence costs the peer a 30-second LMP response
			# timeout and then the link (Vol 2, Part C, 2.4.1).
			refused = self.lmp_refuse(op, tid)
			# Said once per opcode: a real peer repeats a request several
			# times a second, and every one of them is in the capture on
			# standard output already.
			if op not in self._unhandled:
				self._unhandled.add(op)
				warn("LMP opcode %s is not handled in state %d%s"
					%(opcode2str(op), self._state,
						", refused" if refused else ""))
			return False
		else:
			handler = reg[0]
			if len(reg) > 1:
				next_state = reg[1]
			else:
				next_state = self._state
			retval = handler(op, data) or 0
			if retval is not None:
				if retval < 0:
					return retval
				elif retval > 0:
					next_state = retval
			self.set_state(next_state)

	def set_state(self, state):
		if (state != self._state):
			log.info("%.2f sec | Switch state %d -> %d"%(self.time(), self._state, state))
			self._state = state

	def lmp_down(self, pdu):
		self._con.send_acl(LLID_LMP, pdu)

	def pack_lmp(self, opcode, data, tid=0):
		pdu = p8((opcode<<1)|(tid&1)) + data
		pdu = pdu.ljust(17, b"\x00")
		return pdu

	def lmp_send(self, opcode, data, tid=0):
		pdu = self.pack_lmp(opcode, data, tid=tid)
		if self._debug:
			log.info("%.2f sec | >>> lmp_tx (state=%d): %s"%(self.time(),self._state, pdu2str(pdu)))
		return self.lmp_down(pdu)

	def handle_detach(self, op, data):
		# The peer is hanging up, and the reason it gives is the one line
		# that says why a session ended. Vol 2, Part C, 4.1.2 asks the
		# receiver to detach rather than hold the connection open until
		# link supervision ends it some seconds later. The grace timer that
		# section describes, 6 x Tpoll here, is skipped: it is there so the
		# detach can be acknowledged, and this end sends nothing more.
		if data:
			reason = u8(data[0])
			log.info("Peer detached: reason 0x%02x, %s"%(reason,
				LMP_DETACH_REASON2STR.get(reason,
					"a reason this tool does not name")))
		else:
			log.info("Peer detached, giving no reason")
		self._con.stop()

	def handle_name_req(self, op, data):
		if not data:
			# Too short to say which part of the name is wanted, which the
			# answer says rather than raising on the read
			# (Vol 2, Part C, 2.5).
			return self.lmp_send_not_accepted(op,
				p8(LMP_ERR_INVALID_LMP_PARAMETERS), tid=self._rx_tid)
		offset = u8(data[0])
		name = b"Ubertooth"
		return self.lmp_send_name_res(offset, len(name),
			name[offset:offset+14], tid=self._rx_tid)

	def handle_feat_req(self, op, data):	
		log.info("handle_feat_req")
		self.lmp_send_feat(False, tid=self._rx_tid)

	def handle_feat_req_ext(self, op, data):
		log.info("handle_feat_req_ext")
		if not data:
			# Too short to say which features page is wanted. The refusal
			# of a 15-bit opcode is LMP_NOT_ACCEPTED_EXT (2.5, 2.7).
			return self.lmp_send_not_accepted_ext(op & ~EXT_OPCODE,
				LMP_ERR_INVALID_LMP_PARAMETERS, tid=self._rx_tid)
		self.lmp_send_feat_ext(False, num=u8(data[0]), tid=self._rx_tid)

	def handle_vers_req(self, op, data):	
		log.info("handle_vers_req")
		self.lmp_send_version(False, tid=self._rx_tid)

	def handle_io_cap_req(self, op, data):
		log.info("handle_io_cap_req")
		self.lmp_send_io_cap(False, tid=self._rx_tid)

	def handle_notification(self, op, data):
		# Nothing to send back. These PDUs tell this end what the peer
		# allows -- how many slots a packet of ours may take, how long the
		# link may go quiet -- rather than ask it for anything, so being
		# recognized is all they need. The PDU itself is already in the
		# capture and in the trace.
		pass

	def handle_max_slot_req(self, op, data):
		# The peer asks to send packets of that many slots. Both answers
		# are an answer; silence is not. A peer that gets neither
		# LMP_ACCEPTED nor LMP_NOT_ACCEPTED waits out its LMP response
		# timeout, 30 seconds, and then detaches the link
		# (Core v5.4 | Vol 2, Part C, 2.4.1 and 2.7).
		#
		# One slot is where a new connection starts (Vol 2, Part C, 4.1.10)
		# and where this tool keeps the link. The firmware does decode the
		# multi-slot packet kinds, but no capture has exercised that over
		# the air. So one slot is accepted and anything larger refused, and
		# the reason names the value rather than the procedure: multi-slot
		# control is mandatory for both devices (table 4.11), so it is the
		# count that is declined, not the PDU.
		slots = u8(data[0]) if data else 0
		if slots == 1:
			return self.lmp_send_accepted(op, tid=self._rx_tid)
		if slots == 0:
			# No slots at all is not a packet size, and a PDU too short to
			# hold the parameter is the same answer (Vol 2, Part C, 2.5).
			reason = LMP_ERR_INVALID_LMP_PARAMETERS
		else:
			reason = LMP_ERR_UNSUPPORTED_PARAM_VALUE
		return self.lmp_send_not_accepted(op, p8(reason), tid=self._rx_tid)

	def handle_switch_req(self, op, data):
		if len(data) < 4:
			# Too short to hold the switch instant, which the answer
			# below says rather than raising on the unpack
			# (Vol 2, Part C, 2.5).
			return self.lmp_send_not_accepted(op,
				p8(LMP_ERR_INVALID_LMP_PARAMETERS), tid=self._rx_tid)
		instant = u32(data[:4])
		log.info("switch req: instant: %d (in %d ticks)"%(
			instant, (instant<<1)-self._clkn))
		# A role switch would make this end the other side of the link,
		# which it cannot do, and its feature mask does not offer the
		# feature. Refusing is an answer, and the request needs one as much
		# as the one above does (Vol 2, Part C, 4.4.2).
		return self.lmp_send_not_accepted(op,
			p8(LMP_ERR_UNSUPPORTED_REMOTE_FEATURE), tid=self._rx_tid)

	def lmp_send_conn_req(self, **kwargs):
		return self.lmp_send(LMP_HOST_CONNECTION_REQ, b'', **kwargs)

	def lmp_send_name_req(self, offset, **kwargs):
		return self.lmp_send(LMP_NAME_REQ, p8(offset), **kwargs)

	def lmp_send_name_res(self, poffset, psize, payload, **kwargs):
		assert(len(payload)<=14)
		data = p8(poffset) + p8(psize) + payload
		return self.lmp_send(LMP_NAME_RES, data, **kwargs)

	def lmp_send_accepted(self, opcode, **kwargs):
		return self.lmp_send(LMP_ACCEPTED, p8(opcode), **kwargs)

	def lmp_send_not_accepted(self, opcode, data=b'', **kwargs):
		return self.lmp_send(LMP_NOT_ACCEPTED, p8(opcode)+data, **kwargs)

	def lmp_send_not_accepted_ext(self, ext_opcode, reason, **kwargs):
		# A 15-bit opcode is refused with LMP_NOT_ACCEPTED_EXT, which
		# carries the escape opcode as well as the extended one
		# (Vol 2, Part C, 2.7).
		data = (p8(LMP_NOT_ACCEPTED_EXT) + p8(LMP_ESCAPE_4)
			+ p8(ext_opcode) + p8(reason))
		return self.lmp_send(LMP_ESCAPE_4, data, **kwargs)

	def lmp_refuse(self, op, tid):
		"""Refuse a PDU no handler of this state's table names.

		Vol 2, Part C, 2.5: an opcode the LM does not know is refused with
		Unknown LMP PDU (0x19), and a PDU it knows but does not support with
		Unsupported LMP Feature (0x1A). This tool can name nearly all of
		LMP, so the second is what a real device's request gets.

		The refusal carries the transaction ID of what it answers: the two
		are one transaction (2.4), and a peer cannot match an answer sent
		under the other ID to what it asked.

		Returns True if a refusal went out, False for a PDU 2.5 asks to be
		ignored because it expects no reply.
		"""
		if op in LMP_NO_REPLY_EXPECTED:
			return False
		if op & EXT_OPCODE:
			ext_op = op & ~EXT_OPCODE
			self.lmp_send_not_accepted_ext(ext_op,
				LMP_ERR_UNSUPPORTED_LMP_FEATURE if ext_op in LMP_OPEXT2STR
				else LMP_ERR_UNKNOWN_LMP_PDU, tid=tid)
		else:
			# An escape names no PDU of its own, so one arriving as a plain
			# opcode is a PDU this end cannot know.
			named = op in LMP_OP2STR and op < LMP_ESCAPE_1
			self.lmp_send_not_accepted(op,
				p8(LMP_ERR_UNSUPPORTED_LMP_FEATURE if named
					else LMP_ERR_UNKNOWN_LMP_PDU), tid=tid)
		return True

	def lmp_send_io_cap(self, is_req, **kwargs):
		# ext opcode
		if is_req:
			data = p8(LMP_IO_CAPABILITY_REQ)
		else: 
			data = p8(LMP_IO_CAPABILITY_RES)	
		data += b"\x01\x00\x03"
		return self.lmp_send(LMP_ESCAPE_4, data, **kwargs)

	def lmp_send_encap_header(self, size, minor=1,major=1,**kwargs):
		payload = p8(minor)+p8(major) # version
		payload += p8(size)
		return self.lmp_send(LMP_ENCAPSULATED_HEADER, payload, **kwargs)

	def lmp_send_encap_payload(self, data, **kwargs):
		assert(len(data) == 16)
		return self.lmp_send(LMP_ENCAPSULATED_PAYLOAD, data, **kwargs)

	def lmp_send_version(self, is_req, **kwargs):
		# Opcode
		if is_req:	op = LMP_VERSION_REQ
		else:		op = LMP_VERSION_RES
		data = p8(1)		# version num
		data += p16(0xffff)	# Company identifier: none
		data += p16(2020)	# sub version num: 2020
		return self.lmp_send(op, data, **kwargs)
		
	def lmp_send_feat(self, is_req, **kwargs):
		if is_req:	op = LMP_FEATURES_REQ
		else:		op = LMP_FEATURES_RES
		return self.lmp_send(op, self.FEATURES, **kwargs)
		
	def lmp_send_feat_ext(self, is_req, num=1, **kwargs):
		# Ext opcode
		if is_req:	data = p8(LMP_FEATURES_REQ_EXT)
		else:		data = p8(LMP_FEATURES_RES_EXT)
		data += p8(num)	# page 
		data += p8(num)	# Max supported page
		data = data.ljust(11, b"\x00")	# We support none 
		return self.lmp_send(LMP_ESCAPE_4, data, **kwargs)

	def lmp_send_setup_complete(self, **kwargs):
		return self.lmp_send(LMP_SETUP_COMPLETE, b'', **kwargs)

	def lmp_send_set_afh(self, instant, mode, cmap):
		assert(len(cmap)==10)
		log.info("Send AFH req: instant=%d, (cur %d), mode=%d"%(
			instant, self._clkn, mode))
		return self.lmp_send(LMP_SET_AFH, p32(instant>>1)+p8(mode)+cmap)

# What a request the peer refused is kept as, where an answer would be kept:
# what the central asks of these is whether an answer arrived at all, and
# anything that is not None says that one did.
REFUSED = "refused by the peer"

class LMPCentral(LMP):
	def __init__(self, con):
		self._FSM = {
			# First state: fetch device infos
			1: {
				LMP_DETACH: 		(self.handle_detach,),
				LMP_VERSION_REQ: 	(self.handle_vers_req,),
				LMP_FEATURES_REQ:	(self.handle_feat_req,),
				EXT_OPCODE|LMP_FEATURES_REQ_EXT:	(self.handle_feat_req_ext,),
				EXT_OPCODE|LMP_IO_CAPABILITY_REQ: (self.handle_io_cap_req,),
				LMP_NAME_REQ: 		(self.handle_name_req,),
				LMP_FEATURES_RES:	(self.handle_info_res,),
				EXT_OPCODE|LMP_FEATURES_RES_EXT: (self.handle_info_res,),
				LMP_VERSION_RES: 	(self.handle_info_res,),
				EXT_OPCODE|LMP_IO_CAPABILITY_RES: (self.handle_info_res,),
				LMP_NAME_RES: 		(self.handle_info_res,),
				LMP_SLOT_OFFSET:	(self.handle_slot_offset,),
				LMP_SWITCH_REQ:		(self.handle_switch_req,),
				LMP_MAX_SLOT:		(self.handle_notification,),
				LMP_MAX_SLOT_REQ:	(self.handle_max_slot_req,),
				LMP_ACCEPTED:		(self.handle_accepted,),
				LMP_NOT_ACCEPTED:		(self.handle_not_accepted,),
				# A 15-bit opcode is refused under this one, so the
				# refusal of the extended features request arrives here
				# (Vol 2, Part C, 2.7).
				EXT_OPCODE|LMP_NOT_ACCEPTED_EXT: (self.handle_not_accepted,),
				LMP_SETUP_COMPLETE:	(self.handle_setup_complete,),
				LMP_ENCRYPTION_KEY_SIZE_MASK_RES: (self.handle_info_res,),
			},
		}
		self._state = 1
		self.rmt_features = None
		self.rmt_features_ext = None
		self.rmt_iocap = None
		self.rmt_version = None
		self.rmt_name = None
		self.rmt_enc_key_size_mask = None
		# What a connection opens by asking the peer for, in the order the
		# requests go out: the opcode, the attribute the answer is kept in,
		# and how it is sent. One table, so that what to ask for next and
		# what a refusal answers cannot come to disagree.
		self._opening = (
			(LMP_FEATURES_REQ, "rmt_features",
				lambda: self.lmp_send_feat(True)),
			(EXT_OPCODE|LMP_FEATURES_REQ_EXT, "rmt_features_ext",
				lambda: self.lmp_send_feat_ext(True)),
			(LMP_VERSION_REQ, "rmt_version",
				lambda: self.lmp_send_version(True)),
			(LMP_NAME_REQ, "rmt_name",
				lambda: self.lmp_send_name_req(0)),
			(LMP_ENCRYPTION_KEY_SIZE_MASK_REQ, "rmt_enc_key_size_mask",
				lambda: self.lmp_send(LMP_ENCRYPTION_KEY_SIZE_MASK_REQ, b'')),
		)
		self._opening_field = dict(
			(op, field) for op, field, send in self._opening)
		# The request the peer has been asked for and has not answered.
		# Only one is outstanding at a time, which is what lets a refusal
		# be matched to what it answers.
		self._awaiting = None
		# Has this end sent its own LMP_setup_complete yet? The setup ends
		# when each side has sent one and received the other's (Vol 2,
		# Part C, 4.1.2), so this keeps a second one from going out when
		# the peer's arrives.
		self._setup_complete_sent = False
		super().__init__(con)

	def start(self):
		self.send_info_req()

	def handle_not_accepted(self, op, data):
		"""A peer refusing one of the requests this end made.

		A connection opens by asking the peer for the five things
		send_info_req() below goes through, one at a time, each answer
		sending the next request. A refusal is an answer a peer can give to
		any of them, whether or not the procedure is one it must support,
		so it is recorded as that request's answer and the next request goes
		out. Left unrecorded, it stopped the session asking anything
		further, and the link then sat there until link supervision ended it
		with no line saying why.
		"""
		if op & EXT_OPCODE:
			# LMP_NOT_ACCEPTED_EXT carries the escape opcode as well as the
			# extended opcode it refuses, so the opcode is the second of its
			# three parameters (Vol 2, Part C, 2.7).
			refused = (u8(data[1]) | EXT_OPCODE) if len(data) > 1 else None
			reason = u8(data[2]) if len(data) > 2 else None
		else:
			refused = u8(data[0]) if data else None
			reason = u8(data[1]) if len(data) > 1 else None
		if refused is None:
			# A PDU too short to say which request it refuses still answers
			# the one outstanding, which is the only request this end is
			# waiting for. Taking it as that one is what keeps the
			# connection moving; leaving it would stall the session as
			# surely as dropping a refusal that did say.
			if self._awaiting is None:
				warn("the peer refused a request in a PDU too short to say "
					"which one")
				return
			warn("the peer refused a request without saying which; taking "
				"it as %s, the one outstanding"%op2str(self._awaiting))
			refused = self._awaiting
		elif refused != self._awaiting:
			# A refusal of something else: a repeat of one already
			# answered, or a request this end never made. Recording it
			# would mark that request answered and stop it ever being
			# asked. The transaction ID it carries is not checked, because
			# a peer that answers under the other one is still answering
			# (2.4), and this end has only the one request outstanding.
			warn("the peer refused %s, which this end is not waiting for"
				%op2str(refused))
			return
		# A refusal that names the request but carries no reason is still
		# that request's answer: 2.5 allows an implementation value for a
		# parameter a PDU does not carry.
		why = ("no reason given" if reason is None
			else LMP_ERR2STR.get(reason, "reason 0x%02x"%reason))
		field = self._opening_field.get(refused)
		if field is None:
			# Not one of the five, so it is the connection request they
			# lead to: a peer refusing that is declining the connection
			# itself, which is a warning rather than a line in the trace.
			warn("the peer refused %s: %s"%(op2str(refused), why))
			return
		log.info("The peer refused %s: %s"%(op2str(refused), why))
		setattr(self, field, REFUSED)
		self.send_info_req()

	def handle_accepted(self, op, data):
		"""The peer accepting one of this end's requests.

		Only the connection request needs anything done. Accepting it is
		the last step of the setup, and what follows is that each end sends
		LMP_setup_complete and waits for the other's; data may go out once
		both have been exchanged (Vol 2, Part C, 4.1.2). The central sends
		the first one, so a peer that waits for it -- another Ubertooth
		does -- is not left waiting forever.
		"""
		if self._awaiting != LMP_HOST_CONNECTION_REQ:
			return
		# An LMP_accepted names the request it accepts in its first
		# parameter (2.4). One that names something else, or that is too
		# short to name anything and so reads as opcode zero once the
		# packet's padding is counted, accepts a procedure this end did not
		# ask for and leaves the setup where it was.
		if not data or u8(data[0]) != LMP_HOST_CONNECTION_REQ:
			return
		self._awaiting = None
		self._send_setup_complete_once()

	def handle_slot_offset(self, op, data):
		if len(data) < 8:
			# Too short to hold the offset and the address. 2.5 gives two
			# answers for a PDU too short for its parameters: carry on with
			# values of the implementation's own, or refuse it with Invalid
			# LMP Parameters. Nothing here acts on these two numbers, and
			# the PDU expects no reply of any kind (it is in
			# LMP_NO_REPLY_EXPECTED above), so the first answer costs
			# nothing and the trace is all there is to say.
			log.info("slot_offset too short to read: %s"%(
				hexlify(data).decode()))
			return
		offset = u16(data[:2])
		bdaddr = data[2:][::-1]
		log.info("slot_offset from %s: %dusec\n"%(
			hexlify(bdaddr).decode(),offset))

	def send_info_req(self):
		"""Ask the peer for the first of the five it has not answered yet.

		Called again by each answer, so the five go out one at a time, and
		the connection request follows the last of them.
		"""
		for op, field, send in self._opening:
			if getattr(self, field) is None:
				self._awaiting = op
				send()
				return
		self._awaiting = LMP_HOST_CONNECTION_REQ
		self.lmp_send(LMP_HOST_CONNECTION_REQ,b'')

	def handle_info_res(self, op, data):
		if op == LMP_FEATURES_RES:
			self.rmt_features = data
		elif op == LMP_VERSION_RES:
			self.rmt_version = data
		elif op == EXT_OPCODE|LMP_FEATURES_RES_EXT:
			self.rmt_features_ext = data
		elif op == LMP_NAME_RES:
			self.rmt_name = data
		elif op == LMP_ENCRYPTION_KEY_SIZE_MASK_RES:
			self.rmt_enc_key_size_mask = data
		self.send_info_req()

	def _send_setup_complete_once(self):
		"""Send this end's LMP_setup_complete, if it has not already."""
		if self._setup_complete_sent:
			return
		self._setup_complete_sent = True
		self.lmp_send_setup_complete()

	def handle_setup_complete(self, op, data):
		# A peer that sent its own first, which a standard controller may
		# do, still gets this end's.
		self._send_setup_complete_once()
		self._con.handle_setup_complete()

class LMPPeripheral(LMP):
	def __init__(self, con):
		self._FSM = {
			# First state: wait for establishment message
			1: {
				LMP_DETACH: 		(self.handle_detach,),
				LMP_VERSION_REQ: 	(self.handle_vers_req,),
				LMP_FEATURES_REQ:	(self.handle_feat_req,),
				EXT_OPCODE|LMP_FEATURES_REQ_EXT:	(self.handle_feat_req_ext,),
				LMP_NAME_REQ: 		(self.handle_name_req,),
				LMP_HOST_CONNECTION_REQ: (self.handle_host_connection_req,),
				LMP_SETUP_COMPLETE: 	(self.handle_setup_complete,),
				LMP_SET_AFH:		(self.handle_set_afh,),
				LMP_SWITCH_REQ:		(self.handle_switch_req,),
				LMP_MAX_SLOT:		(self.handle_notification,),
				LMP_MAX_SLOT_REQ:	(self.handle_max_slot_req,),
				# Only the central sets the supervision timeout, so this
				# one belongs to the peripheral's table alone
				# (Core v5.4 | Vol 2, Part C, table 5.1).
				LMP_SUPERVISION_TIMEOUT: (self.handle_notification,),
			},
		}
		self._state = 1
		super().__init__(con)

	def handle_host_connection_req(self, op, data):
		self.lmp_send_accepted(op, tid=self._rx_tid)

	def handle_setup_complete(self, op, data):
		self.lmp_send_setup_complete()
		self._con.handle_setup_complete()

	def handle_set_afh(self, op, data):
		if len(data) < AFH_PARAMETERS_LEN:
			# Too short to hold the instant, the mode, and the channel map.
			# 2.5's other answer, a value of the implementation's own, is no
			# use here: these parameters leave the host for the firmware,
			# which hops over whatever map it is given, and no invented map
			# is the one the central set. So this PDU is refused with
			# Invalid LMP Parameters, which is the same transaction the full
			# one gets its LMP_ACCEPTED in.
			return self.lmp_send_not_accepted(op,
				p8(LMP_ERR_INVALID_LMP_PARAMETERS), tid=self._rx_tid)
		instant = u32(data[:4])
		mode = data[4]
		# A real PDU is padded to the 17 bytes a DM1 carries, so the map is
		# taken by its own length rather than to the end of the PDU.
		chan_map = data[5:AFH_PARAMETERS_LEN]
		log.info("AFH req: instant=%d, (cur %d), mode=%d"%(
			instant<<1, self._clkn, mode))
		self.lmp_send_accepted(op, tid=self._rx_tid)
		self._con._bt.send_set_afh_cmd(instant<<1, mode, chan_map)

	def start(self):
		pass
