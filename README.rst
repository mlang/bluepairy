=========
Bluepairy
=========

Check and optionally establish Bluetooth pairing with certain friendly names.

Bluepairy is a simple utility to automatically establish a Bluetooth
pairing with devices with a certain friendly name.  It has been written
for the brlpi_ project, but could be useful for other people as well.

.. _brlpi: https://blind.guru/brlpi.html

An example systemd service file for the Handy Tech Active Star 40
is provided and installed by default.

Installation
------------

.. code-block:: shell

  $ make install

If you are using bluepairy to ensure a pairing between your
Raspberry Pi Zero and your Handy Tech Active Star 40, you can run

.. code-block:: shell

  $ sudo systemctl enable bluepairy-active-star.service

to configure bluepairy to check your pairing before
BRLTTY is started.

