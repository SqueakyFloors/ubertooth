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
		 * The firmware info block, at a fixed offset from the start of
		 * the image so that ubertooth-dfu can read it out of a firmware
		 * file without searching: UBERTOOTH_FW_INFO_OFFSET (0x100) and
		 * UBERTOOTH_FW_INFO_LENGTH (64) in
		 * host/libubertooth/src/ubertooth_interface.h, which
		 * firmware/tests/test_fw_info.c reads and puts against these
		 * numbers. The offset clears the vector table, including the
		 * checksum word at 0x1c that the LPC boot ROM reads over it;
		 * the ASSERTs after this block are what catch a vector table
		 * that has grown that far, and an image whose block is the
		 * wrong size.
		 *
		 * MAX() rather than a plain assignment, because a location
		 * counter cannot move backwards: a vector table past the
		 * offset would fail the link with the linker's own message
		 * about the counter, before any ASSERT ran. Taking the later
		 * of the two addresses lets the link reach the ASSERTs, which
		 * name the problem.
		 *
		 * The bootloader shares this script and carries no block, so
		 * for that one image this leaves 0x100 - 0xcc = 52 bytes of
		 * zeros where the block would go.
		 */
		. = MAX(., _interrupt_vector_table + 0x100);
		_fw_info_start = .;
		KEEP(*(.fw_info))
		_fw_info_end = .;

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

	/* Where we put the heap with cr_clib */
	.cr_heap :
	{
		end = .;
		__end__ = .;
		_pvHeapStart = .;
	} > ram

	/* Leave room above stack for IAP to run */
	_StackTop = ORIGIN(ram) + LENGTH(ram) - 32;

}

/* The firmware info block has to stay where the host looks for it. These are
 * checked after allocation, so they read the addresses the link really chose. */
ASSERT(_interrupt_vector_table_end - _interrupt_vector_table <= 0x100,
	"the interrupt vector table has grown into the firmware info block at offset 0x100")
ASSERT(_fw_info_start - _interrupt_vector_table == 0x100,
	"the firmware info block is not at offset 0x100")
ASSERT(_fw_info_end - _fw_info_start == 0 || _fw_info_end - _fw_info_start == 64,
	"the firmware info block is not 64 bytes")
