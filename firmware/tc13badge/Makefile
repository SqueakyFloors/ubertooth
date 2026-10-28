# Copyright (C) 2011 Michael Ossmann <mike@ossmann.com>
# Copyright (C) 2010 DJ Delorie <dj@redhat.com>

# This file is free software; you can redistribute it and/or modify it under
# the terms of the GNU General Public License as published by the Free
# Software Foundation; either version 3, or (at your option) any later
# version.
#
# This file is distributed in the hope that it will be useful, but WITHOUT ANY
# WARRANTY; without even the implied warranty of MERCHANTABILITY or
# FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
# for more details.
#
# You should have received a copy of the GNU General Public License
# along with this file; see the file COPYING3.  If not see
# <http://www.gnu.org/licenses/>.

CC = m32c-elf-gcc
LD = m32c-elf-gcc -nostartfiles
AR = m32c-elf-ar

# Default program to flash, and port to flash it to.
elf = toorcon
port=ttyUSB0

all : r8c2l.h toorcon.elf

# List of objects common to all R8C programs.
BSP_FILES = \
	ivects.o

bsp.a : $(BSP_FILES)
	$(AR) rvs bsp.a $(BSP_FILES)

# The linker script is a prerequisite, so editing it relinks. The objects are
# then named rather than taken from $^, because $^ would hand the script to the
# compiler as another object file.
#
# For toorcon.elf, the one target anything builds, what is named is exactly
# what $^ gave before -- bsp.a twice over included, once among the inputs and
# once as the archive after -o. Linking an archive twice costs nothing, and
# keeping the command line as it was is what makes this change only about the
# prerequisite. For tc13badge.elf, which this pattern also matches and nothing
# asks for, $* is tc13badge, so tc13badge.o is named twice where $^ gave it
# once; linking an object twice would not work, which is one more reason not to
# ask for that target.
%.elf : init.o %.o bsp.a tc13badge.o R5F212L4.ld
	$(LD) init.o $*.o bsp.a tc13badge.o -o $@ bsp.a -TR5F212L4.ld

.PHONY: flash
flash : ${elf}.elf
	uflash -p /dev/${port} -3 -r -b 38400 ${elf}.elf

# The headers are prerequisites for the same reason the linker script is one
# above: without them, editing a header rebuilds nothing and the image keeps
# whatever the previous compile put in it. Named rather than generated from the
# compiler's own dependency output, because this makefile has no depfile
# machinery.
#
# It is one list for both sources this pattern builds, so it is the four they
# include between them: tc13badge.o does not need bitmaps.h and gets rebuilt
# when it changes. One needless compile of one file is cheaper than two rules.
#
# The recipe compiles one source, its own. It used to name tc13badge.c as well,
# so building either object wrote both of them. That was invisible while
# nothing could make the two stale at the same time -- editing a header
# rebuilt nothing, which is the bug above, and editing one source left the
# other object alone -- and is a race as soon as something can: a header edit
# now puts both out of date, and `make -j` runs two compiles that write
# tc13badge.o at once. Each object is compiled from the same source with the
# same flags as before, by the rule for itself.
#
# r8c2l.h is in the list and is generated, by the rule below; `all` asks for it
# first, and naming it here is what makes an edit to r8c2l.io reach the
# objects.
%.o : %.c tc13badge.h cc2400.h bitmaps.h r8c2l.h
	$(CC) -Os -c $*.c

# The assembler sources are built by make's own `%.o : %.S` rule, which knows
# nothing of what they include, so the one generated file they read is named
# here. init.S reads r8c2l.inc for the register names it writes at startup;
# without this, editing r8c2l.io rebuilds the C objects and leaves init.o as it
# was, which is the hazard above once more.
init.o : r8c2l.inc

# One run of mkports writes both files, and this says so: a pattern rule with
# several targets is one recipe that make holds responsible for all of them.
#
# It used to be written as `r8c2l.h r8c2l.inc : ...`, an explicit rule with two
# targets, which make reads instead as two rules that happen to share a recipe.
# With nothing depending on either file that never came up; now that the
# objects above do, it would mean `make -j` starting mkports twice over, each
# run rewriting both files while a compile might be reading them, and either
# file missing on its own would be rebuilt by a run that was told to write it
# but was not asked for it.
%.h %.inc : %.io mkports
	./mkports $< $*.h $*.inc

.PRECIOUS: %.o %.elf

clean :
	-rm -f *~ *.o bsp.a *.elf r8c2l.h r8c2l.inc
