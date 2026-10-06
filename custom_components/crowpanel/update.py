"""L'aggiornamento del firmware come lo mostra Home Assistant.

PERCHE' UN'ENTITA' "UPDATE" E NON DEI SENSORI. I sensori c'erano gia' - stato e
percentuale - ma chi guarda non vede niente di utile: due righe di diagnostica
in fondo alla pagina del dispositivo, da mettere insieme a mente. Home Assistant
ha un tipo di entita' fatto per questo, e usandolo si ottiene gratis quello che
serve davvero: nella pagina del pannello compare "firmware 0.3.0, disponibile
0.3.1" con il pulsante Installa, e durante l'aggiornamento una barra che si
muove. Lo stesso posto e lo stesso aspetto degli aggiornamenti di Home Assistant
stesso, quindi non c'e' niente da imparare.

COS'E' LA "VERSIONE DISPONIBILE". La piu' recente che sta nell'archivio, non una
versione cercata su internet: questo pannello si aggiorna con i firmware che ci
mette dentro chi lo usa. Se l'archivio e' vuoto, o se dentro c'e' solo quella
gia' installata, non risulta nessun aggiornamento - che e' la verita'.

IL CONFRONTO E' FRA TESTI, non fra numeri. Le versioni di questo firmware sono
numeri separati da punti, e per sapere se 0.3.10 viene dopo 0.3.9 i punti vanno
confrontati come numeri, altrimenti "10" sembra minore di "9". Lo fa _chiave().
"""

from __future__ import annotations

import logging
import re
from typing import Any

from homeassistant.components.update import (
    UpdateDeviceClass,
    UpdateEntity,
    UpdateEntityFeature,
)
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant, callback
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers.dispatcher import async_dispatcher_connect
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from .const import DOMAIN, SIGNAL_NEW_PANEL
from .entity import PannelloEntity

_LOGGER = logging.getLogger(__name__)


async def async_setup_entry(
    hass: HomeAssistant, entry: ConfigEntry, aggiungi: AddEntitiesCallback
) -> None:
    store = hass.data[DOMAIN]

    @callback
    def nuovo(pid: str) -> None:
        aggiungi([Firmware(store, pid)])

    for pid in list(store.noti):
        nuovo(pid)
    entry.async_on_unload(async_dispatcher_connect(hass, SIGNAL_NEW_PANEL, nuovo))


def _chiave(v: str) -> tuple:
    """Una versione in forma confrontabile: 0.3.10 viene dopo 0.3.9."""
    pezzi = re.findall(r"\d+", v or "")
    return tuple(int(p) for p in pezzi) if pezzi else (0,)


class Firmware(PannelloEntity, UpdateEntity):
    _attr_device_class = UpdateDeviceClass.FIRMWARE
    _attr_supported_features = (
        UpdateEntityFeature.INSTALL | UpdateEntityFeature.PROGRESS
    )
    # Le altre entita' non interrogano niente: lo stato arriva da solo dal
    # pannello. Questa invece deve sapere cosa c'e' sul disco di Home Assistant,
    # e quello nessuno glielo manda - quindi va a vederlo lei. Senza questa riga
    # async_update non verrebbe chiamata mai e l'archivio risulterebbe sempre
    # vuoto: nessun aggiornamento disponibile, per sempre.
    _attr_should_poll = True

    def __init__(self, store, pid: str) -> None:
        super().__init__(store, pid, "firmware_update")

    # ------------------------------------------------------------ versioni

    @property
    def installed_version(self) -> str | None:
        """Dal pannello arriva "HA_Display 0.3.1 (Oct 6 2026 18:48:56)": qui
        serve il solo numero, che e' quello che si confronta."""
        fw = self.stato.get("firmware") or ""
        m = re.search(r"(\d+\.\d+(?:\.\d+)?)", fw)
        return m.group(1) if m else (fw or None)

    def _archivio(self) -> list[dict[str, Any]]:
        arch = self.hass.data.get(f"{DOMAIN}_archivio_fw")
        if not arch:
            return []
        # Letto dalla cache che riempie async_update: qui non si tocca il disco,
        # perche' questa proprieta' la chiama Home Assistant a ogni ridisegno.
        return self.hass.data.get(f"{DOMAIN}_fw_elenco") or []

    @property
    def latest_version(self) -> str | None:
        voci = [v for v in self._archivio() if v.get("versione")]
        if not voci:
            # Niente archivio: si dichiara gia' aggiornato invece di inventare
            # un aggiornamento che non esiste.
            return self.installed_version
        piu_nuova = max(voci, key=lambda v: _chiave(v["versione"]))
        installata = self.installed_version or "0"
        # Mai indietro: se in archivio c'e' solo roba piu' vecchia di quella
        # installata, non e' un aggiornamento. Per tornare indietro apposta c'e'
        # il menu dell'archivio, dove la scelta e' esplicita.
        if _chiave(piu_nuova["versione"]) <= _chiave(installata):
            return installata
        return piu_nuova["versione"]

    @property
    def release_summary(self) -> str | None:
        voci = [v for v in self._archivio() if v.get("versione")]
        if not voci:
            return None
        piu_nuova = max(voci, key=lambda v: _chiave(v["versione"]))
        note = piu_nuova.get("note") or ""
        quando = piu_nuova.get("data") or ""
        pezzi = [p for p in (note, f"compilato {quando}" if quando else "") if p]
        return " - ".join(pezzi) or None

    # ------------------------------------------------------------ avanzamento

    @property
    def in_progress(self) -> bool:
        return (self.stato.get("ota") or "fermo") not in ("fermo", "fallito")

    @property
    def update_percentage(self) -> int | None:
        if not self.in_progress:
            return None
        return self.stato.get("ota_pct")

    @property
    def extra_state_attributes(self) -> dict[str, Any]:
        """Lo stato in parole e se l'immagine e' ancora in prova: durante i due
        minuti di prova il firmware e' installato ma non ancora confermato, e
        chi guarda ha il diritto di saperlo."""
        return {
            "stato": self.stato.get("ota"),
            "in_prova": self.stato.get("ota_in_prova"),
            "firmware_completo": self.stato.get("firmware"),
        }

    # ------------------------------------------------------------ installazione

    async def async_update(self) -> None:
        """Rilegge l'archivio. Gira di rado - should_poll e' falso per le altre
        entita', ma questa ha bisogno di sapere cosa c'e' su disco, e leggerlo a
        ogni ridisegno della pagina sarebbe sprecato."""
        from .firmware import archivio

        try:
            voci = await archivio(self.hass).elenca(self.hass)
        except Exception as e:  # noqa: BLE001
            _LOGGER.debug("archivio non leggibile: %s", e)
            return
        self.hass.data[f"{DOMAIN}_fw_elenco"] = voci

    async def async_install(self, version: str | None, backup: bool, **kwargs: Any) -> None:
        voci = [v for v in self._archivio() if v.get("versione")]
        if not voci:
            raise HomeAssistantError(
                "L'archivio dei firmware e' vuoto: caricane uno da Configura."
            )
        if version:
            scelta = next((v for v in voci if v["versione"] == version), None)
        else:
            scelta = max(voci, key=lambda v: _chiave(v["versione"]))
        if not scelta:
            raise HomeAssistantError(f"La versione {version} non e' in archivio.")

        await self.hass.services.async_call(
            DOMAIN, "invia_firmware",
            {"pannello": self._pid, "nome": scelta["nome"]},
            blocking=True,
        )

