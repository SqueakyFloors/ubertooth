/*
 * Copyright 2010, 2011 Michael Ossmann
 *
 * This file is part of Project Ubertooth.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */
/*
 * Linker script for Cortex-M3
 *
 * Version:CodeSourcery Sourcery G++ Lite 2007q3-53
 * BugURL:https://support.codesourcery.com/GNUToolchain/
 *
 *  Copyright 2007 CodeSourcery.
 *
 * The authors hereby grant permission to use, copy, modify, distribute,
 * and license this software and its documentation for any purpose, provided
 * that existing copyright notices are retained in all copies and that this
 * notice is included verbatim in any distributions. No written agreement,
 * license, or royalty fee is required for any of the authorized uses.
 * Modifications to this software may be copyrighted by their authors
 * and need not follow the licensing terms described here, provided that
 * the new terms are clearly indicated on the first page of each file where
 * they apply.
 */

EXTERN(Reset_Handler)
ENTRY(Reset_Handler)

SECTIONS
{
	.text :
	{
		. = ALIGN(0x400); /* Ensure that vector table is aligned as hardware requires. */
		_interrupt_vector_table = .;
		KEEP(*(.irq_handler_table))
		_interrupt_vector_table_end = .;

		/*
		 * Two blocks at fixed offsets from the start of the image, so
		 * that ubertooth-dfu can read and write them without
		 * searching: the image check block
		 * (UBERTOOTH_IMAGE_CHECK_OFFSET, 0xe0, 32 bytes) and the
		 * firmware info block (UBERTOOTH_FW_INFO_OFFSET, 0x100, 64
		 * bytes), both in
		 * host/libubertooth/src/ubertooth_interface.h, which
		 * firmware/tests/test_fw_info.c reads and puts against these
		 * numbers.
		 *
		 * Both offsets clear the vector table, including the checksum
		 * word at 0x1c that a bootloader from this tree reads over it;
		 * the ASSERTs after this block are what catch a vector table
		 * that has grown that far, and a block that is the wrong size.
		 * The image check block has to stay inside the image's first
		 * 256 bytes, which the host writes in one program along with
		 * the vector table: ubertooth_interface.h says why.
		 *
		 * MAX() rather than a plain assignment, because a location
		 * counter cannot move backwards: a vector table past the
		 * offset would fail the link with the linker's own message
		 * about the counter, before any ASSERT ran. Taking the later
		 * of the two addresses lets the link reach the ASSERTs, which
		 * name the problem.
		 *
		 * The bootloader shares this script and carries neither block.
		 * It puts its own version block at the second of the two
		 * offsets instead -- see below -- so the same 0x100 serves
		 * both, and that image leaves 0xe0 - 0xcc = 20 bytes of zeros
		 * where the image check block would go.
		 */
		. = MAX(., _interrupt_vector_table + 0xe0);
		_image_check_start = .;
		KEEP(*(.image_check))
		_image_check_end = .;

		. = MAX(., _interrupt_vector_table + 0x100);
		_fw_info_start = .;
		KEEP(*(.fw_info))
		_fw_info_end = .;

		/*
		 * The bootloader version block, at the same offset as the
		 * firmware info block. The two share it because no image
		 * carries both: an application has a firmware info block and
		 * no version block, the bootloader the other way round. A
		 * reader takes the bytes at the offset and tells them apart by
		 * their magic, so there is one offset to agree on rather than
		 * two, and neither image spends flash on a hole it does not
		 * fill. The ASSERTs after this block are what hold that: an
		 * image carrying both would fail the link rather than quietly
		 * push one block somewhere the host does not look.
		 *
		 * UBERTOOTH_BL_VERSION_OFFSET and UBERTOOTH_BL_VERSION_LENGTH
		 * (48) are in host/libubertooth/src/ubertooth_interface.h,
		 * which firmware/tests/test_bl_version.c reads and puts
		 * against these numbers.
		 */
		_bl_version_start = .;
		KEEP(*(.bl_version))
		_bl_version_end = .;

		/*
		 * The bootloader the updater image carries, at its own fixed
		 * offset above the blocks before it, so ubertooth-dfu reads
		 * the bootloader out of the image file without searching it.
		 * UBERTOOTH_BL_UPDATE_PAYLOAD_OFFSET (0x200) and
		 * UBERTOOTH_BL_UPDATE_PAYLOAD_LENGTH (16 KiB) are in
		 * host/libubertooth/src/ubertooth_interface.h, which
		 * firmware/tests/test_bl_update.c reads and puts against
		 * these numbers.
		 *
		 * Only firmware/bl_update has such a section; every other
		 * image leaves it empty and its code starts where it always
		 * did, because the location counter only moves to the offset
		 * when there is something to put there. The ASSERTs below
		 * allow a zero-length section for that reason.
		 */
		. = DEFINED(bl_payload) ? MAX(., _interrupt_vector_table + 0x200) : .;
		_bl_payload_start = .;
		KEEP(*(.bl_payload))
		_bl_payload_end = .;

		*(.text .text.* .gnu.linkonce.t.*)
		*(.rodata .rodata.* .gnu.linkonce.r.*)

		*(.eh_frame_hdr)
		*(.eh_frame)

		. = ALIGN(4);
		KEEP(*(.init))

		. = ALIGN(4);
		__preinit_array_start = .;
		KEEP (*(.preinit_array))
		__preinit_array_end = .;

		. = ALIGN(4);
		__init_array_start = .;
		KEEP (*(SORT(.init_array.*)))
		KEEP (*(.init_array))
		__init_array_end = .;

		. = ALIGN(0x4);
		KEEP (*crtbegin.o(.ctors))
		KEEP (*(EXCLUDE_FILE (*crtend.o) .ctors))
		KEEP (*(SORT(.ctors.*)))
		KEEP (*crtend.o(.ctors))

		. = ALIGN(4);
		KEEP(*(.fini))

		. = ALIGN(4);
		__fini_array_start = .;
		KEEP (*(.fini_array))
		KEEP (*(SORT(.fini_array.*)))
		__fini_array_end = .;

		KEEP (*crtbegin.o(.dtors))
		KEEP (*(EXCLUDE_FILE (*crtend.o) .dtors))
		KEEP (*(SORT(.dtors.*)))
		KEEP (*crtend.o(.dtors))

		. = ALIGN(8);
		_etext = .;

	} > rom


	.ARM.extab : 
	{
		*(.ARM.extab* .gnu.linkonce.armextab.*)
	} > rom

	__exidx_start = .;
	.ARM.exidx :
	{
		*(.ARM.exidx* .gnu.linkonce.armexidx.*)
	} > rom
	__exidx_end = .;

	_etext = .;

	.persistent ORIGIN(ram) (NOLOAD):
	{
		_persistent_start = .;
		. += 32;
		_persistent_end = .;
	} > ram
	bootloader_ctrl = _persistent_start + 0x1C;
	/* The watchdog breadcrumb (firmware/common/watchdog.c) takes the rest of
	 * the region, below bootloader_ctrl. Nothing initialises either, which is
	 * how they survive a reset. */
	watchdog_crumb = _persistent_start;

	.data :
	{
		_data = .;
		*(vtable)
		*(.data*)
		_edata = .;
	} > ram AT>rom

	/* zero initialized data */
	.bss :
	{
		_bss = .;
		__bss_start__ = .;
		*(.bss*)
		*(COMMON)
		_ebss = .;
		__bss_end__ = .;
	} > ram

	/* Where we put the heap with cr_clib. It has at most a few bytes before
	 * the stack's guard, which starts on the next 32-byte boundary. Neither
	 * image that arms a guard links an _sbrk, so a malloc() in either would
	 * not even link. */
	.cr_heap :
	{
		end = .;
		__end__ = .;
		_pvHeapStart = .;
	} > ram

	/* The end of everything allocated. The stack does not start here: the
	 * guard below comes first. */
	_stack_limit = ALIGN(., 4);

	/* The guard: 32 bytes the memory protection unit is told to refuse, so
	 * that a stack walking off the bottom faults there instead of writing
	 * over the end of .bss. The bootloader leaves the unit alone, for the
	 * reason in LPC17xx_Startup.c, and the room is reserved either way.
	 *
	 * A region has to be aligned to its own size, so the bottom of the
	 * usable stack is the first 32-byte boundary above everything allocated,
	 * plus the guard itself.
	 *
	 * Reset_Handler fills the room between _stack_bottom and the stack
	 * pointer with a known word at boot, and UBERTOOTH_GET_WATCHDOG reports
	 * how much of it has ever been used; see firmware/common/stack_check.h. */
	_stack_guard = ALIGN(_stack_limit, 32);
	_stack_bottom = _stack_guard + 32;

	/* Leave room above stack for IAP to run */
	_StackTop = ORIGIN(ram) + LENGTH(ram) - 32;

	/*
	 * The part's second RAM bank, 16 KB at 0x2007C000, which nothing else
	 * on this board uses: the USB controller needs it only when it is
	 * driven by DMA, and this firmware drives it from the processor. The
	 * large capture and packet buffers live here, so that main SRAM is left
	 * for the stack. Put one there with the AHB_SRAM macro in
	 * firmware/common/ubertooth.h.
	 *
	 * NOLOAD because nothing here is copied from flash: Reset_Handler
	 * zeroes the region the way it zeroes .bss, so only objects that start
	 * at zero belong in it.
	 */
	.ahb_sram (NOLOAD) :
	{
		_ahb_sram = .;
		*(.ahb_sram*)
		. = ALIGN(4);
		_ahb_sram_end = .;
	} > ahb_ram

}

/* Up to four interrupt handlers can nest on the stack -- the two timers, the
 * capture transfer and USB. The guard above catches an overflow that walks
 * into it, but a single frame larger than 32 bytes can step over the guard and
 * land on .bss, so the link still fails rather than shipping an image with no
 * room.
 *
 * This is a floor on what the link leaves, not a safe depth. The deepest any
 * mode has been measured using is under 500 bytes, so 2 KB is several times
 * the worst case seen; ubertooth-util -w is what says whether an image still
 * has room. */
/* The part accepts a protection region only at a multiple of its own size, and
 * this is the address the link really chose rather than the arithmetic above. A
 * misaligned region is simply not in force, which nothing at run time would
 * report. */
ASSERT((_stack_guard & (32 - 1)) == 0,
	"the stack guard is not 32-byte aligned, so the part would not enforce it")

ASSERT(_StackTop - _stack_bottom >= 2048,
	"less than 2 KB of main SRAM is left for the stack: move a large buffer into AHB SRAM with the AHB_SRAM macro")

/* Both blocks have to stay where the host looks for them. These are checked
 * after allocation, so they read the addresses the link really chose. */
ASSERT(_interrupt_vector_table_end - _interrupt_vector_table <= 0xe0,
	"the interrupt vector table has grown into the image check block at offset 0xe0")
ASSERT(_image_check_start - _interrupt_vector_table == 0xe0,
	"the image check block is not at offset 0xe0")
ASSERT(_image_check_end - _image_check_start == 0 || _image_check_end - _image_check_start == 32,
	"the image check block is not 32 bytes")
ASSERT(_fw_info_start - _interrupt_vector_table == 0x100,
	"the firmware info block is not at offset 0x100")
ASSERT(_fw_info_end - _fw_info_start == 0 || _fw_info_end - _fw_info_start == 64,
	"the firmware info block is not 64 bytes")

/* The bootloader version block, which shares the firmware info block's
 * offset. Each check passes for an image that carries no such block, so the
 * application images are not asked to have one. */
ASSERT(_fw_info_end - _fw_info_start == 0 || _bl_version_end - _bl_version_start == 0,
	"one image carries both the firmware info block and the bootloader version block, which share the offset at 0x100")
ASSERT(_bl_version_end - _bl_version_start == 0 || _bl_version_start - _interrupt_vector_table == 0x100,
	"the bootloader version block is not at offset 0x100")
ASSERT(_bl_version_end - _bl_version_start == 0 || _bl_version_end - _bl_version_start == 48,
	"the bootloader version block is not 48 bytes")
/* The word at 0x2fc is the code read protection value the LPC boot ROM reads
 * (NXP LPC176x/5x user manual UM10360). A block placed over it could lock the
 * part's debug and programming interfaces, which is not recoverable here. */
ASSERT(_bl_version_end - _bl_version_start == 0 || _bl_version_end - _interrupt_vector_table <= 0x2fc,
	"the bootloader version block reaches the code read protection word at 0x2fc")

/* The bootloader the updater image carries. Both checks pass for an image
 * that carries none, which is every image but firmware/bl_update. The host
 * reads the payload from the offset alone, so an image whose payload moved,
 * or whose payload is not the whole region, has to fail the link rather than
 * have ubertooth-dfu read the wrong bytes as a bootloader. */
ASSERT(_bl_payload_end - _bl_payload_start == 0 || _bl_payload_start - _interrupt_vector_table == 0x200,
	"the bootloader payload is not at offset 0x200")
ASSERT(_bl_payload_end - _bl_payload_start == 0 || _bl_payload_end - _bl_payload_start == 0x4000,
	"the bootloader payload is not the whole 16 KiB bootloader region")
