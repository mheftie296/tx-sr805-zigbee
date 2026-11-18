"""ZHA quirk for the Onkyo AVR controller sample.

A controller sees the device that ``samples/onkyo_avr`` builds as a plain
dimmable light, because the receiver power and the master volume are exposed on
the standard On/Off and Level Control clusters. The receiver functions that
have no standard cluster, input selection, mute and the on-screen display menu,
live in the manufacturer specific Onkyo AVR control cluster (0xFC00) and
therefore have no matching entity.

This quirk turns that cluster into device controls:

* ``Input``, a select entity backed by the ``input`` attribute,
* ``Mute``, a switch entity backed by the ``mute`` attribute,
* ``On-screen display menu``, a button that sends the OSD menu command.

The attributes of the cluster are read-only status values, so the quirk
translates the attribute writes that the select and the switch send into the
manufacturer specific commands below and updates its attribute cache straight
away. The sample reports the receiver state back through the attributes.

    0x00  Input select   1 byte, input selector
    0x01  Mute           1 byte: 0x00 unmutes, 0x01 mutes, 0x02 toggles
    0x02  Volume step    1 byte, signed number of steps
    0x03  OSD menu       no payload

The master volume is the brightness of the light entity, so the volume step
command has no entity of its own.

Copy this file into a directory of the Home Assistant configuration directory
and point ZHA at that directory::

    zha:
      custom_quirks_path: /config/custom_quirks

Restart Home Assistant and add the device again, so that the quirk is applied
when the device is interviewed.
"""

from __future__ import annotations

from typing import Any, Final

import zigpy.types as t
from zigpy.typing import UNDEFINED, UndefinedType
from zigpy.zcl import foundation
from zigpy.zcl.foundation import (
    BaseAttributeDefs,
    BaseCommandDefs,
    ZCLAttributeDef,
    ZCLCommandDef,
)

from zhaquirks.builder import EntityType, QuirkBuilder
from zhaquirks.clusters import CustomCluster

# Device identification, as reported in the Basic cluster by the sample.
MANUFACTURER = "Nordic Semiconductor ASA"
MODEL = "Onkyo AVR controller"

# Endpoint and cluster the sample exposes the receiver control on.
ENDPOINT_ID = 10
ONKYO_AVR_CLUSTER_ID = 0xFC00

# Manufacturer code of the cluster and of everything it carries. It has to
# match CONFIG_ONKYO_AVR_MANUFACTURER_CODE of the sample, which defaults to
# 0x1234. Replace it with the code the Connectivity Standards Alliance assigned
# to the vendor before shipping a product that is based on the sample.
MANUFACTURER_CODE = 0x1234


class OnkyoAvrInput(t.enum8):
    """Input selector values of the TX-SR805."""

    CBL_SAT = 0x01
    GAME_TV = 0x02
    PC = 0x05
    BD_DVD = 0x10


class OnkyoAvrMute(t.enum8):
    """Payload of the Mute command."""

    UNMUTE = 0x00
    MUTE = 0x01
    TOGGLE = 0x02


class OnkyoAvrControlCluster(CustomCluster):
    """Manufacturer specific Onkyo AVR control cluster."""

    cluster_id = ONKYO_AVR_CLUSTER_ID
    name = "Onkyo AVR control"
    ep_attribute = "onkyo_avr"

    class AttributeDefs(BaseAttributeDefs):
        """Attributes of the Onkyo AVR control cluster."""

        input: Final = ZCLAttributeDef(
            id=0x0000,
            type=OnkyoAvrInput,
            access="r",
            manufacturer_code=MANUFACTURER_CODE,
        )
        mute: Final = ZCLAttributeDef(
            id=0x0001,
            type=t.Bool,
            access="r",
            manufacturer_code=MANUFACTURER_CODE,
        )

    class ServerCommandDefs(BaseCommandDefs):
        """Commands of the Onkyo AVR control cluster."""

        input_select: Final = ZCLCommandDef(
            id=0x00,
            schema={"input": OnkyoAvrInput},
            manufacturer_code=MANUFACTURER_CODE,
        )
        mute: Final = ZCLCommandDef(
            id=0x01,
            schema={"mute": OnkyoAvrMute},
            manufacturer_code=MANUFACTURER_CODE,
        )
        volume_step: Final = ZCLCommandDef(
            id=0x02,
            schema={"steps": t.int8s},
            manufacturer_code=MANUFACTURER_CODE,
        )
        osd_menu: Final = ZCLCommandDef(
            id=0x03,
            schema={},
            manufacturer_code=MANUFACTURER_CODE,
        )

    async def write_attributes(
        self,
        attributes: dict[str | int | ZCLAttributeDef, Any],
        manufacturer: int | UndefinedType | None = UNDEFINED,
        *,
        update_cache: bool = True,
        **kwargs: Any,
    ) -> list[list[foundation.WriteAttributesStatusRecord]]:
        """Send the command that backs an attribute instead of writing it.

        The sample reports the input and the mute state through the attributes
        and only acts on the commands, so the write of the entity that owns the
        attribute is translated into the matching command. The attribute is
        updated right away, so the entity does not wait for the receiver to
        report the state it just applied.
        """
        results: list[foundation.WriteAttributesStatusRecord] = []
        unwritten: dict[str | int | ZCLAttributeDef, Any] = {}

        for attr, value in attributes.items():
            try:
                attr_def = self.find_attribute(attr)
            except KeyError:
                unwritten[attr] = value
                continue

            if attr_def.id == self.AttributeDefs.input.id:
                await self.input_select(OnkyoAvrInput(value))
            elif attr_def.id == self.AttributeDefs.mute.id:
                mute = OnkyoAvrMute.MUTE if value else OnkyoAvrMute.UNMUTE
                await self.mute(mute)
            else:
                unwritten[attr] = value
                continue

            if update_cache:
                self._update_attribute(attr_def.id, value)

            results.append(
                foundation.WriteAttributesStatusRecord(
                    status=foundation.Status.SUCCESS,
                    attrid=attr_def.id,
                )
            )

        if unwritten:
            remaining = await super().write_attributes(
                unwritten, manufacturer=manufacturer, update_cache=update_cache, **kwargs
            )
            results.extend(remaining[0])

        return [results]


(
    QuirkBuilder(MANUFACTURER, MODEL)
    .replaces(OnkyoAvrControlCluster, endpoint_id=ENDPOINT_ID)
    # The sample does not configure attribute reporting, so the input and the
    # mute state are read when the device is set up, instead of being taken
    # from the attribute cache.
    .enum(
        OnkyoAvrControlCluster.AttributeDefs.input.name,
        OnkyoAvrInput,
        OnkyoAvrControlCluster.cluster_id,
        endpoint_id=ENDPOINT_ID,
        entity_type=EntityType.STANDARD,
        attribute_initialized_from_cache=False,
        translation_key="onkyo_avr_input",
        fallback_name="Input",
    )
    .switch(
        OnkyoAvrControlCluster.AttributeDefs.mute.name,
        OnkyoAvrControlCluster.cluster_id,
        endpoint_id=ENDPOINT_ID,
        entity_type=EntityType.STANDARD,
        attribute_initialized_from_cache=False,
        translation_key="onkyo_avr_mute",
        fallback_name="Mute",
    )
    .command_button(
        OnkyoAvrControlCluster.ServerCommandDefs.osd_menu.name,
        OnkyoAvrControlCluster.cluster_id,
        endpoint_id=ENDPOINT_ID,
        entity_type=EntityType.STANDARD,
        translation_key="onkyo_avr_osd_menu",
        fallback_name="On-screen display menu",
    )
    .add_to_registry()
)
