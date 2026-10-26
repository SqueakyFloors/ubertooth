/*
 * Stands in for the version.h the build generates, so ../common/fw_info.c can
 * be compiled with the host compiler and the bytes it produces checked
 * (test_fw_info.c). The real header is written by ../common.mk from the git
 * revision and the image's TARGET name.
 */
#ifndef UBERTOOTH_VERSION_H
#define UBERTOOTH_VERSION_H
#define GIT_REVISION "git-deadbee*"
#define FW_IMAGE_NAME "test_image"
#define COMPILE_BY "builder"
#define COMPILE_HOST "buildhost"
#define TIMESTAMP "Mon Oct 27 11:22:33 2025"
#endif
