# Ubertooth Test Plan

Hardware required: Ubertooth One, Linux machine (Ubuntu 22.04+ preferred), a BLE device
(phone, fitness tracker, etc.) and a classic Bluetooth device for BR/EDR tests.
A second Ubertooth One for the range test (2i).

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
- [ ] Confirm: binaries run (even without a device attached — `error: no
      Ubertooth devices found` is the expected refusal)

---

## 2. Basic Device Communication

*Requires: Ubertooth plugged in via USB*

### 2a. Firmware version check
- [ ] Run `ubertooth-util -v`
- [ ] Confirm: returns `Firmware version: <image> <revision> (API:1.52)` (firmware
      built from this tree; a release build of the host tools refuses any other
      API version and a development build warns about it, and a board whose
      firmware has no info block shows the revision without an image name)
- [ ] Confirm: no `is in use by another program` or `claiming the USB
      interface` errors
- [ ] Confirm: no timeout errors

### 2a-1. Choosing a board, and choosing none
*Requires: two or more Ubertooths attached*
- [ ] Run `ubertooth-util -S` with no `-U`
- [ ] Confirm: it exits 1 with one `error: multiple Ubertooth devices found;
      select one with -U <index|serial>` on standard error, then a
      `  Device <n>: <serial>` line per board, nothing on standard output, and
      no further line after the list
- [ ] Run `ubertooth-util -U 99 -s`
- [ ] Confirm: `error: no Ubertooth at index 99 (<n> attached)` and the line
      about indexes shifting, and nothing after them
- [ ] Run `ubertooth-util -U ffffffffffffffff -s`
- [ ] Confirm: `error: no Ubertooth found with serial matching
      'ffffffffffffffff'`
- [ ] Unplug every board and run `ubertooth-util -s`
- [ ] Confirm: `error: no Ubertooth devices found`, exit 1

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
- [ ] Flash the `btbr` image, which this tree builds, then run
      `ubertooth-util -v`, `ubertooth-util -I` and `ubertooth-specan`
- [ ] Confirm: `-v` names `btbr` at this tree's API version, so the version
      check passes; the other two fail on a request that image does not
      answer, each on stderr with exit 1, and nothing blinks
- [ ] Flash `bluetooth_rxtx` again before the rest of the plan

### 2c-i. Firmware of another API version
Needs a board carrying an older API version. Build `bluetooth_rxtx` from an
earlier commit of this tree whose `UBERTOOTH_API_VERSION` is lower and flash
that; it has a firmware info block, so `ubertooth-dfu` takes it. An older
upstream release such as `2020-12-R1` cannot be used, because `ubertooth-dfu`
refuses an image with no info block and putting one on a board needs ISP or
JTAG.

- [ ] Run `ubertooth-util -I` with host tools configured
      `-DUBERTOOTH_RELEASE=ON` (`-I` is the one ubertooth-util option that
      checks the version; `ubertooth-rx` and the other tools all do)
- [ ] Confirm: `error: Ubertooth firmware API version ...`, the note naming the
      reflash, nothing on stdout, exit 1
- [ ] Run the same command from host tools built from the checkout, with no
      `-DUBERTOOTH_RELEASE`
- [ ] Confirm: the same line as `warning:`, the note saying the command runs
      on because this is a development build, the tool's own result after it,
      exit 0
- [ ] Run it again with `UBERTOOTH_ALLOW_API_MISMATCH=0`
- [ ] Confirm: refused, as a release build refuses it
- [ ] Run a mode the older firmware does not answer
- [ ] Confirm: `error: could not <request>: the board's firmware does not
      support it (stalled, libusb -9)` and `note: the board reports firmware
      API version ..., libubertooth ... requires ...`
- [ ] Reflash the board with this tree's `bluetooth_rxtx` before the rest of
      the plan

### 2d. Hardware info
- [ ] Run `ubertooth-util -b` (board ID)
- [ ] Run `ubertooth-util -p` (part ID)
- [ ] Run `ubertooth-util -s` (serial number)
- [ ] Confirm: all return values without errors

### 2e. Watchdog state
- [ ] Run `ubertooth-util -w` on a freshly plugged-in board
- [ ] Confirm: `Last boot cause: power-on`, the watchdog is armed, and
      0 unrequested resets
- [ ] Run `ubertooth-util -r`, wait for the board, then `-w` again
- [ ] Confirm: `Last boot cause: watchdog` (that is how the part resets) and
      the unrequested count is still 0
- [ ] Reflash with `ubertooth-dfu -d <image>.dfu -r`, then `-w`
- [ ] Confirm: the unrequested count is still 0 — a flash is not a wedge
- [ ] Run `ubertooth-util -x`, leave it blinking for a minute, then `-S` and
      `-w`. This is the tightest margin in the design: xmas mode waits up to
      1600 ms between blinks against a three-second timeout, and it is one of
      the modes whose loop feeds on its own rather than by answering the host.
- [ ] Confirm: the LEDs blink for the whole minute and the unrequested count
      is still 0 — a board working normally is never reset
- [ ] Repeat with `ubertooth-util -I` (identify), which toggles every 167 ms
- [ ] Confirm: the same
- [ ] Run `ubertooth-util -w` while a capture is running (`ubertooth-rx` in
      another shell)
- [ ] Confirm: the last line names a wait of the running mode, not "servicing
      USB" — a reply is always written from the USB path, so that line would
      say nothing
- [ ] Confirm: `Radio waits that gave up since boot:` reads 0 for all four
      counts on a board whose radio is working, before and after a capture

### 2e-1. A mode starts from the same radio state every time

- [ ] Run `ubertooth-debug -U <serial> -r %manand` on a freshly reset board
- [ ] Confirm: `%MANAND = 0xFFFF`, the chip's reset value
- [ ] Run `ubertooth-util -U <serial> -e`, then read `%manand` again
- [ ] Confirm: `0x7FFF` — the running mode's own override
- [ ] Run `ubertooth-util -U <serial> -S`, then read `%manand` again
- [ ] Confirm: `0xFFFF` again, so the next mode inherits nothing
- [ ] Run `ubertooth-specan -U <serial> -t 5` on a freshly reset board, and
      again after a `ubertooth-btle -f` capture has been started and stopped
- [ ] Confirm: both sweeps cover the same range of levels — the sweep no
      longer depends on which mode ran before it

### 2e-2. The on-chip programmer answers the host first

*This leaves the board unusable until its power is cut, so run it last on a
board you can unplug or whose hub port you can power-cycle. Run it once on
`bluetooth_rxtx` and once on `btbr`: each image defers the hand-over in its
own way.*

- [ ] Run `ubertooth-util -U <serial> -i`
- [ ] Confirm: `Entering flash programming (ISP) mode`, then
      `note: the board hands itself to its on-chip programmer and answers
      nothing else until it is power-cycled`, no `libusb -7`, and exit 0
- [ ] Wait a few seconds — the board answers until it hands over, about a
      second on `bluetooth_rxtx` — then run `ubertooth-util -U <serial> -v`
- [ ] Confirm: the board no longer answers — it is in the programmer
- [ ] Power-cycle the board, then `-v` again
- [ ] Confirm: it answers with the firmware it was running

### 2f. Bootloader refuses a read-back past the end of flash

*Needs a board whose part is smaller than 512 KiB (`ubertooth-util -p` reports
0x25011722 for the 128 KiB LPC1754) and ISP or JTAG: `ubertooth-dfu` cannot
write a bootloader.*

*Attach only this board, and run the `ubertooth-dfu` steps below without `-U`:
these steps put the board in DFU by hand first, and DFU mode exposes no serial
number, so an explicit selector looks for an application device that is not
there and the run ends at `DFU entry failed`. See
[`ubertooth-dfu(1)`](host/doc/ubertooth-dfu.md).*

- [ ] Install `firmware/bootloader/bootloader.hex` over ISP or JTAG
- [ ] Put the board in DFU and send `REQ_UPLOAD` (class request 2) with wValue
      512, which is 0x20000, the first block past a 128 KiB part
- [ ] Confirm: the request is refused and `REQ_GETSTATUS` reports status 0x08
      (`ERRADDRESS`); the board still answers `REQ_GETSTATE`
- [ ] Send `REQ_CLRSTATUS`, then run `ubertooth-dfu -u <file> -r`
- [ ] Confirm: the read-back still returns all 114688 bytes above the
      bootloader, and the board comes back running its application

### 2g. Bootloader refuses an image past the end of flash

*Same board and the same ISP or JTAG requirement as 2f. `ubertooth-dfu -d`
refuses an oversize image before it sends anything, so these steps send the
requests directly. The first step starts from the running application, so it
can select the board by serial; the last one starts from DFU, so it must not,
for the reason given in 2f.*

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
- [ ] Send `REQ_CLRSTATUS`, then flash the application again with
      `ubertooth-dfu -d bluetooth_rxtx.dfu -r`, with no `-U`
- [ ] Confirm: it succeeds, so the refusal left the part usable
- [ ] Not reachable on an Ubertooth: the sector probe, which only runs on a
      part the bootloader's table has no entry for. Every board shipped is
      built from a listed part

### 2h. Flashing back off the btbr image

*Needs a board that can be reached at the bench: a run that leaves it in the
bootloader needs the entry pin or a power cycle.*

- [ ] Run `ubertooth-dfu -U <serial> -d firmware/btbr/btbr.dfu -r`
- [ ] Run `ubertooth-dfu -U <serial> -d firmware/bluetooth_rxtx/bluetooth_rxtx.dfu -r`
- [ ] Confirm: each flash exits 0, with no `DFU entry failed`; the board comes
      back and `ubertooth-util -v` reports the image and revision just
      written, `btbr` for the first and `bluetooth_rxtx` for the second
      (`-r` reads both off the board directly). The image name is what makes
      this step a real check: both images carry the same revision, so a write
      that did not take between them would pass on the revision alone
- [ ] Flash `btbr.dfu` again, start `ubertooth-btbr`, and flash
      `bluetooth_rxtx.dfu` while it is capturing
- [ ] Confirm: the same result, and the board does not stop answering

### 2i. Two-board range test (issue #601)

*Needs two Ubertooth boards a short distance apart, both running `bluetooth_rxtx`
from this tree, so both are at the API version the host tools require. The
packets carry no version of their own, so the two boards need not be built from
the same revision. The exchange completes within a couple of seconds.*

- [ ] Run `ubertooth-util -U <repeater> -e`; it keeps running until
      `ubertooth-util -U <repeater> -S` stops it
- [ ] Run `ubertooth-util -U <sender> -n`
- [ ] Confirm: it prints a `waiting for the repeater... (<n> s)` line about
      once a second while the exchange runs, then a level in each direction,
      and exits 0 — not `error: invalid range test: mismatch on byte <n>`.
      Close together, the lowest level gets through first try both ways:

      waiting for the repeater... (1 s)
      request PA level : 0
      request number   : 0
      reply PA level   : 0
      reply number     : 0

- [ ] Run `ubertooth-util -U <sender> -m`
- [ ] Confirm: the same four lines, read back from the board
- [ ] Weaken the link — move the boards apart, or attenuate one — and run `-n`
      again
- [ ] Confirm: the levels still report, with a higher packet number than
      before: more of the sixteen packets at that level were lost before one
      got through. The level itself may rise too, since the firmware moves up
      one once all sixteen are lost. What the figures come out at depends on
      the room, so these are an example rather than values to match:

      request PA level : 0
      request number   : 15
      reply PA level   : 0
      reply number     : 4

- [ ] Reset the sender (`ubertooth-util -U <sender> -r`), wait for it to come
      back, and run `-m` on it again
- [ ] Confirm: `no range test result yet` on standard output, exit 0 — a board
      that has not completed a test since it started has no result, which is
      not a failure
- [ ] Stop the repeater (`ubertooth-util -U <repeater> -S`) and run `-n` on the
      sender with nothing answering it
- [ ] Confirm: a waiting line a second for ten seconds, then one line on
      standard error — `error: no reply from the repeater within 10 s: ...;
      the range test has been stopped` — and exit 1. `-m` afterwards says
      `no range test result yet`, and the board is idle: `-w` reads
      `Now in idle mode`
- [ ] Run `ubertooth-util -U <sender> -n 3` with nothing answering
- [ ] Confirm: the same, after three seconds, with `within 3 s` in the message
- [ ] Run `ubertooth-util -U <sender> -n 0` with nothing answering, let it
      print a few waiting lines, and stop it with Ctrl+C
- [ ] Confirm: `-n 0` waits with no deadline, and an interrupted run leaves
      the test going — `-w` reads `Now in range test mode` — until
      `ubertooth-util -U <sender> -S`, after which `-w` reads
      `Now in idle mode`
- [ ] Swap the two roles — `-e` on the board that was the sender, `-n` on the
      other — and confirm the first two results again, so each board is seen
      transmitting

### 2j. Flashing with the bootloader entry pin still bridged (issue #304)

*Needs a board at the bench: the entry pin has to be bridged by hand, and the
board is left in the bootloader at the end. Nothing here can wedge it; it comes
back by unplugging it with the bridge removed.*

*The bridge goes on while the board is already up and running its application,
not before it is plugged in. The pin is only sampled at reset, so the flash
below still starts normally and the reset at the end of it is the one that
lands in the bootloader. Fitting the bridge first would leave the board in DFU
with no application to open, and `-U <serial>` never adopts a device that was
already in DFU, so the run would end at `DFU entry failed` instead.*

- [ ] With the board plugged in and running its application, bridge pins 1 and
      3 of the expansion header (P4)
- [ ] Leave the bridge fitted and run
      `ubertooth-dfu -U <serial> -d firmware/bluetooth_rxtx/bluetooth_rxtx.dfu -r`
- [ ] Confirm: it exits 1 with
      `error: detach failed: the board reset and came back in the bootloader`,
      then the `note:` lines saying the firmware was written, naming the
      bootloader's two entry conditions, and telling you to remove the bridge.
      No bare libusb status line in front of them, and no second copy of the
      failure from cleanup
- [ ] Unplug the board, remove the bridge, plug it back in, and run
      `ubertooth-util -U <serial> -v`
- [ ] Confirm: it reports the image and revision that were written, so the
      flash the command reported as failed had in fact reached the flash

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

### 3b-2. CRC verification on a followed connection
*Requires: a BLE central and peripheral that connect and keep exchanging data
(two scripted nRF52840 DKs, or a phone and any peripheral it talks to)*
- [ ] Run `ubertooth-btle -f -v1 -c follow.pcap` and let the pair connect while
      it listens
- [ ] Confirm: the capture shows the CONNECT_REQ and then data-channel packets
      for as long as the connection lasts
- [ ] Confirm: the capture does not stop at the CONNECT_REQ. It did until
      [#657](https://github.com/SqueakyFloors/ubertooth/issues/657): the
      connection's CRC starting value was reversed over 32 bits instead of 24,
      so every data packet failed verification and was dropped. Section 3b
      cannot catch that, because dropping everything is also "fewer"

### 3c. BLE PCAP capture
- [ ] Run `ubertooth-btle -f -c capture.pcap` for 10 seconds
- [ ] Confirm: `capture.pcap` file is created and non-empty
- [ ] Open in Wireshark and confirm valid BLE packets visible
- [ ] Confirm: in each packet's PPI header the average RSSI sits between the
      minimum and the maximum of the same packet

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
- [ ] Repeat with `ubertooth-btle -p -r long-packets-promisc.pcapng`, the
      other capture path in the firmware and the one whose long-packet
      handling is covered only by unit tests. `-p` cannot be pointed at a
      connection: leave it running on the established connection and wait
      while it finds the access address, CRCInit, hop interval and hop
      increment, which it reports as it goes. Data PDUs appear only after
      that, so allow a minute or two before expecting packets.
- [ ] Confirm: the same complete payloads once it is following

---

## 4. Spectrum Analyzer (tests specan_ui Python fixes)

### 4a. CLI spectrum analyzer
- [ ] Run `ubertooth-specan`
- [ ] Confirm: starts without Python errors
- [ ] Confirm: outputs spectrum data

### 4a-1. A sweep that cannot be written
*Linux only: `/dev/full` stands in for a full disk*
- [ ] Run `ubertooth-specan -U <serial> -l 2402 -u 2410 -v -d /dev/full`, then
      the same with `-g` and with `-G`
- [ ] Confirm: each ends within a second, exits 1, and says
      `error: writing samples failed: ...` once -- `wrote <n> of 3 bytes` for the
      raw form, `could not write the sample` for `-g` and `-G`, both naming
      `No space left on device`. A run that swept for seconds before reporting
      `could not flush the samples` is the fault to report
- [ ] Run the same three into a file on a disk with room
- [ ] Confirm: each exits 0 on Ctrl-C and the file holds the samples, as text
      for `-g`/`-G` and as 3-byte records without them

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
- [ ] Look at the `n=` noise figure: over a capture long enough to visit many
      channels it should move, not sit at one value in every record. The
      per-channel average behind it takes a few hundred packets to build up, so
      a short capture, or one spent on a few channels, can legitimately show the
      same figure throughout. A single value across hundreds of records on many
      channels is the fault to report

### 5b. Paging (PR #540 fix)
*Requires: the board carrying the `btbr` image*
- [ ] Run `ubertooth-btbr -U <serial>` targeting a known classic BT device
      BD_ADDR
- [ ] Confirm: with two boards attached and no `-U`, the tool lists both and
      opens neither (`error: multiple Ubertooth devices found; select one
      with -U <index|serial>`)
- [ ] Confirm: the `bt>` prompt appears within a couple of seconds
- [ ] Confirm: packets captured after paging completes
- [ ] Confirm: on a board carrying `bluetooth_rxtx` instead, the tool exits 1
      within a few seconds with `error: could not set up Ubertooth <serial>`
      naming the board it was given, and a
      `note: ` naming that firmware and the image to flash, rather than waiting
      on the board
- [ ] Confirm: Ctrl-C before the `bt>` prompt appears ends the tool with exit
      status 1
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

### 8c. Recovering a board that answers nothing (issues #731, #239)
A board that stops answering control requests is reached by killing a capture
mid-stream; about one run in ten does it, so this may take several tries.
- [ ] Run `ubertooth-btle -U <serial> -n`, kill it with `SIGKILL` after a few
      seconds, then run `ubertooth-util -U <serial> -v`
- [ ] Repeat until the version read fails with `1 attached Ubertooth device did
      not answer the serial read`
- [ ] Run `ubertooth-util -U <index> -r` on that board, by index: its serial
      cannot be read, so it cannot be selected by serial
- [ ] Confirm: `Ubertooth device <index> is not answering; resetting its USB
      connection`, then the reset, and `-r` exits 0
- [ ] Run `ubertooth-util -U <serial> -v`, `-b` and a capture
      (`ubertooth-rx -U <serial> -l <LAP> -u <UAP> -t 20`)
- [ ] Confirm: the board answers again and captures, with no re-plug and no
      hub port power cycle

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
- Risk: large change (207 files) reaching beyond this feature — needs trimming before merge

---

## Notes

- The host `ctest` suite needs no Ubertooth, and the tests that run the tools
  do not open one, so the suite can run while boards are in use. Those tests
  set `UBERTOOTH_TEST_NO_DEVICES=1`, which makes device selection behave as if
  nothing were attached; nothing else sets it, so normal use is unaffected.
  Everything below still needs real hardware.
- Tests 3b, 5b, 6 directly exercise the cherry-picked fixes — these are the most important to run
- Memory leak tests (8a) require extended time but are important for production use
- Document any unexpected behavior as a new issue in the repo
