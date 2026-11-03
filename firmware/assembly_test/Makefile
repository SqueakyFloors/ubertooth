# Hey Emacs, this is a -*- makefile -*-

# Target file name (without extension).
TARGET = assembly_test

# List C source files here. (C dependencies are automatically generated.)
#
# usb_serial.c provides USB_IRQHandler, which the interrupt vector table in
# LPC17xx_Interrupts.c reaches; see the comment above the handler. This image
# cannot reach it: USB is driven from its own loop, and enabling the interrupt
# would take more than uncommenting the one line below that does it, because a
# second context driving the controller is what usb_irq.h exists to prevent.
# Do not drop the file on that reading all the same. It costs 96 bytes of text
# here and removing it changes this image, which has to be run on a board to
# say so; nobody has needed those bytes.
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
