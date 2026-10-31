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

See host/python/ubtbr/README.

`ubertooth-btbr` needs this image and nothing else runs it. A board carrying
`bluetooth_rxtx` offers the endpoint the tool writes to but never reads it, so
the tool's first requests sit there unread: that is what the reports of it
hanging shortly after start, right after the EIR setup line, turned out to be
(upstream issue
[#520](https://github.com/greatscottgadgets/ubertooth/issues/520)). The wait is
bounded now, and the tool names that firmware and the image to flash.
