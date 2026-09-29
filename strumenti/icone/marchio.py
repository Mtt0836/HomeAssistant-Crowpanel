# -*- coding: utf-8 -*-
"""Disegna il marchio dell'integrazione per Home Assistant.

HACS e il registro home-assistant/brands vogliono un'icona per mostrare
l'integrazione nei loro elenchi. Il disegno e' fatto qui invece che in un
programma di grafica per due motivi: si rifa' uguale cambiando due numeri, e
chi guarda il repository vede da dove viene invece di trovarsi un PNG e basta.

Il soggetto e' la cosa stessa: un pannello appeso al muro con sopra le card di
una dashboard. Niente casette e niente richiami al marchio di Home Assistant,
che e' loro.

Uso:  python marchio.py
Scrive in custom_components/crowpanel/brand/:
    icon.png      256x256      logo.png      512x256
    icon@2x.png   512x512      logo@2x.png  1024x512
"""

import os
from PIL import Image, ImageDraw

# Gli stessi colori dell'interfaccia del pannello, cosi' l'icona e' lo schermo.
BLU_SU   = (22, 144, 234)
BLU_GIU  = (10, 95, 168)
SCHERMO  = (240, 242, 245)      # il bianco caldo della cornice
CARD     = (27, 32, 48)         # il grigio-blu delle card
ACCENTO  = (11, 125, 218)

SUPER = 4                        # si disegna in grande e si rimpicciolia: bordi lisci


def sfondo_sfumato(w, h):
    """Sfumatura verticale, un pixel per riga."""
    g = Image.new("RGB", (1, h))
    for y in range(h):
        t = y / max(1, h - 1)
        g.putpixel((0, y), tuple(int(a + (b - a) * t) for a, b in zip(BLU_SU, BLU_GIU)))
    return g.resize((w, h), Image.BILINEAR)


def marchio(lato):
    """Il quadrato dell'icona, disegnato a lato*SUPER e poi ridotto."""
    S = lato * SUPER
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))

    # Sfondo: quadrato con gli angoli tondi, come le icone di sistema.
    maschera = Image.new("L", (S, S), 0)
    ImageDraw.Draw(maschera).rounded_rectangle([0, 0, S - 1, S - 1],
                                               radius=int(S * 0.225), fill=255)
    img.paste(sfondo_sfumato(S, S), (0, 0), maschera)

    d = ImageDraw.Draw(img)

    # Il pannello: 16:10, le proporzioni vere del CrowPanel da 10.1 pollici.
    pw = int(S * 0.64)
    ph = int(pw * 10 / 16)
    px = (S - pw) // 2
    py = (S - ph) // 2
    r  = int(S * 0.035)
    d.rounded_rectangle([px, py, px + pw, py + ph], radius=r, fill=SCHERMO)

    # Le card dentro lo schermo. Una grande a sinistra e due impilate a destra:
    # e' la disposizione che si vede davvero sul pannello, e a icona piccola
    # resta leggibile perche' i blocchi sono pochi e grossi.
    m  = int(pw * 0.085)                       # margine interno
    g  = int(pw * 0.055)                       # spazio fra le card
    cx = px + m
    cy = py + m
    cw = pw - 2 * m
    ch = ph - 2 * m
    rc = int(S * 0.018)

    larga = int(cw * 0.52)
    d.rounded_rectangle([cx, cy, cx + larga, cy + ch], radius=rc, fill=CARD)

    destra_x = cx + larga + g
    destra_w = cw - larga - g
    alta = int((ch - g) * 0.58)
    d.rounded_rectangle([destra_x, cy, destra_x + destra_w, cy + alta],
                        radius=rc, fill=ACCENTO)
    d.rounded_rectangle([destra_x, cy + alta + g, destra_x + destra_w, cy + ch],
                        radius=rc, fill=CARD)

    return img.resize((lato, lato), Image.LANCZOS)


def steso(largh, alt):
    """Il marchio centrato su una tela larga, sfondo trasparente."""
    lato = min(largh, alt)
    tela = Image.new("RGBA", (largh, alt), (0, 0, 0, 0))
    tela.paste(marchio(lato), ((largh - lato) // 2, (alt - lato) // 2))
    return tela


if __name__ == "__main__":
    qui = os.path.dirname(os.path.abspath(__file__))
    fuori = os.path.join(qui, "..", "..", "custom_components", "crowpanel", "brand")
    fuori = os.path.normpath(fuori)
    os.makedirs(fuori, exist_ok=True)

    da_fare = [
        ("icon.png",     marchio(256)),
        ("icon@2x.png",  marchio(512)),
        ("logo.png",     steso(512, 256)),
        ("logo@2x.png",  steso(1024, 512)),
    ]
    for nome, im in da_fare:
        p = os.path.join(fuori, nome)
        im.save(p, "PNG", optimize=True)
        print("%-14s %sx%s  %d byte" % (nome, im.size[0], im.size[1], os.path.getsize(p)))
    print("\nscritte in", fuori)
