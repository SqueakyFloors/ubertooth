import usb1
import sys
from binascii import hexlify, unhexlify
from threading import Thread, Event, Lock
from queue import Queue, Empty
from struct import pack, unpack
from time import monotonic, sleep
from ubtbr.lmp import *
# Where every message this module writes goes. In their own module, which
# imports nothing outside the standard library, so that ubertooth-btbr can
# use them before it imports usb1; see streams.py.
from ubtbr.streams import (colored, data_to_str, eprint, error, output_gone,
	output_lost, point_at_the_bin, result, warn)
import logging
# The progress a command makes: plain lines on standard error, carrying
# neither a time nor a level, like every other tool's progress. What a
# command decodes does not come through here; result() below writes that to
# standard output.
log = logging.getLogger("btctl")
log.setLevel(logging.DEBUG)
log_handler = logging.StreamHandler(sys.stderr)
# INFO, not DEBUG. The lines carry no level of their own, so the finer-grained
# ones ("Send idle", "send bb ...") read like ordinary progress and bury it.
# The logger stays at DEBUG so a session that wants them can lower this.
log_handler.setLevel(logging.INFO)
log_formatter = logging.Formatter('%(message)s')
log_handler.setFormatter(log_formatter)
log.addHandler(log_handler)

UBERTOOTH_VENDOR_ID = 0x1d50
UBERTOOTH_PRODUCT_ID = 0x6002

# Every board the C tools recognize, from U0_/U1_/TC13_VENDORID and
# _PRODUCTID in host/libubertooth/src/ubertooth_control.h: Ubertooth One,
# Ubertooth Zero and the TC13 badge. The btbr firmware builds for the Zero
# too, and a board left out of this list is one the selection below cannot
# see: it would not be listed, so a bare command would call the remaining
# board the only one and open it, which is the fault this list exists to
# avoid.
UBERTOOTH_USB_IDS = (
	(UBERTOOTH_VENDOR_ID, UBERTOOTH_PRODUCT_ID),	# Ubertooth One
	(0x1d50, 0x6000),				# Ubertooth Zero
	(0xffff, 0x0004),				# TC13 badge
)

### BTCTL interface
BBPKT_F_HAS_PKT		= 0
BBPKT_F_HAS_HDR		= 1
BBPKT_F_HAS_CRC		= 2
BBPKT_F_GOOD_CRC	= 3

# Commands
BTCTL_DEBUG 		= 0
# Host -> Device
BTCTL_RESET_REQ 	= 20
BTCTL_IDLE_REQ 		= 21
BTCTL_SET_FREQ_OFF_REQ 	= 22
BTCTL_SET_BDADDR_REQ 	= 23
BTCTL_INQUIRY_REQ	= 24
BTCTL_PAGING_REQ	= 25
BTCTL_SET_MAX_AC_ERRORS_REQ	= 26
BTCTL_TX_ACL_REQ	= 27
BTCTL_INQUIRY_SCAN_REQ	= 28
BTCTL_PAGE_SCAN_REQ	= 29
BTCTL_SET_EIR_REQ	= 30
BTCTL_SET_AFH_REQ	= 31
BTCTL_MONITOR_REQ	= 32
# Nothing here sends this one: the board is asked for its on-chip programmer
# over USB (ubertooth-util -i), and the number is kept in step with
# ubertooth_interface.h so it is not given to something else.
BTCTL_ISP_REQ		= 33
# Device -> Host 
BTCTL_RX_PKT		= 40
BTCTL_STATE_RESP	= 41

# The word every message opens with, naming its type, and what a state
# response carries behind it: the state, and the reason it is in that state.
MSG_TYPE_LEN		= 4
STATE_RESP_LEN		= 2

# States
BTCTL_STATE_STANDBY	= 0
BTCTL_STATE_INQUIRY	= 1
BTCTL_STATE_PAGE	= 2
BTCTL_STATE_CONNECTED	= 3
BTCTL_STATE_INQUIRY_SCAN = 4
BTCTL_STATE_PAGE_SCAN	= 5

# Reasons
BTCTL_REASON_SUCCESS	= 0
BTCTL_REASON_TIMEOUT	= 1
# REASON_PAGED indicates that device entered CONNECTED state,
# but the Peripheral did not answer yet
BTCTL_REASON_PAGED	= 2

STATE_TO_STR = {
	BTCTL_STATE_STANDBY:	"STANDBY",
	BTCTL_STATE_INQUIRY:	"INQUIRY",
	BTCTL_STATE_PAGE:	"PAGE",
}

REASON_TO_STR = {
	BTCTL_REASON_SUCCESS:	"SUCCESS",
	BTCTL_REASON_TIMEOUT:	"TIMEOUT",
	BTCTL_REASON_PAGED:	"PAGED"
}


# BT definitions
NULL = 0
POLL = 1
FHS = 2
DM1 = 3
DH1 = 4
DM3 = 10
DH3 = 11 
DM5 = 14
DH5 = 15 

ACL_TYPES_TO_STR = {
	NULL:	"NULL",
	POLL:	"POLL",
	FHS:	"FHS",
	DM1:	"DM1",
	DH1:	"DH1",
	DM3:	"DM3",
	DH3:	"DH3",
	DM5:	"DM5",
	DH5:	"DH5"
}

ACL_TYPE_TBL = [ 
# 	Type, hlen 	plen	fec23
	(DM1,	1,	17,	1	),
	(DM3,	2,	121,	1	),
	(DM5,	2,	224,	1	),
	(DH1,	1,	27,	0	),
	(DH3,	2,	183,	0	),
	(DH5,	2,	339,	0	)
]

LLID_L2CAP_CONT		= 1
LLID_L2CAP_START	= 2
LLID_LMP	 	= 3

LLID_TO_STR = {
	LLID_L2CAP_CONT:	"L2CAP_CONT",
	LLID_L2CAP_START:	"L2CAP_START",
	LLID_LMP:		"LMP"
}

# The L2CAP signalling channel and the two codes of the echo procedure
# (Core v5.4 | Vol 3, Part A, 4, 4.8, and 4.9). An echo request has to be
# answered with a response carrying the same identifier, which is what makes
# `l2ping` report a round trip.
L2CAP_CID_SIGNALLING	= 1
L2CAP_ECHO_REQ		= 0x08
L2CAP_ECHO_RSP		= 0x09

def l2cap_signalling_packet(code, identifier, data=b""):
	"""One L2CAP packet on the signalling channel, ready for an ACL payload."""
	command = pack("<BBH", code, identifier, len(data)) + data
	return pack("<HH", len(command), L2CAP_CID_SIGNALLING) + command

def acl_payload(acl):
	"""The bytes a received ACL packet's own payload header says it carries.

	A packet is padded out to the size of the packet kind that carried it, so
	what follows the payload is not data the sender put there. For a packet
	built here rather than received there is no padding and no declared
	length, and the data is all of it.
	"""
	if acl.raw_data is None:
		return acl.data
	return acl.data[:max(acl.raw_size - acl.hlen, 0)]

def l2cap_echo_request(acl):
	"""The identifier and data of an echo request, or None for anything else.

	`acl` is a received ACL packet. Only a request that arrived whole in one
	packet is read: the echo request `l2ping` sends is twelve bytes and fits
	in the smallest packet the link carries, so there is nothing here to
	reassemble a fragmented one for.
	"""
	if acl.llid != LLID_L2CAP_START:
		return None
	payload = acl_payload(acl)
	if len(payload) < 8:
		return None
	length, cid = unpack("<HH", payload[:4])
	if cid != L2CAP_CID_SIGNALLING:
		return None
	code, identifier, command_length = unpack("<BBH", payload[4:8])
	if code != L2CAP_ECHO_REQ:
		return None
	# Zero is not a valid identifier for a command, so there is nothing to
	# answer it under (Vol 3, Part A, 4).
	if identifier == 0:
		return None
	# The L2CAP length counts the signalling command, its four-byte header
	# and its data (Vol 3, Part A, 3.1 and 4). Anything else is a packet
	# this reads nothing out of: a length that does not describe the command
	# after it, or a packet carrying several commands, which the channel
	# allows and this does not take apart.
	if length != 4 + command_length:
		return None
	# A command that runs past the payload the sender declared is not one
	# this answers: the bytes it would echo back are the packet's padding.
	if len(payload) < 8 + command_length:
		return None
	return identifier, payload[8:8+command_length]

def reverse8(num):
	rev8_tbl = [
		0x00,0x80,0x40,0xc0,0x20,0xa0,0x60,0xe0,0x10,0x90,0x50,0xd0,0x30,0xb0,0x70,0xf0,
		0x08,0x88,0x48,0xc8,0x28,0xa8,0x68,0xe8,0x18,0x98,0x58,0xd8,0x38,0xb8,0x78,0xf8,
		0x04,0x84,0x44,0xc4,0x24,0xa4,0x64,0xe4,0x14,0x94,0x54,0xd4,0x34,0xb4,0x74,0xf4,
		0x0c,0x8c,0x4c,0xcc,0x2c,0xac,0x6c,0xec,0x1c,0x9c,0x5c,0xdc,0x3c,0xbc,0x7c,0xfc,
		0x02,0x82,0x42,0xc2,0x22,0xa2,0x62,0xe2,0x12,0x92,0x52,0xd2,0x32,0xb2,0x72,0xf2,
		0x0a,0x8a,0x4a,0xca,0x2a,0xaa,0x6a,0xea,0x1a,0x9a,0x5a,0xda,0x3a,0xba,0x7a,0xfa,
		0x06,0x86,0x46,0xc6,0x26,0xa6,0x66,0xe6,0x16,0x96,0x56,0xd6,0x36,0xb6,0x76,0xf6,
		0x0e,0x8e,0x4e,0xce,0x2e,0xae,0x6e,0xee,0x1e,0x9e,0x5e,0xde,0x3e,0xbe,0x7e,0xfe,
		0x01,0x81,0x41,0xc1,0x21,0xa1,0x61,0xe1,0x11,0x91,0x51,0xd1,0x31,0xb1,0x71,0xf1,
		0x09,0x89,0x49,0xc9,0x29,0xa9,0x69,0xe9,0x19,0x99,0x59,0xd9,0x39,0xb9,0x79,0xf9,
		0x05,0x85,0x45,0xc5,0x25,0xa5,0x65,0xe5,0x15,0x95,0x55,0xd5,0x35,0xb5,0x75,0xf5,
		0x0d,0x8d,0x4d,0xcd,0x2d,0xad,0x6d,0xed,0x1d,0x9d,0x5d,0xdd,0x3d,0xbd,0x7d,0xfd,
		0x03,0x83,0x43,0xc3,0x23,0xa3,0x63,0xe3,0x13,0x93,0x53,0xd3,0x33,0xb3,0x73,0xf3,
		0x0b,0x8b,0x4b,0xcb,0x2b,0xab,0x6b,0xeb,0x1b,0x9b,0x5b,0xdb,0x3b,0xbb,0x7b,0xfb,
		0x07,0x87,0x47,0xc7,0x27,0xa7,0x67,0xe7,0x17,0x97,0x57,0xd7,0x37,0xb7,0x77,0xf7,
		0x0f,0x8f,0x4f,0xcf,0x2f,0xaf,0x6f,0xef,0x1f,0x9f,0x5f,0xdf,0x3f,0xbf,0x7f,0xff,
	]
	#r = 0
	#for i in range(8):
	#	r |= (num&1)<<(7-i)
	#	num>>=1
	#return r
	return rev8_tbl[num]

def crc_compute(byts, crc):
	crc_tbl = [
		0x0000, 0x1189, 0x2312, 0x329b, 0x4624, 0x57ad, 0x6536, 0x74bf,
		0x8c48, 0x9dc1, 0xaf5a, 0xbed3, 0xca6c, 0xdbe5, 0xe97e, 0xf8f7,
		0x1081, 0x0108, 0x3393, 0x221a, 0x56a5, 0x472c, 0x75b7, 0x643e,
		0x9cc9, 0x8d40, 0xbfdb, 0xae52, 0xdaed, 0xcb64, 0xf9ff, 0xe876,
		0x2102, 0x308b, 0x0210, 0x1399, 0x6726, 0x76af, 0x4434, 0x55bd,
		0xad4a, 0xbcc3, 0x8e58, 0x9fd1, 0xeb6e, 0xfae7, 0xc87c, 0xd9f5,
		0x3183, 0x200a, 0x1291, 0x0318, 0x77a7, 0x662e, 0x54b5, 0x453c,
		0xbdcb, 0xac42, 0x9ed9, 0x8f50, 0xfbef, 0xea66, 0xd8fd, 0xc974,
		0x4204, 0x538d, 0x6116, 0x709f, 0x0420, 0x15a9, 0x2732, 0x36bb,
		0xce4c, 0xdfc5, 0xed5e, 0xfcd7, 0x8868, 0x99e1, 0xab7a, 0xbaf3,
		0x5285, 0x430c, 0x7197, 0x601e, 0x14a1, 0x0528, 0x37b3, 0x263a,
		0xdecd, 0xcf44, 0xfddf, 0xec56, 0x98e9, 0x8960, 0xbbfb, 0xaa72,
		0x6306, 0x728f, 0x4014, 0x519d, 0x2522, 0x34ab, 0x0630, 0x17b9,
		0xef4e, 0xfec7, 0xcc5c, 0xddd5, 0xa96a, 0xb8e3, 0x8a78, 0x9bf1,
		0x7387, 0x620e, 0x5095, 0x411c, 0x35a3, 0x242a, 0x16b1, 0x0738,
		0xffcf, 0xee46, 0xdcdd, 0xcd54, 0xb9eb, 0xa862, 0x9af9, 0x8b70,
		0x8408, 0x9581, 0xa71a, 0xb693, 0xc22c, 0xd3a5, 0xe13e, 0xf0b7,
		0x0840, 0x19c9, 0x2b52, 0x3adb, 0x4e64, 0x5fed, 0x6d76, 0x7cff,
		0x9489, 0x8500, 0xb79b, 0xa612, 0xd2ad, 0xc324, 0xf1bf, 0xe036,
		0x18c1, 0x0948, 0x3bd3, 0x2a5a, 0x5ee5, 0x4f6c, 0x7df7, 0x6c7e,
		0xa50a, 0xb483, 0x8618, 0x9791, 0xe32e, 0xf2a7, 0xc03c, 0xd1b5,
		0x2942, 0x38cb, 0x0a50, 0x1bd9, 0x6f66, 0x7eef, 0x4c74, 0x5dfd,
		0xb58b, 0xa402, 0x9699, 0x8710, 0xf3af, 0xe226, 0xd0bd, 0xc134,
		0x39c3, 0x284a, 0x1ad1, 0x0b58, 0x7fe7, 0x6e6e, 0x5cf5, 0x4d7c,
		0xc60c, 0xd785, 0xe51e, 0xf497, 0x8028, 0x91a1, 0xa33a, 0xb2b3,
		0x4a44, 0x5bcd, 0x6956, 0x78df, 0x0c60, 0x1de9, 0x2f72, 0x3efb,
		0xd68d, 0xc704, 0xf59f, 0xe416, 0x90a9, 0x8120, 0xb3bb, 0xa232,
		0x5ac5, 0x4b4c, 0x79d7, 0x685e, 0x1ce1, 0x0d68, 0x3ff3, 0x2e7a,
		0xe70e, 0xf687, 0xc41c, 0xd595, 0xa12a, 0xb0a3, 0x8238, 0x93b1,
		0x6b46, 0x7acf, 0x4854, 0x59dd, 0x2d62, 0x3ceb, 0x0e70, 0x1ff9,
		0xf78f, 0xe606, 0xd49d, 0xc514, 0xb1ab, 0xa022, 0x92b9, 0x8330,
		0x7bc7, 0x6a4e, 0x58d5, 0x495c, 0x3de3, 0x2c6a, 0x1ef1, 0x0f78,
	]
	for b in byts:
		crc = (crc>>8) ^ crc_tbl[0xff & (b^crc)]
	return crc

def acl_type_for_size(size):
	for tp in ACL_TYPE_TBL:
		t,h,p,fec = tp
		if p >= size:
			return tp

def acl_type_find(type_):
	for tp in ACL_TYPE_TBL:
		t,h,p,fec = tp
		if t == type_:
			return tp

def byte_count(n):
	"""A count of bytes to quote back to the user: 1 byte, 18 bytes."""
	return "%d byte%s"%(n, "" if n == 1 else "s")

def print_state(state, reason):
	ss = STATE_TO_STR.get(state) or str(state)
	rs = REASON_TO_STR.get(reason) or str(reason)
	log.info ("Device state: %s, reason %s"%(ss, rs))

class BTCtlError(Exception):
	"""Something the caller can be told about, rather than a bug in this code."""

class BTCtlFramingError(BTCtlError):
	"""A USB packet, or a message in it, that does not fit what the device sends.

	`kind` names what was wrong without the lengths that vary from one such
	message to the next, so a caller that reports these can say each one once
	however many sizes it arrives in. It defaults to the whole message, for
	the raises whose wording carries no length.
	"""
	def __init__(self, message, kind=None):
		super().__init__(message)
		self.kind = kind or message

class BTCtlNoDeviceError(BTCtlError):
	"""No Ubertooth to control."""

class BTCtlBusyError(BTCtlError):
	"""A command still holds the one message handler the device has."""

class BBHdr:
	def __init__(self, lt_addr, type_, flags=0, hec=0):
		self.lt_addr = lt_addr
		self.type = type_
		self.flags = flags
		self.hec = hec

	@staticmethod
	def unpack(data):
		"""
		typedef struct bthdr_s {
			uint8_t lt_addr;
			uint8_t type;
			uint8_t flags;
			uint8_t hec;
		} __attribute__((packed)) bbhdr_t;
		"""
		lt_addr, type_, flags, hec = data
		return BBHdr(lt_addr, type_, flags, hec)

	def pack(self):
		return bytes((self.lt_addr, self.type, self.flags, self.hec))

	def flagstr(self):
		s = ""
		if self.flags & 1:
			s += "F" # flow
		if self.flags & 2:
			s += "A" # arqn
		if self.flags & 4:
			s += "S" # seqn
		return s

	def __str__(self):
		# A type no table names is its number, as the LMP opcodes are: this
		# header is printed for a raw packet, so it must not raise on one.
		t = ACL_TYPES_TO_STR.get(self.type, "%d"%self.type)
		s = "BBHdr(lt_addr=%d, type=%s, flags=%s)"%(
			self.lt_addr, t, self.flagstr())
		return s

class BTCtlEIR:
	# Set once the malformed-field warning below has gone out, so each
	# command says it once: a device whose response is cut re-sends that
	# response every inquiry cycle, and identical lines bury the rest of a
	# session. Unlike a dropped message, which says something about the
	# board and so is said once for the whole session, this says something
	# about whichever device answered, so the next command starts afresh.
	_said_malformed = False

	@staticmethod
	def forget_warnings():
		"""Let the next command report a malformed response of its own."""
		BTCtlEIR._said_malformed = False

	EIR_FLAGS = 1
	EIR_INCOMPLETE_U16_LIST = 2
	EIR_COMPLETE_U16_LIST = 3
	EIR_INCOMPLETE_U32_LIST = 4
	EIR_COMPLETE_U32_LIST = 5
	EIR_INCOMPLETE_U128_LIST = 6
	EIR_COMPLETE_U128_LIST = 7
	EIR_SHORTENED_LOCAL_NAME = 8
	EIR_COMPLETE_LOCAL_NAME = 9
	# TODO: many more
	TYPE_NAME = {
		EIR_FLAGS: "FLAGS",
		EIR_INCOMPLETE_U16_LIST: "INCOMPLETE_U16_LIST",
		EIR_COMPLETE_U16_LIST: "COMPLETE_U16_LIST",
		EIR_INCOMPLETE_U32_LIST: "INCOMPLETE_U32_LIST",
		EIR_COMPLETE_U32_LIST: "COMPLETE_U32_LIST",
		EIR_INCOMPLETE_U128_LIST: "INCOMPLETE_U128_LIST",
		EIR_COMPLETE_U128_LIST: "COMPLETE_U128_LIST",
		EIR_SHORTENED_LOCAL_NAME: "SHORTENED_LOCAL_NAME",
		EIR_COMPLETE_LOCAL_NAME: "COMPLETE_LOCAL_NAME",
	}
	
	def __init__(self, fields):
		self.fields = fields

	@staticmethod
	def unpack(data):
		buf = data
		fields = []
		def runs_past():
			"""Report a field that does not fit the response, once."""
			if not BTCtlEIR._said_malformed:
				BTCtlEIR._said_malformed = True
				# A warning is not part of the capture on standard output,
				# so it goes to standard error like every other diagnostic.
				warn("malformed EIR: a field runs past the end of the "
					"response, so the rest of it is dropped")
		while (buf):
			# A length of zero is where the significant part of the EIR
			# ends; whatever follows is non-significant padding, which a
			# sender is free to leave as zeros (Core v5.4 | Vol 3, Part C,
			# 8). So the EIR ends here, and nothing is wrong.
			if buf[0] == 0:
				break
			# A field is a length and a type before its data, so one byte
			# left is a field that runs past the end as surely as a short
			# one does. Read without this check it raised instead, which
			# ended the command that was decoding the response.
			if len(buf) < 2:
				runs_past()
				break
			l,t = buf[:2]
			dat,buf = buf[2:1+l], buf[1+l:]
			if (1+len(dat) != l):
				runs_past()
				break
			fields.append((t,dat))
		return BTCtlEIR(fields)

	def pack(self):
		r = b''
		for t, dat in self.fields:
			r += bytes((len(dat)+1,t))+dat
		return r

	def __str__(self):
		s = ""
		for t, dat in self.fields:
			if s: s += "\n"
			tn = BTCtlEIR.TYPE_NAME.get(t)	
			if tn is None: tn = "0x%x"%t
			if t in (BTCtlEIR.EIR_SHORTENED_LOCAL_NAME,
				BTCtlEIR.EIR_COMPLETE_LOCAL_NAME):
				# A name comes off the air and need not be UTF-8 at all, let
				# alone whole: a shortened one can be cut mid-character. A
				# byte that cannot be decoded is replaced rather than
				# raising, which ended the command printing the response.
				ts = "'%s'"%(dat.decode("utf-8", "replace"))
			else:
				ts = data_to_str(dat)
			s += "%s: %s"%(tn, ts)
		return s


class BTCtlFHSPkt:
	# An FHS payload is a fixed 144 bits, 18 bytes, whatever it carries: the
	# 34-bit parity, the address, the class of device, the LT_ADDR the
	# central is giving out, and the clock (Core v5.4 | Vol 2, Part B,
	# 6.5.1.4). The firmware strips the 16-bit CRC that follows.
	PAYLOAD_LEN = 18

	def __init__(self, parity, bdaddr, cls, ltaddr, clk27_2):
		self.parity = parity
		self.bdaddr = bdaddr
		self.cls = cls
		self.ltaddr = ltaddr
		self.clk27_2 = clk27_2

	@staticmethod
	def unpack(data):
		if len(data) < BTCtlFHSPkt.PAYLOAD_LEN:
			# Every field below is read by its own offset, so a short payload
			# used to be read past the end of. Reported as the framing error
			# it is, and the packet dropped.
			raise BTCtlFramingError(
				"FHS packet of %s, too short for the %d an FHS "
				"carries"%(byte_count(len(data)), BTCtlFHSPkt.PAYLOAD_LEN),
				"FHS packet too short for an FHS")
		parity = 0x3ffffffff & ( data[0]|(data[1]<<8)
			|(data[2]<<16)|(data[3]<<24)|((data[4]&3)<<32))
		lap = (   (data[4]>>2)
			| (data[5]<<6)
			| (data[6]<<14)
			| ((3&data[7])<<22))
		uap = data[8]
		nap = data[9]|(data[10]<<8)
		bdaddr = lap|(uap<<24)|(nap<<32)

		cls = data[11]|(data[12]<<8)|(data[13]<<16)
		ltaddr = data[14]&7
		clk27_2 = (data[14]>>3)|(data[15]<<5)|(data[16]<<13)|((data[17]&0x1f)<<21)
	
		return BTCtlFHSPkt(parity, bdaddr, cls, ltaddr, clk27_2)

	def __str__(self):
		return "FHS: bdaddr=%x parity=%x cls=%x ltaddr=%x clk=%x"%(
			self.bdaddr, self.parity,self.cls,self.ltaddr, self.clk27_2)
		
class BTCtlACLPkt:
	def __init__(self, llid, data, flow=1, bt_type = None, raw_data=None, raw_size=0):
		self.llid = llid
		self.data = data
		self.flow = flow
		if bt_type is None:
			# Determine bb type according to data size
			size = len(self.data)
			tp = acl_type_for_size(size)
		else:
			tp = acl_type_find(bt_type)
		bt_type, hlen, plen, fec = tp
		self.bt_type = bt_type
		self.hlen = hlen
		# Raw RX packet fields
		self.raw_data = raw_data
		self.raw_size = raw_size

	@staticmethod
	def unpack(type_, inp):
		tp = acl_type_find(type_)
		if tp is None:
			# The type is what is wrong here, so it belongs in the kind too.
			raise BTCtlFramingError("packet of unknown type %d"%type_,
				"unknown packet type %d"%type_)
		bt_type,hlen,plen,fec = tp
		if len(inp) < hlen:
			# Not even the payload header this packet kind starts with, so
			# there is nothing to read it as. Reported as the framing error
			# it is, where it used to leave the payload unset and end the
			# receive thread in an UnboundLocalError.
			named = ACL_TYPES_TO_STR.get(bt_type, bt_type)
			raise BTCtlFramingError(
				"%s packet of %s, too short for its %d-byte payload "
				"header"%(named, byte_count(len(inp)), hlen),
				"%s packet too short for its payload header"%named)
		if hlen == 1:
			hdr = inp[0]
			data=inp[1:]
		else:
			hdr, = unpack("<H", inp[:2])
			data=inp[2:]
		llid = 3&hdr
		flow = 1&(hdr>>2)
		size = hdr>>3
		return BTCtlACLPkt(llid, data, flow, bt_type, raw_data=inp, raw_size=hlen+size)

	def pack(self):
		# Pack payload header
		data_hdr = pack("<"+(self.hlen == 1 and "B" or "H"),
			(len(self.data)<<3) | ((self.flow&1)<<2) | (self.llid&3))
		return data_hdr + self.data

	def __str__(self):
		s = "ACL(llid=%s, flow=%d, len=%d): %s"%(
			LLID_TO_STR.get(self.llid) or str(self.llid), self.flow, len(self.data),
			data_to_str(self.data))
		return s

class BTCtlRxPkt:
	# What every received-packet message starts with, before the packet
	# itself: the 8-byte record below, then the 4-byte baseband header.
	HEADER_LEN = 12

	def __init__(self, clkn, chan, flags, bb_hdr, bt_data):
		self.clkn = clkn
		self.chan = chan
		self.flags = flags
		self.bb_hdr = bb_hdr
		self.bt_data = bt_data

	@staticmethod
	def unpack(data):
		"""
		 typedef struct {
			 uint32_t clkn;          // Clkn sampled at start of rx
			 uint8_t chan;           // Channel number
			 uint8_t flags;          // Decode flags
			 uint16_t data_size;     // Size of bt_data field
			 bbhdr_t bb_hdr;         // decoded header
			 uint8_t bt_data[0];     // maximum data size for an ACL packet
		 } __attribute__((packed)) btctl_rx_pkt_t;
		"""
		if len(data) < BTCtlRxPkt.HEADER_LEN:
			# Reported before the slicing below, which on a short message
			# used to raise from the struct unpack or from the baseband
			# header's own unpacking, neither of which says what arrived.
			raise BTCtlFramingError(
				"received packet of %s, too short for its %d-byte "
				"header"%(byte_count(len(data)), BTCtlRxPkt.HEADER_LEN),
				"received packet too short for its header")
		hdr,bb_hdr,bt_data = data[:8],data[8:12],data[12:]
		clkn,chan,flags,data_size = unpack("<IBBH", hdr)
		if data_size != len(bt_data):
			raise BTCtlFramingError(
				"packet of %d bytes carrying %d"%(data_size, len(bt_data)),
				"packet carrying other than the size it says")
		bb_hdr = BBHdr.unpack(bb_hdr)
		t = bb_hdr.type
		if t == FHS:
			bt_data = BTCtlFHSPkt.unpack(bt_data)
		else:
			bt_data = BTCtlACLPkt.unpack(t, bt_data)
		return BTCtlRxPkt(clkn,chan,flags,bb_hdr,bt_data)

	def __str__(self):
		s = "RX(clkn=%d, chan=%d):"%(
			self.clkn, self.chan)
		s += "\n  %s"%self.bb_hdr
		s += "\n    %s"%self.bt_data
		return s

class BTCtlCmd(Thread):
	# A stop asks the device to go idle; the state response that follows is
	# what normally ends the command, with its closing log line. Wait that
	# long for it, then end regardless: a device that answers nothing must
	# not keep the command, and with it the whole tool, alive.
	STOP_GRACE = 0.5
	# How often the loop looks at a pending stop while waiting for a message.
	POLL_INTERVAL = 0.05
	# What a caller joining a stopped command should allow it. Asking for the
	# stop already waited behind whatever write was in flight, since both go
	# through the one send lock, so this covers the grace above and a little
	# more. A handler part way through several writes to a board that takes
	# none of them can outlast it; the command is then given up on, and the
	# prompt takes commands again once it ends.
	JOIN_TIMEOUT = 2

	def __init__(self, bt):
		# A daemon thread, so an interpreter shutting down is never held up
		# by a command waiting on a device that stopped answering.
		super().__init__(daemon=True)
		self._bt = bt
		self._msg_q = Queue()
		self._ready = False
		self._done = False
		self._stopping = Event()
		self._bt.register_msg_handler(self._put_msg)

	def release(self):
		"""Give the device's messages back, for a command that never ran.

		The thread is what releases the handler claimed above, when it ends,
		so a command whose thread was never started has to be undone here.
		Only call this on a command that did not start: on a running one it
		would take the handler away from under its own thread.
		"""
		self._bt.unregister_msg_handler(self._put_msg)

	def _start(self):
		pass

	def stop_allowed(self):
		return True

	def stop(self):
		# Set first: a request that cannot be sent must still end the command.
		self._stopping.set()
		try:
			self._bt.send_idle_cmd()
		except (usb1.USBError, BTCtlError) as e:
			# Raising here would come out of whoever asked for the stop,
			# which is the keyboard interrupt handler among others. The
			# command ends on its own below either way.
			#
			# Named by command, because close() has a line of its own with
			# the same cause and both can appear in one session.
			warn("%s: could not ask the device to go idle: %s"%(self, e))

	def _handle_state(self, state, reason):
		print_state(state, reason)

	def _handle_fhs(self, pkt):
		result("RX FHS: %s"%(pkt.bt_data))

	def _handle_lmp(self, pkt):
		result("RX LMP: %s"%(pkt.bt_data))

	def _handle_l2cap(self, pkt):
		result("RX L2CAP: %s"%pkt.bt_data)

	def _handle_raw(self, pkt):
		result("RX Raw: %s"%pkt)

	def _handle_pkt(self, pkt):
		if pkt.bb_hdr.type == FHS:
			self._handle_fhs(pkt)
		else:
			acl = pkt.bt_data
			if pkt.flags & (1<<BBPKT_F_GOOD_CRC):
				if acl.llid == LLID_LMP:
					self._handle_lmp(pkt)
				else:
					self._handle_l2cap(pkt)
			else:
				self._handle_raw(pkt)

	def _put_msg(self, t, data):
		self._msg_q.put((t,data))

	def _run_loop(self):
		# Set once a stop has been asked for, to the time by which the device
		# must have answered it.
		deadline = None
		while not self._done:
			if self._stopping.is_set():
				if deadline is None:
					deadline = monotonic() + self.STOP_GRACE
				elif monotonic() >= deadline:
					warn("%s: the device did not go idle, "
						"stopping anyway"%self)
					return
			try:
				t, msg = self._msg_q.get(timeout=self.POLL_INTERVAL)
			except Empty:
				continue
			if t == BTCTL_STATE_RESP:
				self._handle_state(*msg)
			elif t == BTCTL_RX_PKT:
				if not self._ready:
					warn("a packet arrived before %s was ready: %s"
						%(self,msg))
				else:
					self._handle_pkt(msg)
			else:
				warn("message %d is not handled yet: %s"%(t,msg))

	def run(self):
		try:
			self._start()
			self._run_loop()
		except Exception as e:
			# This runs on the command's own thread, so there is nobody above
			# to report it: anything let out here comes out as a thread
			# traceback, and the finally below is what frees the device for
			# the next command. Named by type, so a bug still reads as one.
			error("%s stopped: %s: %s"%(self, type(e).__name__, e))
		finally:
			# Whatever ended the loop, the command is over: say so before
			# releasing the handler, so the next command can be started.
			self._done = True
			log.debug("%s done"%self)
			try:
				self._bt.unregister_msg_handler(self._put_msg)
			except BTCtlError as e:
				# Nothing can swap the handler underneath a command, so this
				# is only here so that no path out of this thread is a
				# traceback.
				error("%s could not let the device go: %s"%(self, e))

	def done(self):
		return self._done

	def __str__(self):
		return self.__class__.__name__

class BTCtlSuperCmd(Thread):
	# What a caller joining a stopped super command should allow it. Stopping
	# it means stopping the command it is running first, which can cost that
	# command's own join and a write timeout before it.
	JOIN_TIMEOUT = 2 * BTCtlCmd.JOIN_TIMEOUT

	def __init__(self, bt):
		# Daemon for the same reason as BTCtlCmd.
		super().__init__(daemon=True)
		self._bt = bt
		self._stopped = Event()
		self._cmd = None
		self._gave_up = False

	def release(self):
		# Nothing to give back: this one claims the device's messages only
		# through the commands it runs, and each of those releases its own.
		pass

	def _start_cmd(self, cls, *args):
		# Starting a command takes several steps, and an interrupt part way
		# through them leaves nobody holding the command that claimed the
		# device. The prompt has to keep a keyboard interrupt out of its own
		# version of this; here there is nothing to keep out, because this
		# runs on the super command's thread and signals reach the main one.
		if self._cmd is not None:
			log.info ("Not idle")
			return
		log.info("Starting %s"%cls.__name__)
		args = [self._bt]+list(args)
		cmd = cls(*args)
		try:
			cmd.start()
		except RuntimeError:
			# The interpreter can refuse a new thread; release() is what
			# hands the message handler back for a command that never ran.
			# The command is left unheld too: there is no thread to stop or
			# to join, and joining one that never ran raises in turn.
			cmd.release()
			raise
		self._cmd = cmd
		return self._cmd

	def _stop_cmd(self):
		# Trying a command that already outlived its stop again would only
		# spend another join on it.
		if self._cmd is None or self._gave_up:
			return
		self._cmd.stop()
		self._cmd.join(self._cmd.JOIN_TIMEOUT)
		if self._cmd.is_alive():
			# It still holds the message handler, so no other command can be
			# started: keep the handle and give up rather than start one and
			# trip over it.
			error("%s did not stop"%self._cmd)
			self._gave_up = True
			self.stop()
			return
		self._cmd = None

	def stop(self):
		self._stopped.set()

	def stopped(self):
		return self._stopped.is_set()

	def done(self):
		# What a command of its own reports when its loop has ended: this one
		# runs until it is stopped.
		return self.stopped()

	def _wait(self, timeout):
		# Waiting on the stop event rather than sleeping, so a stop is acted
		# on at once instead of at the end of the current step.
		self._stopped.wait(timeout)

	def __str__(self):
		# Named like a BTCtlCmd, since the same messages report both.
		return self.__class__.__name__

	def run(self):
		try:
			while not self.stopped():
				self.run_once()
		except Exception as e:
			# Its own thread too, so the same applies as for BTCtlCmd: the
			# commands it starts can refuse, and a traceback from in here is
			# nobody's to report.
			error("%s stopped: %s: %s"
				%(self.__class__.__name__, type(e).__name__, e))
			self.stop()
		finally:
			self._stop_cmd()
			log.info("%s stopped"%self.__class__.__name__)

class BTCtlInquiryCmd(BTCtlCmd):
	def _start(self):
		# This inquiry's responses come from whatever is nearby now, so a
		# malformed one is reported again even if the last inquiry had one.
		BTCtlEIR.forget_warnings()
		self._bt.send_inquiry_cmd()

	def _handle_state(self, state, reason):
		if state == BTCTL_STATE_INQUIRY:
			self._ready = True
			log.info ("Inquiry state Ready")
			return
		if state != BTCTL_STATE_STANDBY:
			log.info("Invalid state!")
			print_state(state, reason)
		log.info("Inquiry done")
		self._done = True

	def _handle_l2cap(self, pkt):
		acl = pkt.bt_data
		eir = BTCtlEIR.unpack(pkt.bt_data.data)
		result("%s\n%s"%(acl,eir))

class BTCtlInquiryScanCmd(BTCtlCmd):
	def _start(self):
		self._bt.send_inquiry_scan_cmd()

	def _handle_state(self, state, reason):
		if state == BTCTL_STATE_INQUIRY_SCAN:
			self._ready = True
			log.info ("Inquiry Scan state Ready")
			return
		if state != BTCTL_STATE_STANDBY:
			log.info("Invalid state!")
			print_state(state, reason)
		log.info("Inquiry scan done")
		self._done = True

class BTCtlPageScanCmd(BTCtlCmd):
	def __init__(self, bt):
		super().__init__(bt)
		# Set when a connection comes up and says so; until then there is no
		# address to send a packet on.
		self.lt_addr = None
		# Set when the link setup finishes, which is when data may go out.
		self._setup_done = False

	def _start(self):
		self._bt.send_page_scan_cmd()
		self._lmp = LMPPeripheral(self)

	def send_acl(self, llid, data, flow=1):
		self._bt.send_acl_cmd(llid, data, flow, self.lt_addr)

	def stop_allowed(self):
		# Avoid interrupting existing LMP transactions
		return self._done or not self._ready

	def _handle_state(self, state, reason):
		lt_addr = reason>>5
		reason &= 0x1f
		if state == BTCTL_STATE_PAGE_SCAN:
			# The board reports the scan a second time when a
			# connection it came up on carried no packet and it went
			# back to scanning. That one carries the reason the
			# connection ended, where a scan the host asked for
			# carries success, so the two are not read as one scan
			# starting twice.
			if reason == BTCTL_REASON_TIMEOUT:
				log.info("Page scan resumed")
			else:
				log.info ("Page Scan state Ready")
		elif state == BTCTL_STATE_CONNECTED:
			if reason == BTCTL_REASON_SUCCESS:
				self._ready = True
				self.lt_addr = lt_addr
				log.info("Connection ready, lt_addr=%d"%self.lt_addr)
				# A scan that went back to scanning takes the next
				# connection on the same link manager, and its setup has
				# not happened yet.
				self._setup_done = False
				self._lmp.connection_started()
			else:
				log.info("Connection status %d"%reason)
		else:
			if state != BTCTL_STATE_STANDBY:
				log.info("Invalid state!")
				print_state(state, reason)
			log.info("Page scan done")
			self._done = True

	def _handle_lmp(self, pkt):
		# The packet reaches standard output like every other command's, in
		# the shape monitor uses: the baseband header, then the PDU named
		# and in hex, so a piped capture of a connection reads on its own.
		# The link manager traces the same PDU again on standard error, with
		# the state it was in and the seconds since the connection came up;
		# that trace is a diagnostic, not the capture.
		pdu = pkt.bt_data.data
		result("RX LMP: %s | %s"%(pkt.bb_hdr, pdu2str(pdu)))
		self._lmp.receive(pkt.clkn, pdu)

	def _handle_l2cap(self, pkt):
		acl = pkt.bt_data
		result("RX L2CAP: %s"%acl)
		# An echo request is answered, so a peer running `l2ping` against
		# this board gets its round trip. Nothing else on the signalling
		# channel is handled; this board offers no service to connect to.
		request = l2cap_echo_request(acl)
		# Nothing goes out before the link setup has finished, which is the
		# point at which both ends have agreed the connection is open, and
		# nothing goes out without the address a connection gives.
		if request is None or not self._setup_done or self.lt_addr is None:
			return
		identifier, data = request
		answer = l2cap_signalling_packet(L2CAP_ECHO_RSP, identifier, data)
		# A response has to carry the request's data back, and this link
		# sends DM1 packets: the feature mask offers no multi-slot kinds and
		# a peer asking to send them is refused. A request too big to answer
		# in one is left alone rather than answered in a packet the peer has
		# not agreed to receive. `ubertooth-btbr`'s own `l2ping` sends four
		# bytes of echo data and fits; nine is the largest that does.
		if len(answer) > acl_type_find(DM1)[2]:
			warn("an L2CAP echo request of %s of echo data is more than "
				"this link can answer in one packet, so it goes "
				"unanswered"%byte_count(len(data)))
			return
		self.send_acl(LLID_L2CAP_START, answer)

	def handle_setup_complete(self):
		log.info("LMP setup complete")
		self._setup_done = True

class BTCtlPagingCmd(BTCtlCmd):
	LMP_CLS = LMPCentral
	def __init__(self, bt, bdaddr):
		super().__init__(bt)
		self._bdaddr = bdaddr
		self._lmp = self.LMP_CLS(self)
		self.lt_addr = None

	def _start(self):
		self._bt.send_paging_cmd(self._bdaddr)

	def send_acl(self, llid, data, flow=1):
		self._bt.send_acl_cmd(llid, data, flow, self.lt_addr)

	def _handle_state(self, state, reason):
		lt_addr = reason>>5
		reason &= 0x1f
		if state == BTCTL_STATE_PAGE:
			# As in BTCtlPageScanCmd above: a page train picked up
			# again after a connection that carried no packet
			# carries the reason the connection ended.
			if reason == BTCTL_REASON_TIMEOUT:
				log.info("Paging resumed")
			else:
				log.info("Paging started")
		elif state == BTCTL_STATE_CONNECTED:
			if reason == BTCTL_REASON_SUCCESS:
				self._ready = True
				self.lt_addr = lt_addr
				log.info("Connection ready, lt_addr=%d"%self.lt_addr)
				# A page that went back to the page train takes the next
				# connection on the same link manager, and its setup has
				# not happened yet.
				self._lmp.connection_started()
				self._lmp.start()
			else:
				log.info ("Connection status %d"%reason)
		else:
			if state != BTCTL_STATE_STANDBY:
				log.info("Invalid state!")
				print_state(state, reason)
			self._done = True
			log.info("Paging done")

	def handle_setup_complete(self):
		log.info("LMP setup complete")

	def _handle_lmp(self, pkt):
		# As in BTCtlPageScanCmd above: the packet on standard output, then
		# the link manager.
		pdu = pkt.bt_data.data
		result("RX LMP: %s | %s"%(pkt.bb_hdr, pdu2str(pdu)))
		self._lmp.receive(pkt.clkn, pdu)

class BTCtlDiscoverableCmd(BTCtlSuperCmd):
	# How long each of the two scans runs before the other takes its turn.
	SCAN_TIME = 2

	def run_once(self):
		# _start_cmd keeps the handle itself, and returns nothing when it
		# refuses to start one, so assigning its result here would drop it.
		self._start_cmd(BTCtlInquiryScanCmd)
		self._wait(self.SCAN_TIME)
		self._stop_cmd()
		if self.stopped():
			return
		self._start_cmd(BTCtlPageScanCmd)
		self._wait(self.SCAN_TIME)
		# If a connection is running, wait for it to complete
		while (self._cmd is not None and not self._cmd.stop_allowed()
				and not self.stopped()):
			self._wait(0.5)
		self._stop_cmd()

class BTCtlMonitorCmd(BTCtlCmd):
	def __init__(self, bt, peripheral_bdaddr):
		super().__init__(bt)
		self._bdaddr = peripheral_bdaddr
		self._uap = 0 # needs Central bdaddr

	def _start(self):
		self._bt.send_monitor_cmd(self._bdaddr)

	def _handle_state(self, state, reason):
		lt_addr = reason>>5
		reason &= 0x1f
		if state == BTCTL_STATE_PAGE_SCAN:
			log.info("Monitor started")
			self._ready = True
		elif state == BTCTL_STATE_CONNECTED:
			log.info ("Connection status %d"%reason)
		else:
			if state != BTCTL_STATE_STANDBY:
				log.info("Invalid state!")
				print_state(state, reason)
			self._done = True
			log.info("Monitor done")

	def _handle_fhs(self, pkt):
		role = "Peripheral" if (pkt.clkn & 2) else "Central"
		result("RX FHS (%10s:%7x): %s"%(role,pkt.clkn,pkt.bt_data))
		self._uap = 0xff&(pkt.bt_data.bdaddr>>24)

	def _handle_raw(self, pkt):
		acl = pkt.bt_data
		# Check CRC to handle LMP
		data = acl.raw_data[:acl.raw_size+2]
		if len(data) < acl.raw_size+2:
			return self._handle_bad(pkt)
		crc = u16(data[-2:])
		crc_cal = crc_compute(data[:-2], reverse8(self._uap)<<8)
		if crc != crc_cal:
			return self._handle_bad(pkt)
		# Fix acl data
		acl.data = data[acl.hlen:-2]
		if acl.llid == LLID_LMP:
			self._handle_lmp(pkt)
		else:
			self._handle_l2cap(pkt)

	def _handle_bad(self, pkt):
		role = "Peripheral" if (pkt.clkn & 2) else "Central"
		result("RX Raw (%10s:%7x): %s | %s"%(role,pkt.clkn, pkt.bb_hdr,
			data_to_str(pkt.bt_data.raw_data)))

	def _handle_lmp(self, pkt):
		pdu = pkt.bt_data.data
		if not pdu: return
		op = u8(pdu[0])>>1
		data = pdu[1:]
		# Monitor follows a piconet it is not part of, so a PDU too short to
		# hold its parameters is a damaged capture rather than a peer's
		# mistake: there is nobody to refuse it to, and a short channel map
		# would send the firmware hopping over a map the central never set.
		# So the map is left as it was, and the note says why the capture
		# stays on it. The line below reports the PDU either way.
		if op == LMP_SET_AFH:
			if len(data) < AFH_PARAMETERS_LEN:
				warn("an LMP_SET_AFH too short to read, so the capture"
					" keeps the channel map it has: %s"%(
						hexlify(data).decode()))
			else:
				instant = u32(data[:4])
				mode = data[4]
				chan_map = data[5:AFH_PARAMETERS_LEN]
				self._bt.send_set_afh_cmd(instant<<1, mode, chan_map)
				log.info("AFH req: instant=%d, (cur %d), mode=%d"%(
					instant<<1, pkt.clkn, mode))
		role = "Peripheral" if (pkt.clkn & 2) else "Central"
		result("RX LMP (%10s:%7x): %s | %s"%(role,pkt.clkn, pkt.bb_hdr, pdu2str(pdu)))

	def _handle_l2cap(self, pkt):
		role = "Peripheral" if (pkt.clkn & 2) else "Central"
		result("RX L2CAP (%10s:%7x): %s"%(role,pkt.clkn,pkt))

def board_label(serial=None, index=None):
	"""How a message names a board: by serial, by index, or not at all.

	The index form is for a board chosen by index, whose serial was never
	read -- see board_serial(). Without either, "the Ubertooth" is as much as
	the message can say, which is the case of a single attached board. Each
	form carries its own article, as libubertooth's board_label() does, so a
	message puts it straight after its verb: "could not open the Ubertooth",
	"could not open Ubertooth 1cc0...".
	"""
	if serial:
		return "Ubertooth %s"%serial
	if index is not None:
		return "Ubertooth at index %d"%index
	return "the Ubertooth"


def attached_ubertooths(context):
	"""Every board the C tools recognize, in enumeration order.

	The devices only: a serial is read by board_serial() where one is needed.
	"""
	return [device for device in context.getDeviceIterator(skip_on_error=True)
		if (device.getVendorID(), device.getProductID()) in UBERTOOTH_USB_IDS]


def board_serial(device):
	"""The board's serial number, or None when it could not be read.

	Reading it opens the board and asks for a string descriptor, so this is
	called only where a serial is needed: matching a -U serial, and listing
	the boards a refusal names. Selecting by index reads none, as the other
	tools' index path does, so naming a board by index does not disturb the
	others -- ubertooth(7), SELECTING A DEVICE, promises that.

	What makes this fail is therefore an open that fails -- on Linux, usually
	the udev rule not being installed -- or a board that has left the bus. A
	board whose interface another program has claimed still opens and still
	answers this, and fails later at claimInterface().

	The firmware fills this descriptor in from the chip's serial only when
	the chip answered; a board where it did not advertises a serial of zeros
	ending in 1, which comes back here as if it were real.
	"""
	try:
		return device.getSerialNumber()
	except usb1.USBError:
		return None


def several_attached(attached):
	"""The refusal for more than one board and no -U, with the boards listed."""
	listed = ""
	for index, device in enumerate(attached):
		serial = board_serial(device)
		listed += "\n  Device %d: %s"%(index,
			serial if serial else "serial unreadable")
	return ("multiple Ubertooth devices found; select one with "
		"-U <index|serial>"+listed)


def unread_serials_note(serials):
	"""What to add to a serial refusal when some boards did not answer.

	A scan that read every board means something different from one that did
	not: the string the user gave may well be the serial of a board that
	stayed quiet. The remedy is given by index, because a board whose serial
	could not be read cannot be selected by serial. The causes are the ones
	board_serial() names.
	"""
	unread = serials.count(None)
	if not unread:
		return ""
	return ("\n%d attached Ubertooth device%s did not answer the serial read\n"
		"note: a board nobody may open, or one that has left the bus, reads "
		"as having no serial; select it by index with -U<index>"
		%(unread, "" if unread == 1 else "s"))


def open_failure_note(failure):
	"""The usual cause of an open that was refused.

	libubertooth reports an open failure with libusb's own description
	(show_libusb_error()), so the message keeps that and the cause goes in a
	note. There is one: libusb_open() refuses for want of permission, and on
	Linux that is nearly always the udev rules not being installed. It does
	not report a board another program is using -- that shows up later, when
	the interface is claimed -- so nothing here says it does.
	"""
	if isinstance(failure, usb1.USBErrorAccess):
		return ("\nnote: a board that cannot be opened is usually a "
			"permissions problem; on Linux install the udev rules shipped "
			"with this package")
	return ""


def claim_failure_message(board, error):
	"""Why the board's USB interface could not be claimed.

	The wording of libubertooth's ubertooth_report_claim_failed(), so one
	wording covers every tool. A board another program is using gets through
	the open and fails here, which is the whole point of the line: BUSY on
	Linux and ACCESS on macOS both mean that program, not a permissions
	problem, because a board nobody may open never reached the claim.
	"""
	if isinstance(error, (usb1.USBErrorBusy, usb1.USBErrorAccess)):
		return ("%s is in use by another program (libusb %s)"
			%(board, error.value))
	return ("claiming the USB interface of %s failed (libusb %s)"
		%(board, error.value))


def select_ubertooth(selector, attached):
	"""The one board a -U argument names, and how a message should name it.

	`attached` is what attached_ubertooths() returned, and must not be empty.
	Anything that does not name exactly one board raises BTCtlNoDeviceError,
	in libubertooth's wording for the same refusal so that one wording covers
	every tool. Two lines depart from it deliberately: the one for several
	boards and no -U follows .kiro/steering/cli-output.md rather than the
	older C line, and the note for a board that did not answer the serial
	read gives the cause of a failed string-descriptor read rather than of
	the vendor request the C tools use.
	"""
	if not selector:
		if len(attached) > 1:
			raise BTCtlNoDeviceError(several_attached(attached))
		return attached[0], board_label()

	# An all-digits argument is an index. Only these ten characters count:
	# str.isdigit() also accepts digits of other scripts, some of which int()
	# then refuses, and no serial contains any of them.
	if all(character in "0123456789" for character in selector):
		try:
			index = int(selector)
		except ValueError:
			# Python refuses to convert a string of thousands of digits.
			# Such a number is no board's index, so it is refused as one
			# rather than let out as a traceback.
			index = None
		if index is None or index >= len(attached):
			raise BTCtlNoDeviceError(
				"no Ubertooth at index %s (%d attached)\n"
				"indexes change when boards are added or removed; select a "
				"board by serial with -U<serial>"%(selector, len(attached)))
		return attached[index], board_label(index=index)

	# Serials differ in their leading characters and share their trailing
	# ones, so only the start is matched. Case-insensitively, because a
	# serial copied from elsewhere may be upper case.
	serials = [board_serial(device) for device in attached]
	matched = [(device, serial) for device, serial in zip(attached, serials)
		if serial is not None
		and serial.lower().startswith(selector.lower())]
	if len(matched) == 1:
		device, serial = matched[0]
		# The whole serial of the board that matched, so a later failure
		# names the board rather than the few characters the user typed.
		return device, board_label(serial)
	if not matched:
		raise BTCtlNoDeviceError(
			"no Ubertooth found with serial matching '%s'"%selector
			+ unread_serials_note(serials))
	listed = ("'%s' matches %d Ubertooth devices: %s\n"
		%(selector, len(matched),
		  ", ".join(serial for device, serial in matched)))
	if len(set(serial for device, serial in matched)) == 1:
		# A longer string only helps while the matching serials differ:
		# boards reporting one serial cannot be told apart this way.
		raise BTCtlNoDeviceError(listed
			+ "these Ubertooth devices report the same serial number; "
			"select one by index with -U<index>"
			+ unread_serials_note(serials))
	raise BTCtlNoDeviceError(listed
		+ "select one of them by a longer prefix of its serial number"
		+ unread_serials_note(serials))


class BTCtl:
	DATA_IN = 0x82
	DATA_OUT = 0x05
	# The receive loop looks at the stop flag every bulk read timeout, so a
	# join this long only runs out when libusb itself is stuck.
	JOIN_TIMEOUT = 2
	# Milliseconds a request may take to reach the device. Writes were given
	# no timeout at all, so a device that never drains the endpoint, which is
	# what firmware other than ubtbr looks like, blocked the sender forever
	# inside a libusb call no keyboard interrupt can reach. Long enough that
	# nothing working trips it.
	WRITE_TIMEOUT = 2000
	def __init__(self, usb, board=None):
		self._usb = usb
		# How a message names this board, from board_label(). Plain
		# "Ubertooth" when the caller built this from a handle of its own.
		self._board = board or board_label()
		self._rx_thread = None
		# The kinds of message already dropped, so each is said once. Only
		# the receive thread touches it, and the kinds are the fixed set of
		# wordings this file has.
		self._dropped = set()
		self._con = False
		self._con_lock = Lock()
		self._rx_stopped = Event()
		self._rx_stopped.clear()
		self._rx_error = None
		# Set when a message may have been left half delivered, after which
		# the device accepts nothing but the rest of it.
		self._out_of_step = False
		# Whether the USB handle is still open, which outlives _con: a close
		# that had to leave the device alone clears one and not the other.
		self._open = False
		self._msg_handler = None

	@staticmethod
	def find(selector=None):
		"""Open the Ubertooth a -U argument names.

		Selection follows ubertooth(7), SELECTING A DEVICE, as the other
		tools do: no argument takes the only attached board, an all-digits
		argument is a position in the list of attached boards, and anything
		else matches a whole serial number or its first characters, in
		either case. Anything that does not name exactly one board is
		refused, and so is a board that could not be opened. The only error
		this raises is BTCtlNoDeviceError, so a caller has one thing to
		report for everything that can go wrong before a board is open.

		Only the serial path reads serials; see board_serial().

		Refusing matters more here than the message does. This used to open
		whichever board libusb handed over first, so on a host with several
		boards the tool reached an arbitrary one: a user who had flashed
		btbr onto one board got a write timeout from another board still
		carrying bluetooth_rxtx, and a note blaming the firmware of the
		board they had just flashed.
		"""
		context = usb1.USBContext()
		try:
			attached = attached_ubertooths(context)
		except usb1.USBError as e:
			# A bus that could not be listed is not the same as no board
			# being attached, so it is not reported as one.
			raise BTCtlNoDeviceError("could not list USB devices: %s"%e)
		if not attached:
			# The wording the other tools use for the same thing.
			raise BTCtlNoDeviceError("no Ubertooth devices found")
		device, label = select_ubertooth(selector, attached)
		try:
			handle = device.open()
		except usb1.USBError as e:
			raise BTCtlNoDeviceError("could not open %s: %s"%(label, e)
				+ open_failure_note(e))
		return BTCtl(handle, label)

	def register_msg_handler(self, h):
		# There is one handler, so a command that outlived its stop still
		# owns the messages. Saying so beats an assert: the caller can report
		# it and stay up, and under python -O an assert would quietly
		# overwrite the live handler and lose the messages going to it.
		if self._msg_handler is not None:
			raise BTCtlBusyError("the previous command is still running")
		self._msg_handler = h

	def unregister_msg_handler(self, h):
		if self._msg_handler != h:
			raise BTCtlBusyError("another command holds the message handler")
		self._msg_handler = None

	# Both of these print a line the board sent. The board's console ends
	# each line with a C string terminator and counts that byte in the
	# message, so it has to come off: a NUL on standard error makes a saved
	# session a binary file to grep. A byte that is not UTF-8 is replaced
	# rather than raising, which would end the receive thread.
	def _print_console(self, msg):
		eprint(colored(msg.strip(b"\x00").decode("utf-8", "replace"), "32;1"))

	def _print_debug(self, msg):
		eprint(colored(msg.strip(b"\x00").decode("utf-8", "replace"), "31;1"))

	def _handle_default(self, t, msg):
		if t == BTCTL_STATE_RESP:
			print_state(*msg)
		else:
			log.info("No cmd for message %d"%t)

	def _drop(self, reason, kind):
		"""Report a message that could not be read, and say to drop it.

		Said once per `kind`, which names what was wrong without the lengths
		that differ between two such messages: a board sending records of
		every wrong size must not get a line for each, since identical lines
		bury the rest of a session. The LMP unhandled-opcode warning is held
		down the same way. The message is dropped whether a line went out.
		"""
		if kind not in self._dropped:
			self._dropped.add(kind)
			warn("a message from the device was dropped: %s"%reason)

	def _handle_msg(self, data):
		if len(data) < MSG_TYPE_LEN:
			# Every message opens with the word naming its type, so one
			# shorter than that word says nothing to hand on: a message of
			# no bytes raised an IndexError here, and one of one to three
			# handed on an empty body that failed further along.
			return self._drop("message of %s, too short to name its "
				"type"%byte_count(len(data)), "message too short to name "
				"its type")
		t,msg =data[0], data[4:]
		if t == BTCTL_DEBUG:
			self._print_console(msg)
		else:
			if t == BTCTL_RX_PKT:
				try:
					msg = BTCtlRxPkt.unpack(msg)
				except BTCtlFramingError as e:
					# One record from the board that cannot be read as a
					# packet. The stream of messages is still in step --
					# _rx_loop checks that for itself -- so this costs the
					# session one packet rather than the thread that reads
					# the board, and with it the rest of the capture.
					return self._drop(str(e), e.kind)
			elif t == BTCTL_STATE_RESP and len(msg) != STATE_RESP_LEN:
				# Every reader of this message takes the state and the
				# reason for it as the two arguments it carries, so any
				# other length raised a TypeError that ended the command
				# waiting on the state.
				return self._drop("state response of %s, where the state "
					"and its reason are %d"%(byte_count(len(msg)),
						STATE_RESP_LEN), "state response of another size")
			if self._msg_handler is not None:
				self._msg_handler(t,msg)
			else:
				self._handle_default(t, msg)

	def _rx_loop(self):
		msg = None
		msg_size = 0
		while not self._rx_stopped.is_set():
			try:
				data = self._usb.bulkRead(self.DATA_IN, 64,100)
			except usb1.USBErrorTimeout:
				continue
			t = data[:1]
			if t == b'P':
				self._print_debug(data[1:])
			else:
				if  msg is None:
					if t != b'S' or len(data) < 4:
						raise BTCtlFramingError(
							"expected the start of a message, got %r"
							%data[:4])
					msg_size, = unpack("<H",data[2:4])
					msg = data[4:]
					if len(data) != min(64, 4+msg_size):
						raise BTCtlFramingError(
							"message of %d bytes started by a packet of %d"
							%(msg_size, len(data)))
				else:
					if t != b'C':
						raise BTCtlFramingError(
							"expected the rest of a message, got %r"
							%data[:4])
					if len(data) != min(64, 1+msg_size-len(msg)):
						raise BTCtlFramingError(
							"%d bytes left of a message, got a packet of %d"
							%(msg_size-len(msg), len(data)))
					msg += data[1:]
				if len(msg) == msg_size:
					self._handle_msg(msg)
					msg = None

	def _rx_thread_main(self):
		log.info("Rx thread started")
		try:
			self._rx_loop()
			log.debug("Rx thread stopped")
		except Exception as e:
			# Ending silently would leave the tool waiting for messages that
			# can no longer arrive, so keep the reason for close() to report.
			self._rx_error = e
			error("the receive thread stopped: %s: %s"%(type(e).__name__, e))
		finally:
			# Tells a reader of the flag that nothing is reading the device
			# any more, however this thread ended.
			self._rx_stopped.set()

	def connect(self):
		if self.connected():
			warn("the device is already open")
			return
		try:
			self._usb.claimInterface(0)
		except usb1.USBError as e:
			# Raised as the refusals in find() are, because it says the same
			# thing: this board cannot be used, and nothing was asked of it
			# yet. Reported as a failed set-up it reached the generic line
			# for a board that took none of the session's requests, with a
			# note about firmware that has nothing to do with it.
			raise BTCtlNoDeviceError(claim_failure_message(self._board, e))
		self._rx_stopped.clear()
		self._rx_error = None
		# Daemon, so a receive loop stuck in libusb cannot hold up an
		# interpreter that is shutting down.
		self._rx_thread = Thread(target=self._rx_thread_main, daemon=True)
		# Set before the thread runs, so an interrupt landing between the two
		# still leaves a device whoever handles it can see and close.
		self._open = True
		self._con = True
		self._rx_thread.start()
		log.info("USB connected")
		self.send_idle_cmd()
		sleep(1)

	def connected(self):
		return self._con

	def board(self):
		"""How a message names this board.

		A failure early in a session is often the wrong board rather than a
		bad one, so naming it is what tells the two apart.
		"""
		return self._board

	def rx_error(self):
		"""What ended the receive thread, when something other than a stop did."""
		return self._rx_error

	def close(self):
		"""Ask the device to go idle and close it.

		Returns whether the device ended up closed. A receive thread still
		inside libusb, or a command that still owns the messages, leaves the
		handle open, because closing it under either is how a clean exit
		turns into a crash. The caller is the only one that can report that
		the session did not finish what it was asked.
		"""
		if not self.connected():
			warn("the device is not open")
			return not self._open
		self._con = False
		try:
			self.send_idle_cmd()
		except (usb1.USBError, BTCtlError) as e:
			# Said to be the close, because a command's stop has a line with
			# the same cause and both can appear in one session.
			warn("could not ask the device to go idle while closing it: %s"%e)
		if self.rx_error() is not None:
			warn("the receive thread had already stopped: %s"%self.rx_error())
		self._rx_stopped.set()
		self._rx_thread.join(self.JOIN_TIMEOUT)
		if self._rx_thread.is_alive():
			error("the receive thread did not stop; leaving the device open")
			return False
		if self._msg_handler is not None:
			# A command still owns the messages, so it is still running and
			# can still write. Whoever started it may have lost track of it:
			# a command that runs commands of its own can give up on a child
			# and then end itself, leaving nobody holding the child. This is
			# the check that covers every such route.
			error("a command is still running; leaving the device open")
			return False
		self._usb.close()
		self._open = False
		return True

	def _send_usb_bulk(self, bulk):
		assert(len(bulk)<=64)
		self._usb.bulkWrite(self.DATA_OUT, bulk, self.WRITE_TIMEOUT)

	def _send_usb(self, data):
		# Held as a context manager: a write that fails half way through a
		# message used to keep the lock, so the next send never returned.
		with self._con_lock:
			if self._out_of_step:
				raise BTCtlFramingError(
					"the device is waiting for the rest of a message that "
					"could not be sent")
			usb_hdr = b"S\x00"+pack("<H", len(data))
			head,rest = data[:60],data[60:]
			try:
				self._send_usb_bulk(usb_hdr+head)
				while rest:
					chunk,rest = rest[:63],rest[63:]
					self._send_usb_bulk(b'C'+chunk)
			except usb1.USBError:
				if len(data) > 60:
					# The device keeps what it has of a message and takes
					# only the rest of it; anything else is a framing error
					# the firmware does not come back from. A message that
					# needed more than one packet and stopped part way may
					# have left it waiting, so nothing more is sent here.
					self._out_of_step = True
				raise

	def _send_cmd(self, t, data=b''):
		hdr = pack("<I", t)
		self._send_usb(hdr+data)

	def send_debug_cmd(self, msg):
		msg = msg[:256]
		self._send_cmd(BTCTL_DEBUG, msg)

	def send_idle_cmd(self):
		log.info("Send idle")
		self._send_cmd(BTCTL_IDLE_REQ)

	def send_set_freq_off_cmd(self, off):
		self._send_cmd(BTCTL_SET_FREQ_OFF_REQ, p16(off))

	def send_set_max_ac_errors_cmd(self, max_ac_errors):
		self._send_cmd(BTCTL_SET_MAX_AC_ERRORS_REQ, p16(max_ac_errors))

	def send_set_bdaddr_cmd(self, bdaddr):
		self._send_cmd(BTCTL_SET_BDADDR_REQ, pack("<Q", bdaddr))

	def send_inquiry_cmd(self):
		self._send_cmd(BTCTL_INQUIRY_REQ)

	def send_inquiry_scan_cmd(self):
		self._send_cmd(BTCTL_INQUIRY_SCAN_REQ)

	def send_paging_cmd(self, bdaddr):
		self._send_cmd(BTCTL_PAGING_REQ, pack("<Q", bdaddr))

	def send_monitor_cmd(self, bdaddr):
		self._send_cmd(BTCTL_MONITOR_REQ, pack("<Q", bdaddr))

	def send_page_scan_cmd(self):
		self._send_cmd(BTCTL_PAGE_SCAN_REQ)

	def send_acl_cmd(self, llid, data, flow=1, lt_addr=1, flags=0, bt_type=None):
		acl = BTCtlACLPkt(llid, data, flow, bt_type)
		bb_hdr = BBHdr(lt_addr, acl.bt_type, flags)
		payload = bb_hdr.pack() + acl.pack()
		log.debug("send bb %s, acl: %s"%(bb_hdr,acl))
		self._send_cmd(BTCTL_TX_ACL_REQ, payload)

	def send_set_eir_cmd(self, eir_data):
		acl = BTCtlACLPkt(LLID_L2CAP_START, eir_data, 1)
		bb_hdr = BBHdr(0, acl.bt_type)	# send on lt_addr 0
		payload = bb_hdr.pack() + acl.pack()
		log.debug("Set eir %s, acl: %s"%(bb_hdr,acl))
		self._send_cmd(BTCTL_SET_EIR_REQ, payload)

	def send_set_afh_cmd(self, instant, mode, afh_map):
		cmd = pack("<IB", instant,mode)+afh_map
		log.debug("send set afh: %s"%hexlify(cmd))
		self._send_cmd(BTCTL_SET_AFH_REQ, cmd)
