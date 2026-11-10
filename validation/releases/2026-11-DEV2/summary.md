# Validation summary: 2026-11-DEV2

| | |
|---|---|
| Fork commit | `4b28aec9fc56a117e7a7a7ca03c5cc7da3bd453a` (`2026-11-DEV2`) |
| libbtbb commit | `d356e9da7fb03969371aa14d603a8aad2f0ef198` |
| Harness commits | `f24e902` for initial focused runs; `75eafaa` for corrected fast, release, and soak runs |
| Tool and firmware versions | Host tools and firmware both reported `2026-11-DEV2`; libbtbb reported `2020-12-R1+79` |
| Hosts | Linux x86-64 hardware bench |
| Test interval (UTC) | 2026-11-08 18:06 through 2026-11-09 06:12 |
| Verdict | **Not yet qualified**: Classic comprehensive, HCI follow, corrected `btbr`, installer, and bootloader acceptance remain |
| Full sanitized logs | Pending harness issue [#413](https://github.com/SqueakyFloors/ubertooth-test-harness/issues/413); unredacted records remain on the test host |

## Required gates

| Gate | What was run | Result | Notes |
|---|---|---|---|
| Fast | `fast.yaml` / `fast` | **Pass**, 25/25 tests and 17/17 preflight checks, 0h 19m 36s | Corrected rerun after the harness adopted DEV2's two-line version output |
| Main non-soak release | `release.yaml` / `release` | **Pass**, 27/27 tests and 19/19 preflight checks, 1h 13m 57s | Complete run record says passed; an outer wrapper expired after the run had recorded its verdict |
| Alternate `btbr` image | `classic.yaml` / `btbr`, twice | **Not qualified**, 17/20 checks in both pre-fix runs | The harness assumptions were corrected in harness PR #411; a post-fix rerun remains |
| Focused BLE | `ble.yaml` / `ble-comprehensive` | **Pass**, 4/4 tests and 19/19 preflight checks, 0h 10m 49s | 27 cases |
| Focused Classic | `classic.yaml` / `classic-comprehensive`, twice | **Fail** | Individual peer-link capture windows collapsed while adjacent windows were healthy; harness reporting and peer reliability work continue |
| Focused HCI | `hci.yaml` / `hci-comprehensive` | **Blocked by host privilege** | Inquiry passed; follow needs the narrowly scoped `CAP_NET_ADMIN` mechanism added in harness PR #419, then a rerun |
| Weak-link checks | BLE, Classic, and connection weak-link suites | **Pass** | BLE 2/2, Classic 1/1, connection 2/2; no blocked or skipped tests |
| Firmware and DFU acceptance | Firmware built, flashed, and read back by fast and release runs | **Pass for application firmware** | Selected boards reported `2026-11-DEV2` after flashing |
| Installer acceptance | Not run | **Missing** | Required before qualification |
| Bootloader acceptance | Not run | **Missing** | Required on the programmer-paired fixture before qualification |

## Measurements

### Four-hour address soak

`address-soak.yaml` / `address-soak-4h` passed in 4h 01m 44s, including 18/18 preflight checks.

- 239/239 advertiser-on periods detected by both independent readers
- 100% first-detection and sustained-presence share
- 59,506 target advertisements found by each reader
- Zero reader-window disagreements
- Zero process restarts, unexplained exits, forced stops, board resets, and transfer errors
- Maximum in-period packet gap: 0.630 seconds
- Mean target rate: 496.3 packets/minute
- Capture memory grew from 4,956 KiB and stabilized near 73 MiB
- Both capture attempts ended cleanly with SIGTERM
- Device identity and state checks passed afterward

### Weak-link checks

| Suite | Result | Duration |
|---|---:|---:|
| BLE weak link | 2/2 | 0h 4m 26s |
| Classic weak link | 1/1 | 0h 1m 59s |
| Connection weak link | 2/2 | 0h 2m 5s |

## Deviations and open work

- Classic comprehensive remains unqualified because controlled peer links did not produce useful traffic in every required capture window. No result identifies a DEV2 receive-path defect: the same board and build captured hundreds or thousands of records in adjacent windows.
- HCI follow was denied by the host's missing capability. The candidate proceeded when run with the needed privilege, and an older build failed identically without it.
- The first two `btbr` runs exposed stale harness assumptions. A diagnostic showed the board remained connected while the current peer volunteered no traffic; the corrected harness needs a release rerun.
- Installer and programmer-paired bootloader acceptance have not run.

No completed result currently identifies a defect in the DEV2 source. The missing and failed required gates prevent a qualified verdict.

## Evidence status

A concise status was posted to fork issue [#1240](https://github.com/SqueakyFloors/ubertooth/issues/1240). The first public log export refused compiler path fragments that appeared absolute after sanitization. That fail-closed behavior prevented publication of a bundle that had not passed the privacy gate. Harness issue [#413](https://github.com/SqueakyFloors/ubertooth-test-harness/issues/413) tracks the correction. `logs.tar.zst` and `SHA256SUMS` will be added here only after the sanitizer accepts and the files are reviewed.
