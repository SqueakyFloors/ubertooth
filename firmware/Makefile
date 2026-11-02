#-*- makefile -*-
#
# Copyright 2010 Michael Ossmann
#
# This file is part of Project Ubertooth.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2, or (at your option)
# any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; see the file COPYING.  If not, write to
# the Free Software Foundation, Inc., 51 Franklin Street,
# Boston, MA 02110-1301, USA.

DIRS = blinky bluetooth_rxtx cc2400_test clock_test usb_test assembly_test btbr

# btbr's PHY refills the radio's transmit FIFO from the GPIO interrupt on the
# radio's GIO6 pin, and the part has that interrupt on ports 0 and 2 only
# (LPC176x/5x user manual UM10360, section 9.5). GIO6 is on port 1 on the
# boards below, so the image cannot be built for them at all -- btbr's rf.c
# says so with an #error -- and `all` leaves it out rather than fail for the
# images that do build. Building btbr directly still gives that message.
#
# GIO6_HAS_INTERRUPT in firmware/common/ubertooth.h is where the answer lives,
# and make cannot read it, so this list has to track it. It names the boards
# that cannot have the interrupt, not the one that can, so a board added later
# stays in `all` and gets the #error if its GIO6 is on the wrong port, instead
# of being dropped from `all` without anyone noticing.
BOARDS_WITHOUT_GIO6_INTERRUPT = UBERTOOTH_ZERO TC13BADGE
ALL_DIRS = $(DIRS)
ifneq ($(filter $(BOARD),$(BOARDS_WITHOUT_GIO6_INTERRUPT)),)
ALL_DIRS := $(filter-out btbr,$(ALL_DIRS))
endif

# The note belongs to this target and nothing else: it says what `all` did not
# build, so it must not appear when the goal was `clean`, or `btbr` itself.
all: $(ALL_DIRS)
ifneq ($(filter $(BOARD),$(BOARDS_WITHOUT_GIO6_INTERRUPT)),)
	@echo "note: btbr was left out for $(BOARD): its PHY needs a GPIO interrupt on the radio's GIO6 pin, and that pin is on a port without one on this board" >&2
endif

# The bootloader, which is not in DIRS: it is the one image written in C++, so
# `make` here would need the ARM C++ runtime on every machine that builds
# firmware, and it can only be installed over ISP or JTAG anyway. CI and
# tools/ubertooth-release.sh build it through this target.
bootloader:
	$(MAKE) -C bootloader elf hex bin sym

bootloader_clean:
	$(MAKE) -C bootloader clean

# The bootloader updater, which is not in DIRS either: it carries the
# bootloader's binary, so building it builds the bootloader, and that is the
# one image needing the ARM C++ runtime. Its own makefile builds the
# bootloader on demand.
bl_update:
	$(MAKE) -C bl_update bin sym

bl_update_clean:
	$(MAKE) -C bl_update clean

clean: $(DIRS:%=%_clean) bootloader_clean bl_update_clean test_clean

# Host-side unit tests for pure-C firmware logic (no ARM toolchain / hardware).
# Compiled and run with the host compiler; see tests/.
test:
	$(MAKE) -C tests test

test_clean:
	$(MAKE) -C tests clean

ctags: $(DIRS:%=%_ctags)

ctags_clean: $(DIRS:%=%_ctags_clean)

$(DIRS):
	$(MAKE) -C $@

$(DIRS:%=%_clean):
	$(MAKE) -C $(@:%_clean=%) clean

$(DIRS:%=%_ctags):
	@$(MAKE) --no-print-directory -C $(@:%_ctags=%) ctags

$(DIRS:%=%_ctags_clean):
	@$(MAKE) -C $(@:%_ctags_clean=%) ctags_clean

.PHONY : clean all test test_clean bootloader bootloader_clean bl_update \
bl_update_clean $(DIRS)
