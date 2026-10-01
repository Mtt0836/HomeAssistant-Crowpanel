"""Salvataggio e ripristino della configurazione del pannello, da Home Assistant.

COME GIRA. Il pannello e HA si parlano in un verso solo per parte: HA manda
eventi sul bus, il pannello manda comandi WebSocket. Un file che deve tornare
indietro non ha quindi una "risposta" da usare, e il giro e' questo:

    servizio in HA  ->  evento "crowpanel_command"  ->  pannello
    pannello  ->  comando "crowpanel/backup"  ->  qui
    qui  ->  scaricabile da un indirizzo, per i soli amministratori

DOVE FINISCE IL FILE. Solo in memoria, e per pochi minuti. Non in uno stato di
entita' - gli stati li vedono tutti gli utenti di Home Assistant e finiscono
nel database del recorder - e non su disco. Chi lo scarica se lo porta via, e
qui non resta niente.

COSA C'E' DENTRO. Il salvataggio cifrato contiene il permesso di entrare in
Home Assistant e la chiave privata del certificato del pannello; quello in
chiaro no, per scelta (il pannello li toglie). Per questo tutto quello che sta
qui dentro e' riservato agli amministratori, e per questo il file cifrato puo'
passare tranquillamente dal bus degli eventi mentre lo si rimette: cifrato e'
illeggibile, in chiaro non ha segreti. Non c'e' mai un momento in cui un
segreto viaggia in chiaro sul bus.
"""

from __future__ import annotations

import logging
import secrets
import time
from typing import Any

import voluptuous as vol

from homeassistant.components import websocket_api
from homeassistant.components.http import HomeAssistantView
from homeassistant.core import HomeAssistant, ServiceCall, SupportsResponse, callback
from homeassistant.exceptions import HomeAssistantError, Unauthorized
from homeassistant.helpers import config_validation as cv

from .const import DOMAIN

_LOGGER = logging.getLogger(__name__)

# Quanto resta a disposizione un file preparato dal pannello. Poco: e' il tempo
# di cliccare il collegamento, non un archivio.
VALIDITA_S = 600

SERVIZIO_SALVA = "salva_configurazione"
SERVIZIO_CHIAVE = "scarica_chiave"
SERVIZIO_RIMETTI = "rimetti_configurazione"

URL_BASE = "/api/crowpanel/scarica"


class Pronti:
    """I file che i pannelli hanno mandato e che aspettano di essere scaricati."""

    def __init__(self) -> None:
        self.voci: dict[str, dict[str, Any]] = {}

    def metti(self, pid: str, tipo: str, dati: str) -> str:
        self.pulisci()
        gettone = secrets.token_urlsafe(24)
        self.voci[gettone] = {
            "pannello": pid,
            "tipo": tipo,
            "dati": dati,
            "scade": time.monotonic() + VALIDITA_S,
        }
        return gettone

    def prendi(self, gettone: str) -> dict[str, Any] | None:
        self.pulisci()
        # Si consuma: un collegamento per un download, e poi non vale piu'.
        return self.voci.pop(gettone, None)

    def pulisci(self) -> None:
        ora = time.monotonic()
        for g in [g for g, v in self.voci.items() if v["scade"] < ora]:
            self.voci.pop(g, None)


def _pronti(hass: HomeAssistant) -> Pronti:
    return hass.data.setdefault(f"{DOMAIN}_backup", Pronti())


# --------------------------------------------------------------- comando WS

@websocket_api.require_admin
@websocket_api.websocket_command(
    {
        vol.Required("type"): "crowpanel/backup",
        vol.Required("pannello"): cv.string,
        vol.Optional("richiesta", default=""): cv.string,
        vol.Required("tipo"): vol.In(["cifrato", "chiaro", "chiave"]),
        vol.Required("dati"): cv.string,
    }
)
@callback
def ws_backup(hass: HomeAssistant, connection, msg: dict) -> None:
    """Il pannello manda quello che gli abbiamo chiesto."""
    pronti = _pronti(hass)
    gettone = pronti.metti(msg["pannello"], msg["tipo"], msg["dati"])
    _LOGGER.info(
        "pannello %s: arrivato il %s (%d byte), scaricabile per %d minuti",
        msg["pannello"], msg["tipo"], len(msg["dati"]), VALIDITA_S // 60,
    )
    connection.send_result(msg["id"], {"gettone": gettone})
    # Chi ha chiesto il file sta aspettando questo.
    hass.bus.async_fire(
        f"{DOMAIN}_backup_pronto",
        {"pannello": msg["pannello"], "richiesta": msg["richiesta"],
         "tipo": msg["tipo"], "gettone": gettone},
    )


# ------------------------------------------------------------------- vista

class VistaScarica(HomeAssistantView):
    """Consegna il file al browser. Solo amministratori."""

    url = URL_BASE + "/{gettone}"
    name = "api:crowpanel:scarica"
    requires_auth = True

    async def get(self, request, gettone: str):
        hass = request.app["hass"]
        if not request["hass_user"].is_admin:
            raise Unauthorized()

        voce = _pronti(hass).prendi(gettone)
        if not voce:
            return self.json_message(
                "Il collegamento non vale piu': rifai il salvataggio.", 404
            )

        if voce["tipo"] == "chiave":
            nome = f"chiave-{voce['pannello']}.txt"
            tipo = "text/plain; charset=utf-8"
        else:
            nome = f"{voce['pannello']}-{voce['tipo']}.json"
            tipo = "application/json"

        from aiohttp import web

        return web.Response(
            body=voce["dati"].encode("utf-8"),
            content_type=tipo.split(";")[0],
            charset="utf-8",
            headers={"Content-Disposition": f'attachment; filename="{nome}"'},
        )


# ----------------------------------------------------------------- servizi

SCHEMA_SALVA = vol.Schema(
    {
        vol.Required("pannello"): cv.string,
        vol.Optional("cifrato", default=True): cv.boolean,
    }
)
SCHEMA_CHIAVE = vol.Schema({vol.Required("pannello"): cv.string})
SCHEMA_RIMETTI = vol.Schema(
    {
        vol.Required("pannello"): cv.string,
        vol.Required("file"): cv.string,
        vol.Optional("chiave", default=""): cv.string,
    }
)


async def _attendi(hass: HomeAssistant, pid: str, richiesta: str) -> str:
    """Aspetta che il pannello risponda, e restituisce l'indirizzo da aprire."""
    import asyncio

    futuro: asyncio.Future = hass.loop.create_future()

    @callback
    def arrivato(event) -> None:
        d = event.data
        if d.get("pannello") == pid and d.get("richiesta") == richiesta:
            if not futuro.done():
                futuro.set_result(d["gettone"])

    stacca = hass.bus.async_listen(f"{DOMAIN}_backup_pronto", arrivato)
    try:
        gettone = await asyncio.wait_for(futuro, timeout=30)
    except asyncio.TimeoutError:
        raise HomeAssistantError(
            "Il pannello non ha risposto. E' acceso e collegato a Home Assistant?"
        ) from None
    finally:
        stacca()
    return f"{URL_BASE}/{gettone}"


async def registra(hass: HomeAssistant, store) -> None:
    """Registra comando, vista e servizi. Si puo' chiamare piu' volte."""
    websocket_api.async_register_command(hass, ws_backup)
    hass.http.register_view(VistaScarica())

    async def salva(call: ServiceCall) -> dict[str, Any]:
        pid = call.data["pannello"]
        richiesta = secrets.token_hex(8)
        store.comanda(pid, "esporta", cifrato=call.data["cifrato"], richiesta=richiesta)
        url = await _attendi(hass, pid, richiesta)
        return {
            "indirizzo": url,
            "scade_fra_minuti": VALIDITA_S // 60,
            "nota": (
                "Apri l'indirizzo per scaricare il file. Vale una volta sola."
                if call.data["cifrato"]
                else "Salvataggio in chiaro: NON contiene il permesso di Home "
                     "Assistant ne' la chiave del certificato. Per un ripristino "
                     "completo serve quello cifrato."
            ),
        }

    async def chiave(call: ServiceCall) -> dict[str, Any]:
        pid = call.data["pannello"]
        richiesta = secrets.token_hex(8)
        store.comanda(pid, "chiave", richiesta=richiesta)
        url = await _attendi(hass, pid, richiesta)
        return {
            "indirizzo": url,
            "scade_fra_minuti": VALIDITA_S // 60,
            "nota": (
                "Mettila da parte fuori da Home Assistant e fuori dal pannello. "
                "Chi ha questa chiave e un salvataggio cifrato entra anche nel "
                "tuo Home Assistant."
            ),
        }

    async def rimetti(call: ServiceCall) -> None:
        store.comanda(
            call.data["pannello"],
            "importa",
            file=call.data["file"],
            chiave=call.data["chiave"],
        )
        _LOGGER.warning(
            "pannello %s: mandata una configurazione da rimettere; il pannello si "
            "riavviera' da solo se il file e' valido",
            call.data["pannello"],
        )

    hass.services.async_register(
        DOMAIN, SERVIZIO_SALVA, salva, SCHEMA_SALVA,
        supports_response=SupportsResponse.ONLY,
    )
    hass.services.async_register(
        DOMAIN, SERVIZIO_CHIAVE, chiave, SCHEMA_CHIAVE,
        supports_response=SupportsResponse.ONLY,
    )
    hass.services.async_register(DOMAIN, SERVIZIO_RIMETTI, rimetti, SCHEMA_RIMETTI)
