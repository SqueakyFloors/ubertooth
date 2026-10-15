# Documentation Gaps

These are recurring user questions from the upstream GitHub issue tracker that indicate
missing or unclear documentation. Each is a candidate for a new or improved docs page.

## "How do I capture packets in an ongoing connection?" (issues #522, #534)
Users don't understand that Ubertooth needs to observe the connection setup to follow it.
Needs a clear explanation of: connection following vs. advertising-only mode, what
`ubertooth-btle -f` does vs. `-n`, and limitations.

## "Why does ubertooth-btle only capture advertising messages?" (issue #513)
Related to above. Users expect it to capture all BT traffic by default.
Needs a page explaining BLE packet types and what Ubertooth can/cannot capture.

## "How do I do BR/EDR (classic Bluetooth) sniffing?" (issues #504, #526)
Users don't know about `ubertooth-btbr` or the btbr firmware. The classic BT workflow
is much less documented than the BLE workflow.
Needs a dedicated BR/EDR sniffing guide covering: btbr vs bluetooth_rxtx firmware,
the `ubertooth-btbr` tool, and how to use it with Wireshark.

## "How do I guess/find the NAP?" (issue #532)
Advanced topic — users trying to do classic BT sniffing without knowing the target's
BD_ADDR. Needs a practical guide on using `ubertooth-rx` and `ubertooth-afh` together.

## "How do I capture FHS packets?" (issue #549)
Frequency Hop Synchronization packets used in classic BT paging. No documentation exists.

## "ubertooth-specan-ui gives ModuleNotFoundError: No module named 'specan'" (issue #495)
Install/path issue. Needs a clear "after building, here's how to install and run the
spectrum analyzer UI" section in the build guide.

## macOS build instructions are outdated (issue #502, #507)
The brew command in docs is wrong. Needs an updated macOS build guide tested against
modern Homebrew and macOS 12+.

## Windows support questions (issue #527)
Users asking about Windows. Currently unsupported. Needs a clear statement in docs
about supported platforms and why Windows isn't supported (libusb limitations).
