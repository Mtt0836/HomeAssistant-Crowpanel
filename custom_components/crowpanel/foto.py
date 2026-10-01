"""Le foto dello slideshow: mandarle al pannello e toglierle.

PERCHE' LE CONVERTE HOME ASSISTANT. Il pannello ha un acceleratore JPEG dentro
il processore, ma quell'acceleratore non sa rimpicciolire mentre decodifica:
deve tenere in memoria la foto a grandezza naturale. Una foto di telefono da
12 megapixel vuole piu' di venti megabyte, che sul pannello non ci sono, e
viene saltata a ogni giro dello slideshow senza che l'utente capisca perche'.

Chi ha memoria e processore per ridimensionare e' Home Assistant. Quindi la
foto si converte qui - una volta sola, quando la si manda - e sul pannello
arriva gia' della misura giusta. E' lo stesso lavoro che prima andava fatto a
mano sul computer prima di copiare le foto sulla scheda.

COME ARRIVA. Non dentro l'evento: una foto da duecento kilobyte scritta in
base64 sul bus degli eventi sarebbe proprio la raffica che il collegamento fra
il pannello e il suo coprocessore di rete sopporta peggio. Home Assistant la
tiene da parte per qualche minuto a un indirizzo, dice al pannello di andarsela
a prendere, e il pannello la scarica a pezzi come fa con tutto il resto.
"""

from __future__ import annotations

import io
import logging
import mimetypes
import os
import secrets
import time
from typing import Any

import voluptuous as vol

from homeassistant.components.http import HomeAssistantView
from homeassistant.core import HomeAssistant, ServiceCall, SupportsResponse, callback
from homeassistant.exceptions import HomeAssistantError, Unauthorized
from homeassistant.helpers import config_validation as cv

from .const import DOMAIN

_LOGGER = logging.getLogger(__name__)

SERVIZIO_INVIA = "invia_foto"
SERVIZIO_ELIMINA = "elimina_foto"

URL_BASE = "/api/crowpanel/foto"

# Quanto resta a disposizione la foto convertita. Il pannello se la prende in
# pochi secondi; questo e' il margine per una rete lenta, non un archivio.
VALIDITA_S = 300

# Lo schermo del pannello. Una foto piu' grande di cosi' non si vedrebbe
# meglio: occuperebbe solo memoria e tempo.
LARGHEZZA = 1280
ALTEZZA = 800
QUALITA = 85


class Pronte:
    """Le foto convertite che aspettano di essere prese dal pannello."""

    def __init__(self) -> None:
        self.voci: dict[str, dict[str, Any]] = {}

    def metti(self, dati: bytes, nome: str) -> str:
        self.pulisci()
        gettone = secrets.token_urlsafe(24)
        self.voci[gettone] = {"dati": dati, "nome": nome,
                              "scade": time.monotonic() + VALIDITA_S}
        return gettone

    def prendi(self, gettone: str) -> dict[str, Any] | None:
        self.pulisci()
        return self.voci.get(gettone)

    def pulisci(self) -> None:
        ora = time.monotonic()
        for g in [g for g, v in self.voci.items() if v["scade"] < ora]:
            self.voci.pop(g, None)


def _pronte(hass: HomeAssistant) -> Pronte:
    return hass.data.setdefault(f"{DOMAIN}_foto", Pronte())


def _converti(grezza: bytes) -> tuple[bytes, str]:
    """Rimpicciolisce e salva in JPEG. Gira fuori dal filo principale."""
    from PIL import Image, ImageOps

    im = Image.open(io.BytesIO(grezza))
    # Le foto di telefono portano l'orientamento negli attributi EXIF invece
    # che nei pixel: senza questa riga arriverebbero coricate.
    im = ImageOps.exif_transpose(im)
    if im.mode not in ("RGB", "L"):
        # PNG con trasparenza: il JPEG non ce l'ha, e senza fondo diventerebbe
        # nera. Si appoggia su bianco, che e' quello che ci si aspetta.
        fondo = Image.new("RGB", im.size, (255, 255, 255))
        if im.mode in ("RGBA", "LA", "PA"):
            fondo.paste(im.convert("RGBA"), mask=im.convert("RGBA").split()[-1])
        else:
            fondo.paste(im.convert("RGB"))
        im = fondo
    im.thumbnail((LARGHEZZA, ALTEZZA), Image.LANCZOS)

    out = io.BytesIO()
    # "progressive" no: l'acceleratore del pannello legge solo JPEG normali.
    im.save(out, "JPEG", quality=QUALITA, optimize=True, progressive=False)
    return out.getvalue(), f"{im.width}x{im.height}"


class VistaFoto(HomeAssistantView):
    """Consegna la foto convertita. La prende il pannello, che e' autenticato."""

    url = URL_BASE + "/{gettone}"
    name = "api:crowpanel:foto"
    requires_auth = True

    async def get(self, request, gettone: str):
        hass = request.app["hass"]
        voce = _pronte(hass).prendi(gettone)
        if not voce:
            return self.json_message("Non piu' disponibile.", 404)
        from aiohttp import web

        return web.Response(body=voce["dati"], content_type="image/jpeg")


SCHEMA_INVIA = vol.Schema(
    {
        vol.Required("pannello"): cv.string,
        vol.Required("file"): cv.string,
        vol.Optional("nome"): cv.string,
    }
)
SCHEMA_ELIMINA = vol.Schema(
    {
        vol.Required("pannello"): cv.string,
        vol.Exclusive("nome", "cosa"): cv.string,
        vol.Exclusive("tutte", "cosa"): cv.boolean,
    }
)


async def registra(hass: HomeAssistant, store) -> None:
    hass.http.register_view(VistaFoto())

    async def invia(call: ServiceCall) -> dict[str, Any]:
        percorso = call.data["file"]
        # Si legge solo da dove Home Assistant permette di leggere: un percorso
        # qualunque farebbe di questo servizio un modo per tirare fuori file
        # dal server.
        if not hass.config.is_allowed_path(percorso):
            raise HomeAssistantError(
                f"{percorso} non e' fra i percorsi permessi. Aggiungi la sua "
                "cartella ad allowlist_external_dirs nella configurazione."
            )
        if not os.path.isfile(percorso):
            raise HomeAssistantError(f"{percorso} non esiste.")

        grezza = await hass.async_add_executor_job(
            lambda: open(percorso, "rb").read()
        )
        try:
            dati, misura = await hass.async_add_executor_job(_converti, grezza)
        except Exception as e:  # noqa: BLE001
            raise HomeAssistantError(f"Non riesco a convertire {percorso}: {e}") from e

        base = call.data.get("nome") or os.path.basename(percorso)
        nome = os.path.splitext(base)[0][:40] + ".jpg"

        gettone = _pronte(hass).metti(dati, nome)
        url = f"{URL_BASE}/{gettone}"
        store.comanda(call.data["pannello"], "foto", url=url, nome=nome)
        _LOGGER.info(
            "pannello %s: mandata %s -> %s, %d KB (era %d KB)",
            call.data["pannello"], nome, misura, len(dati) // 1024, len(grezza) // 1024,
        )
        return {
            "nome": nome,
            "misura": misura,
            "kb": len(dati) // 1024,
            "kb_prima": len(grezza) // 1024,
        }

    async def elimina(call: ServiceCall) -> None:
        if call.data.get("tutte"):
            store.comanda(call.data["pannello"], "elimina_foto", tutte=True)
            _LOGGER.warning("pannello %s: cancellate TUTTE le foto", call.data["pannello"])
        elif call.data.get("nome"):
            store.comanda(call.data["pannello"], "elimina_foto", nome=call.data["nome"])
        else:
            raise HomeAssistantError("Indica il nome della foto, oppure tutte: true.")

    hass.services.async_register(
        DOMAIN, SERVIZIO_INVIA, invia, SCHEMA_INVIA,
        supports_response=SupportsResponse.OPTIONAL,
    )
    hass.services.async_register(DOMAIN, SERVIZIO_ELIMINA, elimina, SCHEMA_ELIMINA)
