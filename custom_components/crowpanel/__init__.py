"""Integrazione per il pannello a muro CrowPanel.

Il verso del collegamento e' l'opposto del solito: non e' Home Assistant ad
andare a cercare il pannello sulla rete, e' il pannello che tiene aperto il
WebSocket verso HA e che, appena il collegamento e' pronto, si presenta. Da
qui vengono due cose che sarebbe un peccato perdere di vista:

  * non c'e' niente da configurare a mano, nessun indirizzo IP e nessuna
    password da dare a HA: il pannello si e' gia' autenticato per conto suo;
  * funziona da fuori casa esattamente come da dentro, perche' il traffico
    passa dalla connessione che il pannello ha gia', non da una porta aperta.

Il pannello parla con due comandi WebSocket ("crowpanel/announce" e
"crowpanel/state"); HA gli risponde mandando eventi "crowpanel_command" sul
bus, che il pannello ascolta e filtra per identificativo.
"""

from __future__ import annotations

import logging
import time
from typing import Any

import voluptuous as vol

from homeassistant.components import websocket_api
from homeassistant.config_entries import ConfigEntry
from homeassistant.const import Platform
from homeassistant.core import HomeAssistant, ServiceCall, callback
from homeassistant.helpers import config_validation as cv
from homeassistant.helpers.dispatcher import async_dispatcher_send
from homeassistant.helpers.typing import ConfigType

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
    EVENT_COMMAND,
    SIGNAL_NEW_PANEL,
    SIGNAL_UPDATE,
)

_LOGGER = logging.getLogger(__name__)

PLATFORMS = [Platform.SWITCH, Platform.NUMBER, Platform.SENSOR]

CONFIG_SCHEMA = cv.config_entry_only_config_schema(DOMAIN)


class Pannelli:
    """Quello che sappiamo dei pannelli, finche' Home Assistant e' acceso.

    Non viene salvato su disco apposta: e' roba che il pannello ridice da
    solo a ogni collegamento, e tenerne una copia vecchia servirebbe solo a
    far comparire dispositivi che magari non ci sono piu'.
    """

    def __init__(self, hass: HomeAssistant) -> None:
        self.hass = hass
        self.noti: dict[str, dict[str, Any]] = {}
        self.stato: dict[str, dict[str, Any]] = {}
        self.visto: dict[str, float] = {}
        self.entry: ConfigEntry | None = None

    def aggiorna(self, pid: str, info: dict | None = None, stato: dict | None = None) -> bool:
        nuovo = pid not in self.noti
        if info:
            self.noti[pid] = info
        if stato:
            self.stato[pid] = stato
        self.visto[pid] = time.monotonic()
        if nuovo:
            async_dispatcher_send(self.hass, SIGNAL_NEW_PANEL, pid)
        async_dispatcher_send(self.hass, f"{SIGNAL_UPDATE}_{pid}")
        return nuovo

    def opzioni(self, pid: str) -> dict[str, Any]:
        if not self.entry:
            return {}
        return dict(self.entry.options.get(CONF_PANELS, {}).get(pid, {}))

    def gestito(self, pid: str) -> bool:
        return bool(self.opzioni(pid).get(CONF_MANAGED))

    def config_slideshow(self, pid: str) -> dict[str, Any]:
        """Le opzioni nel formato che il pannello si aspetta."""
        o = self.opzioni(pid)
        valori = []
        for v in o.get(CONF_VALUES, []):
            if v.get("entita"):
                valori.append({"id": v["entita"], "nome": (v.get("nome") or "").strip()})
        return {
            CONF_AFTER_MIN: o.get(CONF_AFTER_MIN, 2),
            CONF_OFF_MIN: o.get(CONF_OFF_MIN, 10),
            CONF_PHOTO_S: o.get(CONF_PHOTO_S, 10),
            CONF_SHUFFLE: o.get(CONF_SHUFFLE, True),
            CONF_FOLDER: o.get(CONF_FOLDER, "/sdcard/foto"),
            "sovrimpressione": valori,
        }

    @callback
    def comanda(self, pid: str, comando: str, **extra: Any) -> None:
        """Manda un comando al pannello.

        Passa dal bus degli eventi invece che da un canale privato perche'
        cosi' lo stesso comando si puo' provare a mano da Strumenti per
        sviluppatori, e si vede passare quando qualcosa non torna.
        """
        dati = {"id": pid, "comando": comando}
        dati.update(extra)
        self.hass.bus.async_fire(EVENT_COMMAND, dati)


def _store(hass: HomeAssistant) -> Pannelli:
    return hass.data[DOMAIN]


# --------------------------------------------------------------- comandi WS

# Solo gli amministratori: presentarsi come pannello vuol dire creare un
# dispositivo in Home Assistant e farsi dare la configurazione dello
# slideshow. Il pannello si abbina con un account amministratore (l'abbinamento
# lo pretende), quindi il permesso ce l'ha gia'.
@websocket_api.require_admin
@websocket_api.websocket_command(
    {
        vol.Required("type"): "crowpanel/announce",
        vol.Required("panel"): {
            vol.Required("id"): cv.string,
            vol.Optional("nome"): cv.string,
            vol.Optional("stanza"): cv.string,
            vol.Optional("modello"): cv.string,
            vol.Optional("versione"): cv.string,
            vol.Optional("indirizzo"): cv.string,
            vol.Optional("max_valori"): int,
        },
    }
)
@callback
def ws_announce(hass: HomeAssistant, connection, msg: dict) -> None:
    """Il pannello si presenta. Gli rispondiamo dicendogli chi comanda."""
    store = _store(hass)
    info = msg["panel"]
    pid = info["id"]
    nuovo = store.aggiorna(pid, info=info)
    if nuovo:
        _LOGGER.info("pannello %s (%s) collegato", info.get("nome", pid), pid)

    gestito = store.gestito(pid)
    connection.send_result(
        msg["id"],
        {"managed": gestito, "config": store.config_slideshow(pid) if gestito else None},
    )


@websocket_api.require_admin
@websocket_api.websocket_command(
    {
        vol.Required("type"): "crowpanel/state",
        # Non "id": nel protocollo di HA quel nome e' gia' preso dal numero
        # progressivo del messaggio, e due chiavi uguali nello stesso oggetto
        # si sovrascrivono a vicenda.
        vol.Required("pannello"): cv.string,
        vol.Required("stato"): dict,
    }
)
@callback
def ws_state(hass: HomeAssistant, connection, msg: dict) -> None:
    """Come sta il pannello adesso: arriva ogni mezzo minuto e dopo ogni comando."""
    _store(hass).aggiorna(msg["pannello"], stato=msg["stato"])
    connection.send_result(msg["id"], None)


# ------------------------------------------------------------------ servizi

SERVIZIO_DASHBOARD = "mostra_dashboard"
SERVIZIO_AVVISO = "avviso"
SERVIZIO_INVIA = "invia_slideshow"

SCHEMA_DASHBOARD = vol.Schema(
    {
        vol.Required("pannello"): cv.string,
        vol.Required("percorso"): cv.string,
        vol.Optional("vista", default=0): vol.Coerce(int),
    }
)

SCHEMA_AVVISO = vol.Schema(
    {
        vol.Required("pannello"): cv.string,
        vol.Required("messaggio"): cv.string,
        vol.Optional("voce", default=True): cv.boolean,
    }
)

SCHEMA_INVIA = vol.Schema({vol.Required("pannello"): cv.string})


async def _registra_servizi(hass: HomeAssistant) -> None:
    store = _store(hass)

    async def dashboard(call: ServiceCall) -> None:
        store.comanda(
            call.data["pannello"],
            "dashboard",
            percorso=call.data["percorso"],
            vista=call.data["vista"],
        )

    async def avviso(call: ServiceCall) -> None:
        store.comanda(
            call.data["pannello"],
            "avviso",
            messaggio=call.data["messaggio"],
            voce=call.data["voce"],
        )

    async def invia(call: ServiceCall) -> None:
        pid = call.data["pannello"]
        store.comanda(pid, "slideshow", config=store.config_slideshow(pid))

    hass.services.async_register(DOMAIN, SERVIZIO_DASHBOARD, dashboard, SCHEMA_DASHBOARD)
    hass.services.async_register(DOMAIN, SERVIZIO_AVVISO, avviso, SCHEMA_AVVISO)
    hass.services.async_register(DOMAIN, SERVIZIO_INVIA, invia, SCHEMA_INVIA)

    # Salvataggio e ripristino: stanno in un file loro perche' sono un pezzo a
    # se', con un comando WebSocket e una vista HTTP tutti suoi.
    from .backup import registra as registra_backup
    await registra_backup(hass, store)

    # Le foto dello slideshow: anche queste un pezzo a se', con la conversione
    # che usa Pillow (gia' dentro Home Assistant: non va dichiarata, e
    # hassfest si arrabbia se lo si fa) e una vista HTTP tutta sua.
    from .foto import registra as registra_foto
    await registra_foto(hass, store)


# -------------------------------------------------------------------- avvio

async def async_setup(hass: HomeAssistant, config: ConfigType) -> bool:
    """Registra i comandi WebSocket.

    Va fatto anche senza nessuna voce configurata: e' il pannello a bussare
    per primo, e se il comando non esiste si sente rispondere "non so cosa
    sia" e riprova piu' tardi.
    """
    if DOMAIN not in hass.data:
        hass.data[DOMAIN] = Pannelli(hass)
    websocket_api.async_register_command(hass, ws_announce)
    websocket_api.async_register_command(hass, ws_state)
    from .backup import ws_backup
    websocket_api.async_register_command(hass, ws_backup)
    return True


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    if DOMAIN not in hass.data:
        hass.data[DOMAIN] = Pannelli(hass)
    store = _store(hass)
    store.entry = entry
    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)
    await _registra_servizi(hass)
    entry.async_on_unload(entry.add_update_listener(_opzioni_cambiate))
    return True


async def _opzioni_cambiate(hass: HomeAssistant, entry: ConfigEntry) -> None:
    """Qualcuno ha cambiato le impostazioni: le mandiamo subito ai pannelli."""
    store = _store(hass)
    store.entry = entry
    for pid in store.noti:
        if store.gestito(pid):
            store.comanda(pid, "slideshow", config=store.config_slideshow(pid))
        else:
            # niente da mandare: il pannello se ne accorge al prossimo annuncio
            _LOGGER.debug("pannello %s: lo slideshow lo gestisce il pannello", pid)


async def async_unload_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    ok = await hass.config_entries.async_unload_platforms(entry, PLATFORMS)
    if ok:
        _store(hass).entry = None
    return ok
