====================
Third Party Software
====================

The following are third party applications that support Ubertooth One, either natively or with plugins. Unless otherwise stated, the Ubertooth project does not provide support for these applications.



Kismet
~~~~~~

**Status (2026):** the plugins in ``host/kismet`` are written against Kismet 2013-03-R1, the version built below. Kismet has since been rewritten, and they do not build against current releases. Nothing in this tree builds or tests them, and channel hopping on an Ubertooth source does not work: the plugin reports a channel change it never makes. The instructions below are kept for anyone running that older Kismet.

The version of kismet provided by Debian/Ubuntu is 2008-05-R1, which is too old to support the Ubertooth plugin. In order to use Ubertooth with Kismet it is necessary to compile Kismet from source. First make sure that you have completed the instruction in the :doc:`build guide <build_guide>` and then use the following instruction to build Kismet:

	.. code-block:: sh

		sudo apt-get install libpcap0.8-dev libcap-dev pkg-config build-essential libnl-3-dev libncurses-dev libpcre3-dev libpcap-dev libcap-dev libnl-genl-3-dev
		wget https://kismetwireless.net/code/kismet-2013-03-R1b.tar.xz
		tar xf kismet-2013-03-R1b.tar.xz
		cd kismet-2013-03-R1b
		ln -s /path/to/ubertooth/host/kismet/plugin-ubertooth .
		./configure
		make && make plugins
		sudo make suidinstall
		sudo make plugins-install
		Add "pcapbtbb" to the "logtypes=..." line in kismet.conf

Support for the Kismet is provided by the Kismet project, but the plugins are part of the Ubertooth software releases: ``host/kismet`` in this tree holds both ``plugin-ubertooth`` and ``plugin-ubertooth-phyneutral``, which is the one for Kismet's phy-neutral plugin interface. Neither is built by the Ubertooth host build; each is built by Kismet, as above. For queries about the Ubertooth plugins please see the :doc:`getting help <getting_help>` page.



Flying Squirrel
~~~~~~~~~~~~~~~

`Flying Squirrel <http://www.nrl.navy.mil/itd/chacs/5545/flying-squirrel>`__ has built in support for Ubertooth. Unfortunately Flying Squirrel is not available to the general public. "Flying Squirrel is only available to DOD and federal agencies." this was the reply when one tried to request the download to fsadmin@nrl.navy.mil.



Spectools
~~~~~~~~~

Spectools is a very useful 2.4GHz spectrum monitor, showing multiple views of spectrum usage. It supports Ubertooth One: its most recent release, ``spectools-2016-01-R1``, carries the Ubertooth source, and the releases are on `Kismet's code page <https://www.kismetwireless.net/code/>`__.
