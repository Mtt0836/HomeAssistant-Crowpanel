"""Aggiunta dell'integrazione e impostazioni dei pannelli.

C'e' una voce sola ("Pannelli CrowPanel"): i pannelli veri compaiono dentro
come dispositivi, man mano che si presentano. Non c'e' niente da scrivere
perche' non c'e' niente da sapere: il pannello dira' da solo chi e'.
"""

from __future__ import annotations

from typing import Any

import voluptuous as vol

from homeassistant.config_entries import ConfigEntry, ConfigFlow, OptionsFlow
from homeassistant.core import callback
from homeassistant.helpers import selector

from .const import (
    CONF_AFTER_MIN,
    CONF_FOLDER,
    CONF_MANAGED,
    CONF_OFF_MIN,
    CONF_PANELS,
    CONF_PHOTO_S,
    CONF_SHUFFLE,
    CONF_VALUES,
    DOMAIN,
)


class CrowPanelConfigFlow(ConfigFlow, domain=DOMAIN):
    VERSION = 1

    async def async_step_user(self, user_input: dict[str, Any] | None = None):
        await self.async_set_unique_id(DOMAIN)
        self._abort_if_unique_id_configured()
        if user_input is None:
            return self.async_show_form(step_id="user", data_schema=vol.Schema({}))
        return self.async_create_entry(title="Pannelli CrowPanel", data={})

    @staticmethod
    @callback
    def async_get_options_flow(config_entry: ConfigEntry) -> OptionsFlow:
        return OpzioniFlow(config_entry)


class OpzioniFlow(OptionsFlow):
    """Impostazioni dello slideshow, un pannello alla volta."""

    def __init__(self, entry: ConfigEntry) -> None:
        # Volutamente non si assegna self.config_entry: nelle versioni recenti
        # di Home Assistant e' gia' fornito e riassegnarlo e' deprecato.
        self._entry = entry
        self._pid: str | None = None

    def _store(self):
        return self.hass.data.get(DOMAIN)

    async def async_step_init(self, user_input: dict[str, Any] | None = None):
        store = self._store()
        pannelli = list(store.noti) if store else []
        if not pannelli:
            return self.async_abort(reason="nessun_pannello")
        if len(pannelli) == 1:
            self._pid = pannelli[0]
            return await self.async_step_pannello()
        if user_input is not None:
            self._pid = user_input["pannello"]
            return await self.async_step_pannello()

        nomi = {p: (store.noti[p].get("nome") or p) for p in pannelli}
        return self.async_show_form(
            step_id="init",
            data_schema=vol.Schema(
                {vol.Required("pannello"): vol.In(nomi)}
            ),
        )

    async def async_step_pannello(self, user_input: dict[str, Any] | None = None):
        store = self._store()
        pid = self._pid
        assert pid is not None
        quanti = int(store.noti.get(pid, {}).get("max_valori", 4))
        vecchie = dict(self._entry.options.get(CONF_PANELS, {}).get(pid, {}))
        valori = list(vecchie.get(CONF_VALUES, []))

        if user_input is not None:
            nuovi = []
            for i in range(quanti):
                ent = user_input.get(f"valore_{i}")
                if ent:
                    nuovi.append({"entita": ent, "nome": user_input.get(f"nome_{i}", "")})
            dati = {
                CONF_MANAGED: user_input[CONF_MANAGED],
                CONF_VALUES: nuovi,
                CONF_AFTER_MIN: user_input[CONF_AFTER_MIN],
                CONF_OFF_MIN: user_input[CONF_OFF_MIN],
                CONF_PHOTO_S: user_input[CONF_PHOTO_S],
                CONF_SHUFFLE: user_input[CONF_SHUFFLE],
                CONF_FOLDER: user_input[CONF_FOLDER],
            }
            tutte = dict(self._entry.options.get(CONF_PANELS, {}))
            tutte[pid] = dati
            return self.async_create_entry(title="", data={CONF_PANELS: tutte})

        campi: dict[Any, Any] = {
            vol.Required(CONF_MANAGED, default=vecchie.get(CONF_MANAGED, True)): bool,
        }
        for i in range(quanti):
            ent = valori[i]["entita"] if i < len(valori) else None
            nome = valori[i].get("nome", "") if i < len(valori) else ""
            campi[vol.Optional(f"valore_{i}", description={"suggested_value": ent})] = (
                selector.EntitySelector(selector.EntitySelectorConfig())
            )
            campi[vol.Optional(f"nome_{i}", description={"suggested_value": nome})] = str
        campi.update(
            {
                vol.Required(CONF_AFTER_MIN, default=vecchie.get(CONF_AFTER_MIN, 2)): vol.Coerce(float),
                vol.Required(CONF_OFF_MIN, default=vecchie.get(CONF_OFF_MIN, 10)): vol.Coerce(float),
                vol.Required(CONF_PHOTO_S, default=vecchie.get(CONF_PHOTO_S, 10)): vol.Coerce(int),
                vol.Required(CONF_SHUFFLE, default=vecchie.get(CONF_SHUFFLE, True)): bool,
                vol.Required(CONF_FOLDER, default=vecchie.get(CONF_FOLDER, "/sdcard/foto")): str,
            }
        )
        return self.async_show_form(
            step_id="pannello",
            data_schema=vol.Schema(campi),
            description_placeholders={
                "pannello": store.noti.get(pid, {}).get("nome") or pid,
                "quanti": str(quanti),
            },
        )
