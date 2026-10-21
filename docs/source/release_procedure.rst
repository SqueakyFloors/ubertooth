=================
Release Procedure
=================



An Ubertooth release is not as simple as a standard GitHub release, there are a few extra tasks, as listed below.

    * Tag both libbtbb and ubertooth with the release version. Use an annotated tag. e.g.

    * git tag -a yyyy-mm-Rx

    * Create a release directory using git archive

    * Update the following files to set RELEASE to yyyy-mm-Rx:

    	* ubertooth/libubertooth/src/CMakeLists.txt

    	* libbtbb/lib/src/CMakeLists.txt

    * Build binary firmware images for all board revs and all common firmware images.

    * At time of writing, these are only bluetooth_rxtx and bootloader for Ubertooth One.

    * The firmware version string now comes from a generated ``version.h`` (the
      old ``GIT_REVISION`` ``-D`` flag in ``firmware/common.mk`` was removed).
      ``tools/ubertooth-release.sh`` writes a ``version.h`` carrying the release
      number into each firmware image directory of the archive, and the build
      uses that shipped header as-is when ``.git`` is not present. If you build
      a release by hand rather than via the script, drop an equivalent
      ``version.h`` (defining ``GIT_REVISION`` to the release number) into each
      firmware image directory before building.

    * Build the bluetooth_rxtx firmware and rename bluetooth_rxtx.dfu to ubertooth-one-bin-firmware.dfu

    * Copy built firmare hex/bin/dfu files to /ubertooth-one-firmware-bin

    * Export gerbers from KiCad (or copy from previous release if unchanged)

    * Write release notes, save to top level of archive, add to the wiki and send to the mailing list

    * Write/update the build instructions on the wiki as needed
    
    * Perhaps we should branch and do some of this on the branch before tagging, this seems like good practice.

