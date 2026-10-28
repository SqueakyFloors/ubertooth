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
                            ``ubertooth-util -l [0-1]`` (USR only) and
                            ``-d [0-1]`` (all three). Used by various modes as a
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
the LEDs end off. On a typical Ubertooth One these three appear as adjacent
yellow/orange/red LEDs.

Host control:

- ``ubertooth-util -l 0`` / ``-l 1`` — turn the USR LED off / on.
- ``ubertooth-util -d 0`` / ``-d 1`` — turn all three off / on.
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

Fault codes
~~~~~~~~~~~~~~~

A board whose processor has faulted, or that the firmware has stopped itself,
answers the host nothing, so the three LEDs blinking together are all it has
left to say what happened. It flashes a code as a number of blinks, over and
over, until the watchdog resets it about three seconds later and it comes back
idle.

**Read it with the tool, not with your eyes.** Once the board is back,
``ubertooth-util -w`` names the fault and the mode the board was in, which
counting blinks never gave you::

    Unrequested watchdog resets since power-on: 1 (in 4 boots)
      the last was in a mode the board does not report, in the bus fault handler

The codes below are for the case where that is not available: a board you
cannot reach from a host, or one that keeps faulting as it boots.

In the ``btbr`` image each fault blinks one code, 150 ms a blink, with a longer
pause before it repeats:

===========  ==================================================
Blinks       Meaning
===========  ==================================================
1            The firmware stopped itself (a panic)
2            Hard fault
3            Memory management fault
4            Bus fault
5            Usage fault
===========  ==================================================

The ``bluetooth_rxtx`` image uses the same four numbers for the four faults. It
has no panic code, and after the fault's own run it blinks a second run, of the
same length of blink, saying which bit of the processor's fault status register
was set: one to six blinks, and none at all when no bit it looks at is set.
Its blinks are a rough busy loop rather than a timed one, nearer 150 ms than
the 30 the code claims, so the longer sequences do not finish before the board
resets: treat a count from that image as a hint, and use
``ubertooth-util -w``.
