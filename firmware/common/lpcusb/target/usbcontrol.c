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


/** @file
	Control transfer handler.
	
	This module handles control transfers and is normally installed on the
	endpoint 0 callback.
	
	Control transfers can be of the following type:
	0 Standard;
	1 Class;
	2 Vendor;
	3 Reserved.

	A callback can be installed for each of these control transfers using
	USBRegisterRequestHandler.
	When an OUT request arrives, data is collected in the data store provided
	with the USBRegisterRequestHandler call, up to the size registered with
	it; a request declaring more than that is stalled. When the transfer is
	done, the callback is called.
	When an IN request arrives, the callback is called immediately to either
	put the control transfer data in the data store, or to get a pointer to
	control transfer data. The data is then packetised and sent to the host.
*/

#include <string.h>

#include "type.h"
#include "debug.h"

#include "usbstruct.h"
#include "usbapi.h"
/* The breadcrumb marks below. A control transfer runs inside a USB service
 * pass, so its steps record themselves separately from the pass. */
#include "watchdog.h"



#define	MAX_CONTROL_SIZE	128	/**< maximum total size of control transfer data */
#define	MAX_REQ_HANDLERS	4	/**< standard, class, vendor, reserved */

static TSetupPacket		Setup;	/**< setup packet */

static U8				*pbData;	/**< pointer to data buffer */
static int				iResidue;	/**< remaining bytes in buffer */
static int				iLen;		/**< total length of control transfer */

/** Set when DataIn() had a packet the controller would not take, so the
    transfer still owes the host that packet. See USBControlInRetry(). */
static BOOL				fDataInOwed = FALSE;

/** Set when the reply still needs one zero-length packet to end its data
    stage: the host asked for more bytes than the device has, and what the
    device sent filled whole packets, so nothing in it told the host the data
    had ended. See DataIn(). */
static BOOL				fZeroPacketOwed = FALSE;

/** Array of installed request handler callbacks */
static TFnHandleRequest *apfnReqHandlers[4] = {NULL, NULL, NULL, NULL};
/** Array of installed request data pointers */
static U8				*apbDataStore[4] = {NULL, NULL, NULL, NULL};
/** How many bytes each of those data stores holds. Zero where no handler is
    installed, which refuses every host-to-device request of that type. */
static int				aiDataStoreSize[4] = {0, 0, 0, 0};


/** Internal function filtering Vendor requests to handle OS Descriptors */
BOOL USBFilterOsVendorMessage(TSetupPacket *pSetup, BOOL *pfSuccess, int *piLen, U8 **ppbData);



/**
	Local function to handle a request by calling one of the installed
	request handlers.
		
	In case of data going from host to device, the data is at *ppbData.
	In case of data going from device to host, the handler can either
	choose to write its data at *ppbData or update the data pointer.
		
	@param [in]		pSetup		The setup packet
	@param [in,out]	*piLen		Pointer to data length
	@param [in,out]	ppbData		Data buffer.

	@return TRUE if the request was handles successfully
 */
static BOOL _HandleRequest(TSetupPacket *pSetup, int *piLen, U8 **ppbData)
{
	/* The firmware's own handler for the request runs from here, inside the
	 * USB service pass, so it marks itself: a request that takes seconds to
	 * answer looks exactly like a USB handshake that does. */
	uint8_t site;
	BOOL fHandled;
	TFnHandleRequest *pfnHandler;
	int iType;
	
	iType = REQTYPE_GET_TYPE(pSetup->bmRequestType);
	
	if(iType == REQTYPE_TYPE_VENDOR) {
		BOOL fFilterStatus = FALSE;
		if(USBFilterOsVendorMessage(pSetup, &fFilterStatus, piLen, ppbData)) {
			return fFilterStatus;
		}
	}
	
	pfnHandler = apfnReqHandlers[iType];
	if (pfnHandler == NULL) {
		DBG("No handler for reqtype %d\n", iType);
		return FALSE;
	}

	site = watchdog_mark_inner(WATCHDOG_SITE_USB_REQUEST);
	fHandled = pfnHandler(pSetup, piLen, ppbData);
	watchdog_unmark(site);
	return fHandled;
}


/**
	Local function to stall the control endpoint
	
	@param [in]	bEPStat	Endpoint status
 */
static void StallControlPipe(U8 bEPStat)
{
	U8	*pb;
	int	i;

	/* The stall ends this transfer, so nothing is owed to the host any
	 * more: a retry after it would offer a packet of a transfer the host
	 * has been told to abandon, and a packet that was to end its data
	 * stage has no data stage left to end. */
	fDataInOwed = FALSE;
	fZeroPacketOwed = FALSE;

	USBHwEPStall(0x80, TRUE);

// dump setup packet
	DBG("STALL on [");
	pb = (U8 *)&Setup;
	for (i = 0; i < 8; i++) {
		DBG(" %02x", *pb++);
	}
	DBG("] stat=%x\n", bEPStat);
}


/**
	Reads from the control endpoint, marked as a step of the USB service pass.

	Every control request comes in through here: the setup packet, the data
	stage of a host-to-device request, and the zero-length status stage. The
	wait for the packet inside it is bounded to a few milliseconds, so a read
	that holds the board for seconds is worth being able to tell apart from
	the rest of the pass.
 */
static int ControlEPRead(U8 *pbBuf, int iMaxLen)
{
	uint8_t site = watchdog_mark_inner(WATCHDOG_SITE_USB_SETUP);
	int iRead = USBHwEPRead(0x00, pbBuf, iMaxLen);

	watchdog_unmark(site);
	return iRead;
}


/**
	Sends next chunk of data (possibly 0 bytes) to host
 */
static void DataIn(void)
{
	int iChunk;
	int iWritten;
	uint8_t site;

	iChunk = MIN(MAX_PACKET_SIZE0, iResidue);
	/*
	 * Only move the transfer on for a packet the controller actually took.
	 * A write that came up short has not been validated, so advancing here
	 * would hand the host a reply assembled out of order. Remember that the
	 * packet is still owed instead; USBControlInRetry() below offers it
	 * again.
	 */
	site = watchdog_mark_inner(WATCHDOG_SITE_USB_REPLY);
	iWritten = USBHwEPWrite(0x80, pbData, iChunk);
	watchdog_unmark(site);
	if (iWritten != iChunk) {
		fDataInOwed = TRUE;
		return;
	}
	fDataInOwed = FALSE;
	pbData += iChunk;
	iResidue -= iChunk;
	/* A zero-length packet is the one that ends the data stage, and it ends
	 * it once. */
	if (iChunk == 0)
		fZeroPacketOwed = FALSE;
}


/**
	Offers again a control IN packet the controller would not take.

	A device-to-host control request is answered from one place only: the
	endpoint 0 interrupt that says the last packet has gone to the host. So a
	packet the controller refuses ends the transfer -- the endpoint has
	nothing validated for the host to collect, the host's IN tokens are
	NAKed, no interrupt follows, and the request is never answered. The host
	gives up after its own timeout, which for the Ubertooth tools is one
	second, and `ubertooth-btle` reads each packet with a control request --
	so one refused write is one capture packet the host never gets an answer
	for, and the capture ends there. It is not what ends the captures #505
	records: those stop the board servicing USB altogether. This is a hole on
	the same path, closed because it was open.

	Calling this from the loop that services USB turns that into a retry a
	few microseconds later: a mode loop services USB thousands of times a
	second, far inside the host's timeout. It does nothing unless a packet is
	owed, so it costs one read of a variable on every other pass.
 */
void USBControlInRetry(void)
{
	if (fDataInOwed)
		DataIn();
}


/**
	Forget a control IN packet that is still owed, for a caller that knows
	the host is no longer waiting for it: a bus reset, or an endpoint 0 event
	the firmware had to skip, which is most likely a setup packet.
 */
void USBControlInRetryCancel(void)
{
	fDataInOwed = FALSE;
	fZeroPacketOwed = FALSE;
}


/**
 *	Handles IN/OUT transfers on EP0
 *
 *	@param [in]	bEP		Endpoint address
 *	@param [in]	bEPStat	Endpoint status
 */
void USBHandleControlTransfer(U8 bEP, U8 bEPStat)
{
	int iChunk, iType;

	if (bEP == 0x00) {
		// OUT transfer
		if (bEPStat & EP_STATUS_SETUP) {
			/* A setup packet starts a new transfer, so a packet the
			 * previous one still owed the host is void: the host has
			 * moved on, and offering it now would answer this request
			 * with the last one's data. */
			fDataInOwed = FALSE;
			fZeroPacketOwed = FALSE;

			// setup packet, reset request message state machine
			/*
			 * A setup packet is eight bytes, always. Anything else is not a
			 * request: the read can come back short, or report no packet at
			 * all if the endpoint stopped answering or was no longer the
			 * selected one. The return value used to be ignored, which left
			 * `Setup` holding the *previous* request and ran it again --
			 * whatever it was, including a mode change or a reset -- or
			 * decoded whatever partial bytes had landed in it. Clear the
			 * state machine and stall instead; a stalled control transfer is
			 * something the host retries.
			 */
			if (ControlEPRead((U8 *)&Setup, sizeof(Setup)) != (int)sizeof(Setup)) {
				memset(&Setup, 0, sizeof(Setup));
				pbData = NULL;
				iResidue = 0;
				iLen = 0;
				DBG("short or failed SETUP read\n");
				StallControlPipe(bEPStat);
				return;
			}
			DBG("S%x", Setup.bRequest);

			// defaults for data pointer and residue
			iType = REQTYPE_GET_TYPE(Setup.bmRequestType);
			pbData = apbDataStore[iType];
			iResidue = Setup.wLength;
			iLen = Setup.wLength;

			/*
			 * A host-to-device request's data is collected in the
			 * store its handler registered, a packet at a time, and
			 * the collection used to be bounded only by the length
			 * the host declared. wLength is sixteen bits and the
			 * largest store here holds 258 bytes, so a request
			 * declaring 65,535 wrote about 65 KiB of the host's
			 * bytes over whatever followed that store in memory --
			 * other globals, and eventually nothing mapped. No
			 * shipped tool sends such a request; any program on the
			 * bus can.
			 *
			 * Refuse it instead. A stall is what the host already
			 * expects from a request the device will not carry out,
			 * and truncating it would run a handler on a request
			 * that is not the one the host sent.
			 *
			 * Only the reply endpoint is stalled, as everywhere
			 * else in this file, so the host still sends the data
			 * stage it announced and the branch below absorbs it a
			 * packet at a time -- about a thousand of them for a
			 * request of 65,535 bytes -- before the status stage
			 * tells the host it was refused. The bytes go nowhere,
			 * which is the point; the cost is a few milliseconds of
			 * USB service that a program on the bus can ask for
			 * repeatedly.
			 *
			 * Only host-to-device. A device-to-host request is
			 * answered out of whatever its handler points pbData
			 * at, and the reply is cut to the handler's own length
			 * below, so asking for more than the store holds is
			 * both safe and ordinary: every host asks for 255 bytes
			 * of a string descriptor the eight-byte standard store
			 * never sees.
			 */
			if (Setup.wLength > 0 &&
			    REQTYPE_GET_DIR(Setup.bmRequestType) == REQTYPE_DIR_TO_DEVICE &&
			    (int)Setup.wLength > aiDataStoreSize[iType]) {
				DBG("request data longer than the store\n");
				/* Before the state is cleared, because the
				 * stall dumps the setup packet: this is the
				 * one refusal whose eight bytes are a
				 * complete request and worth reading. */
				StallControlPipe(bEPStat);
				memset(&Setup, 0, sizeof(Setup));
				pbData = NULL;
				iResidue = 0;
				iLen = 0;
				return;
			}

			if ((Setup.wLength == 0) ||
				(REQTYPE_GET_DIR(Setup.bmRequestType) == REQTYPE_DIR_TO_HOST)) {
				// ask installed handler to process request
				if (!_HandleRequest(&Setup, &iLen, &pbData)) {
					DBG("_HandleRequest1 failed\n");
					StallControlPipe(bEPStat);
					return;
				}
				/*
				 * The other half of the same bound. A handler
				 * that answers out of the store, rather than
				 * pointing pbData at a descriptor of its own,
				 * cannot have more to send than the store
				 * holds, so a length it reports above that is
				 * wrong and the reply would read past the
				 * store. No handler here does it; the clamp is
				 * free and the alternative is sending the
				 * firmware's memory to the host.
				 *
				 * A handler that redirected pbData is left
				 * alone: that is how every descriptor is
				 * answered, and the descriptor is nowhere near
				 * the store.
				 */
				if (pbData == apbDataStore[iType] &&
				    iLen > aiDataStoreSize[iType])
					iLen = aiDataStoreSize[iType];
				// send smallest of requested and offered length
				iResidue = MIN(iLen, Setup.wLength);
				/*
				 * Does the reply need a zero-length packet after it to
				 * end its data stage? Only when the host asked for more
				 * than there is to send and what there is fills whole
				 * packets -- a packet shorter than the endpoint's
				 * maximum is itself the end, and a reply as long as the
				 * host asked for ends when the host has counted its
				 * bytes.
				 *
				 * Sending one anyway, which is what this used to do on
				 * the endpoint 0 interrupt below, leaves a validated
				 * zero-length packet on endpoint 0 IN that this
				 * transfer's host never collects. The next request's
				 * IN token then collects it instead, and that request
				 * comes back with no data and no error.
				 */
				fZeroPacketOwed = iResidue < Setup.wLength &&
				                  iResidue % MAX_PACKET_SIZE0 == 0;
				// send first part (possibly a zero-length status message)
				DataIn();
			}
		}
		else {		
			if (iResidue > 0) {
				// store data
				iChunk = ControlEPRead(pbData, iResidue);
				if (iChunk < 0) {
					StallControlPipe(bEPStat);
					return;
				}
				pbData += iChunk;
				iResidue -= iChunk;
				if (iResidue == 0) {
					// received all, send data to handler
					iType = REQTYPE_GET_TYPE(Setup.bmRequestType);
					pbData = apbDataStore[iType];
					if (!_HandleRequest(&Setup, &iLen, &pbData)) {
						DBG("_HandleRequest2 failed\n");
						StallControlPipe(bEPStat);
						return;
					}
					// send status to host
					DataIn();
				}
			}
			else {
				// absorb zero-length status message
				iChunk = ControlEPRead(NULL, 0);
				DBG(iChunk > 0 ? "?" : "");
			}
		}
	}
	else if (bEP == 0x80) {
		/*
		 * IN transfer: the host has collected the last packet, so send
		 * the next one -- but only if the transfer has one left to send.
		 * A reply that is already complete owes the host nothing, and
		 * the zero-length packet this used to write regardless stayed
		 * validated on the endpoint until some later request's IN token
		 * collected it, which is that request answered with no data.
		 */
		if (iResidue > 0 || fZeroPacketOwed)
			DataIn();
	}
	else {
		ASSERT(FALSE);
	}
}


/**
	Registers a callback for handling requests
		
	@param [in]	iType			Type of request, e.g. REQTYPE_TYPE_STANDARD
	@param [in]	*pfnHandler		Callback function pointer
	@param [in]	*pbDataStore	Data storage area for this type of request
	@param [in]	iDataStoreSize	How many bytes that area holds. A
								host-to-device request declaring more data
								than this is stalled, so the collection
								cannot run past the end of the area.
 */
void USBRegisterRequestHandler(int iType, TFnHandleRequest *pfnHandler, U8 *pbDataStore, int iDataStoreSize)
{
	ASSERT(iType >= 0);
	ASSERT(iType < 4);
	ASSERT(iDataStoreSize >= 0);
	apfnReqHandlers[iType] = pfnHandler;
	apbDataStore[iType] = pbDataStore;
	aiDataStoreSize[iType] = iDataStoreSize;
}

