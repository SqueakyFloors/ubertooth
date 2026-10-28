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
# Jared Boone
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

# Target file name (without extension).
TARGET = bootloader

# Library paths
LIBS_PATH = ../common
LPCUSB_PATH = $(LIBS_PATH)/lpcusb/target

# List C source files here. (C dependencies are automatically generated.)
SRC = $(LIBS_PATH)/LPC17xx_Startup.c \
      $(LIBS_PATH)/LPC17xx_Interrupts.c \
      $(LIBS_PATH)/ubertooth.c \
      $(LPCUSB_PATH)/usbcontrol.c \
      $(LPCUSB_PATH)/usbinit.c \
      $(LPCUSB_PATH)/usbhw_lpc.c \
      $(LPCUSB_PATH)/usbstdreq.c

# List C++ source files here. (C dependencies are automatically generated.)
CPPSRC = $(TARGET).cpp \
         dfu.cpp \
         iap.cpp

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

# Where this image's build output for one source file goes. Sources under
# ../common are shared with every other image, which compiles them with its
# own flags, so their output goes under this image's own shared/ instead of
# beside the source. Same definition and same reason as in ../common.mk; this
# image has its own makefile because it is the one written in C++.
per_image = $(addprefix $(OBJDIR)/,$(patsubst ../%,shared/%,$(1)))

# Optimization level, can be [0, 1, 2, 3, s].
#     0 = turn off optimization. s = optimize for size.
#     (Note: 3 is not always the best optimization level. See libc FAQ.)
OPT = s

# Debugging format.
DEBUG = dwarf-2 -g3

# Linker Script for the current MCU
LINKER_SCRIPT = LPC17xx_Linker_Script_bootloader.ld

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
#
# UBERTOOTH_NO_WATCHDOG: this image links the shared USB stack but not
# watchdog.c, and must not write to the breadcrumb at all -- it runs after
# every reset, before the firmware has read what the last run left there. The
# marks in those files compile to nothing here; see firmware/common/watchdog.h.
CDEFS  = -D$(BOARD) -D$(LPCUSB_TARGET) -DUBERTOOTH_NO_WATCHDOG $(COMPILE_OPTS) -Wa,-a,-ad

# Place -D or -U options here for ASM sources
ADEFS =

# Place -D or -U options here for C++ sources
CPPDEFS = -D$(BOARD) -D$(LPCUSB_TARGET) $(COMPILE_OPTS)

#---------------- Compiler Options C ----------------
#  -g*:          generate debugging information
#  -O*:          optimization level
#  -f...:        tuning, see GCC manual
#  -Wall...:     warning level
#  -Wa,...:      tell GCC to pass this to the assembler.
#    -alhms...: create assembler listing
CFLAGS = -g$(DEBUG)
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
CPPFLAGS += -fno-exceptions
CPPFLAGS += -fno-rtti
CPPFLAGS += -fno-threadsafe-statics
CPPFLAGS += -Wall
CPPFLAGS += -Wextra
CPPFLAGS += -Weffc++
#CFLAGS += -Wundef
#CPPFLAGS += -mshort-calls
#CPPFLAGS += -fno-unit-at-a-time
#CPPFLAGS += -Wstrict-prototypes
#CPPFLAGS += -Wunreachable-code
#CPPFLAGS += -Wsign-compare
CPPFLAGS += -Wa,-alhms=$(call per_image,$(<:%.cpp=%.lst))
CPPFLAGS += $(patsubst %,-I%,$(EXTRAINCDIRS))
#CPPFLAGS += $(CSTANDARD)

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
LDFLAGS = -Wl,-Map=$(TARGET).map
#LDFLAGS += -Wl,--relax
#LDFLAGS += --gc-sections
LDFLAGS += $(patsubst %,-L%,$(EXTRALIBDIRS))
LDFLAGS += -static
LDFLAGS += -Wl,--start-group
#LDFLAGS += -L$(THUMB2GNULIB) -L$(THUMB2GNULIB2)
LDFLAGS += -lc -lg -lstdc++ -lsupc++
LDFLAGS += -lgcc -lm
LDFLAGS += -Wl,--end-group

# The linker script, as a path make can depend on, so that editing it relinks
# the image instead of leaving the previous one in place. The link passes the
# script by name (-T) and the linker finds it in one of the -L directories, so
# the same search is run here to get a path a rule can depend on. Same
# mechanism and same reason as in ../common.mk, which explains it; this image
# has its own makefile because it is the one written in C++.
LINKER_SCRIPT_DIRS = . $(EXTRALIBDIRS)
find_linker_script = $(firstword $(wildcard $(addsuffix /$(1),$(LINKER_SCRIPT_DIRS))))
LINKER_SCRIPT_FOUND := $(call find_linker_script,$(LINKER_SCRIPT))
# /dev/null leads awk's file list so that there is always a file to read, even
# when the script was not found; otherwise awk would wait on standard input.
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

# Define Messages
# English
MSG_BEGIN = -------- begin --------
MSG_END = --------  end  --------
MSG_SIZE_BEFORE = Size before:
MSG_SIZE_AFTER = Size after:
MSG_FLASH = Creating load file for Flash:
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

# Define all listing files.
LST = $(call per_image,$(SRC:%.c=%.lst) $(CPPSRC:%.cpp=%.lst) $(ASRC:%.S=%.lst))

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

# Read the dependency files back, so editing a header rebuilds what includes
# it. The compiler writes each one beside its object, so the paths come from
# per_image too.
#
# This include comes AFTER `all:` on purpose, as the one in ../common.mk does:
# the first target make reads becomes the default goal, and a dependency file
# starts with an object's rule. Included above, plain `make` would build one
# object and stop.
DEPFILES = $(call per_image,$(SRC:%.c=%.d) $(CPPSRC:%.cpp=%.d))
-include $(DEPFILES)

# Compile-flag tracking: the flags are recorded in a file that is rewritten
# only when they change, and every object depends on it, so editing a flag
# rebuilds the objects instead of leaving an image built with the old ones.
# Same mechanism and same reason as in ../common.mk, which explains it; this
# image has its own makefile because it is the one written in C++.
#
# Snapshotted with `:=` outside a recipe, so this has to stay below the last
# line that sets a flag.
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
	@# Unchanged: leave the file alone so nothing rebuilds. Otherwise stamp
	@# it into the next whole second, because this make compares mtimes to
	@# the whole second and the objects beside it have to be invalidated.
	@# Only a build with a record to compare against says anything.
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

# A real prerequisite, not an order-only one: an order-only dependency would
# create the file if it were missing and otherwise ignore its timestamp, and
# the timestamp is the whole point.
$(OBJ): $(BUILD_FLAGS_FILE)

# Change the build target to build a HEX file or a library.
build: elf hex bin srec lss sym
#build: lib

elf: $(TARGET).elf
hex: $(TARGET).hex
bin: $(TARGET).bin
srec: $(TARGET).srec
eep: $(TARGET).eep
lss: $(TARGET).lss
sym: $(TARGET).sym
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

%.bin: %.elf
	@echo
	@echo $(MSG_FLASH) $@
	$(OBJCOPY) -O binary $< $(TARGET).bin

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

# Create library from object files.
.SECONDARY : $(TARGET).a
.PRECIOUS : $(OBJ)
%.a: $(OBJ)
	@echo
	@echo $(MSG_CREATING_LIBRARY) $@
	$(AR) $@ $(OBJ)

# Link: create ELF output file from object files.
#
# The linker script files are prerequisites as well, so the command lists the
# objects itself instead of using $^, which would hand the scripts to gcc as
# if they were objects too.
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
	@# always has, and it is what made another image wrong.
	$(REMOVE) $(SRC:.c=.o) $(SRC:.c=.lst) $(CPPSRC:.cpp=.o) $(CPPSRC:.cpp=.lst)
	$(REMOVE) InvalidEvents.tmp
	$(REMOVE) $(TARGET).bin
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
