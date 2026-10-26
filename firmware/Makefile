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

all: $(DIRS)

# The bootloader, which is not in DIRS: it is the one image written in C++, so
# `make` here would need the ARM C++ runtime on every machine that builds
# firmware, and it can only be installed over ISP or JTAG anyway.
#
# Built from clean, and the objects it shares with the other images are removed
# afterwards. Every image writes its objects next to the sources, so the ones
# under common/ are shared, and the bootloader is the only image with its own
# compiler flags: without this it would link another image's objects, or leave
# its own for another image to link. Build it with this target rather than by
# hand, which is also what CI and tools/ubertooth-release.sh do.
bootloader:
	$(MAKE) -C bootloader clean
	$(MAKE) -C bootloader elf hex bin sym
	$(MAKE) -C bootloader clean_shared

bootloader_clean:
	$(MAKE) -C bootloader clean

clean: $(DIRS:%=%_clean) bootloader_clean test_clean

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

.PHONY : clean all test test_clean bootloader bootloader_clean $(DIRS)
