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
        """Il bivio: impostazioni dello slideshow oppure archivio dei firmware."""
        return self.async_show_menu(step_id="init", menu_options=["slideshow", "firmware"])

    # ------------------------------------------------------------ slideshow

    async def async_step_slideshow(self, user_input: dict[str, Any] | None = None):
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
            step_id="slideshow",
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

    # ------------------------------------------------------------- firmware

    # L'archivio dei firmware vive qui dentro invece che in una pagina sua.
    #
    # Il motivo e' pratico: un'integrazione personalizzata non puo' aggiungere
    # pagine a Home Assistant senza portarsi dietro del frontend da compilare e
    # aggiornare a ogni versione. Il meccanismo delle opzioni invece c'e' gia',
    # sa fare i menu, i moduli e il caricamento dei file, e si apre dal browser
    # con due clic: Impostazioni, Dispositivi e servizi, Configura.

    async def async_step_firmware(self, user_input: dict[str, Any] | None = None):
        return self.async_show_menu(
            step_id="firmware",
            menu_options=["fw_carica", "fw_invia", "fw_elimina"],
        )

    async def _archivio(self):
        from .firmware import archivio

        return archivio(self.hass)

    async def _scelte_fw(self) -> dict[str, str]:
        """Le voci dell'archivio, descritte come le si vuole leggere."""
        voci = await (await self._archivio()).elenca(self.hass)
        fuori = {}
        for v in voci:
            desc = v.get("versione") or "?"
            if v.get("data"):
                desc += f" del {v['data']}"
            fuori[v["nome"]] = f"{v['nome']} - {desc} ({v['byte'] // 1024} kB)"
        return fuori

    async def async_step_fw_carica(self, user_input: dict[str, Any] | None = None):
        if user_input is not None:
            from homeassistant.components.file_upload import process_uploaded_file

            def leggi(file_id: str) -> bytes:
                with process_uploaded_file(self.hass, file_id) as percorso:
                    return percorso.read_bytes()

            dati = await self.hass.async_add_executor_job(leggi, user_input["file"])
            try:
                voce = await (await self._archivio()).aggiungi(
                    self.hass, dati, user_input.get("nome") or "firmware.bin",
                    user_input.get("note", ""),
                )
            except Exception as e:  # noqa: BLE001
                return self.async_abort(
                    reason="fw_rifiutato", description_placeholders={"errore": str(e)}
                )
            return self.async_abort(
                reason="fw_caricato",
                description_placeholders={
                    "nome": voce["nome"],
                    "versione": voce.get("versione", "?"),
                    "data": voce.get("data", "?"),
                    "kb": str(voce["byte"] // 1024),
                    "sha256": voce["sha256"][:16] + "...",
                },
            )

        return self.async_show_form(
            step_id="fw_carica",
            data_schema=vol.Schema(
                {
                    vol.Required("file"): selector.FileSelector(
                        selector.FileSelectorConfig(accept=".bin")
                    ),
                    vol.Optional("nome"): str,
                    vol.Optional("note"): str,
                }
            ),
        )

    async def async_step_fw_invia(self, user_input: dict[str, Any] | None = None):
        store = self._store()
        pannelli = list(store.noti) if store else []
        if not pannelli:
            return self.async_abort(reason="nessun_pannello")
        scelte = await self._scelte_fw()
        if not scelte:
            return self.async_abort(reason="archivio_vuoto")

        if user_input is not None:
            try:
                esito = await self.hass.services.async_call(
                    DOMAIN, "invia_firmware",
                    {"pannello": user_input["pannello"], "nome": user_input["firmware"]},
                    blocking=True, return_response=True,
                )
            except Exception as e:  # noqa: BLE001
                return self.async_abort(
                    reason="fw_rifiutato", description_placeholders={"errore": str(e)}
                )
            return self.async_abort(
                reason="fw_mandato",
                description_placeholders={
                    "nome": str((esito or {}).get("nome", user_input["firmware"])),
                    "versione": str((esito or {}).get("versione", "?")),
                },
            )

        nomi = {p: (store.noti[p].get("nome") or p) for p in pannelli}
        return self.async_show_form(
            step_id="fw_invia",
            data_schema=vol.Schema(
                {
                    vol.Required("pannello", default=pannelli[0]): vol.In(nomi),
                    vol.Required("firmware"): vol.In(scelte),
                }
            ),
        )

    async def async_step_fw_elimina(self, user_input: dict[str, Any] | None = None):
        scelte = await self._scelte_fw()
        if not scelte:
            return self.async_abort(reason="archivio_vuoto")

        if user_input is not None:
            if not user_input.get("confermo"):
                return self.async_abort(reason="fw_non_eliminato")
            await (await self._archivio()).elimina(self.hass, user_input["firmware"])
            return self.async_abort(
                reason="fw_eliminato",
                description_placeholders={"nome": user_input["firmware"]},
            )

        # La conferma e' una casella da spuntare e non un semplice "Invia":
        # togliere dall'archivio la versione a cui si sarebbe tornati indietro
        # e' proprio la cosa da non fare per sbaglio.
        return self.async_show_form(
            step_id="fw_elimina",
            data_schema=vol.Schema(
                {
                    vol.Required("firmware"): vol.In(scelte),
                    vol.Required("confermo", default=False): bool,
                }
            ),
        )
