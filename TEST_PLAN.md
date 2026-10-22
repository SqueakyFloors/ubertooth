# Ubertooth Test Plan

Hardware required: Ubertooth One, Linux machine (Ubuntu 22.04+ preferred), a BLE device
(phone, fitness tracker, etc.) and a classic Bluetooth device for BR/EDR tests.

---

## 1. Build Verification

### 1a. Clean build on Ubuntu 22.04
- [ ] Clone repo fresh on Ubuntu 22.04
- [ ] Install dependencies: `sudo apt install cmake gcc libusb-1.0-0-dev python3 python3-numpy python3-qtpy python3-pyqt5 python3-usb1`
- [ ] Build libbtbb from the pinned fork commit (see [the build guide](docs/source/build_guide.rst))
- [ ] Run `cmake .. && make` in `host/build/`
- [ ] Confirm: no errors, no warnings about CMake version
- [ ] Confirm: `ubertooth-util` binary exists in build output
- [ ] Install to a temporary `CMAKE_INSTALL_PREFIX`; with `PYTHONPATH` unset,
      confirm the installed `ubertooth-specan-ui --help` and
      `ubertooth-btbr -h` reach their normal usage output (no import error)

### 1b. Clean build on Ubuntu 24.04
- [ ] Same steps as 1a on Ubuntu 24.04
- [ ] Confirm: Python installation does not modify the source tree or system
      Python outside `CMAKE_INSTALL_PREFIX`
- [ ] Confirm: a `DESTDIR` install stages modules and launchers under
      `DESTDIR + CMAKE_INSTALL_PREFIX`, and staged launchers run without
      `PYTHONPATH`

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
- [ ] Confirm: returns `Firmware version: <revision> (API:1.32)` (firmware
      built from this tree; the host tools refuse any other API version)
- [ ] Confirm: no `is in use by another program` or `claiming the USB
      interface` errors
- [ ] Confirm: no timeout errors

### 2b. Compile info
- [ ] Run `ubertooth-util -V`
- [ ] Confirm: returns compile info without crashing

### 2c. LED test (identify)
- [ ] Run `ubertooth-util -I` (flash all LEDs)
- [ ] Confirm: the command returns at once, and the LEDs keep flashing
- [ ] Run `ubertooth-util -d` a few times while it blinks
- [ ] Confirm: the board still answers, and the values change over time
- [ ] Run `ubertooth-util -S`
- [ ] Confirm: the blink stops and all three LEDs end off (`-d` reads 0 0 0)
- [ ] Run `ubertooth-util -I` then `ubertooth-util -x`
- [ ] Confirm: identify gives way to the xmas pattern; `-S` ends that too
- [ ] With firmware of another API version (for example 2020-12-R1): run
      `ubertooth-util -I` and `ubertooth-specan`
- [ ] Confirm: each prints an API version error and exits 1; nothing blinks

### 2d. Hardware info
- [ ] Run `ubertooth-util -b` (board ID)
- [ ] Run `ubertooth-util -p` (part ID)
- [ ] Run `ubertooth-util -s` (serial number)
- [ ] Confirm: all return values without errors

### 2e. Bootloader refuses a read-back past the end of flash

*Needs a board whose part is smaller than 512 KiB (`ubertooth-util -p` reports
0x25011722 for the 128 KiB LPC1754) and ISP or JTAG: `ubertooth-dfu` cannot
write a bootloader.*

- [ ] Install `firmware/bootloader/bootloader.hex` over ISP or JTAG
- [ ] Put the board in DFU and send `REQ_UPLOAD` (class request 2) with wValue
      512, which is 0x20000, the first block past a 128 KiB part
- [ ] Confirm: the request is refused and `REQ_GETSTATUS` reports status 0x08
      (`ERRADDRESS`); the board still answers `REQ_GETSTATE`
- [ ] Send `REQ_CLRSTATUS`, then run `ubertooth-dfu -U <serial> -u <file> -r`
- [ ] Confirm: the read-back still returns all 114688 bytes above the
      bootloader, and the board comes back running its application

### 2f. Bootloader refuses an image past the end of flash

*Same board and the same ISP or JTAG requirement as 2e. `ubertooth-dfu -d`
refuses an oversize image before it sends anything, so these steps send the
requests directly.*

- [ ] First, with nothing clever: flash the application with
      `ubertooth-dfu -U <serial> -d bluetooth_rxtx.dfu -r`
- [ ] Confirm: it succeeds and `ubertooth-util -U <serial> -v` reports the
      revision just written. This is the step that matters most, because the
      bootloader now reports a failed program instead of discarding it, and
      that is the only change that affects an ordinary flash
- [ ] Put the board in DFU and send `REQ_DNLOAD` (class request 1) with a
      256-byte block and a wValue past the end of the part's flash: 512, which
      is 0x20000, on a 128 KiB part. On a larger part use its own figure, or
      that block is flash the part has and will be programmed
- [ ] Confirm: `REQ_GETSTATUS` reports status 0x08 (`ERRADDRESS`), and the
      board still answers `REQ_GETSTATE`
- [ ] Send `REQ_CLRSTATUS`, then flash the application again
- [ ] Confirm: it succeeds, so the refusal left the part usable
- [ ] Not reachable on an Ubertooth: the sector probe, which only runs on a
      part the bootloader's table has no entry for. Every board shipped is
      built from a listed part

### 2g. Flashing back off the btbr image

*Needs a board that can be reached at the bench: a run that leaves it in the
bootloader needs the entry pin or a power cycle.*

- [ ] Run `ubertooth-dfu -U <serial> -d firmware/btbr/btbr.dfu -r`
- [ ] Run `ubertooth-dfu -U <serial> -d firmware/bluetooth_rxtx/bluetooth_rxtx.dfu -r`
- [ ] Confirm: the second flash exits 0, with no `DFU entry failed`; the board
      comes back and `ubertooth-util -v` reports the revision just written
      (`-r` reads the revision off the board directly, so `btbr`'s fixed
      `bcdDevice` of 1.06 does not get in the way)
- [ ] Flash `btbr.dfu` again, start `ubertooth-btbr`, and flash
      `bluetooth_rxtx.dfu` while it is capturing
- [ ] Confirm: the same result, and the board does not stop answering

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

### 3d. Data Length Extension packet capture
*Requires: a DLE-capable BLE central and peripheral (recommended: two scripted
nRF52840 DKs) plus one Ubertooth.*
- [ ] Have the endpoints negotiate DLE and send deterministic 100-byte and
      251-byte data-channel payloads
- [ ] Run `ubertooth-btle -f -r long-packets.pcapng` before establishing the
      connection, so the CONNECT_IND and subsequent data-channel traffic are
      captured
- [ ] Confirm: PCAPNG contains complete packets over 50 bytes, including the
      expected payload pattern and CRC (no truncation)
- [ ] Confirm: short/legacy advertising packets remain unchanged

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

### 4c. LED spectrum analyzer threshold
- [ ] Run `ubertooth-util -q 120`, then `ubertooth-util -d` a few times
- [ ] Confirm: LEDs light (ambient noise is above -120 dBm)
- [ ] Run bare `ubertooth-util -q` (-85 dBm)
- [ ] Confirm: an LED lights only with Wi-Fi traffic on its channel (TX: 1, RX: 6, USR: 11)
- [ ] Run `ubertooth-util -q 20`
- [ ] Confirm: LEDs stay off (-20 dBm is far above ambient signals)
- [ ] Run `ubertooth-util -q 19` and `-q 121`
- [ ] Confirm: each is refused (exit 1)
- [ ] Run `ubertooth-util -S`

### 4d. Stopping a mode resets the board
- [ ] Run `ubertooth-util -c2470 -z-60`, then `-q 120`, then `-S`
- [ ] Run `ubertooth-util -d -C -z -a`
- [ ] Confirm: LEDs 0 0 0, channel 2441 MHz, squelch -120, PA level unchanged
- [ ] Repeat with `ubertooth-specan` stopped by Ctrl+C, then `-d -C -z`
- [ ] Repeat with `ubertooth-btle -s22:44:66:88:aa:cc` (faux slave, transmits)
      stopped by Ctrl+C, then `-d -C -z`
- [ ] Repeat with `ubertooth-btle -p` stopped by Ctrl+C, then `-d -C -z`
      (this one already reset before the fix; it must still do so)
- [ ] Confirm: the same idle state each time

### 4e. Control requests while a mode runs
- [ ] Run `ubertooth-util -q 120`, then `ubertooth-util -d`, `-C` and `-z`
      300 times each, 0.1 s apart, recording the exit status of each
- [ ] Confirm: every read succeeds; no stalled transfer (`-9`), no timeout
      (`-7`), and every `-d` reports all three LEDs
- [ ] Run `ubertooth-util -S`

### 4f. Two mode requests in a row
- [ ] Run `ubertooth-util -c2470 -z-60`, then `-q 120` and `-x` with no `-S`
      between them, then `ubertooth-util -d -C -z`
- [ ] Confirm: channel 2441 MHz and squelch -120, so leaving `-q` reset the
      board before xmas started
- [ ] Repeat the other way round (`-x` then `-q 120`), then `-S`, then
      `-d -C -z`
- [ ] Confirm: LEDs 0 0 0, channel 2441 MHz, squelch -120

### 4g. A new capture holds only its own packets
- [ ] Run `ubertooth-btle -n -A37` for about 3 s and stop it with Ctrl+C
- [ ] Run `ubertooth-util -S`
- [ ] Run `ubertooth-dump -d dump.bin` for about 2 s and stop it with Ctrl+C
- [ ] Read byte 4 (`pkt_type`) of each 68-byte record in `dump.bin`
- [ ] Confirm: every record is 0 (`BR_PACKET`) — no 1 (`LE_PACKET`) left by
      the BLE capture and no 3 (`KEEP_ALIVE`) at the head
- [ ] Repeat about ten times: it used to show two `LE_PACKET` records at the
      head roughly one run in three

### 4h. Modes still start after repeated starts and stops
- [ ] Five times in a row: run `ubertooth-btle -n` for about 3 s, Ctrl+C, then
      `ubertooth-util -S`
- [ ] Five times in a row: run `ubertooth-specan -d -` for about 3 s, Ctrl+C,
      then `ubertooth-util -S`
- [ ] Confirm: every run produces data, and `ubertooth-util -v` still answers
      after the last one
- [ ] Why: the firmware's waits on the radio give up after a bound rather than
      waiting forever, so a bound set too low would show here as a mode that
      starts but produces nothing

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
- [ ] Confirm: Ctrl-C before the `bt>` prompt appears, which is where the hang
      above sits, ends the tool with exit status 1
- [ ] Confirm: Ctrl-C with a command running returns to the `bt>` prompt, and
      `q` then exits

### 5c. AFH channel map detection (issue #238)
*Requires: two classic BT devices with an active connection, for example audio
streaming, so the piconet is transmitting throughout*
- [ ] Find the piconet's LAP and UAP with `ubertooth-rx`
- [ ] Run `ubertooth-afh -l <lap> -u <uap> -t 30`
- [ ] Confirm: the detection phase prints a map with many channels set, and the
      monitor phase that follows prints `+ channel N is used now` /
      `- channel N is not used any more` lines naming several different
      channels — the board is hopping, not parked on one channel (parked, only
      one channel number can ever appear, and the map holds one bit)
- [ ] Run `ubertooth-afh -l <lap> -u <uap> -r -m 30` and confirm the
      once-a-second binary map has bits in different places over successive
      lines, not a single 1 in the same place every time (individual bits
      coming and going is expected, see #194; `-m 30` makes a channel take
      longer to be dropped than the default 5)
- [ ] Interrupt each run with Ctrl-C, confirm exit status 0, then confirm
      `ubertooth-util -d -C -z` reports LEDs 0 0 0, channel 2441 MHz,
      squelch -120

### 5d. What a board carries over between runs (issues #261, #312)
*Requires: the same active piconet as 5c. The channel a mode is on is sampled
with `ubertooth-util -c`, which answers while a capture runs: a few dozen reads
in a row show whether the board is hopping and over which channels.*
- [ ] Run `ubertooth-afh -l <lap> -u <uap> -t 20` and let it find a map with
      clearly fewer than 79 channels, then interrupt it (Ctrl-C)
- [ ] Start `ubertooth-rx -l <lap> -u <uap>`, sample the channel a few dozen
      times while it runs, and confirm: the channels seen cover the whole band,
      including ones the map above had marked unused — the map did not carry
      over
- [ ] Repeat with the `ubertooth-afh` run killed rather than interrupted
      (`kill -9`, so no stop reaches the board and it is left streaming), and
      confirm the same: the capture that follows does not inherit the killed
      run's hopping. `ubertooth-util -S` puts such a board back to idle
- [ ] With no mode running, confirm `ubertooth-util -C 10` then
      `ubertooth-util -c` still reads `2412 MHz` back, and that a mode started
      afterwards runs on it: a channel set on its own is kept, and the next mode
      starts on it unless that tool sets a channel of its own

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

- The host `ctest` suite needs no Ubertooth, and the tests that run the tools
  do not open one, so the suite can run while boards are in use. Those tests
  set `UBERTOOTH_TEST_NO_DEVICES=1`, which makes device selection behave as if
  nothing were attached; nothing else sets it, so normal use is unaffected.
  Everything below still needs real hardware.
- Tests 3b, 5b, 6 directly exercise the cherry-picked fixes — these are the most important to run
- If ubertooth-btbr still hangs (test 5b), open an issue — issue #520 is not yet fixed
- Memory leak tests (8a) require extended time but are important for production use
- Document any unexpected behavior as a new issue in the repo
