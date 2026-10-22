# Compatibility and Support

This document states which platforms and tool versions this fork **requires**,
which it has been **tested against**, and which are **not supported**.

> **Honesty rule for this doc:** a cell is only marked *Verified* if it was
> actually exercised — by a CI run or on real hardware (see `TEST_PLAN.md` /
> `TEST_LAB.md`). Requirements we intend to meet but have **not yet verified**
> are marked *Target (unverified)*. We do not claim support we have not proven;
> the whole reason this fork exists is that the upstream 2020-12-R1 release
> *claimed* to work on modern systems and did not.

Legend:
- ✅ **Verified** — exercised by CI or hardware, known good.
- 🎯 **Target (unverified)** — intended to be supported; not yet tested. Must
  not be advertised as working until verified.
- ❌ **Unsupported** — explicitly not supported; will not be fixed.

---

## Support matrix

| Dimension | Minimum (required) | Tested / known-good | Unsupported | Status of minimum |
|---|---|---|---|---|
| **Ubuntu** | 20.04 LTS | 20.04, 22.04, 24.04 (CI, host+fw) | ≤ 18.04 (use 2020-12-R1) | ✅ 20.04 verified in CI (container) |
| **macOS** | 14 (Sonoma) | 14, 15, 26 / arm64 (CI, host+fw) | ≤ 13 | ✅ 14 verified in CI |
| **Windows** | — | — | all | ❌ Unsupported (libusb + udev; use Linux/VM) |
| **CMake** | 3.5 | 3.5 … 3.27 (enforced range) | < 3.5 (CMake 4 removed it) | ✅ floor enforced in CMakeLists |
| **GNU make (firmware)** | 3.81 | 3.81 (macOS), 4.3 (Linux) | — | ✅ 3.81 verified; build avoids make ≥ 4.0 features |
| **Python (host tools)** | 3.8 | 3.8 (U20 CI) … 3.14 (macOS CI) | 2.x (removed upstream) | ✅ 3.8 verified in CI |
| **GCC (host)** | TBD | per-runner (recent) | — | 🎯 not yet characterized |
| **arm-none-eabi-gcc (firmware)** | TBD | U20 (~9.x) … 15.3 (CI); 15.3 on hardware | — | 🎯 floor not pinned; builds + runs on old+new |
| **libusb** | 1.0.x | 1.0.30 (local) | pre-1.0 | 🎯 exact floor not pinned |
| **libbtbb** | 1.1 | SqueakyFloors `fix/le-long-packets` (`ee48492`) | ≤ 1.0 lacks long-LE decoder API | ✅ paired macOS + Ubuntu builds/tests |

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
code should build on Intel (the Homebrew include path uses `$(brew --prefix)`,
which resolves to `/usr/local` on Intel), but this is unverified. Test locally
if you build on an Intel Mac.

### CMake 4.x
Modern CMake (4.x) removed compatibility with `cmake_minimum_required < 3.5`.
This fork declares `cmake_minimum_required(VERSION 3.5...3.27)`; the companion
`libbtbb` dependency still needs `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` to
configure under CMake 4 (handled in CI).

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

---

## How these claims are kept honest

1. **Enforced floors** — minimums are asserted in the build where possible
   (`cmake_minimum_required(... )`; add `python_requires` to setup metadata) so
   the floor can't silently drift from the docs.
2. **CI matrix = the "tested" column** — the "Tested / known-good" entries above
   must mirror the pinned CI matrix in `.github/workflows/build.yml`. Pin
   explicit versions (not `-latest`) so the record is reproducible, and test the
   **minimum and the newest** of each dimension.
3. **Hardware-only claims** — anything about radio behaviour (BLE/BR-EDR
   capture) is only "tested" when run on a real Ubertooth per `TEST_PLAN.md`.

## TODO before this matrix can be trusted
- [x] Add pinned Ubuntu 20.04 (container) / 22.04 / 24.04 to CI; verified green.
- [x] Add pinned macOS versions (14/15/26); verified green.
- [ ] Characterize GCC (host) and arm-none-eabi-gcc floors (oldest that builds).
      (Firmware builds on both U20's old arm-gcc and 15.3, and 15.3 output runs
      on real hardware; exact oldest floor still TBD.)
- [ ] Pin a libusb minimum and verify.
- [x] Verify the firmware build on GNU make 3.81 (the version shipped with
      macOS). Verified: all three firmware images build, track the git revision
      across incremental rebuilds, no-op cleanly, and regenerate a missing
      version.h, on make 3.81 and 4.3; the build avoids make ≥ 4.0 features.
      Also verified end-to-end on hardware (build → flash → on-device `-v`
      matches) on macOS and the Linux bench.
- [ ] Add `python_requires` to specan_ui packaging reflecting the 3.8 floor.
- [ ] Replace remaining 🎯 with ✅ (and a CI link) or ❌ as results come in.
