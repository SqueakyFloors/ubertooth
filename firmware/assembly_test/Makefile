# Hey Emacs, this is a -*- makefile -*-

# Target file name (without extension).
TARGET = assembly_test

# List C source files here. (C dependencies are automatically generated.)
#
# usb_serial.c is here for USB_IRQHandler alone, as in the shipped images. This
# one serves USB from its own loop with the USB interrupt disabled, so the
# handler cannot run as the image stands; it is kept so that uncommenting the
# one line in assembly_test.c that enables the interrupt does not wedge the
# board. See the comment above USB_IRQHandler in firmware/common/usb_serial.c.
SRC = $(TARGET).c \
	$(LIBS_PATH)/cc2400_wait.c \
	$(LIBS_PATH)/usb_serial.c \
	$(LIBS_PATH)/LPC17xx_Startup.c \
	$(LIBS_PATH)/LPC17xx_Interrupts.c \
	$(LIBS_PATH)/ubertooth.c \
	$(LIBS_PATH)/watchdog.c \
	$(LPCUSB_PATH)/usbcontrol.c \
	$(LPCUSB_PATH)/usbinit.c \
	$(LPCUSB_PATH)/usbhw_lpc.c \
	$(LPCUSB_PATH)/usbstdreq.c

# No memory protection region at the bottom of this image's stack: a
# development image with no main loop to arm the watchdog, so nothing here
# would report a fault, and it hands the board back to the on-chip programmer
# itself. See firmware/common/LPC17xx_Startup.c.
COMPILE_OPTS += -DUBERTOOTH_NO_STACK_GUARD

# The assembly_test firmware should be installed via ISP or JTAG without a
# bootloader present.  After the test succeeds, install the bootloader with ISP
# or JTAG.
LINKER_SCRIPT = LPC17xx_Linker_Script_without_bootloader.ld

include ../common.mk
