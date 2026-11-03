# Hey Emacs, this is a -*- makefile -*-

# Target file name (without extension).
TARGET = btbr

COMPILE_OPTS=-Iinclude
LINKER_OPTS=--specs=nano.specs

# List C source files here. (C dependencies are automatically generated.)
#
# usb_serial.c is in the list for USB_IRQHandler, which the interrupt vector
# table points at. Nothing here names it, and nothing here uses the serial
# console in the same file, but dropping the file sends the USB interrupt to
# the default handler, which spins forever.
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
