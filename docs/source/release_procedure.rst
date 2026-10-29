=================
Release Procedure
=================



Release names
-------------

Tags keep the ``yyyy-mm-Rn`` form, with two kinds of tag before a release:

    * ``yyyy-mm-DEVn`` is a development pre-release: a snapshot of ``main``
      published for integration testing, and it may be deleted later.

    * ``yyyy-mm-RCn`` is a release candidate.

    * ``yyyy-mm-Rn`` is the release.

Publish the development and release-candidate tags as GitHub pre-releases, so
the release page's "Latest" link stays on a release. Host tools and firmware
from one tag belong together: tools built from a tagged commit refuse a board
carrying a different firmware API version.

Procedure
---------

An Ubertooth release is not as simple as a standard GitHub release, there are a few extra tasks, as listed below.

    * Tag both libbtbb and ubertooth with the release version. Use an annotated tag. e.g.

    * git tag -a yyyy-mm-Rx

    * Create a release directory using git archive

    * Set the release version in these files, which is what a build with no
      git history reports:

    	* ``ubertooth/host/cmake/set_release.cmake`` (``LATEST_RELEASE``),
    	  which ``tools/ubertooth-release.sh`` sets in the archive

    	* libbtbb/lib/src/CMakeLists.txt

    * Bump the firmware API version to the release's own number.
      ``UBERTOOTH_API_VERSION`` in
      ``host/libubertooth/src/ubertooth_interface.h`` already moves in every
      development pull request that changes firmware behaviour the host relies
      on; the release moves it to the next major version to mark that its host
      tools and firmware only work with each other, whether or not this cycle
      changed the firmware. The first fork release takes ``0x0200``. It is
      binary-coded decimal (``0x0200`` reads as 2.00), and the host tools
      require firmware of exactly this version, so the move means every board
      has to be reflashed after the release. Update the firmware row of
      ``COMPATIBILITY.md``, the expected ``API:`` string in ``TEST_PLAN.md``,
      the pinned strings in the host tests, and note the reflash in
      ``CHANGELOG.md``.

    * Configure the host tools with ``-DUBERTOOTH_RELEASE=ON``. Without it the
      build works the build type out from the tree: an archive with no ``.git``
      is read as a release, which is what ``git archive`` gives, but a build
      made in a checkout is read as a development build and only warns about
      firmware of another API version instead of refusing it. Worse, CMake
      decides this when it configures, so an existing build directory keeps
      whatever it decided before the tag was checked out. Passing the flag
      settles it.

    * Build binary firmware images for all board revs and all common firmware images.

    * For Ubertooth One these are bluetooth_rxtx (full and receive-only),
      btbr, the bootloader and assembly_test, which is what
      ``tools/ubertooth-release.sh`` builds and copies into
      ``ubertooth-one-firmware-bin``.

    * The script pins the firmware build stamp to
      ``VERSION_BY=ubertooth VERSION_HOST=release``, so a published image does
      not carry the name of the user or machine that built it. A release built
      by hand should pass the same.

    * Build the bootloader with ``make -C firmware bootloader``, which is what
      ``tools/ubertooth-release.sh`` and CI do. It is not part of ``make -C
      firmware``: it is the one image written in C++, so it needs the ARM C++
      runtime (``libstdc++-arm-none-eabi-newlib``) that nothing else here does.

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

