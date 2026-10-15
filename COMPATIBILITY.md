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
| **Python (host tools)** | 3.8 | 3.8 (U20 CI) … 3.14 (macOS CI) | 2.x (removed upstream) | ✅ 3.8 verified in CI; see setuptools note |
| **GCC (host)** | TBD | per-runner (recent) | — | 🎯 not yet characterized |
| **arm-none-eabi-gcc (firmware)** | TBD | U20 (~9.x) … 15.3 (CI) | — | 🎯 floor not pinned, but builds on old+new |
| **libusb** | 1.0.x | 1.0.30 (local) | pre-1.0 | 🎯 exact floor not pinned |
| **libbtbb** | 2020-12-R1 | current master (C lib) | older than 2020-12-R1 | 🎯 |

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

### CMake 4.x
Modern CMake (4.x) removed compatibility with `cmake_minimum_required < 3.5`.
This fork declares `cmake_minimum_required(VERSION 3.5...3.27)`; the companion
`libbtbb` dependency still needs `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` to
configure under CMake 4 (handled in CI).

### Python 3 only
Python 2 support was removed upstream in 2020-12-R1. This fork targets modern
Python 3; verified from 3.8 (Ubuntu 20.04) through 3.14 (current macOS runner).
The specan_ui `setup.py` must build cleanly across that range (see the open
setup.py / setup.py.in divergence issue). Older distros (e.g. Ubuntu 18.04 /
Python 3.6) are out of scope — see above.

**setuptools required on Python 3.12+.** Python 3.12 removed the bundled
`setuptools`/`distutils` (PEP 632), so building specan_ui needs setuptools
installed explicitly (`pip install setuptools`). On distros that still package
it (e.g. `python3-setuptools` on Debian/Ubuntu) install that; on current macOS
runners it is pip-installed in CI.

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
      (Firmware now builds on both U20's old arm-gcc and 15.3; exact floor TBD.)
- [ ] Pin a libusb minimum and verify.
- [ ] Add `python_requires` to specan_ui packaging reflecting the 3.8 floor.
- [ ] Replace remaining 🎯 with ✅ (and a CI link) or ❌ as results come in.
