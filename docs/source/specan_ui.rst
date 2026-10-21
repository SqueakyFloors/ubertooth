==================================
Spectrum analyzer UI (specan_ui)
==================================

``ubertooth-specan-ui`` is a graphical real-time spectrum analyzer for the
Ubertooth. It is a Python/Qt application that reads spectrum data from the
device (via ``ubertooth-specan``) and plots it.

This guide covers installing and running it, and the common
``ModuleNotFoundError: No module named 'specan'`` error.

Requirements
~~~~~~~~~~~~

- Python 3 (3.8+). On Python 3.12+ you also need **setuptools** installed to
  build it (``python3 -m pip install setuptools`` or the distro
  ``python3-setuptools`` package) — PEP 632 removed the bundled copy.
- Python packages: ``numpy``, ``qtpy``, and a Qt binding such as **PySide6**
  (or PyQt5). On Debian/Ubuntu: ``python3-numpy python3-qtpy python3-pyside6``;
  or with pip: ``pip install numpy qtpy pyside6``.
- The built Ubertooth host tools (so ``ubertooth-specan`` is available).

Install
~~~~~~~

``ubertooth-specan-ui`` has two parts: the launcher script (installed to your
``bin`` directory) and the ``specan`` Python package (which the launcher
imports). **Both** are installed when you install the host tools:

.. code-block:: sh

   cd host/build
   cmake ..
   make
   sudo make install          # installs the launcher AND the specan package

The install step runs the spectrum UI's ``setup.py install``, which places the
``specan`` package on the Python path.

Run
~~~

.. code-block:: sh

   ubertooth-specan-ui

On Linux you may need ``sudo`` (or udev rules) for device access:

.. code-block:: sh

   sudo ubertooth-specan-ui

Troubleshooting ``ModuleNotFoundError: No module named 'specan'``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

This means the launcher ran but the ``specan`` package is not importable by the
Python interpreter running it. Common causes and fixes:

1. **You ran it without installing.** Running ``ubertooth-specan-ui`` straight
   from the build tree skips the package install. Run ``sudo make install``
   from ``host/build`` first (that installs the ``specan`` package), or add the
   source dir to ``PYTHONPATH`` for a quick test:

   .. code-block:: sh

      PYTHONPATH=host/python/specan_ui ubertooth-specan-ui

2. **The package installed to a different Python than the launcher uses.** If
   the build selected one Python (e.g. ``/usr/local/bin/python3``) but the
   launcher runs under another (e.g. ``/usr/bin/python3``), ``specan`` is
   installed where the launcher can't see it. Point the build at the Python you
   will run, and ensure that interpreter has setuptools:

   .. code-block:: sh

      python3 -m pip install setuptools
      cmake .. -DPython3_EXECUTABLE=$(command -v python3)
      make && sudo make install

   Check which Python a build selected in the CMake output:
   ``-- Found Python3: /path/to/python3``.

3. **Using a virtualenv.** Build and install with the venv active (and its
   ``python`` passed as ``-DPython3_EXECUTABLE``) so the launcher and the
   ``specan`` package share one interpreter.

If the UI starts but cannot reach the device, that is a permissions/udev issue,
not this error — see the getting-started / udev documentation.
