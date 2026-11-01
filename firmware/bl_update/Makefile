# Hey Emacs, this is a -*- makefile -*-

# Target file name (without extension).
TARGET = bl_update

# List C source files here. (C dependencies are automatically generated.)
#
# No radio and no USB: this image sets up the pins and the clock, programs the
# bootloader region through the boot ROM, and resets into DFU.
SRC = bl_update_main.c \
	$(LIBS_PATH)/bl_update.c \
	$(LIBS_PATH)/LPC17xx_Startup.c \
	$(LIBS_PATH)/LPC17xx_Interrupts.c \
	$(LIBS_PATH)/ubertooth.c

# The bootloader this image carries, included by the assembler.
ASRC = bl_payload.S

# Nothing above arms the watchdog, and that is deliberate: programming the
# whole bootloader region takes longer than the three seconds the
# application's watchdog allows, and a reset in the middle of that write is
# the one failure that leaves a board needing the serial programming
# interface. watchdog.c is what arms it, and the files that write the
# breadcrumb are the shared USB stack, so leaving all of them out of SRC above
# is the whole of it. reset() still works: it starts the watchdog itself, with
# the shortest timeout the counter takes, which is how every image resets.

include ../common.mk

# The bootloader binary the payload is built from. It is not in firmware/'s
# `all`, because it is the one image that needs the ARM C++ runtime, so this
# builds it on demand rather than failing on a missing file.
BOOTLOADER_BIN = ../bootloader/bootloader.bin

$(BOOTLOADER_BIN):
	$(MAKE) -C ../bootloader bin

# The assembler's generated dependency file does not name the file .incbin
# read, so say it here: a rebuilt bootloader has to rebuild the payload.
$(OBJDIR)/bl_payload.o: $(BOOTLOADER_BIN)

# The payload has to be at the offset the host reads it from, and it has to be
# the bootloader that was built beside it. Both are checked on the finished
# image: the linker script's ASSERTs cannot do it, because they have to allow
# the zero-length section every other image has. 512 is
# UBERTOOTH_BL_UPDATE_PAYLOAD_OFFSET in
# host/libubertooth/src/ubertooth_interface.h, and
# firmware/tests/test_bl_update.c checks that this number agrees with it.
bin: check-payload

.PHONY: check-payload
check-payload: $(TARGET).bin $(BOOTLOADER_BIN)
	@# wc pads its count with spaces on some systems, which dd will not take.
	@size=`wc -c < $(BOOTLOADER_BIN) | tr -d ' '`; \
	if ! dd if=$(TARGET).bin bs=1 skip=512 count=$$size 2>/dev/null | \
			cmp -s - $(BOOTLOADER_BIN); then \
		echo "$(TARGET).bin does not carry $(BOOTLOADER_BIN) at offset 0x200," \
		     "so ubertooth-dfu would read the wrong bytes as the bootloader" >&2; \
		exit 1; \
	fi; \
	echo "  payload: $(BOOTLOADER_BIN) at offset 0x200, $$size bytes"
