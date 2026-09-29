"""Diagnostica del pannello: da quanto e' acceso, memoria, dashboard mostrata."""

from __future__ import annotations

from dataclasses import dataclass
from collections.abc import Callable
from typing import Any

from homeassistant.components.sensor import (
    SensorDeviceClass,
    SensorEntity,
    SensorEntityDescription,
    SensorStateClass,
)
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers.dispatcher import async_dispatcher_connect
from homeassistant.const import EntityCategory
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from .const import DOMAIN, SIGNAL_NEW_PANEL
from .entity import PannelloEntity


@dataclass(frozen=True, kw_only=True)
class Misura(SensorEntityDescription):
    leggi: Callable[[dict[str, Any]], Any]


MISURE: tuple[Misura, ...] = (
    Misura(
        key="acceso_da",
        native_unit_of_measurement="s",
        device_class=SensorDeviceClass.DURATION,
        state_class=SensorStateClass.TOTAL_INCREASING,
        entity_category=EntityCategory.DIAGNOSTIC,
        leggi=lambda s: s.get("acceso_da"),
    ),
    Misura(
        key="ram_interna",
        native_unit_of_measurement="B",
        device_class=SensorDeviceClass.DATA_SIZE,
        state_class=SensorStateClass.MEASUREMENT,
        entity_category=EntityCategory.DIAGNOSTIC,
        leggi=lambda s: s.get("ram_interna"),
    ),
    Misura(
        key="ram_psram",
        native_unit_of_measurement="B",
        device_class=SensorDeviceClass.DATA_SIZE,
        state_class=SensorStateClass.MEASUREMENT,
        entity_category=EntityCategory.DIAGNOSTIC,
        leggi=lambda s: s.get("ram_psram"),
    ),
    Misura(
        # Quante volte il collegamento fra i due processori si e' dovuto
        # riprendere: se questo numero cresce, e' li' che c'e' da guardare.
        key="recuperi",
        state_class=SensorStateClass.TOTAL_INCREASING,
        entity_category=EntityCategory.DIAGNOSTIC,
        leggi=lambda s: s.get("recuperi"),
    ),
    Misura(
        # Dopo un crash il pannello si riavvia da solo: senza questo, il
        # crash passerebbe inosservato.
        key="riavvio",
        icon="mdi:restart-alert",
        entity_category=EntityCategory.DIAGNOSTIC,
        leggi=lambda s: s.get("riavvio"),
    ),
    Misura(
        key="dashboard",
        icon="mdi:view-dashboard",
        leggi=lambda s: s.get("dashboard"),
    ),
    Misura(
        # Tensione letta dal coprocessore sulla scheda. Attenzione a come si
        # legge: con il pannello alimentato dall'USB e nessuna batteria
        # collegata, questa e' la tensione dell'alimentatore, non di una
        # batteria che non c'e'.
        key="alimentazione_mv",
        native_unit_of_measurement="mV",
        device_class=SensorDeviceClass.VOLTAGE,
        state_class=SensorStateClass.MEASUREMENT,
        entity_category=EntityCategory.DIAGNOSTIC,
        leggi=lambda s: s.get("alimentazione_mv"),
    ),
    Misura(
        key="alimentazione_pct",
        native_unit_of_measurement="%",
        device_class=SensorDeviceClass.BATTERY,
        state_class=SensorStateClass.MEASUREMENT,
        entity_category=EntityCategory.DIAGNOSTIC,
        leggi=lambda s: s.get("alimentazione_pct"),
    ),
    Misura(
        # Il numero grezzo del coprocessore. Cosa voglia dire non e'
        # documentato: sta qui perche' lo si possa guardare mentre cambia -
        # staccando la corrente, collegando una batteria - e dargli un nome
        # quando lo si sara' capito. Meglio un numero onesto che un
        # "in carica: si" inventato.
        key="alimentazione_stato",
        icon="mdi:power-plug",
        entity_category=EntityCategory.DIAGNOSTIC,
        entity_registry_enabled_default=False,
        leggi=lambda s: s.get("alimentazione_stato"),
    ),
)


async def async_setup_entry(
    hass: HomeAssistant, entry: ConfigEntry, aggiungi: AddEntitiesCallback
) -> None:
    store = hass.data[DOMAIN]

    @callback
    def nuovo(pid: str) -> None:
        aggiungi([Sensore(store, pid, m) for m in MISURE])

    for pid in list(store.noti):
        nuovo(pid)
    entry.async_on_unload(async_dispatcher_connect(hass, SIGNAL_NEW_PANEL, nuovo))


class Sensore(PannelloEntity, SensorEntity):
    entity_description: Misura

    def __init__(self, store, pid: str, m: Misura) -> None:
        super().__init__(store, pid, m.key)
        self.entity_description = m

    @property
    def native_value(self) -> Any:
        return self.entity_description.leggi(self.stato)
