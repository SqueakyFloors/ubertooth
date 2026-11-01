/*
 * What a bootloader updater image says about the bootloader it carries.
 * See bl_update_file.h.
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
#include <string.h>

#include "bl_update_file.h"
#include "dfu_recovery.h"
#include "ubertooth_interface.h"

int bl_update_file_check(FILE *file, const char *image_name,
                         struct bl_update_file *out)
{
	uint8_t payload[UBERTOOTH_BL_UPDATE_PAYLOAD_LENGTH];
	uint8_t region[UBERTOOTH_BL_UPDATE_PAYLOAD_LENGTH];
	uint32_t checksum;
	size_t i;

	if (out == NULL)
		return UBERTOOTH_BL_UPDATE_NOT_UPDATER;
	memset(out, 0, sizeof(*out));
	out->result = UBERTOOTH_BL_UPDATE_NOT_UPDATER;
	if (file == NULL || image_name == NULL)
		return out->result;

	/* The image's own name is what tells an updater from any other firmware.
	 * Without it, --update-bootloader given an ordinary application image
	 * would read 16 KiB of that image's code and write it over the
	 * bootloader region. */
	if (strcmp(image_name, UBERTOOTH_BL_UPDATE_IMAGE_NAME) != 0)
		return out->result;

	out->result = UBERTOOTH_BL_UPDATE_FILE_SHORT;
	if (fseek(file, UBERTOOTH_BL_UPDATE_PAYLOAD_OFFSET, SEEK_SET) != 0)
		return out->result;
	if (fread(payload, 1, sizeof(payload), file) != sizeof(payload))
		return out->result;

	out->crp_word = (uint32_t)payload[UBERTOOTH_LPC_CRP_OFFSET] |
		((uint32_t)payload[UBERTOOTH_LPC_CRP_OFFSET + 1] << 8) |
		((uint32_t)payload[UBERTOOTH_LPC_CRP_OFFSET + 2] << 16) |
		((uint32_t)payload[UBERTOOTH_LPC_CRP_OFFSET + 3] << 24);

	/* The same checks the updater makes on the board, over the same bytes.
	 * Whether the board already has this bootloader is not asked here:
	 * the board's own region is read from the board, not from a file. */
	out->result = ubertooth_bl_update_payload_check(payload, sizeof(payload),
	                                               &out->bootloader_version);
	if (out->result != UBERTOOTH_BL_UPDATE_OK)
		return out->result;

	/* The region as the board will hold it, which is the payload with the
	 * vector checksum filled in, and its hash. The bootloader report an
	 * application answers gives the inverted CRC-32
	 * (firmware/common/bl_report.c), where a DFU suffix uses the same CRC
	 * without that final inversion, so the one implementation here serves
	 * both and the inversion is the whole of the difference. */
	checksum = ubertooth_bl_update_vector_checksum(payload);
	for (i = 0; i < sizeof(region); i++)
		region[i] = ubertooth_bl_update_byte(payload, i, checksum);
	out->region_crc32 = ~dfu_image_crc32(region, sizeof(region));
	return out->result;
}

int bl_update_file_report(FILE *out, const char *name,
                          const struct bl_update_file *info)
{
	if (out == NULL || name == NULL || info == NULL) {
		/* The pair's other half guards the same way. Nothing to print and
		 * nothing worth writing, so this reads as a refusal. */
		return 1;
	}
	if (info->result < 0) {
		fprintf(out, "error: %s: %s\n", name,
		        ubertooth_bl_update_result_string(info->result));
		if (info->result == UBERTOOTH_BL_UPDATE_CRP)
			fprintf(out, "error: the word at 0x%x is 0x%08x (%s); "
			        "nothing in this tree asks for that, and it cannot "
			        "be undone from outside the part\n",
			        UBERTOOTH_LPC_CRP_OFFSET, info->crp_word,
			        ubertooth_lpc_crp_name(info->crp_word));
		return 1;
	}
	fprintf(out, "Bootloader to install: version %u.%02u, "
	        "region CRC32:0x%08x\n",
	        (unsigned)(info->bootloader_version >> 8),
	        (unsigned)(info->bootloader_version & 0xff),
	        (unsigned)info->region_crc32);
	return 0;
}
