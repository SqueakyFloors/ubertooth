/*
 * Copyright 2010 Michael Ossmann
 * Copyright 2013 Dominic Spill
 *
 * This file is part of Project Ubertooth.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

/*
	LPCUSB, an USB device driver for LPC microcontrollers
	Copyright (C) 2006 Bertrik Sikken (bertrik@sikken.nl)

	Redistribution and use in source and binary forms, with or without
	modification, are permitted provided that the following conditions are met:

	1. Redistributions of source code must retain the above copyright
	   notice, this list of conditions and the following disclaimer.
	2. Redistributions in binary form must reproduce the above copyright
	   notice, this list of conditions and the following disclaimer in the
	   documentation and/or other materials provided with the distribution.
	3. The name of the author may not be used to endorse or promote products
	   derived from this software without specific prior written permission.

	THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
	IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
	OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
	IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
	INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
	NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
	DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
	THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
	(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
	THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "usbapi.h"
#include "usbhw_lpc.h"
#include "ubertooth.h"
#include "ubertooth_usb.h"
#include "watchdog.h"
#include <string.h>

#ifdef UBERTOOTH_ZERO
#define ID_VENDOR 0x1D50
#define ID_PRODUCT 0x6000
#elif defined UBERTOOTH_ONE
#define ID_VENDOR 0x1D50
#define ID_PRODUCT 0x6002
#elif defined TC13BADGE
#define ID_VENDOR 0xFFFF
#define ID_PRODUCT 0x0004
#else
#define ID_VENDOR 0xFFFF
#define ID_PRODUCT 0x0004
#endif

#define BULK_IN_EP		0x82
#define BULK_OUT_EP		0x05

#define MAX_PACKET_SIZE	64

#define LE_WORD(x)		((x)&0xFF),((x)>>8)

/*
 * This is supposed to be a lock-free ring buffer, but I haven't verified
 * atomicity of the operations on head and tail.
 */

static u8 abDescriptors[] = {

/* Device descriptor */
	0x12,
	DESC_DEVICE,
	LE_WORD(0x0200),		// bcdUSB
	0xFF,              		// bDeviceClass
	0x00,              		// bDeviceSubClass
	0x00,              		// bDeviceProtocol
	MAX_PACKET_SIZE0,  		// bMaxPacketSize
	LE_WORD(ID_VENDOR),		// idVendor
	LE_WORD(ID_PRODUCT),		// idProduct
	/* The host reads this as the firmware's API version (bcdDevice), so it
	 * comes straight from the shared constant and cannot drift from it. */
	LE_WORD(UBERTOOTH_API_VERSION),	// bcdDevice
	0x01,              		// iManufacturer
	0x02,              		// iProduct
	0x03,              		// iSerialNumber
	0x01,              		// bNumConfigurations

// configuration
	0x09,
	DESC_CONFIGURATION,
	LE_WORD(0x20),  		// wTotalLength
	0x01,  					// bNumInterfaces
	0x01,  					// bConfigurationValue
	0x00,  					// iConfiguration
	0x80,  					// bmAttributes
	0x6e,  					// bMaxPower (220mA)

// interface
	0x09,
	DESC_INTERFACE,
	0x00,  		 			// bInterfaceNumber
	0x00,   				// bAlternateSetting
	0x02,   				// bNumEndPoints
	0xFF,   				// bInterfaceClass
	0x00,   				// bInterfaceSubClass
	0x00,   				// bInterfaceProtocol
	0x00,   				// iInterface

// bulk in
	0x07,
	DESC_ENDPOINT,
	BULK_IN_EP,				// bEndpointAddress
	0x02,   				// bmAttributes = BULK
	LE_WORD(MAX_PACKET_SIZE),// wMaxPacketSize
	0,						// bInterval

// bulk out
	0x07,
	DESC_ENDPOINT,
	BULK_OUT_EP,			// bEndpointAddress
	0x02,   				// bmAttributes = BULK
	LE_WORD(MAX_PACKET_SIZE),// wMaxPacketSize
	0,						// bInterval

// string descriptors
	0x04,
	DESC_STRING,
	LE_WORD(0x0409),

	// manufacturer string
	40,
	DESC_STRING,
	'G', 0x00,
	'r', 0x00,
	'e', 0x00,
	'a', 0x00,
	't', 0x00,
	' ', 0x00,
	'S', 0x00,
	'c', 0x00,
	'o', 0x00,
	't', 0x00,
	't', 0x00,
	' ', 0x00,
	'G', 0x00,
	'a', 0x00,
	'd', 0x00,
	'g', 0x00,
	'e', 0x00,
	't', 0x00,
	's', 0x00,

	// product string
	28,
	DESC_STRING,
	'U', 0x00,
	'b', 0x00,
	'e', 0x00,
	'r', 0x00,
	't', 0x00,
	'o', 0x00,
	'o', 0x00,
	't', 0x00,
	'h', 0x00,
	' ', 0x00,
	'O', 0x00,
	'n', 0x00,
	'e', 0x00,

	// serial number string
	0x42,
	DESC_STRING,
	'0', 0, '0', 0, '0', 0, '0', 0, '0', 0, '0', 0, '0', 0, '0', 0,
	'0', 0, '0', 0, '0', 0, '0', 0, '0', 0, '0', 0, '0', 0, '0', 0,
	'0', 0, '0', 0, '0', 0, '0', 0, '0', 0, '0', 0, '0', 0, '0', 0,
	'0', 0, '0', 0, '0', 0, '0', 0, '0', 0, '0', 0, '0', 0, '1', 0,

	// terminator
	0
};

#define USB_SERIAL_OFFSET 124

u8 abVendorReqData[VENDOR_REQ_DATA_LEN];

/* Unused functions
void usb_bulk_in_handler(u8 bEP, u8 bEPStatus)
{
	if (!(bEPStatus & EP_STATUS_DATA))
		dequeue_send();
}

void usb_bulk_out_handler(u8 bEP, u8 bEPStatus)
{
}
*/

VendorRequestHandler *v_req_handler;

BOOL usb_vendor_request_handler(TSetupPacket *pSetup, int *piLen, u8 **ppbData)
{
	int rv;
	u16 params[3] = {pSetup->wValue, pSetup->wIndex, pSetup->wLength};
	/*
	 * Bit 7 of bmRequestType is the data stage direction (USB 2.0 9.3.1).
	 * Only a host-to-device request brings the handler input, and the
	 * stack waits for all of it before calling; a device-to-host request
	 * is handled at SETUP time, with nothing of the host's in the buffer.
	 */
	int data_from_host =
		REQTYPE_GET_DIR(pSetup->bmRequestType) == REQTYPE_DIR_TO_DEVICE;
	unsigned data_in_len = data_from_host ? pSetup->wLength : 0;

	rv = v_req_handler(pSetup->bRequest, params, *ppbData, data_from_host,
	                   data_in_len, piLen);
	return (BOOL) (rv==1);
}


void set_serial_descriptor(u8 *descriptors) {
	u8 buf[17], *desc, nibble;
	int len, i;
	get_device_serial(buf, &len);
	if(buf[0] == 0) { /* IAP success */
		desc = descriptors + USB_SERIAL_OFFSET;
		for(i=0; i<16; i++) {
			nibble  = (buf[i+1]>>4) & 0xF;
			desc[i * 4] = (nibble > 9) ? ('a' + nibble - 10) : ('0' + nibble);
			desc[1+ i * 4] = 0;
			nibble = buf[i+1]&0xF;
			desc[2 + i * 4] = (nibble > 9) ? ('a' + nibble - 10) : ('0' + nibble);
			desc[3 + i * 4] = 0;
		}
	}
}

int ubertooth_usb_init(VendorRequestHandler *vendor_req_handler)
{
	// initialise stack
	USBInit();

	set_serial_descriptor(abDescriptors);
	
	// register device descriptors
	USBRegisterDescriptors(abDescriptors);

	// Request handler
	v_req_handler = vendor_req_handler;

	// override standard request handler
	USBRegisterRequestHandler(REQTYPE_TYPE_VENDOR, usb_vendor_request_handler,
	                          abVendorReqData, sizeof(abVendorReqData));

	// register endpoints
	//USBHwRegisterEPIntHandler(BULK_IN_EP, usb_bulk_in_handler);
	//USBHwRegisterEPIntHandler(BULK_OUT_EP, usb_bulk_out_handler);

	// enable USB interrupts
	//ISER0 = ISER0_ISE_USB;

	// Enable WCID / driverless setup on Windows - Consumes Vendor Request 0xFF
	USBRegisterWinusbInterface(0xFF, "{8ac47a88-cc26-4aa9-887b-42ca8cf07a63}");

	// connect to bus
	USBHwConnect(TRUE);

	return 0;
}

usb_pkt_rx fifo[128];

volatile u32 head = 0;
volatile u32 tail = 0;

/* When the host last had something, so a quiet capture can still be told the
 * board is alive. See dequeue_send() below. */
#define USB_KEEP_ALIVE 400000
u32 last_usb_pkt = 0;

void usb_queue_init(void)
{
	head = 0;
	tail = 0;
	memset(fifo, 0, sizeof(fifo));

	/*
	 * The keep-alive timer goes with the queue. It holds a clock reading
	 * from the mode that set it, and cc2400_idle() restarts the clock at
	 * zero, so carrying it across a mode change made `clkn - last_usb_pkt`
	 * wrap and look like a long silence: the board sent a keep-alive into
	 * the endpoint the moment the next capture cleared it, and that landed
	 * at the head of the capture as a record of whatever the host's receive
	 * buffer happened to hold. A basic-rate dump opened with one.
	 */
	last_usb_pkt = 0;
}

usb_pkt_rx *usb_enqueue(void)
{
	u8 h = head & 0x7F;
	u8 t = tail & 0x7F;
	u8 n = (t + 1) & 0x7F;

	/* fail if queue is full */
	if (h == n) {
		return NULL;
	}

	++tail;
	return &fifo[t];

}

usb_pkt_rx *dequeue(void)
{
	u8 h = head & 0x7F;
	u8 t = tail & 0x7F;

	/* fail if queue is empty */
	if (h == t) {
		return NULL;
	}

	++head;
	return &fifo[h];
}

/* How many more records the ring can take before usb_enqueue() starts
 * refusing them. A caller with several records to add, such as the fragments
 * of one long LE packet, asks first so it never adds half of them. */
unsigned usb_queue_space(void)
{
	/* One of the 128 entries is always left empty, which is what tells a
	 * full ring from an empty one. */
	return (head - tail - 1) & 0x7F;
}

/* The channel, clock and signal readings of the packet, copied from the
 * caller's template onto every record that carries part of it. */
static void fill_le_metadata(usb_pkt_rx *f, const le_pkt_meta *meta,
                             uint8_t pkt_type)
{
	f->pkt_type = pkt_type;
	f->status = meta->status;
	f->channel = meta->channel;
	f->clkn_high = meta->clkn_high;
	f->clk100ns = meta->clk100ns;
	f->rssi_max = meta->rssi_max;
	f->rssi_min = meta->rssi_min;
	f->rssi_avg = meta->rssi_avg;
	f->rssi_count = meta->rssi_count;
}

/* Copies into one record's 50 data bytes the part of the captured packet that
 * starts at byte `offset`. The captured packet is the 4-byte access address
 * followed by `rest`, so only the first record straddles the two; the rest of
 * the record is zeroed, because records are reused. */
static void fill_le_chunk(usb_pkt_rx *f, const uint8_t *aa,
                          const uint8_t *rest, unsigned offset, unsigned chunk)
{
	unsigned from_address = 0;

	if (offset < 4) {
		from_address = 4 - offset;
		if (from_address > chunk)
			from_address = chunk;
		memcpy(f->data, aa + offset, from_address);
	}
	if (chunk > from_address)
		memcpy(f->data + from_address, rest + offset + from_address - 4,
		       chunk - from_address);
	if (chunk < DMA_SIZE)
		memset(f->data + chunk, 0, DMA_SIZE - chunk);
}

int usb_enqueue_le_packet(const le_pkt_meta *meta, const uint8_t *aa,
                          const uint8_t *rest, unsigned rest_len)
{
	/* The whole captured packet: the access address, then the PDU header,
	 * the payload and the CRC that rest_len counts. */
	unsigned total = 4 + rest_len;
	unsigned fragments, offset;
	usb_pkt_rx *f;

	if (total > LE_MAX_USB_PACKET_SIZE)
		return 0;

	/* One record holds a packet of 50 bytes or less, which is nearly all of
	 * them, and is sent as it always was. */
	if (total <= DMA_SIZE) {
		f = usb_enqueue();
		if (f == NULL)
			return 0;
		fill_le_metadata(f, meta, LE_PACKET);
		f->reserved[0] = 0;
		f->reserved[1] = 0;
		fill_le_chunk(f, aa, rest, 0, total);
		return 1;
	}

	/* Each fragment is one record carrying 50 bytes of the packet. The first
	 * and continuation record types mark the chain; reserved[0] and bit 0 of
	 * reserved[1] carry the total length, and bits 7..1 of reserved[1] a
	 * fragment index that counts up from zero. */
	fragments = (total + DMA_SIZE - 1) / DMA_SIZE;
	if (usb_queue_space() < fragments)
		return 0; /* all or nothing: half a packet is worse than none */

	for (offset = 0; offset < total; offset += DMA_SIZE) {
		unsigned chunk = total - offset;
		if (chunk > DMA_SIZE)
			chunk = DMA_SIZE;
		/* The space check above means this cannot fail, so there is no
		 * half-enqueued packet to undo. */
		f = usb_enqueue();
		if (f == NULL)
			return 0;
		fill_le_metadata(f, meta,
			(offset == 0) ? LE_PACKET_START : LE_PACKET_CONT);
		f->reserved[0] = total & 0xff;
		f->reserved[1] = ((total >> 8) & LE_FRAG_TOTAL_HIGH_MASK) |
			((offset / DMA_SIZE) << LE_FRAG_INDEX_SHIFT);
		fill_le_chunk(f, aa, rest, offset, chunk);
	}

	return 1;
}

int dequeue_send(u32 clkn)
{
	usb_pkt_rx *pkt = dequeue();
	if (pkt != NULL) {
		last_usb_pkt = clkn;
		USBHwEPWrite(BULK_IN_EP, (u8 *)pkt, sizeof(usb_pkt_rx));
		return 1;
	} else {
		if (clkn - last_usb_pkt > USB_KEEP_ALIVE) {
			/* USBHwEPWrite() moves whole words, so the source has to be
			 * word-sized even when only the first byte is sent. */
			u8 pkt_type[4] = { KEEP_ALIVE, 0, 0, 0 };
			last_usb_pkt = clkn;
			USBHwEPWrite(BULK_IN_EP, pkt_type, 1);
		}
		return 0;
	}
}

/*
 * Service the control path only, for a mode whose host collects packets with
 * UBERTOOTH_POLL control requests instead of reading the bulk IN endpoint.
 *
 * Such a mode must leave its packets in the queue, because that is where the
 * request looks for them. handle_usb() below does the opposite: it hands a
 * queued packet to each free bulk IN hardware buffer. There are two of them
 * and nothing drains them while a poll-read mode runs, so every one of those
 * packets was simply gone -- the first two packets of every session.
 *
 * The USB interrupt is held off for the reason handle_usb() gives.
 */
void handle_usb_control(void)
{
	u32 usb_irq = usb_irq_hold();
	/* As in handle_usb(): a board the watchdog resets while it is in here
	 * says so rather than blaming the wait this pass was called from. */
	uint8_t site = watchdog_mark(WATCHDOG_SITE_USB);

	/* polled "interrupt" */
	USBHwISR();

	watchdog_mark(site);
	/* The LE engine's loop services USB only through here, so this is its
	 * feed: without it the watchdog would reset a board capturing happily
	 * with ubertooth-btle -f. */
	watchdog_usb_serviced();

	usb_irq_release(usb_irq);
}

/*
 * Service USB from the loop that is running, with the USB interrupt held off,
 * and keep the bulk IN endpoint loaded for a host that reads it.
 *
 * Everything in here drives the USB controller through registers there is only
 * one of: USBCtrl selects which endpoint reads and writes apply to, USBTxPLen
 * and USBRxPLen belong to that selection, and the protocol engine is driven by
 * a command-code handshake reported through the device interrupt bits. The
 * control transfer handler in usbcontrol.c also keeps one set of state for the
 * transfer it is part way through. None of it can take a second caller.
 *
 * USB is driven from two contexts in the firmware. Six modes service it from
 * the USB interrupt (bt_generic_le(), bt_le_sync(), bt_slave_le(), ego, xmas,
 * identify); the other modes and the main loop call this from their loop, or
 * handle_usb_control() above where the mode's host reads packets with
 * UBERTOOTH_POLL. What keeps the two apart today is only that no mode does
 * both: each of those six enables the interrupt on the way in and disables it
 * on the way out, and nothing calls either of these functions in between. That
 * is a property of fifteen mode functions, not of this code, and nothing used
 * to state it or check it.
 *
 * Holding the interrupt off here makes it a property of this code: whatever a
 * mode does, this is the only context driving USB while it runs. The interrupt
 * is not lost -- the NVIC keeps it pending and takes it as soon as the mask is
 * lifted -- and nothing is delayed that was not USB work anyway. On the modes
 * that exist, the hold costs one register read.
 */
void handle_usb(u32 clkn)
{
	u8 epstat;
	u32 usb_irq = usb_irq_hold();
	/* So that a board the watchdog resets while it is in here says so, rather
	 * than blaming the wait this service pass was called from. */
	uint8_t site = watchdog_mark(WATCHDOG_SITE_USB);

	/* write queued packets to USB if possible */
	epstat = USBHwEPGetStatus(BULK_IN_EP);
	if (!(epstat & EPSTAT_B1FULL) || !(epstat & EPSTAT_B2FULL)) {
		/* Handing a packet over is the one part of a pass that is not the
		 * host's request, so it records itself separately: a capture that
		 * stops answering while it is in here is a different fault from one
		 * that stops answering a request. */
		uint8_t step = watchdog_mark_inner(WATCHDOG_SITE_USB_BULK);

		if (!(epstat & EPSTAT_B1FULL))
			dequeue_send(clkn);
		if (!(epstat & EPSTAT_B2FULL))
			dequeue_send(clkn);
		watchdog_unmark(step);
	}

	/* polled "interrupt" */
	USBHwISR();

	watchdog_mark(site);
	/* A pass got all the way through, so the host's requests are being
	 * answered: the watchdog's whole subject. */
	watchdog_usb_serviced();

	usb_irq_release(usb_irq);
}
