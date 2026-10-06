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
        # La stima del pannello, da una curva tensione-carica che si tara da
        # sola su una scarica completa. Non e' la percentuale del coprocessore
        # (quella sta qui sotto, disabilitata): il coprocessore segue la
        # tensione, e sotto carica la tensione non e' quella della cella.
        key="alimentazione_pct",
        native_unit_of_measurement="%",
        device_class=SensorDeviceClass.BATTERY,
        state_class=SensorStateClass.MEASUREMENT,
        entity_category=EntityCategory.DIAGNOSTIC,
        leggi=lambda s: s.get("alimentazione_pct"),
    ),
    Misura(
        # Lo stato di carica in parole. L'enum e' quello del BSP di Elecrow
        # (bsp_stc8h1kxx.h). Da leggere sapendo questo: senza batteria
        # collegata il coprocessore dice "carica" esattamente come con una
        # batteria piena, perche' da' per finita una carica che non parte.
        key="alimentazione",
        icon="mdi:power-plug",
        device_class=SensorDeviceClass.ENUM,
        options=["sconosciuto", "in_carica", "carica", "a_batteria", "errore"],
        entity_category=EntityCategory.DIAGNOSTIC,
        leggi=lambda s: s.get("alimentazione"),
    ),
    Misura(
        # Quanto resta con il consumo attuale. C'e' solo se qualcuno ha
        # misurato a pinza i due assorbimenti del pannello (comando console
        # "batteria ma <spento> <acceso>") e il pannello ha completato una
        # scarica di taratura: senza quei dati il tempo non si puo' sapere, e
        # un numero inventato qui sarebbe peggio di nessun numero.
        key="batteria_autonomia_min",
        native_unit_of_measurement="min",
        device_class=SensorDeviceClass.DURATION,
        state_class=SensorStateClass.MEASUREMENT,
        icon="mdi:battery-clock",
        entity_category=EntityCategory.DIAGNOSTIC,
        leggi=lambda s: s.get("batteria_autonomia_min") or None,
    ),
    Misura(
        # La capacita' vera del pacco, misurata sulla scarica di taratura.
        # Dice se i mAh stampati sull'etichetta sono quelli.
        key="batteria_mah",
        native_unit_of_measurement="mAh",
        icon="mdi:battery-heart-variant",
        entity_category=EntityCategory.DIAGNOSTIC,
        entity_registry_enabled_default=False,
        leggi=lambda s: s.get("batteria_mah") or None,
    ),
    Misura(
        # La percentuale del coprocessore, tenuta per confronto: e' quella che
        # saltava attaccando e staccando la corrente.
        key="alimentazione_pct_stc8",
        native_unit_of_measurement="%",
        state_class=SensorStateClass.MEASUREMENT,
        icon="mdi:battery-unknown",
        entity_category=EntityCategory.DIAGNOSTIC,
        entity_registry_enabled_default=False,
        leggi=lambda s: s.get("alimentazione_pct_stc8"),
    ),
    Misura(
        # Quale firmware sta girando: nome del progetto, versione e data di
        # compilazione, letti dal descrittore dentro l'immagine. Serve a sapere
        # cosa c'e' installato senza doverlo chiedere al pannello.
        key="firmware",
        icon="mdi:chip",
        entity_category=EntityCategory.DIAGNOSTIC,
        leggi=lambda s: s.get("firmware"),
    ),
    Misura(
        # A che punto e' un aggiornamento. Resta "fermo" quasi sempre: diventa
        # interessante per il minuto in cui si aggiorna.
        key="ota",
        icon="mdi:cloud-download",
        device_class=SensorDeviceClass.ENUM,
        options=["fermo", "scarica", "verifica", "pronto", "fallito"],
        entity_category=EntityCategory.DIAGNOSTIC,
        leggi=lambda s: s.get("ota"),
    ),
    Misura(
        key="ota_pct",
        native_unit_of_measurement="%",
        state_class=SensorStateClass.MEASUREMENT,
        icon="mdi:progress-download",
        entity_category=EntityCategory.DIAGNOSTIC,
        entity_registry_enabled_default=False,
        leggi=lambda s: s.get("ota_pct"),
    ),
    Misura(
        # Il numero grezzo del coprocessore, ora che si sa cosa vuol dire:
        # 0 inattivo, 1 in carica, 2 piena, 3 non in carica, 4 errore.
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
