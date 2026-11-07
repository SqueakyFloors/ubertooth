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

    * Point the libbtbb pin at libbtbb's release tag, once that tag exists:
      ``LIBBTBB_REF`` in ``.github/workflows/build.yml``, which is where the
      pin is decided. The ``libbtbb pin`` check names every other file that
      has to carry the same value. Until then the pin is a commit, and the
      configure step compares the library it finds against it; a tag names no
      commit to compare, so from the release on that line reports both ends
      and claims nothing.

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
      ``COMPATIBILITY.md``, the expected ``API:`` string in
      ``validation/TEST_PLAN.md``, the pinned strings in the host tests, and
      note the reflash in ``CHANGELOG.md``.

    * Configure the host tools with ``-DUBERTOOTH_RELEASE=ON``. Without it the
      build works the build type out from the tree: an archive with no ``.git``
      is read as a release, which is what ``git archive`` gives, as is a
      checkout of the release tag itself -- but a clone with no tags fetched is read as
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
      btbr, the bootloader, bl_update and assembly_test, which is what
      ``tools/ubertooth-release.sh`` builds and copies into
      ``ubertooth-one-firmware-bin``. bl_update goes into a
      ``bootloader-update`` subdirectory of its own, because flashing it as
      ordinary firmware programs the bootloader region. The receive-only
      image ships as
      ``bluetooth_rx_only.*`` and calls itself ``bluetooth_rx`` in its
      firmware info block, so a board running it can be told from one running
      the full image; the script takes that name from the image's own
      Makefile.

    * A release built by hand passes ``HAVE_GIT=false`` on every make line, as
      the script does. It stops the build regenerating the shipped
      ``version.h``, which it would do if the release directory happened to
      sit inside a git work tree, stamping the images with that tree's version
      instead of the release. There is nothing else to pin: an image records
      the version it was built from and nothing about when, where or by whom
      it was built.

    * Build the bootloader with ``make -C firmware bootloader``, which is what
      ``tools/ubertooth-release.sh`` and CI do. It is not part of ``make -C
      firmware``: it is the one image written in C++, so it needs the ARM C++
      runtime (``libstdc++-arm-none-eabi-newlib``) that nothing else here does.

    * The firmware version string comes from a generated ``version.h``.
      ``tools/ubertooth-release.sh`` writes a ``version.h`` carrying the release
      number into each firmware image directory of the archive, and the build
      uses that shipped header as-is when ``.git`` is not present. If you build
      a release by hand rather than via the script, drop an equivalent
      ``version.h`` into ``firmware/bluetooth_rxtx``, ``firmware/btbr``,
      ``firmware/bl_update``, ``firmware/assembly_test`` and
      ``firmware/bootloader`` before building. It has
      to define ``GIT_REVISION`` (the release number) and ``FW_IMAGE_NAME``
      (the image directory's name, which ``firmware/common/fw_info.c`` asserts
      the length of); a header missing either fails to compile. Nothing else
      goes in it: no build date, no time, no user, no host, so the same source
      builds a byte-identical image. The bootloader uses only
      ``GIT_REVISION``, for the version block ``ubertooth-util -B`` reads.
      Write the same release into ``RELEASE-VERSION`` as well, which is what
      the host tools read and what a firmware image directory with no shipped
      header falls back to, so every half of the archive names one release.

    * A build from a git checkout needs none of that: it works the release out
      from the tags. ``firmware/version.mk`` and
      ``host/cmake/set_release.cmake`` both ask git for the nearest tag named
      ``YYYY-MM-Rn`` (or ``-RCn`` or ``-DEVn``) and how many commits HEAD is
      past it, so a build of the release tag reports the release and a build
      after it reports, for example, ``2026-11-R1+22``, with ``*`` added when
      the tree has uncommitted changes. A tag in any other form is ignored, so
      a new tag has to be named that way to be reported.

    * A build with no git history to ask -- a source archive, a release
      tarball -- takes the release from ``RELEASE-VERSION`` at the top of the
      tree. ``.gitattributes`` marks that file ``export-subst``, so ``git
      archive`` fills the release into it from the commit it archived, and
      nothing has to be edited before a tag. ``tools/ubertooth-release.sh``
      writes the release it was asked for into the same file, which is the
      name to carry when a release is built from a commit the tag is not on
      yet. Filling it in needs git 2.35 or newer where the archive is made --
      that is the version whose ``git archive`` understands every option in
      the placeholder --
      the git that unpacks or builds it does not matter. The file is read only
      where git places the tree in no checkout of its own, because a
      checkout's own history is the better answer and a version file beside
      one was written for some other source. A build with no release to read
      either way -- a checkout whose history reaches no release tag, a partial
      copy of these sources -- reports ``unknown``. libbtbb carries the same
      file and reads it the same way.

    * Because the version names a release rather than a commit, two builds of
      different commits between the same pair of tags can report the same
      version. Anything that has to tell two images apart -- an A/B comparison
      of two firmware builds, for instance -- compares the ``.dfu`` file's
      SHA-256 checksum, not the version string.

    * ``make clean`` deletes that ``version.h``, so write the headers after the
      last clean as well. They ship in the archive for whoever builds the
      firmware from the release.

    * Export gerbers from KiCad (or copy from previous release if unchanged)

    * Run the test harness's release plan on the release candidate, then
      commit that release's evidence folder under ``validation/releases/`` and
      link its ``summary.md``, with the harness version used, from the release
      notes. ``validation/README.md`` explains which commits qualify a release
      and which gates are required; ``validation/releases/README.md`` gives the
      folder's contents and the sanitizing rule.

    * Write the release notes in ``tools/RELEASENOTES``, which
      ``tools/ubertooth-release.sh`` copies to the top level of the archive,
      and put them on the GitHub release page as well. Start from the
      Highlights section at the top of the release's ``CHANGELOG.md`` entry,
      which is kept as the one draft of what this release is worth reading
      about, and add what only release notes carry: the libbtbb version the
      tools need, the firmware API version and that every board has to be
      reflashed, and where to get help. This fork has no wiki and no mailing
      list.

    * Write or update the build instructions in :doc:`build_guide` as needed
    
    * Perhaps we should branch and do some of this on the branch before tagging, this seems like good practice.

