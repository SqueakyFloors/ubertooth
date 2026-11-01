/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * What this bootloader says about itself: its version and the revision it was
 * built from, in the fixed-layout block a running application reports.
 *
 * A bootloader is installed over ISP or JTAG, so a board keeps the one it
 * came with through every firmware update. This block is how the board can
 * then say which one that is. ../common/sections.ld puts it at
 * UBERTOOTH_BL_VERSION_OFFSET, where it is at a known place in the region
 * whatever else the bootloader grows into.
 *
 * The layout, the offset and the magic are in
 * host/libubertooth/src/ubertooth_interface.h, which the host reads them from
 * too. GIT_REVISION comes from the generated version.h.
 */
#include "ubertooth_interface.h"
#include "version.h"

/*
 * A compiler truncates a string that does not fit the field it initializes,
 * which would ship a cut-off revision with nothing to say it was cut. Fail
 * the build instead.
 */
_Static_assert(sizeof(GIT_REVISION) - 1 <= UBERTOOTH_BL_VERSION_BUILD_LEN,
               "GIT_REVISION does not fit the bootloader version block");

/*
 * `used` keeps the block through --gc-sections: nothing in the bootloader
 * refers to it, so the linker would otherwise drop the section it is the only
 * member of. sections.ld also KEEPs it.
 *
 * The named section is only asked for on ELF, which is every target this
 * firmware is built for. ../tests/test_bl_version.c compiles this file with
 * the host compiler to check the bytes it produces, and a Mach-O host rejects
 * a section name with no segment in front of it; the section placement is not
 * what that test is about.
 */
#ifdef __ELF__
#define BL_VERSION_PLACEMENT __attribute__((section(".bl_version"), used))
#else
#define BL_VERSION_PLACEMENT __attribute__((used))
#endif

BL_VERSION_PLACEMENT
const struct ubertooth_bl_version_block bl_version_block = {
	/* UBERTOOTH_BL_VERSION_MAGIC, byte by byte: the field holds no
	 * terminator, and a string literal that exactly fills it draws a
	 * warning from newer compilers. The bootloader test puts these bytes
	 * against the macro. */
	{ 'U', 'B', 'T', 'B', 'L', 'V', 'E', 'R' },
	UBERTOOTH_BL_VERSION_FORMAT,
	UBERTOOTH_BOOTLOADER_VERSION,
	{ 0, 0, 0, 0 },
	GIT_REVISION,
};
