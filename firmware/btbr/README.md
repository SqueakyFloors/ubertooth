# UBTBR firmware

This firmware implements partial support of the Bluetooth BR/EDR baseband layer.

## Supported features

- Inquiry/Inquiry scan: Perform inquiry to discover nearby devices, or reply to inquiries to be visible.
- Page/Page scan: Establish a connection as a Central, or allow connection as Peripheral.
- Link-Layer: Poor-man link-layer to maintain a connection.
- Monitoring: Spy on pairing procedures of remote devices

## Limitations / TODOs
- Due to limitations of the cc2400, only BR packets are supported.
- Packet types supported: ID, NULL, POLL, FHS, DM*, DH*.
- Link-layer only supports ACL packets.
- Central side of link-layer drains too much battery of the Peripheral (FIXME: POLL less).
- Role-switching and related LMP procedures are not yet implemented.
- Encryption of packets is not yet implemented.

## Host code
The *ubtbr* python modules contains the host control interface and a minimal implementation of the LMP protocol.

UBTBR can be controlled using the *ubtbr* python module, and the ubertooth-btbr tool.

See [host/python/ubtbr/README.md](../../host/python/ubtbr/README.md).

## Which image this is

The build writes `btbr.dfu`. `ubertooth-btbr` is the only tool that runs it,
and a board carries one image at a time, so any tool that captures or drives
the radio needs `bluetooth_rxtx`. This image does answer the
device-administration requests -- among them the version, the board ID, the
serial, the LEDs, the amplifier, reset, ISP entry, and the watchdog -- so
`ubertooth-dfu` works from either image, and so does `ubertooth-util` for those
requests; its channel, squelch, range-test, and mode options do not.
ubertooth-btbr(1) has the flash commands and says what starting the tool on a
`bluetooth_rxtx` board does.
