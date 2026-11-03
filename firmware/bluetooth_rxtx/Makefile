# Hey Emacs, this is a -*- makefile -*-

# Target file name (without extension).
TARGET = bluetooth_rxtx

# A build with the transmit half left out is a different image from the same
# target, so it says so: a board running it would otherwise read exactly like
# one running the full image. Not bluetooth_rx_only, the name the release
# gives the files, which is two bytes past what the firmware info block holds.
# ubertooth.c accepts this name alongside the full one. See IMAGE_NAME in
# ../common.mk.
ifdef DISABLE_TX
IMAGE_NAME = bluetooth_rx
endif

# List C source files here. (C dependencies are automatically generated.)
#
# usb_serial.c is here for USB_IRQHandler alone. No source in this image calls
# anything in that file, and the serial console it also holds is unused, so it
# reads like a source this image does not need. It is not: LPC17xx_Interrupts.c
# puts USB_IRQHandler in the interrupt vector table and declares the name weak
# and aliased to the default handler, so the link succeeds either way. Drop the
# file and the USB interrupt reaches that default handler, which never returns:
# the board stops answering the host and the watchdog resets it every few
# seconds.
SRC = $(TARGET).c \
	bluetooth.c \
	bluetooth_le.c \
	ubertooth_usb.c \
	ubertooth_rssi.c \
	ubertooth_cs.c \
	ubertooth_clock.c \
	ubertooth_dma.c \
	le_phy.c \
	queue.c \
	cc2400_rangetest.c \
	rx_fifo_lead.c \
	ego.c \
	debug_uart.c \
	tinyprintf.c \
	fault.c \
	xmas.c \
	identify.c \
	identify_blink.c \
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

include ../common.mk
