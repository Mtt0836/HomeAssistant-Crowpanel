"""Base comune alle entita' del pannello."""

from __future__ import annotations

import time
from typing import Any

from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.dispatcher import async_dispatcher_connect
from homeassistant.helpers.entity import Entity

from .const import DOMAIN, OFFLINE_AFTER_S, SIGNAL_UPDATE


class PannelloEntity(Entity):
    """Un'entita' legata a un pannello.

    Lo stato non lo andiamo a chiedere: arriva da solo, quindi qui non c'e'
    nessun polling e should_poll resta falso. Quando il pannello smette di
    farsi vivo le entita' diventano "non disponibile" invece di restare
    ferme sull'ultimo valore, che sarebbe peggio: uno guarda e crede che lo
    schermo sia acceso mentre magari e' staccato da un'ora.
    """

    _attr_should_poll = False
    _attr_has_entity_name = True

    # Il nome visibile lo danno le traduzioni (translations/*.json), non il
    # codice: scrivendolo qui vincerebbe sempre l'italiano anche per chi ha
    # Home Assistant in inglese.
    def __init__(self, store, pid: str, chiave: str) -> None:
        self._store = store
        self._pid = pid
        self._attr_translation_key = chiave
        self._attr_unique_id = f"{pid}_{chiave}"

    @property
    def stato(self) -> dict[str, Any]:
        return self._store.stato.get(self._pid, {})

    @property
    def available(self) -> bool:
        visto = self._store.visto.get(self._pid)
        return visto is not None and (time.monotonic() - visto) < OFFLINE_AFTER_S

    @property
    def device_info(self) -> DeviceInfo:
        info = self._store.noti.get(self._pid, {})
        return DeviceInfo(
            identifiers={(DOMAIN, self._pid)},
            name=info.get("nome") or f"Pannello {self._pid[-6:]}",
            manufacturer="Elecrow",
            model=info.get("modello") or "CrowPanel",
            sw_version=info.get("versione"),
            suggested_area=info.get("stanza") or None,
            configuration_url=f"https://{info['indirizzo']}/" if info.get("indirizzo") else None,
        )

    async def async_added_to_hass(self) -> None:
        self.async_on_remove(
            async_dispatcher_connect(
                self.hass, f"{SIGNAL_UPDATE}_{self._pid}", self.async_write_ha_state
            )
        )
