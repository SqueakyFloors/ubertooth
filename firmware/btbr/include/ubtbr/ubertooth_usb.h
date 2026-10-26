/*
 * Copyright 2010 Michael Ossmann
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


#ifndef __UBERTOOTH_USB_H
#define __UBERTOOTH_USB_H

#include "ubertooth.h"
#include "ubertooth_interface.h"

/*
 * `data_in_len` is how many bytes of its own the request arrived with, which
 * is not what wLength says: the USB stack calls the handler once a whole
 * host-to-device data stage has landed in `data`, but for a device-to-host
 * request it calls at SETUP time, before any data stage, where wLength is the
 * host's read size and `data` still holds the previous request's bytes. So a
 * case that reads `data` may read `data_in_len` bytes of it and no more.
 *
 * The stack also calls at SETUP time for a host-to-device request of no
 * length, so an empty data stage and no data stage at all arrive the same way,
 * with data_in_len zero. `data_from_host` is what tells them apart, and for a
 * request whose body may legitimately be empty that is the only difference
 * between the host asking for something and a read of the same number asking
 * for nothing.
 */
typedef int (VendorRequestHandler)(u8 request, u16 *request_params, u8 *data,
                                   int data_from_host, unsigned data_in_len,
                                   int *data_len);

int ubertooth_usb_init(VendorRequestHandler *vendor_req_handler);
void usb_send_sync(void *buf, int size);
int usb_work(void);

#endif /* __UBERTOOTH_USB_H */
