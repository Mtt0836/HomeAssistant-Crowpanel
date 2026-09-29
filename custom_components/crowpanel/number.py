"""Luminosita' dello schermo."""

from __future__ import annotations

from homeassistant.components.number import NumberEntity, NumberMode
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
        aggiungi([Luminosita(store, pid)])

    for pid in list(store.noti):
        nuovo(pid)
    entry.async_on_unload(async_dispatcher_connect(hass, SIGNAL_NEW_PANEL, nuovo))


class Luminosita(PannelloEntity, NumberEntity):
    _attr_icon = "mdi:brightness-6"
    _attr_native_min_value = 1
    _attr_native_max_value = 100
    _attr_native_step = 1
    _attr_native_unit_of_measurement = "%"
    _attr_mode = NumberMode.SLIDER

    def __init__(self, store, pid: str) -> None:
        super().__init__(store, pid, "luminosita")

    @property
    def native_value(self) -> float | None:
        v = self.stato.get("luminosita")
        # A schermo spento il pannello dice 0, che qui sarebbe fuori scala:
        # meglio non mostrare niente che mostrare un valore impossibile.
        return v if v else None

    async def async_set_native_value(self, value: float) -> None:
        self._store.comanda(self._pid, "luminosita", valore=int(value))
