=================
Release Procedure
=================



Release names
-------------

Tags take three forms, all built on the ``yyyy-mm-Rn`` one:

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
      git history reports. Commit the change before tagging, so a source
      download of the tag reports the release too:

    	* ``ubertooth/host/cmake/set_release.cmake`` (``LATEST_RELEASE``).
    	  ``tools/ubertooth-release.sh`` also sets it in the archive it builds.

    	* ``libbtbb/cmake/set_release.cmake`` (``LATEST_RELEASE``), which
    	  ``lib/src/CMakeLists.txt`` includes

    * Update the version pointer in README.md's Releases section, "The current
      pre-release is …", to name the new tag. Leave the upstream pointer above
      it alone: it names Great Scott Gadgets' most recent release, not ours.

    * Bump the firmware API version to the release's own number.
      ``UBERTOOTH_API_VERSION`` in
      ``host/libubertooth/src/ubertooth_interface.h`` already moves in every
      development pull request that changes firmware behavior the host relies
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
      is read as a release, which is what ``git archive`` gives, as is a
      checkout of the tag itself -- but a clone with no tags fetched is read as
      a development build, and only warns about firmware of another API
      version instead of refusing it. Worse, CMake
      decides this when it configures, so an existing build directory keeps
      whatever it decided before the tag was checked out. Passing the flag
      settles it.

    * Build binary firmware images for all board revs and all common firmware
      images. Install the host tools first: each image's build packages its
      ``.dfu`` with ``ubertooth-dfu -s``, so the firmware build needs
      ``ubertooth-dfu`` on ``PATH``.

    * For Ubertooth One these are bluetooth_rxtx (full and receive-only),
      btbr, the bootloader and assembly_test, which is what
      ``tools/ubertooth-release.sh`` builds and copies into
      ``ubertooth-one-firmware-bin``. The receive-only image ships as
      ``bluetooth_rx_only.*`` and calls itself ``bluetooth_rx`` in its
      firmware info block, so a board running it can be told from one running
      the full image; the script takes that name from the image's own
      Makefile.

    * The script pins the firmware build stamp, and a release built by hand
      should pass the same on every make line:
      ``HAVE_GIT=false VERSION_BY=ubertooth VERSION_HOST=release``. The last
      two keep the name of the user and the machine out of a published image.
      ``HAVE_GIT=false`` stops the build regenerating the shipped
      ``version.h``, which it would do if the release directory happened to
      sit inside a git work tree, stamping the images with that tree's
      revision instead of the release.

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
      ``version.h`` into ``firmware/bluetooth_rxtx``, ``firmware/btbr`` and
      ``firmware/assembly_test`` before building -- the three images a release
      ships; the bootloader includes no ``version.h`` at all. It has
      to define ``GIT_REVISION`` (the release number), ``FW_IMAGE_NAME`` (the
      image directory's name, which ``firmware/common/fw_info.c`` asserts the
      length of), ``COMPILE_BY``, ``COMPILE_HOST`` and ``TIMESTAMP``; a header
      missing any of them fails to compile.

    * ``make clean`` deletes that ``version.h``, so write the headers after the
      last clean as well. They ship in the archive for whoever builds the
      firmware from the release.

    * Export gerbers from KiCad (or copy from previous release if unchanged)

    * Write the release notes in ``tools/RELEASENOTES``, which
      ``tools/ubertooth-release.sh`` copies to the top level of the archive,
      and put them on the GitHub release page as well. This fork has no wiki
      and no mailing list.

    * Write or update the build instructions in :doc:`build_guide` as needed
    
    * Perhaps we should branch and do some of this on the branch before tagging, this seems like good practice.

