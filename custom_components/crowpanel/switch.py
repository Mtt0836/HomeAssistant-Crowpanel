"""Lo schermo del pannello, acceso o spento."""

from __future__ import annotations

from homeassistant.components.switch import SwitchEntity
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers.dispatcher import async_dispatcher_connect
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from .const import DOMAIN, SIGNAL_NEW_PANEL
from .entity import PannelloEntity


async def async_setup_entry(
    hass: HomeAssistant, entry: ConfigEntry, aggiungi: AddEntitiesCallback
) -> None:
    store = hass.data[DOMAIN]

    @callback
    def nuovo(pid: str) -> None:
        aggiungi([Schermo(store, pid)])

    # I pannelli gia' presentati non fanno scattare il segnale: vanno presi ora.
    for pid in list(store.noti):
        nuovo(pid)
    entry.async_on_unload(async_dispatcher_connect(hass, SIGNAL_NEW_PANEL, nuovo))


class Schermo(PannelloEntity, SwitchEntity):
    _attr_icon = "mdi:monitor"

    def __init__(self, store, pid: str) -> None:
        super().__init__(store, pid, "schermo")

    @property
    def is_on(self) -> bool | None:
        return self.stato.get("schermo")

    async def async_turn_on(self, **kwargs) -> None:
        self._store.comanda(self._pid, "schermo", acceso=True)

    async def async_turn_off(self, **kwargs) -> None:
        self._store.comanda(self._pid, "schermo", acceso=False)
