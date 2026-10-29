/*
 * Copyright 2010 Michael Ossmann
 * Copyright 2013 Dominic Spill
 * Copyright 2020 Etienne Helluy-Lafont, Univ. Lille, CNRS.
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

#include <string.h>
#include <stddef.h>
#include <usbapi.h>
#include <usbhw_lpc.h>
#include <ubertooth.h>
#include <ubtbr/ubertooth_usb.h>
#include <ubtbr/btphy.h>
#include <ubtbr/btctl_intf.h>
#include <watchdog.h>

#ifdef UBERTOOTH_ZERO
#define ID_VENDOR 0x1D50
#define ID_PRODUCT 0x6000
#elif defined UBERTOOTH_ONE
#define ID_VENDOR 0x1D50
#define ID_PRODUCT 0x6002
#else
#define ID_VENDOR 0xFFFF
#define ID_PRODUCT 0x0004
#endif

#define USB_SERIAL_OFFSET 124
#define BULK_IN_EP		0x82
#define BULK_OUT_EP		0x05

#define MAX_PACKET_SIZE	64

#define LE_WORD(x)		((x)&0xFF),((x)>>8)

static uint8_t abVendorReqData[258];

// Current msg we're sending to host
static msg_t *tx_msg = NULL;
// Current msg we're receiving from host
static msg_t *rx_msg = NULL;

/*
 * This is supposed to be a lock-free ring buffer, but I haven't verified
 * atomicity of the operations on head and tail.
 */

static uint8_t abDescriptors[] = {

/* Device descriptor */
	0x12,
	DESC_DEVICE,
	LE_WORD(0x0200),		// bcdUSB
	0xFF,              		// bDeviceClass
	0x00,              		// bDeviceSubClass
	0x00,              		// bDeviceProtocol
	MAX_PACKET_SIZE,  		// bMaxPacketSize
	LE_WORD(ID_VENDOR),		// idVendor
	LE_WORD(ID_PRODUCT),		// idProduct
	/* The host reads this as the firmware's API version (bcdDevice), so it
	 * comes straight from the shared constant and cannot drift from it, as
	 * it did while this was the literal 0x0106. */
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

/* Not used
void usb_bulk_in_handler(u8 bEP, u8 bEPStatus)
{
}
*/

/* Bulk is received from host */
void usb_bulk_out_handler(u8 bEP, u8 bEPStatus)
{
	uint8_t data[MAX_PACKET_SIZE], *p, type;
	uint16_t packet_size;
	int bulk_size, wsize;

	// Read the received packet 
	bulk_size = USBHwEPRead(BULK_OUT_EP, (u8*)data, MAX_PACKET_SIZE);

	/* A read that failed, or a packet too short to hold the four-byte header
	 * below, has nothing to work with: the sizes computed from it go negative
	 * and reach memcpy(). Drop it; the host's message is lost, which is what
	 * a lost bulk packet always is. */
	if (bulk_size < 4)
		return;

	/* copy/pasted from host code */
	type = data[0];

	/* Receive normal messages */
	if (rx_msg == NULL)
	{
		/* Receive first chunk */
		if (type != BTUSB_MSG_START)
		{
			DIE("Unexpected type %c while waiting rx_msg\n", type);
		}
		packet_size = data[2]|(data[3]<<8);
		rx_msg = msg_alloc(packet_size);

		// skip packet type / packet size 
		wsize = MIN(bulk_size-4, msg_write_avail(rx_msg));
		p = msg_put(rx_msg, wsize);
		memcpy(p, data+4, wsize);
	}
	else
	{
		/* Receive next chunks */
		if (type != BTUSB_MSG_CONT)
		{
			DIE("Invalid type %d while waiting rx_msg cont\n", type);
		}
		// skip 1 byte of header
		wsize = MIN(bulk_size-1, msg_write_avail(rx_msg));
		p = msg_put(rx_msg, wsize);
		memcpy(p, data+1, wsize);
	}
	if (msg_write_avail(rx_msg) == 0)
	{
		btctl_rx_enqueue(rx_msg);
		rx_msg = NULL;
	}
}

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
	USBHwRegisterEPIntHandler(BULK_OUT_EP, usb_bulk_out_handler);

	// enable USB interrupts
	ISER0 = ISER0_ISE_USB;

	// Enable WCID / driverless setup on Windows - Consumes Vendor Request 0xFF
	USBRegisterWinusbInterface(0xFF, "{8ac47a88-cc26-4aa9-887b-42ca8cf07a63}");

	// connect to bus
	USBHwConnect(TRUE);

	return 0;
}

volatile uint32_t usb_send_drops = 0;

/*
 * Send one message to the host now, rather than through the queue usb_work()
 * drains. This is the debug and fault path: early_printf() and die() use it
 * because the loop that would drain the queue is not running.
 */
void usb_send_sync(void *buf, int size)
{
	/* How many calls to this are on the stack. Servicing USB below reaches
	 * usb_bulk_out_handler(), which DIE()s on a message it cannot make
	 * sense of, and die() prints through here -- so this can be entered
	 * from inside itself. A nested call sends if there is room and drops
	 * its message if there is not, rather than starting a second wait. One
	 * bound is then the whole wait, however the firmware got here. */
	static volatile unsigned depth = 0;
	/* Set once a wait has run the whole bound and given up, cleared by the
	 * next message that gets through. The caller is usually printing a run
	 * of lines -- the fault handler dumps the stack 16 bytes at a time --
	 * and a host that did not read the first line will not read the rest, so
	 * without this each line pays the full bound and a dump costs seconds
	 * with interrupts masked. Once it is set, a message is sent if there is
	 * room and dropped if there is not; the rest of such a run is not a
	 * chance to service USB, since nothing polls the interrupt on the way
	 * through. */
	static volatile unsigned host_stalled = 0;
	unsigned polls, i;
	u8 epstat;
	uint32_t usb_irq = usb_irq_hold();
	/* So that a board the watchdog resets while it is in here says so, rather
	 * than blaming whatever the firmware was doing when it called this. */
	uint8_t site = watchdog_mark(WATCHDOG_SITE_USB);

	/* Read after the hold, so a nested call really does see depth non-zero. */
	polls = (depth || host_stalled) ? 1 : USB_SEND_SYNC_POLLS;
	depth++;

	/*
	 * Wait for room in the bulk IN endpoint, with a bound.
	 *
	 * Both endpoint buffers stay full until the host reads them, and the
	 * host may never read: its process was killed, or it is not reading
	 * bulk at all. This wait used to have no bound and ran with the USB
	 * interrupt masked, which is how this firmware services USB, so a host
	 * that stopped reading left a board that still enumerated and answered
	 * nothing until it was power-cycled.
	 *
	 * Room is one free buffer of the endpoint's two, which is all a packet
	 * needs and what usb_work() below checks for; waiting for both to
	 * drain, as this did, waits on a host that is reading steadily. The
	 * USB interrupt is polled between passes, so control requests are
	 * answered while we wait -- the rest of the firmware is not running,
	 * which is why the bound matters -- and giving up costs one debug
	 * message the host was not going to read. bluetooth_rxtx's handle_usb()
	 * drops one without waiting at all.
	 */
	for (i = 0; ; i++) {
		epstat = USBHwEPGetStatus(BULK_IN_EP);
		if (!(epstat & EPSTAT_B1FULL) || !(epstat & EPSTAT_B2FULL))
			break;
		if (i+1 >= polls) {
			usb_send_drops++;
			/* Only a call that ran the whole bound has learnt
			 * anything about the host. A call that skipped the wait
			 * may be looking at a packet the call below it has just
			 * written, with a host that is reading perfectly well. */
			if (polls == USB_SEND_SYNC_POLLS)
				host_stalled = 1;
			goto end;
		}
		/* polled "interrupt" */
		USBHwISR();
		/* A pass through the controller answered whatever the host had
		 * asked for, which is what the watchdog wants to see. Feeding
		 * inside the wait matters here: the bound below is tens of
		 * thousands of passes, and the main loop -- this image's other
		 * feed -- is not running while we are in here, so without this
		 * the watchdog would cut a fault dump off part-way. */
		watchdog_usb_serviced();
	}

	host_stalled = 0;

	size = MIN(size, MAX_PACKET_SIZE);

	USBHwEPWrite(BULK_IN_EP, buf, size);
	
	/* polled "interrupt" */
	USBHwISR();
	watchdog_usb_serviced();
end:
	watchdog_mark(site);
	depth--;
	usb_irq_release(usb_irq);
}

int usb_work(void)
{
	uint8_t epstat, b1full, b2full;
	int len;
	uint8_t buf[MAX_PACKET_SIZE];
	uint16_t full_len, buf_idx = 0;
	int work = 0;
	uint32_t usb_irq = usb_irq_hold();
	/* As in usb_send_sync(): a reset from in here names this path. */
	uint8_t site = watchdog_mark(WATCHDOG_SITE_USB);

	epstat = USBHwEPGetStatus(BULK_IN_EP);
	b1full = epstat & EPSTAT_B1FULL;
	b2full = epstat & EPSTAT_B2FULL;

	/* If both buffers are full, do nothing */
	if (b1full && b2full)
	{
		goto end;
	}
	if (!tx_msg)
	{
		if(!(tx_msg = btctl_tx_dequeue()))
			goto end;
		/* Write message size in buffer first */
		full_len = msg_read_avail(tx_msg);
		/* write 4 bytes header */
		buf[0] = BTUSB_MSG_START;
		buf[1] = 0;
		buf[2] = full_len & 0xff;
		buf[3] = full_len >> 8;
		buf_idx = 4;
	}
	else{
		buf[0] = BTUSB_MSG_CONT;
		buf_idx = 1;
	}
	// now we should have a msg to send
	len = MIN(msg_read_avail(tx_msg), MAX_PACKET_SIZE-buf_idx);
	if (len && !b1full)
	{
		memcpy(buf+buf_idx, msg_pull(tx_msg, len), len);
		USBHwEPWrite(BULK_IN_EP, buf, buf_idx+len);
		buf[0] = BTUSB_MSG_CONT;
		buf_idx = 1;
		/* length for second buffer */
		len = MIN(msg_read_avail(tx_msg),MAX_PACKET_SIZE-buf_idx);
	}
	if (len && !b2full)
	{
		memcpy(buf+buf_idx, msg_pull(tx_msg, len), len);
		USBHwEPWrite(BULK_IN_EP, buf, buf_idx+len);
	}
	/* polled "interrupt" */
	USBHwISR();
	watchdog_usb_serviced();

	/* Free message if completed */
	if (!msg_read_avail(tx_msg))
	{
		/* Done with this message */
		msg_free(tx_msg);
		tx_msg = NULL;
	}
	work = 1;
end:
	watchdog_mark(site);
	usb_irq_release(usb_irq);
	return work;
}
