# Hey Emacs, this is a -*- makefile -*-

# Target file name (without extension).
TARGET = bluetooth_rxtx

# What a DISABLE_TX build of this target calls itself: the release ships it as
# bluetooth_rx_only.*, which is two bytes past what the firmware info block
# holds. See IMAGE_NAME in ../common.mk.
RX_ONLY_NAME = bluetooth_rx

# List C source files here. (C dependencies are automatically generated.)
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
	$(LIBS_PATH)/serial_fifo.c \
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
