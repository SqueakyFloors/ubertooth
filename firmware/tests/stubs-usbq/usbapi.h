/*
 * Host-test stand-in for firmware/common/lpcusb/target/usbapi.h.
 *
 * ubertooth_usb.c is the firmware's packet queue plus the USB descriptors and
 * the glue that hands a packet to the bulk IN endpoint. The queue is pure
 * logic and runs here; the endpoint is reached only through the handful of
 * calls below, so the test supplies its own and watches what the queue does
 * to the endpoint.
 *
 * The real header pulls in LPC17xx types and the whole lpcusb stack. Only the
 * declarations ubertooth_usb.c uses are repeated here, with the same
 * signatures, so a change to one of them stops compiling rather than drifting
 * from what the firmware calls. usbhw_lpc.h is register #defines and
 * prototypes with no hardware dependency, so the real one is used as is.
 */
#ifndef UBERTOOTH_FW_TESTS_STUB_USBAPI_H
#define UBERTOOTH_FW_TESTS_STUB_USBAPI_H

#include <stdint.h>

typedef uint8_t U8;
typedef uint16_t U16;
typedef uint32_t U32;
typedef int BOOL;

#define TRUE 1
#define FALSE 0

/* Descriptor tags and the control-request type, as the firmware's descriptor
 * table and request registration use them. */
#define DESC_DEVICE        1
#define DESC_CONFIGURATION 2
#define DESC_STRING        3
#define DESC_INTERFACE     4
#define DESC_ENDPOINT      5

#define MAX_PACKET_SIZE0   64
#define REQTYPE_TYPE_VENDOR 2

/* The direction bit of a control request, which the vendor-request bridge
 * reads to tell a request that carries a body from one that does not. */
#define REQTYPE_GET_DIR(x)    (((x) >> 7) & 0x01)
#define REQTYPE_DIR_TO_DEVICE 0
#define REQTYPE_DIR_TO_HOST   1

/* Only the fields ubertooth_usb.c's vendor-request bridge reads. */
typedef struct {
	U8  bmRequestType;
	U8  bRequest;
	U16 wValue;
	U16 wIndex;
	U16 wLength;
} TSetupPacket;

typedef BOOL (TFnHandleRequest)(TSetupPacket *pSetup, int *piLen, U8 **ppbData);

void USBInit(void);
void USBRegisterDescriptors(const U8 *pabDescriptors);
void USBRegisterRequestHandler(int iType, TFnHandleRequest *pfnHandler,
                               U8 *pbDataStore);
void USBRegisterWinusbInterface(U8 bRequest, const char *pcGUID);
void USBHwConnect(BOOL fConnect);
void USBHwISR(void);
int  USBHwEPWrite(U8 bEP, U8 *pbBuf, U32 iLen);
void USBHwEPStall(U8 bEP, BOOL fStall);

#endif /* UBERTOOTH_FW_TESTS_STUB_USBAPI_H */
