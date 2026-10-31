=========
ToDo List
=========

.. note::

   This is the historical **upstream** project wishlist, inherited with the
   fork. It is aspirational and is **not** a commitment or roadmap for this
   fork. For this fork's actual tracked work, see the GitHub issue tracker,
   `COMPATIBILITY.md <https://github.com/SqueakyFloors/ubertooth/blob/main/COMPATIBILITY.md>`__ (support matrix), and `validation/ <https://github.com/SqueakyFloors/ubertooth/blob/main/validation/README.md>`__ (hardware validation).
   Obsolete items (e.g. GR-Bluetooth migration, Windows via libusbx/WICD) have
   been removed.

PCAP
~~~~

* Obtain DLT/linktype from libPCAP devs for BTBB, see `here <https://ubertooth.readthedocs.io/en/latest/bluetooth_captures_PCAP.html>`__.

* Define format for including meta-data (using PPI), see `here <https://ubertooth.readthedocs.io/en/latest/bluetooth_captures_PCAP.html>`__.

* Move Wireshark plugins to Wireshark repo.

* Add pcap support to ubertooth tools (currently only in kismet) by moving it to libbtbb.



Basic rate / libbtbb
~~~~~~~~~~~~~~~~~~~~

* Better handling of AFH maps when trying clock values. The code currently makes the assumption that AFH is enabled but all channels are in use. (See also the AFH ``FIXME`` in ``firmware/btbr/src/btphy/hop.c``.)

* Detect syncword on Ubertooth for known piconets.

* Make logging configurable - possibly combine text based logging, file dumps and pcap writing in to one logging system.



Releases
~~~~~~~~

* Binary packages for both libbtbb and ubertooth - rpm, deb, others?

* More frequent releases - requires better testing of code in git and separation of half-implemented features. This is more about process than an actionable todo item.



Bigger / more vague projects
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

* Transmit basic rate packets - inquiry scan to start with. (Note: ``ubertooth-tx`` is currently non-functional.)

* Implement full BTLE device in firmware.

* ANT/ANT+ sniffing.

    * `http://www.thisisant.com/developer/resources/downloads/#documents_tab <http://www.thisisant.com/developer/resources/downloads/#documents_tab>`__

    * `http://www.thisisant.com/developer/resources/tech-faq/ <http://www.thisisant.com/developer/resources/tech-faq/>`__

* Communicate between Ubertooth devices using SPI on the expansion port.

* Communicate with other devices using SPI on the expansion port.

* Write to SD card using SPI on the expansion port.
