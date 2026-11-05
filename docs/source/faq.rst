===
FAQ
===

**Question: What is the latest release?**

**Answer:** The most recent upstream release is `2020-12-R1
<https://github.com/greatscottgadgets/ubertooth/releases/tag/2020-12-R1>`__;
check `the upstream releases
<https://github.com/greatscottgadgets/ubertooth/releases>`__ and `this fork's
releases <https://github.com/SqueakyFloors/ubertooth/releases>`__ for the most
up to date information. See the :doc:`build guide <build_guide>` for how to
download and build the software.

----------

**Question: I run Windows or macOS. What's the best way to use Ubertooth?**

**Answer:** On **macOS**, install the host tools natively. ``./install.sh``
builds them from source, taking its dependencies from `Homebrew
<https://brew.sh>`__, which has to be installed first. Every tool but
``ubertooth-scan`` and ``ubertooth-follow`` is built there; those two drive a
second, ordinary Bluetooth adapter through BlueZ, which is Linux only, and
:doc:`software` says what to use in their place. Use ``--venv <path>`` on a Mac
if you want the Python tools, ``ubertooth-specan-ui`` and ``ubertooth-btbr``:
Homebrew's Python will not take their packages, and the script says so and
carries on without them.

On **Windows** there is no supported path. Run the host tools on Linux, either
natively or in a virtual machine with the Ubertooth passed through; a live
distro such as `Kali <https://www.kali.org/>`__ booted from USB is the quickest
way to try one. The entry below covers what to watch for in a virtual
machine.

----------

**Question: How do I update firmware?**

**Answer:** See the :doc:`firmware` page.

----------

**Question: Can I use my Ubertooth with a VM?**

**Answer:** Yes. Many people have reported successfully using their Ubertooth with host tools installed in a virtual machine. There are, as always, some things to be aware of when attempting this.

    * The Ubertooth uses USB2.0, which requires the `VirtualBox extension pack <https://www.virtualbox.org/wiki/Downloads>`__ to be downloaded and installed.
    * The Ubertooth also uses different vendor and product identifiers when in normal operation and firmware upload mode, so rules for both of these will need to be added to the virtual machine for full operation.

Other virtual machine environments may differ and therefore present different issues. If you successfully use the Ubertooth with a different virtualisation system, please let us know so that we can help future users.

----------

**Question: What can the Ubertooth capture?**

**Answer:** The Ubertooth is able to capture and demodulate signals in the 2.4GHz ISM band with a bandwidth of 1MHz using a modulation scheme of Frequency Shift Keying or related methods.

This includes, but is not limited to:

    * Bluetooth Basic Rate packets

    * Bluetooth Low Energy (Bluetooth Smart)

The following may be possible:

    * 802.11 FHSS (1MBit)

    * Some proprietary 2.4GHz wireless devices

----------

**Question: Can I listen to my phone calls?**

**Answer:** Not yet, this will require full frequency hopping support and possibly require breaking encryption by sniffing the connection process. It's on the todo list, but there's a lot of hard work to go before we get there. If you're interested in making it happen, you can get involved in the project.

----------

**Question: What is the max capture size of BLE packets?**

**Answer:** In ``ubertooth-btle -f`` and ``-n``, a whole packet, up to the
251-byte payload Bluetooth 4.2 Data Length Extension allows. That is 260 bytes
with the access address, the PDU header, and the CRC. Older firmware cut every
packet to 50 bytes. Promiscuous mode, ``-p``, still does while it is working
out the connection's access address by listening; once it has one and is
following the connection, it too delivers whole packets.
