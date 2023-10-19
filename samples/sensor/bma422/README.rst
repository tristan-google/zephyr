.. _bma422:

BMA422 Accelerometer
####################

Overview
********

This sample shows how to use the Zephyr :ref:`sensor_api` API driver for the
Bosch BMA422 accelerometer. The sample targets the nRF52840-DK board
(Zephyr board type ``nrf52840dk_nrf52840``) with the sensor on the ``i2c0`` bus
(SDA = ``P0.26``, SCL = ``P0.27``). The sample should work with other sensors
in the BMA4xx series.

The sensor shell can be used to retrieve readings from the sensor:

.. code-block:: none

   uart:~$ sensor get bma422@19 all
   channel idx=3 accel_xyz shift=6 num_samples=1 value=10933679782ns, (0.670123, -0.248870, 9.959386)
   channel idx=12 die_temp shift=8 num_samples=1 value=10933679782ns (26.000000)
