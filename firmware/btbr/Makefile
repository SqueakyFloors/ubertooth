# Hey Emacs, this is a -*- makefile -*-

# Target file name (without extension).
TARGET = btbr

COMPILE_OPTS=-Iinclude
LINKER_OPTS=--specs=nano.specs

# List C source files here. (C dependencies are automatically generated.)
#
# usb_serial.c is here for USB_IRQHandler alone. Nothing in this image names
# anything in the file, so do not drop it on that reading: the comment above
# USB_IRQHandler in firmware/common/usb_serial.c says what happens to a board
# whose image leaves it out.
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
