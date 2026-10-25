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
    USB hardware layer
 */

#include "type.h"
#include "debug.h"

#ifdef LPC17xx
#include "lpc17.h"
#endif
#ifdef LPC214x
#include "lpc214x.h"
#endif
#ifdef LPC23xx
#include "lpc23xx.h"
#endif
#include "usbhw_lpc.h"
#include "usbapi.h"


#ifdef DEBUG
// comment out the following line if you don't want to use debug LEDs
#define DEBUG_LED
#endif

#ifdef DEBUG_LED
#ifdef LPC17xx
#define DEBUG_LED_INIT(x)
#define DEBUG_LED_ON(x)
#define DEBUG_LED_OFF(x)
#elif LPC214x
#define DEBUG_LED_ON(x)     IOCLR0 = (1 << x);
#define DEBUG_LED_OFF(x)    IOSET0 = (1 << x);
#define DEBUG_LED_INIT(x)   PINSEL0 &= ~(0x3 << (2*x)); IODIR0 |= (1 << x); DEBUG_LED_OFF(x);
#else
#define DEBUG_LED_ON(x)     FIO2SET = (1 << (x-8));
#define DEBUG_LED_OFF(x)    FIO2CLR = (1 << (x-8));
#define DEBUG_LED_INIT(x)   SCS |= 1; PINSEL10 = 0; FIO2DIR |= (1 << (x-8)); DEBUG_LED_OFF(x);
#endif
#else
#define DEBUG_LED_INIT(x)   /**< LED initialisation macro */
#define DEBUG_LED_ON(x)     /**< turn LED on */
#define DEBUG_LED_OFF(x)    /**< turn LED off */
#endif

/** Installed device interrupt handler */
static TFnDevIntHandler *_pfnDevIntHandler = NULL;
/** Installed endpoint interrupt handlers */
static TFnEPIntHandler  *_apfnEPIntHandlers[16];
/** Installed frame interrupt handlers */
static TFnFrameHandler  *_pfnFrameHandler = NULL;
/** Whether a device status read has already been given a second chance; see
    USBHwISR(). */
static BOOL dev_stat_retried = FALSE;

/** convert from endpoint address to endpoint index */
#define EP2IDX(bEP) ((((bEP)&0xF)<<1)|(((bEP)&0x80)>>7))
/** convert from endpoint index to endpoint address */
#define IDX2EP(idx) ((((idx)<<7)&0x80)|(((idx)>>1)&0xF))


/** Number of hardware handshakes that gave up. Non-zero means the USB
    controller stopped answering at least once; see the comment on
    USBHW_POLLS. */
volatile U32 usbhw_timeouts = 0;


/**
    Local function to wait for a device interrupt (and clear it)

    Bounded. This used to be `while ((USBDevIntSt & dwIntr) != dwIntr);`, and
    it is reached from every control request and every bulk packet: the
    protocol engine is driven by command codes whose completion is reported
    only here. If that completion never arrives, the caller never returns, and
    because USB in this firmware is serviced from whatever loop is running
    (handle_usb()), a board whose firmware is stuck in here answers no further
    USB request at all. That is the state reported repeatedly from the bench:
    the board still enumerates, every control request times out, and only a
    power cycle brings it back. A bound turns it into one transfer the host
    retries.

    This handshake is also one set of registers -- USBCmdCode, USBCmdData and
    these interrupt bits -- so it cannot take a second caller: two contexts
    running a command at once would each clear the other's CCEMTY/CDFULL, and
    whichever was left waiting would be waiting for a flag no outstanding
    command will set. handle_usb() holds the USB interrupt off for that
    reason.

    @param [in] dwIntr      Bitmask of interrupts to wait for

    @return TRUE if the interrupt bits arrived, FALSE if the wait gave up
 */
static BOOL Wait4DevIntBounded(U32 dwIntr, U32 dwPolls)
{
    U32 i;

    // wait for specific interrupt
    for (i = 0; i < dwPolls; ++i) {
        if ((USBDevIntSt & dwIntr) == dwIntr) {
            // clear the interrupt bits
            USBDevIntClr = dwIntr;
            return TRUE;
        }
    }

    /*
     * Clear the bits on the way out too, so this wait is self-contained. The
     * answer may still be coming: if it lands after we have given up and the
     * bits are left set, the *next* wait for them returns at once and reads
     * whatever the previous command left in USBCmdData -- a status byte
     * belonging to another endpoint, or an endpoint realization that never
     * happened. A bound that moved the failure to the next caller would not
     * be worth much.
     */
    USBDevIntClr = dwIntr;
    ++usbhw_timeouts;
    return FALSE;
}


static BOOL Wait4DevInt(U32 dwIntr)
{
    return Wait4DevIntBounded(dwIntr, USBHW_POLLS);
}


/**
    Local function to send a command to the USB protocol engine
        
    @param [in] bCmd        Command to send
 */
static BOOL USBHwCmd(U8 bCmd)
{
    // clear CDFULL/CCEMTY
    USBDevIntClr = CDFULL | CCEMTY;
    // write command code
    USBCmdCode = 0x00000500 | (bCmd << 16);
    return Wait4DevInt(CCEMTY);
}


/**
    Local function to send a command + data to the USB protocol engine
        
    @param [in] bCmd        Command to send
    @param [in] bData       Data to send
 */
static void USBHwCmdWrite(U8 bCmd, U16 bData)
{
    // write command code
    if (!USBHwCmd(bCmd))
        return;

    // write command data
    USBCmdCode = 0x00000100 | (bData << 16);
    Wait4DevInt(CCEMTY);
}


/**
    Local function to send a command to the USB protocol engine and read data
        
    @param [in] bCmd        Command to send

    @return the data
 */
static BOOL USBHwCmdReadChecked(U8 bCmd, U8 *pbData)
{
    // write command code
    if (!USBHwCmd(bCmd))
        return FALSE;

    // get data
    USBCmdCode = 0x00000200 | (bCmd << 16);
    if (!Wait4DevInt(CDFULL))
        return FALSE;
    *pbData = USBCmdData;
    return TRUE;
}


/**
    Local function to send a command to the USB protocol engine and read data

    @param [in] bCmd        Command to send

    @return the data, or zero if the handshake gave up

    A caller that cannot act sensibly on a wrong answer should use
    USBHwCmdReadChecked() instead. Zero is a safe answer for
    USBHwEPGetStatus(), the hot caller: an endpoint that reads as neither full
    nor stalled makes handle_usb() offer it another packet, which is what it
    would have done anyway. It is not safe for a device status read, which is
    why USBHwISR() uses the checked form there.
 */
static U8 USBHwCmdRead(U8 bCmd)
{
    U8 bData = 0;

    (void)USBHwCmdReadChecked(bCmd, &bData);
    return bData;
}


/**
    'Realizes' an endpoint, meaning that buffer space is reserved for
    it. An endpoint needs to be realised before it can be used.
        
    From experiments, it appears that a USB reset causes USBReEp to
    re-initialise to 3 (= just the control endpoints).
    However, a USB bus reset does not disturb the USBMaxPSize settings.
        
    @param [in] idx         Endpoint index
    @param [in] wMaxPSize   Maximum packet size for this endpoint
 */
static void USBHwEPRealize(int idx, U16 wMaxPSize)
{
    USBReEp |= (1 << idx);
#ifdef LPC17xx
    USBEpIn = idx;
#else
    USBEpInd = idx;
#endif
    USBMaxPSize = wMaxPSize;
    /* Realizing an endpoint is the one wait here that is not a register
     * handshake -- the controller reorganises its buffer space -- so it gets
     * a bound of its own, well clear of the others. A false timeout would
     * leave an endpoint unrealized, which is a board that enumerates and does
     * nothing, so this bound exists only to stop a dead controller hanging
     * the firmware, not to catch a slow one. */
    Wait4DevIntBounded(EP_RLZED, USBHW_REALIZE_POLLS);
}


/**
    Enables or disables an endpoint
        
    @param [in] idx     Endpoint index
    @param [in] fEnable TRUE to enable, FALSE to disable
 */
static void USBHwEPEnable(int idx, BOOL fEnable)
{
    USBHwCmdWrite(CMD_EP_SET_STATUS | idx, fEnable ? 0 : EP_DA);
}


/**
    Configures an endpoint and enables it
        
    @param [in] bEP             Endpoint number
    @param [in] wMaxPacketSize  Maximum packet size for this EP
 */
void USBHwEPConfig(U8 bEP, U16 wMaxPacketSize)
{
    int idx;
    
    idx = EP2IDX(bEP);
    
    // realise EP
    USBHwEPRealize(idx, wMaxPacketSize);

    // enable EP
    USBHwEPEnable(idx, TRUE);
}


/**
    Registers an endpoint event callback
        
    @param [in] bEP             Endpoint number
    @param [in] pfnHandler      Callback function
 */
void USBHwRegisterEPIntHandler(U8 bEP, TFnEPIntHandler *pfnHandler)
{
    int idx;
    
    idx = EP2IDX(bEP);

    ASSERT(idx<32);

    /* add handler to list of EP handlers */
    _apfnEPIntHandlers[idx / 2] = pfnHandler;
    
    /* enable EP interrupt */
    USBEpIntEn |= (1 << idx);
    USBDevIntEn |= EP_SLOW;
    
    DBG("Registered handler for EP 0x%x\n", bEP);
}


/**
    Registers an device status callback
        
    @param [in] pfnHandler  Callback function
 */
void USBHwRegisterDevIntHandler(TFnDevIntHandler *pfnHandler)
{
    _pfnDevIntHandler = pfnHandler;
    
    // enable device interrupt
    USBDevIntEn |= DEV_STAT;

    DBG("Registered handler for device status\n");
}


/**
    Registers the frame callback
        
    @param [in] pfnHandler  Callback function
 */
void USBHwRegisterFrameHandler(TFnFrameHandler *pfnHandler)
{
    _pfnFrameHandler = pfnHandler;
    
    // enable device interrupt
    USBDevIntEn |= FRAME;

    DBG("Registered handler for frame\n");
}


/**
    Sets the USB address.
        
    @param [in] bAddr       Device address to set
 */
void USBHwSetAddress(U8 bAddr)
{
    USBHwCmdWrite(CMD_DEV_SET_ADDRESS, DEV_EN | bAddr);
}


/**
    Connects or disconnects from the USB bus
        
    @param [in] fConnect    If TRUE, connect, otherwise disconnect
 */
void USBHwConnect(BOOL fConnect)
{
#ifdef LPC23xx
#ifndef LPC2378_PORTB
  if(fConnect)
    FIO2CLR = (1<<9);
  else
    FIO2SET = (1<<9);
#else
  if(fConnect)
    FIO0CLR = (1<<14);
  else
    FIO0SET = (1<<14);
#endif
#endif
    USBHwCmdWrite(CMD_DEV_STATUS, fConnect ? CON : 0);
}


/**
    Enables interrupt on NAK condition
        
    For IN endpoints a NAK is generated when the host wants to read data
    from the device, but none is available in the endpoint buffer.
    For OUT endpoints a NAK is generated when the host wants to write data
    to the device, but the endpoint buffer is still full.
    
    The endpoint interrupt handlers can distinguish regular (ACK) interrupts
    from NAK interrupt by checking the bits in their bEPStatus argument.
    
    @param [in] bIntBits    Bitmap indicating which NAK interrupts to enable
 */
void USBHwNakIntEnable(U8 bIntBits)
{
    USBHwCmdWrite(CMD_DEV_SET_MODE, bIntBits);
}


/**
    Gets the status from a specific endpoint.
        
    @param [in] bEP     Endpoint number
    @return Endpoint status byte (containing EP_STATUS_xxx bits)
 */
U8  USBHwEPGetStatus(U8 bEP)
{
    int idx = EP2IDX(bEP);

    return USBHwCmdRead(CMD_EP_SELECT | idx);
}


/**
    Sets the stalled property of an endpoint
        
    @param [in] bEP     Endpoint number
    @param [in] fStall  TRUE to stall, FALSE to unstall
 */
void USBHwEPStall(U8 bEP, BOOL fStall)
{
    int idx = EP2IDX(bEP);

    USBHwCmdWrite(CMD_EP_SET_STATUS | idx, fStall ? EP_ST : 0);
}


/**
    Writes data to an endpoint buffer
        
    @param [in] bEP     Endpoint number
    @param [in] pbBuf   Endpoint data
    @param [in] iLen    Number of bytes to write
            
    @return number of bytes written into the endpoint buffer
*/
int USBHwEPWrite(U8 bEP, U8 *pbBuf, U32 iLen)
{
    U32 idx;
    U32 written;
    
    idx = EP2IDX(bEP);
    
    // set write enable for specific endpoint
    USBCtrl = WR_EN | ((bEP & 0xF) << 2);
    
    // set packet length
    USBTxPLen = iLen;
    
    /* Write data, a word at a time, until the packet is written or the
     * hardware says it has had enough.
     *
     * The loop used to be `while (USBCtrl & WR_EN)` alone, trusting the
     * controller to clear WR_EN once it had USBTxPLen bytes. A controller that
     * does not clear it -- for whatever reason it stopped answering -- walks
     * pbBuf through the whole of RAM and off the end of it: either forever, or
     * into a bus fault and the LED loop in fault.c, which stops USB just as
     * effectively. Counting the bytes we were asked to write bounds it, which
     * is what NXP's own driver for this part does; WR_EN still ends the loop
     * early as it always did. */
    for (written = 0; written < iLen; written += 4) {
        if (!(USBCtrl & WR_EN))
            break;
        USBTxData = (pbBuf[3] << 24) | (pbBuf[2] << 16) | (pbBuf[1] << 8) | pbBuf[0];
        pbBuf += 4;
    }

    USBCtrl = 0;

    /*
     * A packet the controller would not take whole is not a packet. Validating
     * the buffer anyway would send a short one, and for a control transfer a
     * short packet is how the host is told the data has ended -- so a reply
     * cut off part way would read as complete, and the rest of it would arrive
     * at the wrong offset. Leave the buffer unvalidated instead: the host gets
     * no answer for this packet and retries the transfer. The caller is told
     * how much was taken, and DataIn() uses that to leave its transfer where
     * it was.
     */
    if (written < iLen) {
        ++usbhw_timeouts;
        return (int)written;
    }

    // select endpoint and validate buffer
    USBHwCmd(CMD_EP_SELECT | idx);
    USBHwCmd(CMD_EP_VALIDATE_BUFFER);
    
    return iLen;
}


/**
    Reads data from an endpoint buffer
        
    @param [in] bEP     Endpoint number
    @param [in] pbBuf   Endpoint data
    @param [in] iMaxLen Maximum number of bytes to read
            
    @return the number of bytes available in the EP (possibly more than iMaxLen),
    or <0 in case of error.
 */
int USBHwEPRead(U8 bEP, U8 *pbBuf, U32 iMaxLen)
{
    U32 i, idx;
    U32 dwData, dwLen;
    
    idx = EP2IDX(bEP);
    
    // set read enable bit for specific endpoint
    USBCtrl = RD_EN | ((bEP & 0xF) << 2);
    
    /* Wait for PKT_RDY, with a bound.
     *
     * This is on the path of every control request: USBHandleControlTransfer()
     * reads the SETUP packet through here. The wait used to have no bound, and
     * the length it polls comes from USBRxPLen, which only answers while this
     * endpoint is the one selected in the global USBCtrl register -- so a
     * controller that stops answering, or a selection that is no longer this
     * endpoint, left this polling forever and the firmware servicing USB never
     * again. Callers have to check the result: a read that gave
     * up has not filled the buffer, and acting on what was already in it is
     * how a stale request gets run (see USBHandleControlTransfer()). */
    for (i = 0; i < USBHW_POLLS; ++i) {
        dwLen = USBRxPLen;
        if (dwLen & PKT_RDY)
            break;
    }
    if (i == USBHW_POLLS) {
        /* No CMD_EP_CLEAR_BUFFER here: there is no packet to release, and
         * claiming one that has not arrived would throw away the next. For
         * endpoint 0 that is enough -- a SETUP token resets the control
         * endpoint in hardware, and the caller stalls, so the host's next
         * setup packet starts clean. For a bulk OUT endpoint it means the
         * buffer stays unclaimed until the controller answers again, which
         * is an endpoint that NAKs rather than a firmware that hangs. Worth
         * a look in the hardware soak. */
        ++usbhw_timeouts;
        USBCtrl = 0;
        return -1;
    }
    
    // packet valid?
    if ((dwLen & DV) == 0) {
        USBCtrl = 0;
        return -1;
    }
    
    // get length
    dwLen &= PKT_LNGTH_MASK;
    
    // get data
    dwData = 0;
    for (i = 0; i < dwLen; i++) {
        if ((i % 4) == 0) {
            dwData = USBRxData;
        }
        if ((pbBuf != NULL) && (i < iMaxLen)) {
            pbBuf[i] = dwData & 0xFF;
        }
        dwData >>= 8;
    }

    // make sure RD_EN is clear
    USBCtrl = 0;

    // select endpoint and clear buffer
    USBHwCmd(CMD_EP_SELECT | idx);
    USBHwCmd(CMD_EP_CLEAR_BUFFER);
    
    return dwLen;
}

static void fast_wait() {
#ifdef __GNUC__
    asm volatile("nop\n"); 
#endif

#ifdef __arm__
    volatile uint32_t fast_wait = 1;
    while(--fast_wait);
#endif
}


int USBHwISOCEPRead(const U8 bEP, U8 *pbBuf, const U32 iMaxLen)
{
    U32 i, idx;
    U32 dwData, dwLen;

    idx = EP2IDX(bEP);

    // set read enable bit for specific endpoint
    USBCtrl = RD_EN | ((bEP & 0xF) << 2);
    
    //Note: for some reason the USB perepherial needs a cycle to set bits in USBRxPLen before 
    //reading, if you remove this ISOC wont work. This may be a but in the chip, or due to 
    //a mis-understanding of how the perepherial is supposed to work.    
    fast_wait();
    
    dwLen = USBRxPLen;
    if( (dwLen & PKT_RDY) == 0 ) {
        USBCtrl = 0;// make sure RD_EN is clear
        return(-1);
    }

    // packet valid?
    if ((dwLen & DV) == 0) {
        USBCtrl = 0;// make sure RD_EN is clear
        return -1;
    }

    // get length
    dwLen &= PKT_LNGTH_MASK;

    // get data
    dwData = 0;
    for (i = 0; i < dwLen; i++) {
        if ((i % 4) == 0) {
            dwData = USBRxData;
        }
        if ((pbBuf != NULL) && (i < iMaxLen)) {
            pbBuf[i] = dwData & 0xFF;
        }
        dwData >>= 8;
    }

    // make sure RD_EN is clear
    USBCtrl = 0;

    // select endpoint and clear buffer
    USBHwCmd(CMD_EP_SELECT | idx);
    USBHwCmd(CMD_EP_CLEAR_BUFFER);

    return dwLen;
}


/**
    Sets the 'configured' state.
        
    All registered endpoints are 'realised' and enabled, and the
    'configured' bit is set in the device status register.
        
    @param [in] fConfigured If TRUE, configure device, else unconfigure
 */
void USBHwConfigDevice(BOOL fConfigured)
{
    // set configured bit
    USBHwCmdWrite(CMD_DEV_CONFIG, fConfigured ? CONF_DEVICE : 0);
}


/**
    USB interrupt handler
        
    @todo Get all 11 bits of frame number instead of just 8

    Endpoint interrupts are mapped to the slow interrupt
 */
void USBHwISR(void)
{
    U32 dwStatus;
    U32 dwIntBit;
    U8  bEPStat, bDevStat, bStat;
    int i;
    U16 wFrame;

// LED9 monitors total time in interrupt routine
DEBUG_LED_ON(9);

    // handle device interrupts
    dwStatus = USBDevIntSt;
    
    // frame interrupt
    if (dwStatus & FRAME) {
        // clear int
        USBDevIntClr = FRAME;
        // call handler
        if (_pfnFrameHandler != NULL) {
            U8 bFrame = 0;
            /* A frame number that could not be read is not a frame number;
             * skip this one rather than report frame zero. */
            if (USBHwCmdReadChecked(CMD_DEV_READ_CUR_FRAME_NR, &bFrame)) {
                wFrame = bFrame;
                _pfnFrameHandler(wFrame);
            }
        }
    }
    
    // device status interrupt
    if (dwStatus & DEV_STAT) {
        /*  Clear DEV_STAT interrupt before reading DEV_STAT register.
            This prevents corrupted device status reads, see
            LPC2148 User manual revision 2, 25 july 2006.
        */
        USBDevIntClr = DEV_STAT;
        /*
         * The interrupt is cleared before the read, so a read that gave up
         * would lose the event -- and a lost bus reset means the stack keeps
         * an address and configuration the host has just taken away, which is
         * a board that answers nothing. Put the interrupt back instead and
         * read it on the next pass.
         *
         * Once, though. The device interrupt stays asserted while the bit is
         * set, so against an engine that has stopped answering for good this
         * would re-enter the handler for ever in the modes that service USB
         * from the interrupt. One retry recovers the transient case; after
         * that the event is dropped, and the endpoint work below still runs.
         */
        if (!USBHwCmdReadChecked(CMD_DEV_STATUS, &bDevStat)) {
            if (!dev_stat_retried) {
                dev_stat_retried = TRUE;
                USBDevIntSet = DEV_STAT;
            }
            bDevStat = 0;
        } else {
            dev_stat_retried = FALSE;
        }
        if (bDevStat & (CON_CH | SUS_CH | RST)) {
            // convert device status into something HW independent
            bStat = ((bDevStat & CON) ? DEV_STATUS_CONNECT : 0) |
                    ((bDevStat & SUS) ? DEV_STATUS_SUSPEND : 0) |
                    ((bDevStat & RST) ? DEV_STATUS_RESET : 0);
            // call handler
            if (_pfnDevIntHandler != NULL) {
DEBUG_LED_ON(8);        
                _pfnDevIntHandler(bStat);
DEBUG_LED_OFF(8);       
            }
        }
    }
    
    // endpoint interrupt
    if (dwStatus & EP_SLOW) {
        // clear EP_SLOW
        USBDevIntClr = EP_SLOW;
        // check all endpoints
        for (i = 0; i < 32; i++) {
            dwIntBit = (1 << i);
            if (USBEpIntSt & dwIntBit) {
                /* Clear CDFULL first, the way USBHwCmd() does before a
                 * command: this is the one place an answer is asked for
                 * without going through it, so a flag left over from
                 * anywhere else would make the read below return the wrong
                 * endpoint's status. */
                USBDevIntClr = CDFULL;
                // clear int (and retrieve status)
                USBEpIntClr = dwIntBit;
                /* A handshake that gave up leaves USBCmdData holding some
                 * earlier command's answer. Acting on that could call the
                 * endpoint handler with, say, the setup bit of a transfer
                 * that is long over, so skip the endpoint instead: the
                 * interrupt bit is cleared, and the host retries. */
                if (!Wait4DevInt(CDFULL))
                    continue;
                bEPStat = USBCmdData;
                // convert EP pipe stat into something HW independent
                bStat = ((bEPStat & EPSTAT_FE) ? EP_STATUS_DATA : 0) |
                        ((bEPStat & EPSTAT_ST) ? EP_STATUS_STALLED : 0) |
                        ((bEPStat & EPSTAT_STP) ? EP_STATUS_SETUP : 0) |
                        ((bEPStat & EPSTAT_EPN) ? EP_STATUS_NACKED : 0) |
                        ((bEPStat & EPSTAT_PO) ? EP_STATUS_ERROR : 0);
                // call handler
                if (_apfnEPIntHandlers[i / 2] != NULL) {
DEBUG_LED_ON(10);       
                    _apfnEPIntHandlers[i / 2](IDX2EP(i), bStat);
DEBUG_LED_OFF(10);
                }
            }
        }
    }
    
DEBUG_LED_OFF(9);       
}



/**
    Initialises the USB hardware
        
    This function assumes that the hardware is connected as shown in
    section 10.1 of the LPC2148 data sheet:
    * P0.31 controls a switch to connect a 1.5k pull-up to D+ if low.
    * P0.23 is connected to USB VCC.
    
    Embedded artists board: make sure to disconnect P0.23 LED as it
    acts as a pull-up and so prevents detection of USB disconnect.
        
    @return TRUE if the hardware was successfully initialised
 */
BOOL USBHwInit(void)
{
#ifdef LPC17xx
	// P2.9 is USB_CONNECT
    PINSEL4 = (PINSEL4 & ~(3 << 18)) | (1 << 18);

	// P1.18 is USB_UP_LED
    PINSEL3 = (PINSEL3 & ~(3 << 4)) | (1 << 4);

	// P1.30 is VBUS
    PINSEL3 = (PINSEL3 & ~(3 << 28)) | (2 << 28);

	// P0.29 is USB_D+
    PINSEL1 = (PINSEL1 & ~(3 << 26)) | (1 << 26);

	// P0.30 is USB_D-
    PINSEL1 = (PINSEL1 & ~(3 << 28)) | (1 << 28);

	// enable PUSB
	PCONP |= PCONP_PCUSB;

	/* enable the USB controller clocks */
    USBClkCtrl |= (USBClkCtrl_DEV_CLK_EN | USBClkCtrl_AHB_CLK_EN);
    while (!(USBClkSt & USBClkSt_DEV_CLK_ON));
    while (!(USBClkSt & USBClkSt_AHB_CLK_ON));
#endif

#ifdef LPC214x
    
    // configure P0.23 for Vbus sense
    PINSEL1 = (PINSEL1 & ~(3 << 14)) | (1 << 14);   // P0.23
    // configure P0.31 for CONNECT
    PINSEL1 = (PINSEL1 & ~(3 << 30)) | (2 << 30);   // P0.31

    // enable PUSB
    PCONP |= (1 << 31);     

    // initialise PLL
    PLL1CON = 1;            // enable PLL
    PLL1CFG = (1 << 5) | 3; // P = 2, M = 4
    PLL1FEED = 0xAA;
    PLL1FEED = 0x55;
    while ((PLL1STAT & (1 << 10)) == 0);

    PLL1CON = 3;            // enable and connect
    PLL1FEED = 0xAA;
    PLL1FEED = 0x55;

#endif

#ifdef LPC23xx
#ifdef LPC2378_PORTB
    PINSEL1 = (PINSEL1 & ~(3 << 30)) | (1 << 30);
    PINSEL3 = (PINSEL3 & ~(3 << 28)) | (2 << 28);
    /* Due to a bug in the LPC23xx chips, the connection functionality must be
    * simulated using GPIO. Hopefully for production this will be fixed and the
    * commented out code will work */
    //PINSEL0 = (PINSEL0 & ~((3 << 26) | (3 << 28))) | (1 << 26) | (1 << 28); /* Doesn't work due to bug in chip */
    PINSEL0 = (PINSEL0 & ~((3 << 26) | (3 << 28))) | (1 << 26);
    FIO0DIR |= (1<<14); /* Set pin to output */
    FIO0SET = (1<<14); /* Set output high to disconnect */
    
#else
    PINSEL1 = (PINSEL1 & ~((3 << 26) | (3 << 28))) | (1 << 26) | (1 << 28);
    PINSEL3 = (PINSEL3 & ~((3 << 4) | (3 << 28))) | (1 << 4) | (2 << 28);
    /* Due to a bug in the LPC23xx chips, the connection functionality must be
    * simulated using GPIO. Hopefully for production this will be fixed and the
    * commented out code will work */
    //PINSEL4 = (PINSEL4 & ~(3 << 18)) | (1 << 18); /* Doesn't work due to bug in chip */
    PINSEL4 = (PINSEL4 & ~(3 << 18)); /* Use pin as GPIO */
    FIO2DIR |= (1<<9); /* Set pin to output */
    FIO2SET = (1<<9); /* Set output high to disconnect */
#endif

    // enable PUSB
    PCONP |= (1 << 31);     

  /* The LPC23xx uses a single PLL, and has multiple clock dividers for each
   * peripheral. These settings assume a PLL frequency of 288 MHz */

    USBCLKCFG = 5; /* 288 MHz / 48 MHz = 6 */

#ifdef LPC2378_PORTB
    USBClkCtrl = (1 << 1) | (1 << 3) | (1 << 4); /* Enable the clocks */
    while (!(USBClkSt & ((1 << 1) | (1 << 3) | (1 << 4))));
    USBPortSel = 0x3; /* Set LPC to use USB Port B pins */
#else
    USBClkCtrl = (1 << 1) | (1 << 4); /* Enable the clocks */
    while (!(USBClkSt & ((1 << 1) | (1 << 4))));
#endif

#endif
    
    // disable/clear all interrupts for now
    USBDevIntEn = 0;
    USBDevIntClr = 0xFFFFFFFF;
    USBDevIntPri = 0;

    USBEpIntEn = 0;
    USBEpIntClr = 0xFFFFFFFF;
    USBEpIntPri = 0;

    // by default, only ACKs generate interrupts
    USBHwNakIntEnable(0);
    
    // init debug leds
    DEBUG_LED_INIT(8);
    DEBUG_LED_INIT(9);
    DEBUG_LED_INIT(10);

    return TRUE;
}






//FIXME check all the DMA stuff

/**
    This function is used to setup and populated the various elements of a LPC2148 DMA desccriptor such that
    after calling this function, the DMA descriptor could be used as part of a DMA tranfer.
    
        
    @param [in] dmaDescriptor    A pointer to a 4 or 5 element long array of U32's that the DMA descriptor data is to be stored into, it should point to some place in DMA RAM.
    @param [in] nextDdPtr        The value to be placed in the "Next_DD_Pointer" value of the DMA desriptor, set to NULL if there is no next-pointer.
    @param [in] isIsocFlag       Flag to indicate if this DMA descriptor is for an ISOC endpoint (with a 5 element dmaDescriptor array pointer), 1 indicates ISOC, 0 indicates non-ISOC. 
    @param [in] maxPacketSize    The maximum packet size that can be sent/received for the endpoint in question.
    @param [in] dmaLengthIsocNumFrames    For non-ISOC endpoints, the number of bytes in the buffer to be transfered, for ISOC endpoints, the number of frames to be transfered.
    @param [in] dmaBufferStartAddress    Start address for the dma transfer (location to store data for an OUT endpoint, location to pull data from for an IN endpoint), it should point to some place in DMA RAM.
    @param [in] isocPacketSizeMemoryAddress   If a non-ISOC endpoint, set this to NULL, if an ISOC endpoint, then set this to a pointer to an array of U32's that represent how

    @return  void
 */
void USBSetupDMADescriptor(
		volatile U32 dmaDescriptor[], 
		volatile U32 nextDdPtr[],
		const U8 isIsocFlag, 
		const U16 maxPacketSize, 
		const U16 dmaLengthIsocNumFrames,
		void *dmaBufferStartAddress,
		U32 *isocPacketSizeMemoryAddress ) 
{
	dmaDescriptor[1] = 0;
	dmaDescriptor[0] = (U32) nextDdPtr;
	dmaDescriptor[1] |= ((maxPacketSize & 0x3FF) << 5);//Set maxPacketSize
	dmaDescriptor[1] |= (dmaLengthIsocNumFrames << 16);//aka number of ISOC packets if in ISOC mode
	if( isIsocFlag ) {
		dmaDescriptor[1] |= (1<<4);//enable isoc type
	}
	if( nextDdPtr != NULL ) {
		dmaDescriptor[1] |= (1<<2); //mark next DD as valid
	}
	dmaDescriptor[2] = (U32) dmaBufferStartAddress;
	
	if( isIsocFlag && isocPacketSizeMemoryAddress != NULL ) {
		dmaDescriptor[4] = (U32) isocPacketSizeMemoryAddress;
	}
	dmaDescriptor[3] = 0; //mark DD as valid and reset all status bits
}

/**
    This function disables DMA for the endpoint indicated by the paramater.
        
    @param [in] bEndpointNumber the endpoint number to be disabled.

    @return   void
 */
void USBDisableDMAForEndpoint(const U8 bEndpointNumber) {
	int idx = EP2IDX(bEndpointNumber);
	USBEpDMADis = (1<<idx);
}

/**
    Enables DMA for a particular endpoint.
        
    @param [in] bEndpontNumber The endpoint to to enable DMA for.

    @return 
 */
void USBEnableDMAForEndpoint(const U8 bEndpointNumber) {
	int idx = EP2IDX(bEndpointNumber);
	USBEpDMAEn = (1<<idx);
}

/**
    This function is used to initialized an ISOC frame array
        
    @param [in|out] isocFrameArr        The array of ISOC frame descriptors to be set, it should point to some place in DMA RAM.
    @param [in] numElements             The number of elements in the isocFrameArr array.
    @param [in] startFrameNumber        Number to start at for numbering each frame.
    @param [in] defaultFrameLength      The frame length to set all the frame descriptors to.

    @return  void
 */
void USBInitializeISOCFrameArray(U32 isocFrameArr[], const U32 numElements, const U16 startFrameNumber, const U16 defaultFrameLength) {
	U16 i;
	U16 frameNumber = startFrameNumber;
	
	for(i = 0; i < numElements; i++ ) {
		isocFrameArr[i] = (frameNumber<<16) | (1<<15) | (defaultFrameLength & 0x3FF);
		frameNumber++;
	}
}

/**
    This function is used to set the DMA descriptor head for the linked list of a particular endpoint.
        
    @param [in] bEp                    The endpoint number to set the DMA linked list head for.
    @param [in|out] udcaHeadArray      The array of pointers that point to the head DD elements for each endpoint, it should point to some place in DMA RAM.
    @param [in|out] dmaDescriptorPtr   The address of the DMA descriptor that is to be the new head, it should point to some place in DMA RAM.

    @return 
 */
void USBSetHeadDDForDMA(const U8 bEp, volatile U32* udcaHeadArray[32], volatile U32 *dmaDescriptorPtr) {
	udcaHeadArray[EP2IDX(bEp)] = dmaDescriptorPtr;
}

/**
    This function is used to initialize the USB DMA controller
        
    @param [in|out] udcaHeadArray   This is a pointer to the array of DMA descriptor linked-list head pointers, it should point to some place in DMA RAM.

    @return 
 */
void USBInitializeUSBDMA(volatile U32* udcaHeadArray[32]) {
	//set following 32 pointers to be null
	int i;
	for(i = 0; i < 32; i++ ) {
		udcaHeadArray[i] = NULL;
	}
	USBUDCAH = (U32) udcaHeadArray;
}










