/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * What this image says about itself: which image it is, which revision it was
 * built from, and which firmware API version it speaks.
 *
 * ../common.mk compiles this into every image, so no image can leave the
 * block out, and ../common/sections.ld puts it at a fixed offset from the
 * start of the image. That fixed offset is the point: ubertooth-dfu reads the
 * block out of a firmware file without searching it, and the firmware returns
 * the same bytes through UBERTOOTH_GET_FW_INFO.
 *
 * The layout, the offset and the magic are in
 * host/libubertooth/src/ubertooth_interface.h, which the host reads them from
 * too. FW_IMAGE_NAME and GIT_REVISION come from the generated version.h.
 */
#include "ubertooth_interface.h"
#include "version.h"

/*
 * A compiler truncates a string that does not fit the field it initialises,
 * which would ship a cut-off name or revision and fail the revision check
 * ubertooth-dfu -r makes after a write. Fail the build instead.
 */
_Static_assert(sizeof(FW_IMAGE_NAME) - 1 <= UBERTOOTH_FW_INFO_IMAGE_LEN,
               "FW_IMAGE_NAME does not fit the firmware info block");
_Static_assert(sizeof(GIT_REVISION) - 1 <= UBERTOOTH_FW_INFO_REVISION_LEN,
               "GIT_REVISION does not fit the firmware info block");

/*
 * `used` keeps the block through --gc-sections: nothing in the firmware refers
 * to it, so the linker would otherwise drop the section it is the only member
 * of. sections.ld also KEEPs it.
 *
 * The named section is only asked for on ELF, which is every target this
 * firmware is built for. ../tests/test_fw_info.c compiles this file with the
 * host compiler to check the bytes it produces, and a Mach-O host rejects a
 * section name with no segment in front of it; the section placement is not
 * what that test is about.
 */
#ifdef __ELF__
#define FW_INFO_PLACEMENT __attribute__((section(".fw_info"), used))
#else
#define FW_INFO_PLACEMENT __attribute__((used))
#endif

FW_INFO_PLACEMENT
const struct ubertooth_fw_info_block fw_info_block = {
	/* UBERTOOTH_FW_INFO_MAGIC, byte by byte: the field holds no terminator,
	 * and a string literal that exactly fills it draws a warning from newer
	 * compilers. The firmware test puts these bytes against the macro. */
	{ 'U', 'B', 'T', 'F', 'I', 'N', 'F', 'O' },
	UBERTOOTH_FW_INFO_FORMAT,
	UBERTOOTH_API_VERSION,
	{ 0, 0, 0, 0 },
	FW_IMAGE_NAME,
	GIT_REVISION,
};
