"""L'archivio dei firmware del pannello.

COSA FA. Tiene dentro Home Assistant le versioni del firmware, le descrive
leggendole, le manda al pannello e permette di tornare a una precedente. Le
cancella solo quando glielo si dice.

PERCHE' UN ARCHIVIO E NON UN SOLO FILE. Perche' la domanda vera, dopo un
aggiornamento, non e' "come aggiorno" ma "come torno indietro". Un archivio con
dentro le versioni di prima rende il ritorno una scelta invece che un recupero
d'emergenza. Il pannello ha gia' il suo ritorno automatico - un firmware che non
regge due minuti collegato viene rimesso dal bootloader - ma quello copre il
firmware che non parte, non quello che parte e si comporta peggio di prima.

COME ARRIVA UN FIRMWARE. Dal browser, mentre si guarda Home Assistant: si apre
"Configura" sull'integrazione e si sceglie il file. Lo carica il meccanismo di
Home Assistant (file_upload), quindi il .bin non passa dentro un evento ne' da
una cartella da configurare a mano.

COSA SI SA DI UN FIRMWARE SENZA CHIEDERLO. Un'immagine ESP-IDF porta dentro di
se' un descrittore con nome del progetto, versione, data e ora di compilazione e
versione di ESP-IDF. Si legge dal file, quindi l'elenco non dipende da come
qualcuno ha chiamato il file: due "firmware.bin" caricati a un mese di distanza
si distinguono da soli.

L'IMPRONTA. Di ogni file si calcola lo SHA-256 al momento del caricamento e la
si tiene nell'indice. Quando il firmware parte verso il pannello, l'impronta
parte con lui e il pannello la ricalcola sui byte che arrivano: se non
coincidono butta l'immagine senza nemmeno dirlo al bootloader. Copre tutto il
tragitto, compreso il disco di Home Assistant.
"""

from __future__ import annotations

import hashlib
import json
import logging
import os
import re
import secrets
import struct
import time
from datetime import datetime, timezone
from typing import Any

import voluptuous as vol

from homeassistant.components.http import HomeAssistantView
from homeassistant.core import HomeAssistant, ServiceCall, SupportsResponse
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers import config_validation as cv

from .const import DOMAIN

_LOGGER = logging.getLogger(__name__)

SERVIZIO_INVIA = "invia_firmware"
SERVIZIO_ELIMINA = "elimina_firmware"

URL_BASE = "/api/crowpanel/firmware"
CARTELLA = "crowpanel_firmware"
INDICE = "indice.json"

# Quanto resta valido l'indirizzo con cui il pannello viene a prendersi
# l'immagine. Generoso: 4,5 MB su una rete di casa ci mettono una ventina di
# secondi, ma una rete lenta o un pannello occupato possono tardare.
VALIDITA_S = 15 * 60

# Un'immagine applicativa per ESP32 comincia per 0xE9 e porta il descrittore
# subito dopo le intestazioni: 24 byte di esp_image_header_t piu' 8 di
# esp_image_segment_header_t.
OFFSET_DESC = 0x20
MAGIC_DESC = 0xABCD5432

# Tetto di sicurezza: la partizione del pannello e' da 7 MB.
MAX_BYTE = 7 * 1024 * 1024


# --------------------------------------------------------------- descrittore


def _testo(b: bytes) -> str:
    """Una stringa a lunghezza fissa dentro il descrittore, fino al primo zero."""
    return b.split(b"\x00", 1)[0].decode("utf-8", "replace").strip()


def leggi_descrittore(dati: bytes) -> dict[str, Any]:
    """Nome, versione e data di compilazione, letti dentro l'immagine.

    Ritorna un dizionario vuoto se il file non sembra un'applicazione ESP-IDF:
    meglio accorgersene qui, al caricamento, che sul pannello dopo aver
    scaricato quattro megabyte e mezzo.
    """
    if len(dati) < OFFSET_DESC + 160 or dati[0] != 0xE9:
        return {}
    d = dati[OFFSET_DESC:OFFSET_DESC + 160]
    (magic,) = struct.unpack_from("<I", d, 0)
    if magic != MAGIC_DESC:
        return {}
    return {
        "versione": _testo(d[16:48]),
        "progetto": _testo(d[48:80]),
        "ora": _testo(d[80:96]),
        "data": _testo(d[96:112]),
        "idf": _testo(d[112:144]),
    }


def nome_pulito(nome: str) -> str:
    """Un nome di file che non possa uscire dalla cartella dell'archivio."""
    base = os.path.basename(nome or "")
    base = re.sub(r"[^A-Za-z0-9._-]", "_", base)
    base = base.lstrip(".")
    if not base.endswith(".bin"):
        base += ".bin"
    return base[:64] or "firmware.bin"


# --------------------------------------------------------------- l'archivio


class Archivio:
    """I firmware su disco piu' l'indice che li descrive."""

    def __init__(self, hass: HomeAssistant) -> None:
        self.dir = hass.config.path(CARTELLA)
        self.indice = os.path.join(self.dir, INDICE)

    # -- su disco (girano fuori dal filo principale)

    def _leggi_indice(self) -> list[dict[str, Any]]:
        try:
            with open(self.indice, encoding="utf-8") as f:
                voci = json.load(f)
            return voci if isinstance(voci, list) else []
        except FileNotFoundError:
            return []
        except (OSError, ValueError):
            _LOGGER.warning("indice dell'archivio illeggibile: riparto da vuoto")
            return []

    def _scrivi_indice(self, voci: list[dict[str, Any]]) -> None:
        os.makedirs(self.dir, exist_ok=True)
        # Prima su un nome provvisorio e poi al suo posto: se Home Assistant
        # muore a meta' scrittura, l'indice vecchio resta intero invece di
        # diventare mezzo file che non si apre piu'.
        tmp = self.indice + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(voci, f, ensure_ascii=False, indent=1)
        os.replace(tmp, self.indice)

    def _aggiungi(self, dati: bytes, nome: str, note: str) -> dict[str, Any]:
        os.makedirs(self.dir, exist_ok=True)
        voci = self._leggi_indice()
        impronta = hashlib.sha256(dati).hexdigest()

        # Gia' in archivio: non si duplica, si aggiorna la data. Ricaricare per
        # sbaglio due volte lo stesso file non deve riempire il disco.
        for v in voci:
            if v.get("sha256") == impronta and os.path.isfile(os.path.join(self.dir, v["nome"])):
                v["caricato"] = datetime.now(timezone.utc).isoformat(timespec="seconds")
                if note:
                    v["note"] = note
                self._scrivi_indice(voci)
                v = dict(v)
                v["gia_presente"] = True
                return v

        base = nome_pulito(nome)
        presi = {v["nome"] for v in voci}
        finale, n = base, 1
        while finale in presi or os.path.exists(os.path.join(self.dir, finale)):
            finale = f"{base[:-4]}-{n}.bin"
            n += 1

        with open(os.path.join(self.dir, finale), "wb") as f:
            f.write(dati)

        voce: dict[str, Any] = {
            "nome": finale,
            "byte": len(dati),
            "sha256": impronta,
            "caricato": datetime.now(timezone.utc).isoformat(timespec="seconds"),
            "note": note,
        }
        voce.update(leggi_descrittore(dati))
        voci.insert(0, voce)          # il piu' recente in cima
        self._scrivi_indice(voci)
        return voce

    def _elimina(self, nome: str) -> bool:
        voci = self._leggi_indice()
        restano = [v for v in voci if v["nome"] != nome]
        if len(restano) == len(voci):
            return False
        try:
            os.remove(os.path.join(self.dir, nome))
        except FileNotFoundError:
            pass
        self._scrivi_indice(restano)
        return True

    def _dati(self, nome: str) -> tuple[bytes, dict[str, Any]]:
        voci = self._leggi_indice()
        voce = next((v for v in voci if v["nome"] == nome), None)
        if not voce:
            raise HomeAssistantError(f"{nome} non e' in archivio.")
        percorso = os.path.join(self.dir, nome)
        if not os.path.isfile(percorso):
            raise HomeAssistantError(f"{nome} e' nell'indice ma il file non c'e' piu'.")
        with open(percorso, "rb") as f:
            dati = f.read()
        # L'impronta si ricontrolla qui, non si crede all'indice: fra il
        # caricamento e adesso il file e' stato su un disco, e un archivio che
        # manda al pannello qualcosa di diverso da quello che dice di avere
        # sarebbe peggio di nessun archivio.
        vera = hashlib.sha256(dati).hexdigest()
        if vera != voce.get("sha256"):
            raise HomeAssistantError(
                f"{nome} non corrisponde piu' alla sua impronta: il file e' cambiato "
                "o si e' rovinato sul disco. Non lo mando al pannello."
            )
        return dati, voce

    # -- dal filo principale

    async def elenca(self, hass: HomeAssistant) -> list[dict[str, Any]]:
        return await hass.async_add_executor_job(self._leggi_indice)

    async def aggiungi(self, hass: HomeAssistant, dati: bytes, nome: str,
                       note: str = "") -> dict[str, Any]:
        if len(dati) > MAX_BYTE:
            raise HomeAssistantError(
                f"{len(dati) // 1024} kB: non ci stanno nella partizione del "
                f"pannello ({MAX_BYTE // 1024} kB)."
            )
        if not leggi_descrittore(dati):
            raise HomeAssistantError(
                "Questo file non sembra un'applicazione ESP-IDF: manca il "
                "descrittore. Deve essere il .bin dell'applicazione, non il "
                "bootloader ne' la tabella delle partizioni."
            )
        return await hass.async_add_executor_job(self._aggiungi, dati, nome, note)

    async def elimina(self, hass: HomeAssistant, nome: str) -> bool:
        return await hass.async_add_executor_job(self._elimina, nome)

    async def dati(self, hass: HomeAssistant, nome: str) -> tuple[bytes, dict[str, Any]]:
        return await hass.async_add_executor_job(self._dati, nome)


def archivio(hass: HomeAssistant) -> Archivio:
    return hass.data.setdefault(f"{DOMAIN}_archivio_fw", Archivio(hass))


# --------------------------------------------------------------- consegna


class Pronti:
    """I firmware che aspettano di essere presi dal pannello."""

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


def _pronti(hass: HomeAssistant) -> Pronti:
    return hass.data.setdefault(f"{DOMAIN}_fw_pronti", Pronti())


class VistaFirmware(HomeAssistantView):
    """Consegna il firmware. Lo prende il pannello, che e' autenticato."""

    url = URL_BASE + "/{gettone}"
    name = "api:crowpanel:firmware"
    requires_auth = True

    async def get(self, request, gettone: str):
        hass = request.app["hass"]
        voce = _pronti(hass).prendi(gettone)
        if not voce:
            return self.json_message("Non piu' disponibile.", 404)
        from aiohttp import web

        return web.Response(body=voce["dati"],
                            content_type="application/octet-stream")


# --------------------------------------------------------------- servizi


SCHEMA_INVIA = vol.Schema(
    {
        vol.Required("pannello"): cv.string,
        vol.Required("nome"): cv.string,
    }
)
SCHEMA_ELIMINA = vol.Schema({vol.Required("nome"): cv.string})


async def registra(hass: HomeAssistant, store) -> None:
    hass.http.register_view(VistaFirmware())

    async def invia(call: ServiceCall) -> dict[str, Any]:
        arch = archivio(hass)
        dati, voce = await arch.dati(hass, call.data["nome"])

        gettone = _pronti(hass).metti(dati, voce["nome"])
        store.comanda(
            call.data["pannello"], "firmware",
            url=f"{URL_BASE}/{gettone}",
            sha256=voce["sha256"],
            byte=voce["byte"],
        )
        _LOGGER.warning(
            "pannello %s: mandato il firmware %s (%s %s, %d kB)",
            call.data["pannello"], voce["nome"], voce.get("progetto", "?"),
            voce.get("versione", "?"), voce["byte"] // 1024,
        )
        return {
            "nome": voce["nome"],
            "versione": voce.get("versione", ""),
            "progetto": voce.get("progetto", ""),
            "kb": voce["byte"] // 1024,
            "sha256": voce["sha256"],
        }

    async def elimina(call: ServiceCall) -> None:
        if not await archivio(hass).elimina(hass, call.data["nome"]):
            raise HomeAssistantError(f"{call.data['nome']} non e' in archivio.")
        _LOGGER.warning("firmware %s tolto dall'archivio", call.data["nome"])

    hass.services.async_register(
        DOMAIN, SERVIZIO_INVIA, invia, SCHEMA_INVIA,
        supports_response=SupportsResponse.OPTIONAL,
    )
    hass.services.async_register(DOMAIN, SERVIZIO_ELIMINA, elimina, SCHEMA_ELIMINA)
