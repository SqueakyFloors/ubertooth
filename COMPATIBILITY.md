# Compatibility and Support

This document states which platforms and tool versions this fork **requires**,
which it has been **tested against**, and which are **not supported**.

> **Honesty rule for this doc:** a cell is only marked *Verified* if it was
> actually exercised — by a CI run or on real hardware (see [TEST_PLAN.md](TEST_PLAN.md) /
> [TEST_LAB.md](TEST_LAB.md)). Requirements we intend to meet but have **not yet verified**
> are marked *Target (unverified)*. We do not claim support we have not proven:
> this fork exists to make the tree build and run on current systems, and the
> matrix is the record of where that has actually been exercised.

Legend:
- ✅ **Verified** — exercised by CI or hardware, known good.
- 🎯 **Target (unverified)** — intended to be supported; not yet tested. Must
  not be advertised as working until verified.
- ❌ **Unsupported** — explicitly not supported; will not be fixed.

---

## Support matrix

| Dimension | Minimum (required) | Tested / known-good | Unsupported | Status of minimum |
|---|---|---|---|---|
| **Ubuntu** | 20.04 LTS | 20.04, 22.04, 24.04 (CI, host+fw); 24.04.4 x86_64 on hardware (host tools) | ≤ 18.04 (use 2020-12-R1) | ✅ 20.04 verified in CI (container) |
| **macOS** | 14 (Sonoma) | 14, 15, 26 / arm64 (CI, host+fw); 26.7.1 arm64 on hardware (host tools) | ≤ 13 | ✅ 14 verified in CI |
| **Windows** | — | — | all | ❌ Unsupported (libusb + udev; use Linux/VM) |
| **CMake** | 3.5 | 3.5 … 3.27 (enforced range); 3.28.3 (Ubuntu 24.04) and 4.4.4 (macOS 26) build the host tools | < 3.5 (CMake 4 removed it) | ✅ floor enforced in CMakeLists |
| **GNU make (firmware)** | 3.81 | 3.81 (macOS), 4.3 (Linux) | — | ✅ 3.81 verified; build avoids make ≥ 4.0 features |
| **perl (firmware tests)** | 5, any version | the system perl on the Ubuntu 24.04 and macOS 26 CI runners | — | ✅ runs in CI on both. Only `make -C firmware test` needs it, to generate the ToorCon badge's register header |
| **Python (host tools)** | 3.8 | 3.8 (U20 CI) … 3.14 (macOS CI) | 2.x (removed upstream) | ✅ 3.8 verified in CI |
| **GCC (host)** | 9.4 | 9.4 (Ubuntu 20.04 CI), 11.4 (22.04), 13.3 (24.04), 16.2 (Fedora CI); Apple clang 15 (macOS 14), 17 (macOS 15), 21 (macOS 26); `-std=c17`/`-std=c2x` too, on 24.04 and macOS 26 | older than 9.4 (not tested) | ✅ 9.4 verified in CI (oldest tested) |
| **arm-none-eabi-gcc (firmware)** | 9-2019-q4 (GCC 9.2.1) | 9-2019-q4 (Ubuntu 20.04 CI), 10.3-2021.07 (22.04), 13.2.rel1 (24.04), 15.3.1 (macOS 14/15/26 CI); 13.2 (Linux) and 15.3 (macOS) on hardware | older than 9-2019-q4 (not tested) | ✅ 9-2019-q4 verified in CI (oldest tested) |
| **libusb** | 1.0.23 | 1.0.23 (Ubuntu 20.04 CI), 1.0.25 (22.04), 1.0.27 (24.04), 1.0.30 (macOS) | pre-1.0; older than 1.0.23 (not tested) | ✅ 1.0.23 verified in CI (oldest tested) |
| **libbtbb** | the pinned fork commit ([`2fd0d39`](https://github.com/SqueakyFloors/libbtbb/commit/2fd0d391d99e9b0620146f63f439c1ec74f79861), C lib) | that commit (CI, `install.sh`); upstream master was CI's dependency until the switch to the pin | one missing a function the tools call: refused at configure time. Any other libbtbb, upstream's included: untested here | ✅ the pin is what CI builds |
| **libbluetooth (BlueZ)** — optional, for `ubertooth-scan` and `ubertooth-follow` | 5.53 | 5.53 (Ubuntu 20.04 CI), 5.64 (22.04), 5.72 (24.04), 5.87 (Fedora CI) | macOS (no BlueZ: both tools are skipped there) | 🎯 5.53 verified in CI; neither tool has run against a real adapter and piconet ([#44](https://github.com/SqueakyFloors/ubertooth/issues/44)) |
| **ronn-ng (man pages)** | 0.10.1, exactly | 0.10.1 (`Man pages` CI job on Ubuntu 24.04; macOS 26 via Homebrew) | any other version: it renders the same Markdown differently, so the committed pages would stop matching a refresh | ✅ 0.10.1 verified in CI. Optional: only `refresh-manpages` needs it, and a build neither needs it nor runs it |
| **Ubertooth firmware (API)** | 1.53 (this tree), exactly | 1.53 on an Ubertooth One running either image: `bluetooth_rxtx` and `btbr` both report this tree's version | any other version, 1.52 and 1.07 (`2020-12-R1`) included: refused. `ubertooth-dfu` reflashes a board running any of them, with an image from this tree; `ubertooth-util -v`/`-V` read any firmware | 🎯 1.53 is promiscuous mode reporting the access address of the connection it follows, verified on a lab Ubertooth One: `ubertooth-btle -p` printed the access address of a link two nRF52840 boards held up, in two of three passes. The hop interval the same mode reports is either the link's own or not reported at all ([#824](https://github.com/SqueakyFloors/ubertooth/issues/824)). 🎯 1.52 is the low-energy packet the board reports dropping for want of queue room and the channel set that reaches the board while it follows a connection, HWPENDING ✅ 1.51 is every E-GO run starting the mode it asks for, verified on a lab Ubertooth One: `-r -c 2418` after a killed `-f` read 2418 MHz eight times over, where `main` read four different E-GO channels. ✅ 1.50 is the AFH map a mode start keeps and the bound on the clock drift the board accumulates, verified on a lab Ubertooth One: after a spectrum scan killed without a stop, a map stated after the hopping request left the board hopping the map's 20 channels, ten reads of 2402-2421 MHz where before it hopped the whole band, and 110 drift requests at 320 PPM left the clock 377 PPM slow instead of 115 PPM fast. Its third part, a FIFO flush in the generic receive, has since come back out: on a pair of boards a record arrives shifted as often with it as without. 1.49 is the channel a mode start keeps and the E-GO access code it resets, verified on a lab Ubertooth One: after a killed `ubertooth-specan` and a killed `ubertooth-rx` left a capture running, the board's channel read back six times across the band rather than 2441 MHz six times over. `ubertooth-ego` names the shared default access code, but no E-GO device was on the bench, so what the capture listens for has not been read off the air. ✅ 1.48 is the finer breadcrumb sites inside USB service, verified on a lab Ubertooth One: a board that wedged in `ubertooth-rx` reported `running a control request` rather than `servicing USB`. ✅ 1.47 verified on an Ubertooth One: flashing both images, `util -v`/`-b`/`-w`, `btle -n`, an `rx -z` Classic capture, a specan sweep, the self-reset, and a `btbr` session of inquiry, inquiry scan and page scan in which an inquiry was answered and no radio wait gave up. At 1.46: flashing `bluetooth_rxtx` and `btbr`, `util -v`, `btle -n` and `-f`, `rx -z`, `specan`, a `btbr` inquiry session, and the two-board range test at PA level 0. At 1.45: flashing `bluetooth_rxtx`, `util -v`, and `btle -f` and `btle -f -v1` captures of live connections, every data packet’s CRC valid. At 1.44, on two boards: flashing `bluetooth_rxtx` on both, and 20 two-board range tests that all gave a result. At 1.43, on a pair of boards: flashing `bluetooth_rxtx`, generic transmit packets of 28, 33 and 50 bytes — the last two longer than the radio's 32-byte FIFO — sent from one board and read back off the air by the other, the advertising mode captured by the second board, the two-board range test, `btle -n`, an `rx -z` Classic capture, a specan sweep and `util -v`/`-b`/`-w`. Four `btle -f -v1` captures of advertising packets ran at 1.42. Flashing both images, the radio wait counts, the register the mode teardown puts back and two specan sweeps that agree across a mode change, the transmit test starting and stopping, and the hand-over to the on-chip programmer on both images last ran at 1.41; a Classic capture whose noise figure was read back against `main`'s on the same board at 1.40; the breadcrumb at 1.39, flashing `btbr` and a `btbr` session at 1.38, the self-reset at 1.32; follow at an earlier version. The `btbr` image has not been read off a board at 1.43, and the symbol transmit mode and the [#251](https://github.com/SqueakyFloors/ubertooth/issues/251) mode checks have not run |
| **Ubertooth bootloader** | whichever one is on the board; no tool checks it (its `bcdDevice` is a fixed 1.00) | the one the boards here shipped with. CI compiles and links this tree's, which is also the check that it fits its 16 KiB region: Ubuntu 24.04 and macOS 26 on every pull request, the rest of the firmware row's toolchains in the full tier | — | 🎯 this tree's bootloader is built and unit-tested only, never flashed: installing one needs ISP or JTAG. It bounds both a read-back and an image by the flash the attached part has, from the sectors the part reports when its table cannot identify it ([#256](https://github.com/SqueakyFloors/ubertooth/issues/256), [#423](https://github.com/SqueakyFloors/ubertooth/issues/423), [#424](https://github.com/SqueakyFloors/ubertooth/issues/424)) |

---

## Key compatibility notes

### Ubuntu 18.04 and older: not supported by this fork (use 2020-12-R1)
This fork targets modern distributions and does **not** support Ubuntu 18.04 or
older. We will not verify or maintain compatibility with that era's toolchain
(CMake 3.10, Python 3.6, GCC 7.x).

Users on Ubuntu 18.04 should use the **`2020-12-R1` legacy release**, which was
built for that era. A maintainer of this fork has run that release on Ubuntu
18.04 for years in regular use, so the common workflows are known to work there;
that is the known-good path for 18.04 — not this fork's current branch. It is
mirrored on this fork (unmodified upstream code, provided as-is) for convenience:
<https://github.com/SqueakyFloors/ubertooth/releases/tag/2020-12-R1>
(original: <https://github.com/greatscottgadgets/ubertooth/releases/tag/2020-12-R1>)

> Note: this reflects first-hand use of the *upstream* release on 18.04 for a
> maintainer's typical workflow, not exhaustive feature testing, and not any
> claim about this fork on 18.04.

### Windows is not supported
libusb on Windows and the absence of udev rules make this impractical. Run the
host tools under Linux (native or VM). Documented as a hard non-goal.

### macOS: arm64 verified, Intel (x86_64) not CI-tested
CI covers Apple-Silicon (arm64) macOS 14/15/26. Intel macOS is not tested in CI:
GitHub's `macos-13` (last Intel) runners are capacity-constrained and queue
unreliably, so an Intel CI job stalls rather than giving a timely signal. The
code should build on Intel (no include path in the build names a Homebrew
prefix: the configure step takes libusb's include directories from libusb,
wherever Homebrew put it), but this is unverified. Test locally if you build on
an Intel Mac.

### The host library needs C11 atomics
`libubertooth` uses `<stdatomic.h>` for the two pointers of the capture ring
buffer, which one thread advances and the other reads. The header is a public
one, so the C11 floor applies to any C program that includes `ubertooth.h`, not
only to this project's own build. Every compiler in the table above has
atomics, and the default standard of GCC and clang (`gnu17`) is past C11, so a
normal build is unaffected; a `CFLAGS` of `-std=c99` or older is not supported,
and an implementation that defines `__STDC_NO_ATOMICS__` is refused with a
message. C++ code that includes the header is unaffected: it sees those two
fields as the plain integer they are built on, and nothing outside the library
touches them.

### `install.sh` platforms
`install.sh` supports Debian/Ubuntu (`apt`) and macOS with Homebrew. On macOS it
installs into the Homebrew prefix without `sudo`, builds without BlueZ (no
`ubertooth-scan`/`ubertooth-follow`), and installs no udev rules. Verified on
macOS 26 arm64 with two Ubertooths: a full run that installed the missing
Homebrew packages (`fftw`, `dfu-util`), the host tools, and a firmware build
and flash of both boards with `--flash-device all`. Intel Macs are unverified.

### CMake 4.x
Modern CMake (4.x) removed compatibility with `cmake_minimum_required < 3.5`.
This fork declares `cmake_minimum_required(VERSION 3.5...3.27)`, and the
pinned `libbtbb` fork commit declares a range too, so neither needs
`-DCMAKE_POLICY_VERSION_MINIMUM=3.5` any more. Upstream `libbtbb` still
declares `VERSION 2.8` and does need it; building against upstream instead of
the pin (`install.sh --libbtbb-url`/`--libbtbb-ref`) is on you.

### libbtbb comes from this project's fork, at a pinned commit
CI, `install.sh` and the build guide all build the same
`SqueakyFloors/libbtbb` commit, so a push to libbtbb cannot change an
Ubertooth build. The pin lives in `.github/workflows/build.yml`; the
`libbtbb pin` workflow, which runs on every change including a documentation
one, fails if any other file naming it, this one included, falls behind, or if
the pin is not a commit on the fork's default branch: a squash-merged pull request
head whose branch has been deleted is still served to CI by its hash, but is
in nobody's clone. Only the C library is built (`-DENABLE_PYTHON=OFF`): the
Ubertooth tools link nothing else from libbtbb.

The pin changes no part of `btbb.h`, so the interface these tools compile and
link against is the same as upstream's, and upstream master — which CI built
until this change — should keep working. It is simply no longer what gets
tested. Older upstream releases, `2020-12-R1` included, have not been
exercised here.

### Firmware API version: exact match, bumped for a breaking change
The host tools require firmware whose API version (`UBERTOOTH_API_VERSION`,
reported as the USB `bcdDevice`) is exactly their own. A release build refuses
any other — only `ubertooth-util` reading the version and `ubertooth-dfu`
reflashing still work, and `ubertooth-dfu` only writes an image from this tree,
since it refuses a file without a firmware info block. A build made from a git
checkout that is not on a tag is a development build: it warns and runs the
command, because host tools and firmware from different commits are the normal
state of lab work. `UBERTOOTH_ALLOW_API_MISMATCH` sets either answer whatever
the build (`1` runs on, `0` refuses). During development the minor version
stays where it is through an ordinary firmware change, because host tools and
firmware are built and flashed together and a development build only warns
when the two disagree; it is bumped for a change that breaks older host
tools -- a request removed or renumbered, a reply layout an older host would
misread -- so that such a board is reported rather than used silently: refused
by a release build, warned about by a development build. The first fork
release takes 2.00, to mark that its host tools and firmware only work with
each other.

The bootloader is not covered by that number. It reports a fixed `bcdDevice`
of 1.00, no host tool checks it, and `./install.sh --flash` writes the
application only, so a change to the bootloader reaches a board only when
someone flashes it deliberately. A bump that moves bootloader behaviour
therefore marks the change in the tree rather than anything a host can read
off a board sitting in DFU.

The number still does not pin down a commit, since every commit that does not
bump it shares the number before it: hence "normally"
above, and hence the firmware revision shown by `ubertooth-util -v` beside the
API version is what identifies a build. Host tools and firmware built from the
same commit always match, so after pulling, rebuild and reflash both from the
same commit (`./install.sh --flash`).

### Firmware build: GNU make 3.81 (macOS) and the generated version.h
The firmware builds with the stock GNU make on macOS (**3.81**) as well as the
modern make on Linux (4.3). The build intentionally avoids make ≥ 4.0 features
(e.g. `$(file ...)`) so a Mac with only Xcode Command Line Tools can build
firmware without installing a newer make.

The firmware version string reported by `ubertooth-util -v` / `-V` is produced
by a generated `version.h` (git revision + build info), not a `-D` compile
flag. It is regenerated only when the build identity changes, so `-v` always
matches the firmware actually built and flashed — verified across incremental
rebuilds on make 3.81 and 4.3, and end-to-end on real hardware (built revision
== on-device `-v`) on both macOS and the Linux test bench. A source archive
with no `.git` (downloaded zip / release tarball) uses a shipped `version.h`
as-is, or falls back to `git-unknown`; `tools/ubertooth-release.sh` writes the
release version into the archive.

### Python 3 only
Python 2 support was removed upstream in 2020-12-R1. This fork targets modern
Python 3; verified from 3.8 (Ubuntu 20.04) through 3.14 (current macOS runner).
Older distros (e.g. Ubuntu 18.04 / Python 3.6) are out of scope — see above.

**Prefix-safe Python installation.** CMake installs the in-tree pure-Python
``specan`` and ``ubtbr`` modules directly under ``CMAKE_INSTALL_PREFIX``; it no
longer invokes the deprecated ``setup.py install`` or requires setuptools for a
normal CMake build/install. Configured launchers locate those modules relative
to their own installed path, so custom prefixes and ``DESTDIR`` work without
``PYTHONPATH`` and without modifying system Python (#33). Runtime dependencies
remain external: NumPy/QtPy/PySide6 for specan_ui and ``libusb1`` for ubtbr.

### Wireshark extcap: interfaces are named by serial
`btle-extcap.py` names each capture interface `ubertooth<serial>` rather than
`ubertooth<index>` (#269), so a board keeps its interface across a replug. The
name changed with it: an interface saved by an earlier Wireshark session is
gone from the list and the Ubertooth has to be picked once more, after which
Wireshark remembers it. An `ubertooth<index>` from such a profile is still
accepted on the command line.

The script tolerates `--extcap-version` and `--extcap-capture-filter`, the
options Wireshark adds to its own calls, and refuses the ones Wireshark sends
only to an extcap program that advertised the feature behind them. A word that
is not an option or the value of one is refused, as the other host tools refuse
one. A capture filter is not applied: Wireshark's verification call is answered
with the reason, which turns the filter box red (#343).

Every call now exits 0 or 1 (#343). `--capture` used to pass
`ubertooth-btle`'s own exit status through, and report 128 plus the number of
the signal that killed it, so a script reading those statuses sees 1 instead.
See `host/python/extcap/README.md`.

---

## Hardware validation

Hardware evidence is not accumulated in this file. It is recorded per release in
a validation report published with the release (tracked in #85). The "on
hardware" entries in the matrix above reflect the most recent runs, which
covered the host tools only (build, unit tests, read-only device queries and
firmware flashing), not radio capture.

---

## How these claims are kept honest

1. **Enforced floors** — minimums are asserted in the build where possible
   (`cmake_minimum_required(... )`) so the floor can't silently drift from the
   docs. The Python 3.8 floor is checked in `host/python/cmake/UbertoothPythonFloor.cmake`:
   an older interpreter skips the Python tools with a warning, or fails the
   configure with `-DREQUIRE_ALL=ON`.
2. **CI matrix = the "tested" column** — the "Tested / known-good" entries above
   must mirror the pinned full-tier CI matrix in `.github/workflows/build.yml`,
   which runs weekly, on a published release, or manually. Pull requests run
   only Ubuntu 24.04 and macOS 26, so the minimum versions are exercised by the
   weekly run, not on every PR. Pin explicit versions (not `-latest`) so the
   record is reproducible, and test the **minimum and the newest** of each
   dimension. Three narrower workflows cover one thing each on their own
   triggers: `install-sh.yml` (Ubuntu 24.04 and macOS 15, when `install.sh`
   changes), `libbtbb-pin.yml` (Ubuntu 24.04, on every change with no path
   filter at all, since the files that must agree on the pin include this one
   and the build guides) and `manpages.yml` (Ubuntu 24.04, when `host/doc`
   changes), which installs the pinned ronn-ng and fails if the committed man
   pages no longer match their Markdown. One job in `build.yml` builds the
   host tools a second way: `host-installed-libubertooth` (Ubuntu 24.04, when
   the host changed) installs libubertooth and then configures, builds and
   tests `host/ubertooth-tools` on its own, which is what `FindUBERTOOTH.cmake`
   is for. Linux alone, because what it covers — which include directory a
   target is given — is the same on every platform.
3. **Hardware-only claims** — anything about radio behaviour (BLE/BR-EDR
   capture) is only "tested" when run on a real Ubertooth per [TEST_PLAN.md](TEST_PLAN.md).
