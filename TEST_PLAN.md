# Ubertooth Test Plan

Hardware required: Ubertooth One, Linux machine (Ubuntu 22.04+ preferred), a BLE device
(phone, fitness tracker, etc.) and a classic Bluetooth device for BR/EDR tests.

---

## 1. Build Verification

### 1a. Clean build on Ubuntu 22.04
- [ ] Clone repo fresh on Ubuntu 22.04
- [ ] Install dependencies: `sudo apt install cmake gcc libusb-1.0-0-dev python3-setuptools`
- [ ] Build libbtbb from this repo's companion (or upstream)
- [ ] Run `cmake .. && make` in `host/build/`
- [ ] Confirm: no errors, no warnings about CMake version
- [ ] Confirm: `ubertooth-util` binary exists in build output

### 1b. Clean build on Ubuntu 24.04
- [ ] Same steps as 1a on Ubuntu 24.04
- [ ] Confirm: no `InvalidVersion` setuptools error
- [ ] Confirm: no `distutils` import error
- [ ] Confirm: specan_ui builds without errors

### 1c. Clean build on macOS (Homebrew)
- [ ] `brew install cmake libusb pkg-config`
- [ ] Build libbtbb with local prefix
- [ ] Build host tools
- [ ] Confirm: binaries run (even without device attached — "could not open Ubertooth" is OK)

---

## 2. Basic Device Communication

*Requires: Ubertooth plugged in via USB*

### 2a. Firmware version check
- [ ] Run `ubertooth-util -v`
- [ ] Confirm: returns `Firmware version: 2020-12-R1 (API:1.07)`
- [ ] Confirm: no `usb_claim_interface` errors
- [ ] Confirm: no timeout errors

### 2b. Compile info
- [ ] Run `ubertooth-util -V`
- [ ] Confirm: returns compile info without crashing

### 2c. LED test
- [ ] Run `ubertooth-util -I` (flash all LEDs)
- [ ] Confirm: LEDs flash on the device visually

### 2d. Hardware info
- [ ] Run `ubertooth-util -b` (board ID)
- [ ] Run `ubertooth-util -p` (part ID)
- [ ] Run `ubertooth-util -s` (serial number)
- [ ] Confirm: all return values without errors

---

## 3. BLE Sniffing (tests cherry-picked PR #550 — CRC fix)

*Requires: a BLE advertising device nearby (phone with BT on is sufficient)*

### 3a. Basic BLE advertising capture
- [ ] Run `ubertooth-btle -f` for 10 seconds, Ctrl+C
- [ ] Confirm: packets are captured (non-zero output)
- [ ] Confirm: no hangs or crashes

### 3b. CRC verification mode (the PR #550 fix)
- [ ] Run `ubertooth-btle -v1 -f` for 10 seconds
- [ ] Confirm: fewer packets than without `-v1` (bad-CRC packets are now filtered)
- [ ] Confirm: no crash
- [ ] Compare packet count with and without `-v1` — `-v1` should be equal or fewer

### 3c. BLE PCAP capture
- [ ] Run `ubertooth-btle -f -c capture.pcap` for 10 seconds
- [ ] Confirm: `capture.pcap` file is created and non-empty
- [ ] Open in Wireshark and confirm valid BLE packets visible

---

## 4. Spectrum Analyzer (tests specan_ui Python fixes)

### 4a. CLI spectrum analyzer
- [ ] Run `ubertooth-specan`
- [ ] Confirm: starts without Python errors
- [ ] Confirm: outputs spectrum data

### 4b. Spectrum analyzer UI
- [ ] Run `ubertooth-specan-ui`
- [ ] Confirm: GUI launches without `ModuleNotFoundError: No module named 'specan'`
- [ ] Confirm: spectrum data displayed

---

## 5. Classic Bluetooth / BR/EDR (tests cherry-picked PR #540 — paging improvement)

*Requires: a classic BT device (not BLE-only)*

### 5a. Basic BR/EDR capture
- [ ] Run `ubertooth-rx` for 10 seconds
- [ ] Confirm: no crash, some output

### 5b. Paging (PR #540 fix)
- [ ] Run `ubertooth-btbr` targeting a known classic BT device BD_ADDR
- [ ] Confirm: starts without hanging (tests issue #520 — if it still hangs, note it)
- [ ] Confirm: packets captured after paging completes

---

## 6. RSSI (tests cherry-picked PR #481)

- [ ] Run `ubertooth-btle -f` and observe RSSI values in output
- [ ] Confirm: RSSI values are in a plausible range (-100 to 0 dBm)
- [ ] Confirm: no out-of-bounds channel values in RSSI output

---

## 7. Scapy Integration (tests cherry-picked PR #519)

*Requires: Python scapy installed (`pip3 install scapy`)*

- [ ] Run the pcapdump script from `host/python/extcap/` against a capture file
- [ ] Confirm: no `PcapWriter` deprecation warnings
- [ ] Confirm: output is valid

---

## 8. Stress / Stability

### 8a. Long-running BLE capture (tests memory leak issues #542, #543)
- [ ] Run `ubertooth-btle -f` for 30 minutes
- [ ] Monitor memory usage: `watch -n 5 'ps aux | grep ubertooth'`
- [ ] Confirm: RSS memory does not grow unboundedly
- [ ] Note: if memory grows consistently, memory leaks are NOT fixed yet

### 8b. Device reconnection
- [ ] Start `ubertooth-btle -f`
- [ ] Unplug and replug Ubertooth
- [ ] Confirm: tool detects disconnect and exits cleanly (rather than hanging)

---

## 9. Deferred / Untested PRs

These branches exist in the repo but are NOT in master yet:

### PR #488 — Serial number support (`feature/serial-number-support`)
- Allows selecting a specific Ubertooth by serial number with `-U <serial>`
- Test: plug in two Ubertooth devices, confirm each can be addressed independently
- Risk: large change (68 files), not yet reviewed

### PR #362 — Capture to stdout (`feature/capture-to-stdout`)
- Adds `-c -` to pipe PCAP to stdout (for piping into Wireshark live)
- Test: `ubertooth-btle -f -c - | wireshark -k -i -`
- Risk: large change (207 files), includes unrelated deletions — needs cleanup before merge

---

## Notes

- Tests 3b, 5b, 6 directly exercise the cherry-picked fixes — these are the most important to run
- If ubertooth-btbr still hangs (test 5b), open an issue — issue #520 is not yet fixed
- Memory leak tests (8a) require extended time but are important for production use
- Document any unexpected behavior as a new issue in the repo
