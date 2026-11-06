/*
 * Stands in for the version.h the build generates, so ../common/fw_info.c can
 * be compiled with the host compiler and the bytes it produces checked
 * (test_fw_info.c). The real header is written by ../common.mk from the
 * version git says the tree is and the image's TARGET name.
 */
#ifndef UBERTOOTH_VERSION_H
#define UBERTOOTH_VERSION_H
#define GIT_REVISION "2026-11-DEV1+22*"
#define FW_IMAGE_NAME "test_image"
#endif
