# Hey Emacs, this is a -*- makefile -*-

# Target file name (without extension).
TARGET = btbr

COMPILE_OPTS=-Iinclude
LINKER_OPTS=--specs=nano.specs

# List C source files here. (C dependencies are automatically generated.)
#
# usb_serial.c is here for USB_IRQHandler, which no source in this image calls
# and which it must have all the same: the interrupt vector table in
# LPC17xx_Interrupts.c is what reaches it. See the comment above the handler in
# firmware/common/usb_serial.c.
SRC = $(shell find src -name '*.c') \
	$(LIBS_PATH)/usb_serial.c \
	$(LIBS_PATH)/LPC17xx_Startup.c \
	$(LIBS_PATH)/LPC17xx_Interrupts.c \
	$(LIBS_PATH)/ubertooth.c \
	$(LIBS_PATH)/cc2400_wait.c \
	$(LIBS_PATH)/bl_report.c \
	$(LIBS_PATH)/watchdog.c \
	$(LPCUSB_PATH)/usbcontrol.c \
	$(LPCUSB_PATH)/usbinit.c \
	$(LPCUSB_PATH)/usbhw_lpc.c \
	$(LPCUSB_PATH)/usbstdreq.c

OPT = 3

include ../common.mk
