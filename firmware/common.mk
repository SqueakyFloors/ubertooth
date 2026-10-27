# Hey Emacs, this is a -*- makefile -*-
#----------------------------------------------------------------------------
# WinAVR Makefile Template written by Eric B. Weddington, Jörg Wunsch, et al.
#  >> Modified for use with the LUFA project. <<
#
# Released to the Public Domain
#
# Altered for ARM Cortex-M3 by Opendous Inc. - 2010-01-05
# Altered for Project Ubertooth by Michael Ossmann 2010
#
# Additional material for this makefile was written by:
# Peter Fleury
# Tim Henigan
# Colin O'Flynn
# Reiner Patommel
# Markus Pfaff
# Sander Pool
# Frederik Rouleau
# Carlos Lamas
# Dean Camera
# Denver Gingerich
# Opendous Inc.
# Michael Ossmann
#
#----------------------------------------------------------------------------
# On command line:
#
# make all = Make software.
#
# make clean = Clean out built project files.
#
# make program = Download the hex file to the device
#
# make doxygen = Generate DoxyGen documentation for the project (must have
#                DoxyGen installed)
#
# make filename.s = Just compile filename.c into the assembler code only.
#
# make filename.i = Create a preprocessed source file for use in submitting
#                   bug reports to the GCC project.
#
# To rebuild project do "make clean" then "make all".
#----------------------------------------------------------------------------

# set to UBERTOOTH_ZERO or UBERTOOTH_ONE
BOARD  ?= UBERTOOTH_ONE

UBERTOOTH_OPTS = -D$(BOARD)

ifeq ($(DISABLE_TX), )
	# comment to disable RF transmission
	UBERTOOTH_OPTS += -DTX_ENABLE
endif

DIRTY = $(shell git status -s --untracked-files=no)
ifneq ($(DIRTY), )
	DIRTY_FLAG = *
endif

# The firmware version and compile info (GIT_REVISION, COMPILE_BY/HOST,
# TIMESTAMP) are provided by a generated version.h, not -D flags -- see the
# "Version identity tracking" section below. DIRTY_FLAG (set above) feeds the
# revision string generated there.

# CPU architecture
CPU = cortex-m3

# Instruction Set to use (also known as CPU Mode)
CPU_MODE = mthumb

# Additional CPU flags
CPU_FLAGS = -mapcs-frame
CPU_FLAGS_ASM = -mthumb-interwork

# Target definition for lpcusb library
LPCUSB_TARGET = LPC17xx

# Output format. (can be srec, ihex, binary)
FORMAT = ihex

# Output Directory
OUTDIR = .

# Library paths
LIBS_PATH = ../common
LPCUSB_PATH = $(LIBS_PATH)/lpcusb/target

# List Assembler source files here.
#     Make them always end in a capital .S.  Files ending in a lowercase .s
#     will not be considered source files but generated files (assembler
#     output from the compiler), and will be deleted upon "make clean"!
#     Even though the DOS/Win* filesystem matches both .s and .S the same,
#     it will preserve the spelling of the filenames, and gcc itself does
#     care about how the name is spelled on its command-line.
ASRC =

# Object files directory
#     To put object files in current directory, use a dot (.), do NOT make
#     this an empty or blank macro!
OBJDIR = .

# Where this image's build output for one source file goes.
#
# Sources in the image's own directory compile beside themselves, under
# $(OBJDIR). Sources above it -- $(LIBS_PATH) is ../common, which every image
# shares -- must not, or ../common/ubertooth.c would compile to
# ../common/ubertooth.o, in the directory the next image built in this tree
# also looks in. make compares timestamps, not compiler flags, so that image
# would take the object as up to date and link it, even though the images
# compile the shared code differently (btbr at -O3, the others at -Os).
#
# Prefixing with shared/ keeps every output under the image's own directory:
#   bluetooth.c           ->  ./bluetooth.o
#   ../common/ubertooth.c ->  ./shared/common/ubertooth.o
# Objects, listings and depfiles all go through here, so they stay together.
#
# shared/ is reserved for build output: clean removes the whole tree, so no
# image may keep sources in a directory of that name.
#
# It strips one leading ../, which is as far up as any image reaches today. A
# source two levels up would keep its second ../ and land outside the image
# directory, so one of those needs another line here.
per_image = $(addprefix $(OBJDIR)/,$(patsubst ../%,shared/%,$(1)))

# Optimization level, can be [0, 1, 2, 3, s].
#     0 = turn off optimization. s = optimize for size.
#     (Note: 3 is not always the best optimization level. See libc FAQ.)
ifeq ($(OPT),)
OPT = s
endif

# Debugging format.
DEBUG = dwarf-2 -g3

# Linker Script for the current MCU
LINKER_SCRIPT ?= LPC17xx_Linker_Script_with_bootloader.ld

# List any extra directories to look for include files here.
#     Each directory must be seperated by a space.
#     Use forward slashes for directory separators.
#     For a directory that has spaces, enclose it in quotes.
EXTRAINCDIRS = $(LIBS_PATH) $(LPCUSB_PATH) ../../host/libubertooth/src

# Compiler flag to set the C Standard level.
#     c89   = "ANSI" C
#     gnu89 = c89 plus GCC extensions
#     c99   = ISO C99 standard (not yet fully implemented)
#     gnu99 = c99 plus GCC extensions
CSTANDARD = -std=gnu99

# Place -D or -U options here for C sources
CDEFS  = -D$(LPCUSB_TARGET) $(UBERTOOTH_OPTS) $(COMPILE_OPTS) -Wa,-a,-ad

# Place -D or -U options here for ASM sources
ADEFS =

# Place -D or -U options here for C++ sources
CPPDEFS = -D$(BOARD) -D$(LPCUSB_TARGET) $(COMPILE_OPTS)

#---------------- Compiler Options C ----------------
#  -g:          generate debugging information
#  -O*:          optimization level
#  -f...:        tuning, see GCC manual
#  -Wall...:     warning level
#  -Wa,...:      tell GCC to pass this to the assembler.
#    -alhms...: create assembler listing
CFLAGS = -g
CFLAGS += $(CDEFS)
CFLAGS += -O$(OPT)
CFLAGS += -Wall
CFLAGS += -Wno-unused
CFLAGS += -Wno-comments
CFLAGS += -fmessage-length=0
CFLAGS += -fno-builtin
CFLAGS += -ffunction-sections
CFLAGS += -Wextra
CFLAGS += -D__thumb2__=1
CFLAGS += -msoft-float
CFLAGS += -mno-sched-prolog
CFLAGS += -fno-hosted
CFLAGS += -mtune=cortex-m3
CFLAGS += -march=armv7-m
CFLAGS += -mfix-cortex-m3-ldrd
#CFLAGS += -Wundef
#CFLAGS += -funsigned-char
#CFLAGS += -funsigned-bitfields
#CFLAGS += -fno-inline-small-functions
#CFLAGS += -fpack-struct
#CFLAGS += -fshort-enums
#CFLAGS += -Wstrict-prototypes
#CFLAGS += -fno-unit-at-a-time
#CFLAGS += -Wunreachable-code
#CFLAGS += -Wsign-compare
CFLAGS += -Wa,-alhms=$(call per_image,$(<:%.c=%.lst))
CFLAGS += $(patsubst %,-I%,$(EXTRAINCDIRS))
CFLAGS += $(CSTANDARD)
# GIT_REVISION/COMPILE_*/TIMESTAMP now come from the generated version.h, not
# -D flags -- see "Version identity tracking" below.

#---------------- Compiler Options C++ ----------------
#  -g*:          generate debugging information
#  -O*:          optimization level
#  -f...:        tuning, see GCC manual
#  -Wall...:     warning level
#  -Wa,...:      tell GCC to pass this to the assembler.
#    -alhms...: create assembler listing
CPPFLAGS = -g$(DEBUG)
CPPFLAGS += $(CPPDEFS)
CPPFLAGS += -O$(OPT)
#CPPFLAGS += -funsigned-char
#CPPFLAGS += -funsigned-bitfields
#CPPFLAGS += -fpack-struct
#CPPFLAGS += -fshort-enums
#CPPFLAGS += -fno-exceptions
CPPFLAGS += -Wall
#CFLAGS += -Wundef
#CPPFLAGS += -mshort-calls
#CPPFLAGS += -fno-unit-at-a-time
#CPPFLAGS += -Wstrict-prototypes
#CPPFLAGS += -Wunreachable-code
#CPPFLAGS += -Wsign-compare
CPPFLAGS += -Wa,-alhms=$(call per_image,$(<:%.cpp=%.lst))
CPPFLAGS += $(patsubst %,-I%,$(EXTRAINCDIRS))
#CPPFLAGS += $(CSTANDARD)
CPPFLAGS += -fno-exceptions
CPPFLAGS += -fno-rtti
CPPFLAGS += -fno-threadsafe-statics
CPPFLAGS += -Wextra
CPPFLAGS += -Weffc++

#---------------- Assembler Options ----------------
#  -Wa,...:   tell GCC to pass this to the assembler.
#  -alhms:   create listing
#  -gstabs:   have the assembler create line number information
#  -listing-cont-lines: Sets the maximum number of continuation lines of hex
#       dump that will be displayed for a given single line of source input.
ASFLAGS = -g$(DEBUG) $(ADEFS) -I.  -alhms=$(call per_image,$(<:%.S=%.lst))

#---------------- Library Options ----------------

# List any extra directories to look for libraries here.
#     Each directory must be seperated by a space.
#     Use forward slashes for directory separators.
#     For a directory that has spaces, enclose it in quotes.
EXTRALIBDIRS = $(LIBS_PATH)

#---------------- Linker Options ----------------
#  -Wl,...:     tell GCC to pass this to linker.
#    -Map:      create map file
#    --cref:    add cross reference to  map file
LDFLAGS = -Wl,-Map=$(TARGET).map $(LINKER_OPTS)
#LDFLAGS += -Wl,--relax
LDFLAGS += -Wl,--gc-sections
LDFLAGS += $(patsubst %,-L%,$(EXTRALIBDIRS))
LDFLAGS += -static
LDFLAGS += -Wl,--start-group
#LDFLAGS += -L$(THUMB2GNULIB) -L$(THUMB2GNULIB2)
LDFLAGS += -lc -lg
LDFLAGS += -lgcc -lm
LDFLAGS += -Wl,--end-group

# The linker script, as a path make can depend on.
#
# The link passes the script by name (-T) and leaves the linker to find it:
# the linker looks in the directory the link runs in first, then in each -L
# directory, which here is $(EXTRALIBDIRS) -- ../common, where the scripts
# live. So $(LINKER_SCRIPT) on its own is a name, not a path, and a rule
# cannot depend on it. Running the same search here turns it into a path, and
# the link rule below takes that as a prerequisite. Without one, editing a
# memory map or a section layout compiles nothing, links nothing and leaves
# the previous image in place.
#
# Each script pulls in one more file with an INCLUDE line, which the linker
# resolves through the same search, so those files are prerequisites too.
# They are read out of the script rather than named here, so adding an
# INCLUDE needs no change in this file. Only the script's own INCLUDE lines
# are read, not those of the files it includes; no script in the tree nests
# them further.
#
# Finding nothing means no searched directory holds the file. The prerequisite
# list is then short by that file, and for a name the linker cannot find either
# the link fails with the linker's own message about the missing script, which
# says more than make could about a missing prerequisite. A $(LINKER_SCRIPT)
# written as a path rather than a name would be the one case that links fine
# with nothing tracked, so keep naming the script and let the search find it.
LINKER_SCRIPT_DIRS = . $(EXTRALIBDIRS)
find_linker_script = $(firstword $(wildcard $(addsuffix /$(1),$(LINKER_SCRIPT_DIRS))))
LINKER_SCRIPT_FOUND := $(call find_linker_script,$(LINKER_SCRIPT))
# /dev/null leads awk's file list so that there is always a file to read: with
# the script not found the list would otherwise be empty, and awk would wait
# on standard input while make sat there expanding a variable.
LINKER_SCRIPT_INCLUDED := $(shell awk '$$1 == "INCLUDE" { print $$2 }' /dev/null $(LINKER_SCRIPT_FOUND))
LINKER_SCRIPT_FILES := $(LINKER_SCRIPT_FOUND) \
	$(foreach f,$(LINKER_SCRIPT_INCLUDED),$(call find_linker_script,$(f)))

#---------------- Programming Options ----------------
LPCISP  ?= $(shell which lpc21isp)
PROGDEV ?= /dev/ttyUSB0

#============================================================================

# Define programs and commands.
CROSS_COMPILE ?= arm-none-eabi-
CC = $(CROSS_COMPILE)gcc
LD = $(CROSS_COMPILE)gcc -T
AS = $(CROSS_COMPILE)as
OBJCOPY = $(CROSS_COMPILE)objcopy
OBJDUMP = $(CROSS_COMPILE)objdump
READELF  = $(CROSS_COMPILE)readelf
SIZE = $(CROSS_COMPILE)size
AR = $(CROSS_COMPILE)ar -r
NM = $(CROSS_COMPILE)nm
REMOVE = rm -f

# DFU tool should be ubertooth-dfu
DFU_TOOL ?= ubertooth-dfu

# Define Messages
# English
MSG_BEGIN = -------- begin --------
MSG_END = --------  end  --------
MSG_SIZE_BEFORE = Size before:
MSG_SIZE_AFTER = Size after:
MSG_FLASH = Creating load file for Flash:
MSG_DFU = Creating DFU firmware file:
MSG_EEPROM = Creating load file for EEPROM:
MSG_EXTENDED_LISTING = Creating Extended Listing:
MSG_SYMBOL_TABLE = Creating Symbol Table:
MSG_LINKING = Linking:
MSG_COMPILING = Compiling C:
MSG_COMPILING_CPP = Compiling C++:
MSG_ASSEMBLING = Assembling:
MSG_CLEANING = Cleaning project:
MSG_CREATING_LIBRARY = Creating library:

# Define all object files.
OBJ = $(call per_image,$(SRC:%.c=%.o) $(CPPSRC:%.cpp=%.o) $(ASRC:%.S=%.o))

# The firmware info block (../common/fw_info.c), which every image carries so
# that ubertooth-dfu can name the image and revision a file holds. It is added
# here rather than to each image's SRC so that an image cannot leave it out.
# Like every shared source it builds into this image's own object, which
# matters more here than elsewhere: the block bakes in FW_IMAGE_NAME, so one
# object shared between images would claim the wrong image in all but one.
FW_INFO_SRC = $(LIBS_PATH)/fw_info.c
FW_INFO_OBJ = $(call per_image,$(FW_INFO_SRC:%.c=%.o))
OBJ += $(FW_INFO_OBJ)

# Define all listing files.
LST = $(call per_image,$(SRC:%.c=%.lst) $(CPPSRC:%.cpp=%.lst) $(ASRC:%.S=%.lst)) \
	$(FW_INFO_OBJ:%.o=%.lst)

# Compiler flags to generate dependency files.
GENDEPFLAGS = -MMD -MP -MD

# Combine all necessary flags and optional flags.
# Add target processor to flags.
ALL_CFLAGS = -mcpu=$(CPU) -$(CPU_MODE) $(CPU_FLAGS) -I. $(CFLAGS) $(GENDEPFLAGS)
ALL_CPPFLAGS = -mcpu=$(CPU) -$(CPU_MODE) $(CPU_FLAGS) -I. -x c++ $(CPPFLAGS) $(GENDEPFLAGS)
ALL_ASFLAGS = -mcpu=$(CPU) -$(CPU_MODE) $(CPU_FLAGS_ASM) $(ASFLAGS)
# only difference between Linker flags and CFLAGS is CPU_FLAGS_ASM as -mapcs-frame is not needed
ALL_CFLAGS_LINKER = -mcpu=$(CPU) -$(CPU_MODE) $(CPU_FLAGS_ASM)

# Default target.
all: begin gccversion sizebefore build showtarget sizeafter end

#----------------------------------------------------------------------------
# Rebuild correctness: header deps and compile-flag deps.
#
# These are declared AFTER `all:` so that `all` remains the default goal.
#
# 1. Header dependencies. The compile rules pass $(GENDEPFLAGS) (-MMD -MP),
#    which makes GCC emit a `.d` file of the headers each object depends on.
#    Those files are only useful if we include them, so do that here. Now
#    editing a header rebuilds exactly the objects that include it.
#
# 2. Compile-flag dependencies. make decides what to rebuild from file
#    timestamps, so a changed command line is invisible to it: edit OPT, or
#    DISABLE_TX, or BOARD, and every object still looks up to date. make would
#    link the objects compiled with the old flags into an image the makefile no
#    longer describes, and say nothing. The flags are therefore recorded in a
#    file that is rewritten only when they change, and every object depends on
#    that file -- see "Compile-flag tracking" below. (The firmware version
#    string is not a flag: it comes from a generated header, so the depfiles of
#    1 carry it -- see "Version identity tracking".)
DEPFILES = $(call per_image,$(SRC:%.c=%.d) $(CPPSRC:%.cpp=%.d)) \
	$(FW_INFO_OBJ:%.o=%.d)
-include $(DEPFILES)

# Version identity tracking.
#
# The firmware version string (-v / -V) must match the git revision it was
# built from. We generate a header, version.h, containing the revision and the
# compile info, and the version-bearing sources #include it. Because version.h
# is a real compiler input, make's normal file-dependency tracking (via the
# -MMD depfiles -include'd above) rebuilds exactly the objects that include it,
# and only when the header actually changes -- no stale version strings, no
# full-project rebuilds.
#
# The header is regenerated every build but only REWRITTEN when its contents
# change (write to a temp file, cmp, replace), so its mtime -- and therefore a
# recompile -- is triggered exactly on a real change. GIT_REVISION carries an
# optional "*" dirty flag.
#
# version.h is generated (listed in .gitignore), not committed; a source
# distribution that ships it keeps the revision available without a .git dir.
#
# Plain shell + cmp only, so this works on GNU make 3.81 (macOS); no $(file ...)
# (that needs make >= 4.0).
VERSION_HEADER := $(OBJDIR)/version.h
VERSION_IDFILE := $(OBJDIR)/version.id
# Is this a real git checkout? A distribution archive (GitHub "Download ZIP",
# a release tarball, git-archive output) has no .git, so `git log` returns
# nothing. In that case we must NOT regenerate version.h -- we rely on the one
# shipped in the archive (see the `dist` target) -- and if none exists we fall
# back to a clearly-marked "unknown" rather than blanking the version.
HAVE_GIT := $(shell git rev-parse --is-inside-work-tree 2>/dev/null)
ifeq ($(HAVE_GIT),true)
VERSION_NOW := git-$(shell git log --pretty=format:'%h' -n 1 2>/dev/null)$(DIRTY_FLAG)
else
VERSION_NOW := git-unknown
endif
VERSION_BY := $(shell whoami)
VERSION_HOST := $(shell hostname -s)
# The build identity: revision (incl. the "*" dirty flag) + who/where built it.
# The header is only regenerated -- and the TIMESTAMP refreshed -- when THIS
# string changes, so the recorded timestamp reflects the actual build of the
# flashed firmware. Preserving the timestamp otherwise matters for A->B->A
# switches: returning to A must keep A's original build time, not re-stamp it.
# The identity is stored verbatim in a sidecar file (version.id) and compared
# exactly, rather than parsed back out of the header. The image name is part of
# it because the header carries that too, and the firmware info block is built
# from it.
VERSION_ID := $(VERSION_NOW)|$(VERSION_BY)|$(VERSION_HOST)|$(TARGET)

.PHONY: force-version-check
$(VERSION_HEADER): force-version-check
	@mkdir -p $(OBJDIR)
	@# With git: regenerate (fresh TIMESTAMP) when the identity changes OR the
	@# header is missing; otherwise leave it (and its timestamp) untouched so
	@# dependent objects are not recompiled.
	@# Without git (distribution archive): keep an existing shipped version.h as
	@# the authoritative source and never overwrite it; only synthesize a
	@# git-unknown header if none was shipped.
	@if [ "$(HAVE_GIT)" = "true" ]; then \
		if [ "`cat $(VERSION_IDFILE) 2>/dev/null`" != "$(VERSION_ID)" ] || [ ! -f $(VERSION_HEADER) ]; then \
			printf '%s' "$(VERSION_ID)" > $(VERSION_IDFILE); \
			{ \
				echo '/* Generated by common.mk -- do not edit, not committed. */'; \
				echo '#ifndef UBERTOOTH_VERSION_H'; \
				echo '#define UBERTOOTH_VERSION_H'; \
				echo '#define GIT_REVISION "$(VERSION_NOW)"'; \
				echo '#define COMPILE_BY "$(VERSION_BY)"'; \
				echo '#define COMPILE_HOST "$(VERSION_HOST)"'; \
				echo '#define FW_IMAGE_NAME "$(TARGET)"'; \
				echo '#define TIMESTAMP "'"`date`"'"'; \
				echo '#endif'; \
			} > $(VERSION_HEADER); \
			echo "  version.h -> $(VERSION_NOW) (built by $(VERSION_BY)@$(VERSION_HOST))"; \
		fi; \
	elif [ ! -f $(VERSION_HEADER) ]; then \
		{ \
			echo '/* Generated by common.mk (no git, no shipped version.h). */'; \
			echo '#ifndef UBERTOOTH_VERSION_H'; \
			echo '#define UBERTOOTH_VERSION_H'; \
			echo '#define GIT_REVISION "git-unknown"'; \
			echo '#define COMPILE_BY "$(VERSION_BY)"'; \
			echo '#define COMPILE_HOST "$(VERSION_HOST)"'; \
			echo '#define FW_IMAGE_NAME "$(TARGET)"'; \
			echo '#define TIMESTAMP "'"`date`"'"'; \
			echo '#endif'; \
		} > $(VERSION_HEADER); \
		echo "  version.h -> git-unknown (no git, no shipped header)"; \
	else \
		echo "  version.h: using shipped header (no git)"; \
	fi

# Make the header available on the include path and ensure it exists before any
# compile (order-only: its own recipe handles change detection, so it must not
# force rebuilds by mtime here). Objects that #include it pick up real changes
# through the -MMD depfiles.
CFLAGS += -I$(OBJDIR)
$(OBJ): | $(VERSION_HEADER)

# Compile-flag tracking.
#
# The flags that decide what the compiler and the linker produce are written
# to a file, and every object depends on that file. The file is rewritten only
# when its contents change (write a temp file, cmp, replace), so its timestamp
# moves on a real flag change and on nothing else: change OPT and the objects
# are rebuilt, change nothing and make still has nothing to do.
#
# The link flags are recorded in the same file, so changing a linker option,
# or which linker script is named, recompiles as well as relinks. That is more
# than is strictly needed, but it is correct and those change rarely.
#
# What is recorded is what reaches a compile or a link command line, and no
# more. Two things outside that still want `make clean`: the compiler itself,
# since a new arm-none-eabi-gcc with the same flags leaves the objects alone;
# and the objcopy options that turn the .elf into a .hex, .bin or .srec, since
# those run after the link and their outputs do not depend on this file
# either. The contents of the linker script are not among them: what is
# recorded here is the script's name, but the link rule depends on the file --
# see "Linker Options" above.
#
# A timestamp is a coarse signal, and on macOS a coarser one than it looks.
# make rebuilds a target when a prerequisite is newer, and the make here (GNU
# make 3.81, as shipped by Apple) compares mtimes to the whole second: a
# prerequisite written 0.8 s after its target, inside the same second, counts
# as not newer. So writing this file is not enough on its own. If the previous
# build's objects landed in the second this file is rewritten in, make would
# call them up to date and the stale image this whole mechanism exists to
# prevent would come back -- rarely, and only for a build started within a
# second of the last one, which is exactly what a script does.
#
# Hence the `sleep 1` below: whenever the record is written, it is stamped into
# the next whole second, so it is unambiguously newer than anything the
# previous build wrote. That costs a second on a build that was about to
# recompile everything anyway, and nothing at all on the builds in between,
# which leave the file alone and never reach it.
#
# (Every other mtime dependency here, headers included, has the same
# whole-second coarseness; this is the one place the makefile can fix it,
# because it is the one place that knows a change happened.)
#
# The flags are snapshotted here with `:=`, so this has to stay below the last
# line in this file that sets one. Snapshotting outside a recipe also leaves $<
# empty, which keeps the per-source assembler listing path -- an output name,
# not a flag -- out of the recorded text.
#
# Plain shell + cmp only, so this works on GNU make 3.81 (macOS); no
# $(file ...) (that needs make >= 4.0).
BUILD_FLAGS_FILE := $(OBJDIR)/build-flags
BUILD_FLAGS_C := $(ALL_CFLAGS)
BUILD_FLAGS_CPP := $(ALL_CPPFLAGS)
BUILD_FLAGS_AS := $(ALL_ASFLAGS)
BUILD_FLAGS_LINK := $(LINKER_SCRIPT) $(ALL_CFLAGS_LINKER) $(LDFLAGS)

.PHONY: force-flags-check
$(BUILD_FLAGS_FILE): force-flags-check
	@mkdir -p $(OBJDIR)
	@# One setting per line, so a diff of this file names what changed.
	@printf '%s\n' \
		'CC=$(CC)' \
		'CFLAGS=$(BUILD_FLAGS_C)' \
		'CPPFLAGS=$(BUILD_FLAGS_CPP)' \
		'AS=$(AS)' \
		'ASFLAGS=$(BUILD_FLAGS_AS)' \
		'LD=$(LD)' \
		'LINK=$(BUILD_FLAGS_LINK)' \
		> $(BUILD_FLAGS_FILE).tmp
	@# Unchanged: leave the file alone, so its timestamp does not move and
	@# nothing rebuilds. Otherwise the record is new or different, and the
	@# objects beside it -- if any; a tree can keep objects and lose the
	@# record -- have to be invalidated, so stamp it into the next whole
	@# second. Only a build that had something to say about a change says
	@# it, which is what keeps a first build quiet.
	@if cmp -s $(BUILD_FLAGS_FILE).tmp $(BUILD_FLAGS_FILE); then \
		rm -f $(BUILD_FLAGS_FILE).tmp; \
	else \
		if [ -f $(BUILD_FLAGS_FILE) ]; then \
			echo "  build flags changed -- rebuilding objects"; \
		fi; \
		mv $(BUILD_FLAGS_FILE).tmp $(BUILD_FLAGS_FILE); \
		sleep 1; \
		touch $(BUILD_FLAGS_FILE); \
	fi

# A real prerequisite, not an order-only one: the whole point is that this
# file's timestamp invalidates the objects.
$(OBJ): $(BUILD_FLAGS_FILE)
#----------------------------------------------------------------------------

# Change the build target to build a HEX file or a library.
build: elf hex bin srec lss sym dfu
#build: lib

elf: $(TARGET).elf
hex: $(TARGET).hex
bin: $(TARGET).bin
srec: $(TARGET).srec
eep: $(TARGET).eep
lss: $(TARGET).lss
sym: $(TARGET).sym
dfu: $(TARGET).dfu
LIBNAME=lib$(TARGET).a
lib: $(LIBNAME)

# Eye candy.
begin:
	@echo
	@echo $(MSG_BEGIN)

end:
	@echo $(MSG_END)
	@echo

# Display size of file.
ELFSIZE = $(SIZE) $(FORMAT_FLAG) $(TARGET).elf

sizebefore:
	@if test -f $(TARGET).elf; then echo; echo $(MSG_SIZE_BEFORE); $(ELFSIZE); \
	2>/dev/null; echo; fi

sizeafter:
	@if test -f $(TARGET).elf; then echo; echo $(MSG_SIZE_AFTER); $(ELFSIZE); \
	2>/dev/null; echo; fi

showtarget:
	@echo
	@echo --------- Target Information ---------
	@echo ARM Model: $(CPU)
	@echo Board:     $(BOARD)
	@echo --------------------------------------

# Display compiler version information.
gccversion:
	@$(CC) --version

program: $(TARGET).hex
	$(LPCISP) -control $(TARGET).hex $(PROGDEV)  230400 4000

# Create final output files (.hex, .eep) from ELF output file.
%.hex: %.elf
	@echo
	@echo $(MSG_FLASH) $@
	$(OBJCOPY) -O $(FORMAT) $< $@

# The firmware info block has to be in the finished image: ubertooth-dfu
# refuses a file without one, so an image that lost it cannot be flashed at
# all. The ASSERTs in ../common/sections.ld cannot catch that on their own,
# because they have to allow a zero-length block (the bootloader shares the
# script and carries none), so check the bytes at the offset here. 256 is
# UBERTOOTH_FW_INFO_OFFSET and UBTFINFO is UBERTOOTH_FW_INFO_MAGIC, both from
# host/libubertooth/src/ubertooth_interface.h; firmware/tests/test_fw_info.c
# checks that these two agree with it.
%.bin: %.elf
	@echo
	@echo $(MSG_FLASH) $@
	$(OBJCOPY) -O binary $< $(TARGET).bin
	@magic=`dd if=$(TARGET).bin bs=1 skip=256 count=8 2>/dev/null`; \
	if [ "$$magic" != "UBTFINFO" ]; then \
		echo "$(TARGET).bin carries no firmware info block at offset 0x100," \
		     "so ubertooth-dfu would refuse it" >&2; \
		exit 1; \
	fi

%.srec: %.elf
	@echo
	@echo $(MSG_FLASH) $@
	$(OBJCOPY) -O srec $< $(TARGET).srec

%.eep: %.elf
	@echo
	@echo $(MSG_EEPROM) $@
	-$(OBJCOPY) -j .eeprom --set-section-flags=.eeprom="alloc,load" \
	--change-section-lma .eeprom=0 --no-change-warnings -O $(FORMAT) $< $@ || exit 0

# Create extended listing file from ELF output file.
%.lss: %.elf
	@echo
	@echo $(MSG_EXTENDED_LISTING) $@
	$(OBJDUMP) -h -z -S $< > $@

# Create a symbol table from ELF output file.
%.sym: %.elf
	@echo
	@echo $(MSG_SYMBOL_TABLE) $@
	$(NM) -n $< > $@

# Create a DFU file with checksum and 16 byte suffix
%.dfu: %.bin
	@echo
	@echo $(MSG_DFU) $@
	$(DFU_TOOL) -s $(TARGET).bin

# Create library from object files.
.SECONDARY : $(TARGET).a
.PRECIOUS : $(OBJ)
%.a: $(OBJ)
	@echo
	@echo $(MSG_CREATING_LIBRARY) $@
	$(AR) $@ $(OBJ)

# Link: create ELF output file from object files.
#
# The linker script files are prerequisites as well, so that editing one
# relinks the image -- see "Linker Options" above. The command therefore lists
# the objects itself instead of using $^, which would hand the scripts to gcc
# as if they were objects too.
.SECONDARY : $(TARGET).elf
.PRECIOUS : $(OBJ)
%.elf: $(OBJ) $(LINKER_SCRIPT_FILES)
	@echo
	@echo $(MSG_LINKING) $@
	$(LD) $(LINKER_SCRIPT) $(ALL_CFLAGS_LINKER) $(LDFLAGS) -o $@ $(OBJ)

# The compile commands, written once.
#
# Each kind of source needs two rules: one for a source in this directory, one
# for a source above it, whose output $(per_image) puts under shared/. The two
# differ only in the paths they match, so the commands live here and both
# rules run them. mkdir is for the shared/ case, which has directories to
# create; for the other it is a no-op on $(OBJDIR), which already exists.
define compile_c
@echo
@echo $(MSG_COMPILING) $<
@mkdir -p $(@D)
$(CC) -c $(ALL_CFLAGS) $< -o $@
endef

define compile_cpp
@echo
@echo $(MSG_COMPILING_CPP) $<
@mkdir -p $(@D)
$(CC) -c $(ALL_CPPFLAGS) $< -o $@
endef

define assemble
@echo
@echo $(MSG_ASSEMBLING) $<
@mkdir -p $(@D)
$(AS) $< $(ALL_ASFLAGS) --MD $@.d -o $@
endef

# Compile: create object files from C source files.
$(OBJDIR)/%.o : %.c
	$(compile_c)

$(OBJDIR)/shared/%.o : ../%.c
	$(compile_c)

# Compile: create object files from C++ source files.
$(OBJDIR)/%.o : %.cpp
	$(compile_cpp)

$(OBJDIR)/shared/%.o : ../%.cpp
	$(compile_cpp)

# Compile: create assembler files from C source files.
%.s : %.c
	$(AS) -S $(ALL_CFLAGS) $< -o $@

# Compile: create assembler files from C++ source files.
%.s : %.cpp
	$(CC) -S $(ALL_CPPFLAGS) $< -o $@

# Assemble: create object files from assembler source files.
$(OBJDIR)/%.o : %.S
	$(assemble)

$(OBJDIR)/shared/%.o : ../%.S
	$(assemble)

# Create preprocessed source for use in sending a bug report.
%.i : %.c
	$(CC) -E -mmcu=$(CPU) -I. $(CFLAGS) $< -o $@

# Target: clean project.
clean: begin clean_list clean_binary end

clean_binary:
	$(REMOVE) $(TARGET).hex

clean_list:
	@echo $(MSG_CLEANING)
	$(REMOVE) $(TARGET).eep
	$(REMOVE) $(TARGET)eep.hex
	$(REMOVE) $(TARGET).cof
	$(REMOVE) $(TARGET).elf
	$(REMOVE) $(TARGET).map
	$(REMOVE) $(TARGET).sym
	$(REMOVE) $(TARGET).lss
	@# Every object, listing and depfile this image builds, taken from the
	@# same lists the build uses, so clean cannot drift from it.
	$(REMOVE) $(OBJ)
	$(REMOVE) $(LST)
	$(REMOVE) $(call per_image,$(ASRC:%.S=%.o.d))
	$(REMOVE) $(DEPFILES)
	$(REMOVE) $(VERSION_HEADER) $(VERSION_IDFILE)
	$(REMOVE) $(BUILD_FLAGS_FILE) $(BUILD_FLAGS_FILE).tmp
	@# The shared/ tree the lines above emptied. addsuffix yields nothing at
	@# all if OBJDIR is somehow blank, rather than a path at the root.
	rm -rf $(addsuffix /shared,$(OBJDIR))
	$(REMOVE) *.o.d
	$(REMOVE) $(SRC:.c=.s)
	$(REMOVE) $(SRC:.c=.d)
	$(REMOVE) $(CPPSRC:.cpp=.d)
	$(REMOVE) $(SRC:.c=.i)
	@# Output left in ../common by a build from before the per-image layout.
	@# Nothing reads it now, but leaving it would remove less than clean
	@# always has, and it is what made the second image wrong.
	$(REMOVE) $(SRC:.c=.o) $(SRC:.c=.lst) $(CPPSRC:.cpp=.o) $(CPPSRC:.cpp=.lst)
	$(REMOVE) InvalidEvents.tmp
	$(REMOVE) $(TARGET).bin
	$(REMOVE) $(TARGET).dfu
	$(REMOVE) $(TARGET).srec

doxygen:
	@echo Generating Project Documentation...
	@doxygen Doxygen.conf
	@echo Documentation Generation Complete.

clean_doxygen:
	rm -rf Documentation

# Create object files directory
$(shell mkdir $(OBJDIR)/ 2>/dev/null)

# Listing of phony targets.
.PHONY : all showtarget begin end sizebefore sizeafter \
gccversion build elf hex eep lss sym program clean \
clean_list clean_binary doxygen

include ../ctags.mk

