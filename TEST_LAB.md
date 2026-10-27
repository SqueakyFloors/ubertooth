# Ubertooth Test Lab Design

Planning notes for a permanent hardware test rack that validates the host tools
and firmware across operating systems and runs continuous (24h+) sniffer soak
tests with data-stream sampling.

This complements [TEST_PLAN.md](TEST_PLAN.md) (the manual per-release checklist). This doc is
about the *infrastructure* to run that checklist repeatedly and automatically.

> Status: design notes / proposal. Nothing here is built yet. Open items are
> marked **TODO** and are good candidates for GitHub issues.

---

## 1. What actually has to be tested (drives everything below)

From the codebase:

- **Host tools** — 15 CLI binaries (`ubertooth-util/-btle/-rx/-scan/-specan/-afh/
  -dfu/-ego/-follow/-dump/-debug/-ducky/-tx`) + 2 Python UIs (`specan_ui`,
  Wireshark `extcap`). Built with CMake + libusb + libbtbb; specan needs fftw.
- **Firmware** — two images, `bluetooth_rxtx` and `btbr`, cross-compiled with
  `arm-none-eabi-gcc`. **Not** in CI today. One image at a time per device.
- **Radio behaviour** — BLE sniffing, BR/EDR sniffing/paging, spectrum analyzer,
  AFH map detection, RSSI. These need **real Bluetooth traffic** and a real
  Ubertooth; they cannot be faked or unit-tested in software.
- **Modern-OS regressions** — the whole reason this fork exists: CMake 4.x,
  Python 3.12 setuptools `InvalidVersion`, distutils removal. The OS matrix must
  include **both old and new** distros to catch breakage in either direction.

Implication: testing splits cleanly into two tiers:
1. **Build/static tier** — pure software, parallelizable across OSes, no radio.
2. **Device/radio tier** — needs physical Ubertooth hardware + BT emitters, and
   is where the 24h soak lives.

---

## 2. Minimum vs. recommended machine matrix

### Build/static tier (no Ubertooth needed)

These can be VMs or containers — they only compile and run `--help`/version
smoke checks. CI (GitHub Actions) already covers macOS + Ubuntu-latest for the
host build; the gap is **old + new** distros and firmware.

| Role | Minimum | Recommended | Why |
|------|---------|-------------|-----|
| Linux "new" | Ubuntu 24.04 | +Fedora latest, Arch | Catches CMake4 / py3.12 / setuptools breakage |
| Linux "old" | Ubuntu 20.04 | +Debian stable, 22.04 | Catches regressions that break old toolchains |
| macOS | 1 current macOS | +1 Intel + 1 Apple-Silicon | libusb path differs; `/opt/homebrew` vs `/usr/local` |
| Firmware build | any Linux w/ `arm-none-eabi-gcc` | pinned toolchain version | Firmware not in CI; `-O2`/gcc-version sensitive (upstream PR #191/#409) |
| Windows | *out of scope* | — | AI_CONTEXT: not supported (libusb/udev). Document as unsupported. |

Minimum build tier = **effectively 0 dedicated machines** if you lean on CI +
containers for Linux and 1 Mac. Containers cover the Linux OS spread cheaply
(see §5). A dedicated firmware-build host is only needed if you don't want the
ARM toolchain in CI — **TODO: add firmware build to CI** (biggest single win).

### Device/radio tier (needs physical hardware)

Radio tests must run on **bare metal with USB passthrough** — USB + timing
sensitivity (DFU re-enumeration, bulk transfer timing) makes VMs unreliable.
Recommended minimum:

| Node | OS | Attached | Purpose |
|------|----|---------| --------|
| rx-linux-1 | Ubuntu 22.04 | 1-2 Ubertooth One | Primary BLE/BR soak + functional |
| rx-linux-2 | Ubuntu 24.04 | 1 Ubertooth One | New-OS radio regression |
| rx-mac-1 | current macOS | 1 Ubertooth One | macOS radio path (lower priority) |

Small, cheap SBCs (e.g. Intel NUC / Raspberry Pi 4+ on arm64 — note arm64 host
is itself a useful portability signal) are ideal rack nodes: low power, run 24/7.

---

## 3. Bluetooth emitters (the "device under observation" side)

Radio tests are meaningless without controlled BT traffic. You need emitters,
not just sniffers. Minimum coverage:

| Emitter type | Example | Exercises |
|--------------|---------|-----------|
| BLE advertiser | spare phone, BLE dev board (nRF52 dongle), fitness band | `ubertooth-btle`, RSSI, specan |
| BLE connection | two nRF52 boards running a known connection | `-f` follow mode, CRC filter (PR #550) |
| Classic BR/EDR | old BT headset / BT-classic dongle | `ubertooth-rx`, `ubertooth-btbr` paging (PR #540) |
| Controllable/scriptable | nRF52 + Zephyr, or a Linux host with BlueZ advertising | reproducible, scriptable traffic for CI assertions |

Key point: for **repeatable automated assertions** you want a *scriptable*
emitter (nRF board or a BlueZ host you control), not just "a phone nearby."
Ambient BT is fine for soak/liveness but not for "expect exactly N packets."

**RF isolation TODO:** for deterministic results, consider a shielded enclosure
(RF test box / Faraday bag) around an emitter+Ubertooth pair so ambient 2.4GHz
(WiFi, other BT) doesn't skew packet counts. Minimum viable: run the rack in a
relatively quiet RF area and treat counts as ranges, not exact values.

---

## 4. The 24-hour soak + data sampling

Goal (from [TEST_PLAN.md](TEST_PLAN.md) section 8): run sniffers continuously, watch for
memory growth (issues #542/#543), crashes, hangs, and USB dropouts.

Design:

- **Supervised long-run process** per device: a wrapper that launches
  `ubertooth-btle -f` (and/or `ubertooth-rx`) under a process supervisor
  (`systemd` service, or `supervisord`) so it restarts on crash and logs *why*.
- **Sampling sidecar** every N seconds, recorded to a timeseries:
  - process RSS/VSZ (`ps`/`/proc/<pid>/status`) -> memory-leak detection
  - packet throughput (count/sec parsed from tool output or pcap size growth)
  - USB error counters (dmesg / libusb transfer errors)
  - device still enumerated? (`lsusb` for 1d50:6002)
- **Rolling pcap capture** with size/time rotation so 24h doesn't fill the disk;
  sample (e.g. keep 1 min out of every 10) if full retention is too large.
- **Pass/fail criteria**, codified:
  - RSS slope over 24h ~ flat (define a threshold, e.g. <5% growth) -> leak gate
  - zero unhandled crashes / zero hangs (watchdog: no output for X min = fail)
  - packet throughput stays within expected band for the emitter
- **Watchdog**: a timer that flags "no packets in X minutes" or "process gone"
  and captures a diagnostic bundle (last logs, dmesg, memory sample).

**TODO:** write the supervisor + sampler as a small Python script (stdlib +
`psutil`) committed under `tools/soak/`. Keep it dependency-light so it runs on
every node identically. Output: a CSV/JSON timeseries + a one-page summary.

---

## 5. Deploy & orchestration - how to run it efficiently

Tiered to match section 2:

### Build/static tier -> containers + CI
- **GitHub Actions** already builds host on macOS + Ubuntu-latest. Extend the
  matrix to old/new Ubuntu + Fedora via containers, and **add the firmware
  build** (`arm-none-eabi-gcc` is apt-installable in the runner). This is the
  cheapest, highest-leverage automation and needs no owned hardware.
- For local repro, a `docker/` dir with one Dockerfile per target OS lets you
  reproduce a distro's build without owning a box. **TODO.**

### Device/radio tier -> Ansible over the rack
- VMs/containers can't do USB-timing radio tests reliably, so the rack is bare
  metal. **Ansible** is the right fit (agentless, SSH, good for a fixed fleet):
  - inventory = the rack nodes (rx-linux-1, rx-linux-2, rx-mac-1)
  - a `site.yml` that: installs deps, clones/builds libbtbb + host tools at a
    given git ref, flashes a given firmware image via `ubertooth-dfu`, installs
    the soak systemd service, and starts it.
  - a `collect.yml` that pulls back each node's timeseries + logs to a central
    results dir (or an S3 bucket / small InfluxDB+Grafana if you want dashboards).
- Why Ansible over pure Python scripts: idempotent, inventory-driven, parallel
  across nodes, and the firmware-flash + build steps are naturally expressed as
  ordered tasks. Python scripts are better for the *sampler* (runs on-node); use
  both - Ansible to deploy, Python to sample.

### Results aggregation
- Minimum: `rsync`/`ansible fetch` CSVs to one host, a script that renders a
  per-run markdown summary (pass/fail per node + memory slope + crash count).
- Nicer: push samples to InfluxDB/Prometheus, visualize in Grafana (memory over
  time per device makes leaks obvious at a glance). **TODO: decide tier.**

---

## 6. Minimum viable lab (if starting tomorrow)

The smallest thing that meaningfully tests this fork:

1. **1 Linux node** (Ubuntu 22.04, Intel NUC/Pi) + **2 Ubertooth One** devices.
2. **1 scriptable BLE emitter** (nRF52 dongle) + **1 classic BT device**.
3. **CI matrix extended** to Ubuntu 20.04/24.04 + macOS + **firmware build**
   (no owned hardware needed for this part).
4. **Soak script** (`tools/soak/`) running `ubertooth-btle -f` under systemd on
   the one node, sampling RSS + throughput, with a flat-memory pass gate.

That covers: build portability (CI), old+new OS radio behaviour (1 node can
dual-boot or you add a 2nd later), the memory-leak soak, and the two-device case
for the serial-number feature (PR #488). Everything else (macOS radio node, RF
shielding, Grafana, multi-distro radio) is incremental.

---

## 7. Open questions / TODO (candidates for GitHub issues)

- [ ] Add firmware (`arm-none-eabi-gcc`) build to CI - closes the biggest gap.
- [ ] Write `tools/soak/` supervisor + sampler (Python, psutil).
- [ ] Define concrete pass/fail thresholds (memory slope %, min throughput band).
- [ ] Ansible inventory + `site.yml`/`collect.yml` for the rack.
- [ ] `docker/` per-OS build images for local repro of distro-specific breakage.
- [ ] Decide results tier: flat CSV vs InfluxDB/Grafana.
- [ ] RF isolation plan (shielded box vs quiet-area + tolerance ranges).
- [ ] Scriptable emitter setup (nRF52 + Zephyr/BlueZ) for deterministic counts.
- [ ] Confirm macOS radio-tier priority (is macOS a supported *runtime* target,
      or build-only? AI_CONTEXT lists macOS as a build target; radio behaviour
      on macOS is less proven).
