.. _zigbee_onkyo_avr_sample:

Zigbee: Onkyo AVR controller
############################

.. contents::
   :local:
   :depth: 2

This sample implements a Zigbee Router that controls an Onkyo TX-SR805 AV
receiver over the receiver's RS-232 port using the ISCP protocol.

The receiver is switched on and off, its master volume, mute state and input
selection are controlled, and the on-screen display menu can be opened.
Everything the receiver reports back over the serial link is reflected in the
Zigbee attributes, so a controller always sees the actual receiver state.

You can use this sample as a starting point for controlling other Onkyo or
Integra receivers that implement ISCP.

Requirements
************

The sample supports the following development kit:

.. list-table::
   :header-rows: 1

   * - Board
     - Target
   * - Pro Micro nRF52840
     - ``promicro_nrf52840/nrf52840``

To control the receiver, you also need:

* An Onkyo TX-SR805 or a compatible Onkyo or Integra receiver with an RS-232
  port.
* An RS-232 level shifter, for example a MAX3232 breakout board.
  The development kit pins are 3.3 V TTL, while the receiver's serial port
  uses RS-232 voltage levels.

To test this sample, you also need to program the
:ref:`Zigbee Network coordinator <zigbee_network_coordinator_sample>` sample or
the :ref:`Zigbee Shell <zigbee_shell_sample>` sample on one separate device.
A Zigbee coordinator of another ecosystem, such as the one of Home Assistant,
can be used instead.

Hardware setup
**************

The receiver is connected to UART1 of the development kit, which is routed to
the pins normally used by SPI2. SPI2 is disabled in the board overlay of this
sample, so those pins are free.

.. list-table::
   :header-rows: 1

   * - Development kit pin
     - Signal
     - Receiver side
   * - P1.01
     - UART1 TX
     - RX of the level shifter
   * - P1.02
     - UART1 RX
     - TX of the level shifter
   * - GND
     - Ground
     - Ground

Connect the RS-232 side of the level shifter to the receiver's RS-232 port.

The link runs at 9600 baud, 8 data bits, no parity and one stop bit, which is
the serial configuration of the receiver.

The pins and the baud rate can be changed in
:file:`boards/promicro_nrf52840_nrf52840.overlay`.

Overview
********

The sample takes the Zigbee Router role and exposes a single endpoint with the
following server clusters:

* Basic (0x0000) and Identify (0x0003), which are required by the Zigbee Home
  Automation profile. The Basic cluster carries the manufacturer name, the
  model identifier and the versions of the device, which controllers use to
  identify it.
* On/Off (0x0006), used to switch the receiver between standby and on.
* Level Control (0x0008), used for the receiver's master volume.
* Onkyo AVR control (0xFC00), a manufacturer specific cluster with the
  commands that have no equivalent in the standard clusters.

The simple descriptor uses the *Dimmable Light* device ID (0x0101), so that a
controller presents the On/Off and Level Control clusters as one dimmable light,
where the brightness is the receiver's master volume. Controllers only build a
light entity for the lighting device types, so the *Level Controllable Output*
device ID (0x0003) would only be usable as a plain on/off switch.

Zigbee interface
****************

The clusters are exposed on endpoint 10.

On/Off cluster
==============

.. list-table::
   :header-rows: 1

   * - Command or attribute
     - Effect on the receiver
   * - ``Off``, ``On``, ``Toggle`` commands
     - ``!1PWR00`` or ``!1PWR01``
   * - ``OnOff`` attribute
     - ``!1PWR00`` or ``!1PWR01`` when written

Level Control cluster
=====================

The Home Assistant brightness range (0 to 254) is mapped to receiver master
volume 30 to 70. The receiver's reported volume is mapped back into that
range, with values below 30 represented as 0 and values above 70 represented
as 254. This gives finer control over the useful operating range.

.. list-table::
   :header-rows: 1

   * - Command or attribute
     - Effect on the receiver
   * - ``MoveToLevel``, ``Move``, ``Step``, ``Stop`` commands
     - ``!1MVL<xx>``
   * - ``CurrentLevel`` attribute
     - ``!1MVL<xx>`` when written

Because the receiver steps the volume in whole steps while the standard
``Step`` commands describe a step size and transition time, both
``MoveToLevel`` and ``Step`` are translated into the receiver's own absolute
and relative volume commands.

Onkyo AVR control cluster
=========================

All commands of this cluster use standard ZCL frames and are addressed to
cluster 0xFC00.

Cluster IDs at and above 0xFC00 are manufacturer specific, so the cluster
carries the manufacturer code from the ``CONFIG_ONKYO_AVR_MANUFACTURER_CODE``
Kconfig option, which defaults to a placeholder that must be replaced with the
code the Connectivity Standards Alliance assigned to the vendor. The commands
and the attributes of the cluster are manufacturer specific as well, so a
client that addresses them, such as a Home Assistant quirk or a Zigbee2MQTT
converter, has to use the same value. The
:ref:`Zigbee Shell <zigbee_shell_sample>` cannot send a manufacturer code, so it
can send the commands below, which the device accepts as its own, but it cannot
read or write the attributes of the cluster.

.. list-table::
   :header-rows: 1

   * - Command ID
     - Name
     - Payload
     - Effect on the receiver
   * - 0x00
     - Input select
     - 1 byte, input selector
     - ``!1SLI<xx>``
   * - 0x01
     - Mute
     - 1 byte: 0 unmutes, 1 mutes, 2 toggles
     - ``!1AMT00`` or ``!1AMT01``
   * - 0x02
     - Volume step
     - 1 byte, signed number of steps
     - ``!1MVLUP`` or ``!1MVLDOWN``
   * - 0x03
     - OSD menu
     - none
     - ``!1OSDMENU``

.. list-table::
   :header-rows: 1

   * - Attribute ID
     - Type
     - Value
   * - 0x0000
     - 8-bit enumeration
     - Input selector currently active on the receiver
   * - 0x0001
     - Boolean
     - Mute state reported by the receiver

The input selectors used by the TX-SR805 are:

.. list-table::
   :header-rows: 1

   * - Value
     - Input
   * - 0x01
     - CBL/SAT
   * - 0x02
     - GAME/TV
   * - 0x05
     - PC
   * - 0x10
     - BD/DVD

Home Assistant
**************

The ZHA integration of Home Assistant builds the following entities from these
clusters, without any device specific handling:

.. list-table::
   :header-rows: 1

   * - Entity control
     - Effect on the receiver
   * - Light on/off
     - ``!1PWR00`` or ``!1PWR01``
   * - Light brightness
     - ``!1MVL<xx>``, scaled to the 30 to 70 master volume range
   * - Reported light state
     - The state the receiver reports over the serial link

The Onkyo AVR control cluster has no standard equivalent, so the input
selection, the mute state and the on-screen display menu are only reachable
from Home Assistant with a device handler. The ZHA quirk in
:file:`zha_quirks/onkyo_avr.py` adds the entities that are missing:

.. list-table::
   :header-rows: 1

   * - Entity
     - Effect on the receiver
   * - ``Input`` select
     - ``!1SLI<xx>``, through the Input select command (0x00)
   * - ``Mute`` switch
     - ``!1AMT00`` or ``!1AMT01``, through the Mute command (0x01)
   * - ``On-screen display menu`` button
     - ``!1OSDMENU``, through the OSD menu command (0x03)

The attributes of the cluster report the receiver state and cannot be written,
so the quirk translates the attribute writes that the select and the switch
send into the commands of the cluster.

To install the quirk, copy :file:`zha_quirks/onkyo_avr.py` into a directory of
the Home Assistant configuration directory and point ZHA at that directory:

.. code-block:: yaml

   zha:
     custom_quirks_path: /config/custom_quirks

Restart Home Assistant and add the device again, so that the quirk is applied
when the device is interviewed.

The quirk addresses the cluster with the manufacturer code ``0x1234``, so it
matches the default value of ``CONFIG_ONKYO_AVR_MANUFACTURER_CODE`` only. Set
``MANUFACTURER_CODE`` in the quirk to the code the sample was built with before
you install it.

.. note::
   The sample does not configure attribute reporting, so ZHA reads the input
   and the mute state when the device is set up, and the quirk updates the
   entities when you control the device from Home Assistant. A change that is
   made on the receiver itself, or with the Zigbee Shell sample, is picked up
   when the device is configured again.

Send the commands of that cluster with the :ref:`Zigbee Shell
<zigbee_shell_sample>` sample instead if you want to reach the receiver without
a device handler.

Media player entity
===================

ZHA has no media player platform and a quirk cannot add one, so a controller
presents the receiver as a light entity. To present it as a media player
instead, wrap it in the `universal media player`_ integration that Home
Assistant ships, which does not need a custom component.

The wrapper reads the state of the light and of the entities that the quirk
adds, and it needs a helper sensor that reports the volume as the 0 to 1
fraction that a media player uses. Add the following to
:file:`configuration.yaml`, and replace the entity IDs with the ones that Home
Assistant created for the device:

.. code-block:: yaml

   template:
     - sensor:
         - name: "Onkyo volume"
           unique_id: onkyo_avr_volume
           state: "{{ (state_attr('light.onkyo_avr_controller', 'brightness') or 0) / 255 }}"

   media_player:
     - platform: universal
       name: Onkyo TX-SR805
       unique_id: onkyo_avr_controller
       state_template: "{{ 'on' if is_state('light.onkyo_avr_controller', 'on') else 'off' }}"
       attributes:
         volume_level: sensor.onkyo_volume
         is_volume_muted: switch.onkyo_avr_controller_mute
         source: select.onkyo_avr_controller_input
         source_list: select.onkyo_avr_controller_input|options
       commands:
         turn_on:
           action: light.turn_on
           target:
             entity_id: light.onkyo_avr_controller
         turn_off:
           action: light.turn_off
           target:
             entity_id: light.onkyo_avr_controller
         volume_set:
           action: light.turn_on
           target:
             entity_id: light.onkyo_avr_controller
           data:
             brightness_pct: "{{ (volume_level * 100) | round(0) }}"
         volume_up:
           action: light.turn_on
           target:
             entity_id: light.onkyo_avr_controller
           data:
             brightness_step_pct: 2
         volume_down:
           action: light.turn_on
           target:
             entity_id: light.onkyo_avr_controller
           data:
             brightness_step_pct: -2
         volume_mute:
           action: switch.toggle
           target:
             entity_id: switch.onkyo_avr_controller_mute
         select_source:
           action: select.select_option
           target:
             entity_id: select.onkyo_avr_controller_input
           data:
             option: "{{ source }}"

The media player takes its power state and its volume from the light, and it
lists the input selectors of the Onkyo AVR control cluster as its sources,
which the ``options`` attribute of the select entity carries.

.. note::
   The volume of the media player is the brightness of the light, so setting the
   volume switches the receiver on. Use a script with a state condition instead
   of the ``light.turn_on`` action if the receiver should stay in standby.

The quirk only adds entities, so the light stays next to the media player. Hide
or disable it in Home Assistant if the media player should be the only control
for the receiver.

Joining the network
===================

The device joins the network that the coordinator created, so open the network
in Home Assistant while the device scans for it. The device scans all channels
and reports what it does:

.. code-block:: console

   I: rejoin attempt #7 finished
   I: Network steering was not successful (status: -1)

This means that the device did not find a network that accepts new devices. It
repeats the scan with growing intervals, starting with a few seconds, so keep
the network open, or restart the device to make it scan again right away.

The board has no user button, so the factory reset of the sample is not
available on it. If the device keeps trying to rejoin a network it was part of
before, remove the stored Zigbee configuration by programming it again with a
mass erase (``west flash --erase``, which the debug probe runners support). A
mass erase also removes the UF2 bootloader, which then has to be programmed
again from the release of the board vendor.

User interface
**************

LED 1 on the Pro Micro nRF52840 has two roles, because the board breaks out a
single LED:

* It is turned on when the device has joined a Zigbee network.
* It blinks while the device is in Identify mode, which is the indication that
  the Zigbee base device behavior defines for a device with a single LED.

The board has no user buttons, so the sample binds Identify mode and the
factory reset to a button only on boards that provide one.

Configuration
*************

The sample has one configuration option:

.. list-table::
   :header-rows: 1

   * - Option
     - Description
   * - ``CONFIG_ONKYO_AVR_MANUFACTURER_CODE``
     - Manufacturer code of the manufacturer specific Onkyo AVR control
       cluster (0xFC00). The default 0x1234 is a placeholder that must be
       replaced with the manufacturer code that the Connectivity Standards
       Alliance assigned to the vendor, before the device is used with a client
       that addresses the cluster, such as a Home Assistant quirk or a
       Zigbee2MQTT converter.
   * - ``CONFIG_ZIGBEE_CHANNEL_SELECTION_MODE_MULTI``
     - The sample joins a network that another device created, so it scans all
       the channels of ``CONFIG_ZIGBEE_CHANNEL_MASK`` (all channels by default)
       instead of the single channel 16 that the default selection mode uses.
       Change the mask if the device should not scan every channel.
   * - ``CONFIG_ZIGBEE_APP_CB_QUEUE_LENGTH``
     - Number of slots in the application callback queue, raised to 17 because
       a scan needs one slot per channel.

Building and running
********************

|enable_zigbee_before_testing|

#. Build and program the sample:

   .. code-block:: console

      west build -b promicro_nrf52840/nrf52840 samples/onkyo_avr
      west flash

   To program the board with its UF2 bootloader instead of a debug probe, build
   the ``uf2`` variant of the board target, which produces
   :file:`zephyr/zephyr.uf2`:

   .. code-block:: console

      west build -b promicro_nrf52840/nrf52840/uf2 samples/onkyo_avr
      west flash

   For that variant, ``west flash`` copies the file to the mass storage device
   that the bootloader exposes.

#. Program the :ref:`Zigbee Network coordinator <zigbee_network_coordinator_sample>`
   sample on another development kit and turn both devices on.

   When **LED 1** turns on, the sample has become a Router inside the network.

On startup the sample asks the receiver for its power state, master volume,
mute state and active input, so the Zigbee attributes are populated without
user interaction.

Testing
=======

To control the receiver from a controller that runs the
:ref:`Zigbee Shell <zigbee_shell_sample>` sample, complete the following steps:

1. Check the short address of the device that runs this sample:

   .. code-block:: console

      zdo short

2. Switch the receiver on and off. Replace ``0x1234`` with the short address
   obtained in the previous step:

   .. code-block:: console

      zcl cmd 0x1234 10 0x0006 0x01
      zcl cmd 0x1234 10 0x0006 0x00

3. Set the master volume to about 50 percent:

   .. code-block:: console

      zcl cmd 0x1234 10 0x0008 0x00 -l 7F0A00

4. Mute the receiver (the payload is the first byte of the Mute command):

   .. code-block:: console

      zcl cmd 0x1234 10 0xFC00 0x01 -l 01

5. Select the BD/DVD input:

   .. code-block:: console

      zcl cmd 0x1234 10 0xFC00 0x00 -l 10

6. Open the on-screen display menu:

   .. code-block:: console

      zcl cmd 0x1234 10 0xFC00 0x03

The application log shows every command that is sent to the receiver and every
frame that is received from it, for example:

.. code-block:: console

   [00:00:12.345,678] <inf> app: ISCP TX: PWR01
   [00:00:12.456,789] <inf> app: ISCP RX: !1PWR01
   [00:00:12.456,790] <inf> app: Receiver power: on

Since the sample reports the receiver state back through the ZCL attributes,
the ``CurrentLevel`` and the Onkyo AVR control cluster attributes show the
values that the receiver actually applies.

Dependencies
************

This sample uses the following |NCS| libraries:

* :ref:`lib_zigbee_error_handler`
* :ref:`lib_zigbee_application_utilities`
* Zigbee subsystem:

  * :file:`zb_nrf_platform.h`

* `DK Buttons and LEDs`_

It uses the following libraries:

* :ref:`zigbee_zboss` |zboss_version| (`API documentation`_)

In addition, it uses the following Zephyr libraries:

* :file:`include/zephyr/kernel.h`
* :file:`include/zephyr/drivers/uart.h`
* :file:`include/zephyr/sys/ring_buffer.h`
* `Logging`_
