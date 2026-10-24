=========
LEDs
=========

The Ubertooth One has several LEDs. Some are driven by firmware (and can be
controlled from the host); others are hardwired power/status indicators that
the CPU cannot control.

Firmware-controlled LEDs
~~~~~~~~~~~~~~~~~~~~~~~~~

These three are the LEDs that ``ubertooth-util -I`` flashes, and the only LEDs
the firmware can turn on and off (Ubertooth One pins shown):

================  ========  ====================================================
LED               Pin       Meaning
================  ========  ====================================================
USR (user)        P1.1      General-purpose indicator. Host-settable with
                            ``ubertooth-util -l[0-1]`` (USR only) and
                            ``-d[0-1]`` (all three). Used by various modes as a
                            state/activity light (e.g. range test, LED spectrum
                            analyzer).
RX                P1.4      Lit while the radio is receiving. Toggled throughout
                            the capture paths (``RXLED_SET`` / ``RXLED_CLR``).
TX                P1.8      Lit while the radio is transmitting
                            (``TXLED_SET`` / ``TXLED_CLR``).
================  ========  ====================================================

``ubertooth-util -I`` ("identify") toggles all three together, about three
on-off blinks a second, so you can pick a specific device out of several. The
blink runs on the board as its own mode, so the command returns at once; it keeps
blinking until ``ubertooth-util -S`` stops it or another mode replaces it, and
the LEDs end off. (Firmware older than API version 1.08 has no identify mode;
with it the host blinks the LEDs over USB for 10 seconds or until Ctrl-C
instead.) On a typical Ubertooth One these three appear as adjacent
yellow/orange/red LEDs.

Host control:

- ``ubertooth-util -l0`` / ``-l1`` — turn the USR LED off / on.
- ``ubertooth-util -d0`` / ``-d1`` — turn all three off / on.
- ``ubertooth-util -I`` — flash all three to identify the device;
  ``ubertooth-util -S`` stops it.

Power / status LEDs (not software-controllable)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

These indicate power rails and are always on when powered; firmware cannot blink
them, so they do **not** respond to ``-I``, ``-l``, or ``-d``:

================  ========  ====================================================
Indicator         Pin       Meaning
================  ========  ====================================================
CC1V8             P1.9      CC2400 radio 1.8 V rail present.
CC3V3             P1.14     3.3 V rail present.
VBUS              P1.30     USB bus power present.
================  ========  ====================================================

If a power LED is off, suspect power/USB rather than firmware. If ``-I`` only
affects some of the lit LEDs, that is expected: it drives USR/RX/TX only, not
the power indicators.
