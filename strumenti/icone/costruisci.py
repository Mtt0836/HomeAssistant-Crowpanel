# Costruisce il font delle icone di Home Assistant per il pannello.
#
# Material Design Icons e' un font con piu' di settemila glifi: qui se ne
# prende solo il sottoinsieme elencato in elenco.txt, in due misure, e si
# genera anche la tabella nome -> codice che il firmware usa per risolvere a
# runtime le icone che arrivano da HA ("mdi:weather-sunny").
#
# Lo lancia strumenti/costruisci_icone.ps1, che sistema prima il percorso di
# Node. Non serve rilanciarlo per compilare: i file generati stanno nel
# repository. Serve solo quando si aggiunge un'icona all'elenco.

import io
import os
import re
import subprocess
import sys

QUI = os.path.dirname(os.path.abspath(__file__))
CSS = os.path.join(QUI, 'node_modules', '@mdi', 'font', 'css', 'materialdesignicons.css')
TTF = os.path.join(QUI, 'node_modules', '@mdi', 'font', 'fonts', 'materialdesignicons-webfont.ttf')
ELENCO = os.path.join(QUI, 'elenco.txt')
DEST = os.path.abspath(os.path.join(QUI, '..', '..', 'V0.2', 'components', 'apps',
                                    'home_dashboard', 'assets'))
MISURE = [20, 32]          # piccola per le tile, grande per meteo e pulsanti
BPP = 4                    # sfumature: senza, i bordi curvi sono scalettati


def codici_da_css():
    """nome dell'icona -> codice unicode, letti dal CSS di @mdi/font."""
    testo = io.open(CSS, encoding='utf-8').read()
    coppie = re.findall(r'\.mdi-([a-z0-9-]+)::before\s*\{\s*content:\s*"\\([0-9A-Fa-f]+)"', testo)
    return {nome: int(cod, 16) for nome, cod in coppie}


def elenco_voluto():
    voluti = []
    for riga in io.open(ELENCO, encoding='utf-8'):
        riga = riga.split('#')[0].strip()
        if riga:
            voluti.append(riga)
    return voluti


def main():
    if not os.path.isfile(CSS):
        print("Manca @mdi/font. Lancia prima:\n  npm install @mdi/font --no-save")
        return 1

    codici = codici_da_css()
    voluti = elenco_voluto()

    scelti = []
    mancanti = []
    visti = set()
    for nome in voluti:
        if nome in visti:
            continue
        visti.add(nome)
        if nome in codici:
            scelti.append((nome, codici[nome]))
        else:
            mancanti.append(nome)

    if mancanti:
        print("Questi nomi non esistono in Material Design Icons e li salto:")
        for n in mancanti:
            print("  -", n)

    print("%d icone scelte su %d disponibili" % (len(scelti), len(codici)))

    # lv_font_conv vuole i codici come intervalli separati da virgola
    ranges = ','.join('0x%X' % c for _, c in scelti)

    os.makedirs(DEST, exist_ok=True)
    for misura in MISURE:
        uscita = os.path.join(DEST, 'lv_font_mdi_%d.c' % misura)
        cmd = ['lv_font_conv', '--font', TTF, '-r', ranges,
               '--size', str(misura), '--bpp', str(BPP), '--no-compress',
               '--format', 'lvgl', '--lv-include', 'lvgl.h',
               '--lv-font-name', 'lv_font_mdi_%d' % misura, '-o', uscita]
        print("genero", os.path.basename(uscita), "...")
        r = subprocess.run(cmd, shell=True)
        if r.returncode != 0:
            print("lv_font_conv ha fallito")
            return 1
        print("   %d KB" % (os.path.getsize(uscita) // 1024))

    # tabella nome -> codice per il firmware, in ordine alfabetico cosi' la
    # ricerca puo' essere binaria
    scelti.sort(key=lambda x: x[0])
    tab = os.path.join(DEST, 'mdi_nomi.h')
    with io.open(tab, 'w', encoding='utf-8', newline='\n') as f:
        f.write('// Generato da strumenti/icone/costruisci.py: non modificare a mano.\n')
        f.write('// Nomi delle icone di Home Assistant e loro codice nel font.\n')
        f.write('// In ordine alfabetico: mdi_icon.cpp ci fa la ricerca binaria.\n')
        f.write('#pragma once\n\n')
        f.write('typedef struct { const char *nome; uint32_t codice; } mdi_voce_t;\n\n')
        f.write('static const mdi_voce_t MDI_NOMI[] = {\n')
        for nome, cod in scelti:
            f.write('    { "%s", 0x%X },\n' % (nome, cod))
        f.write('};\n')
    print("scritta la tabella dei nomi:", len(scelti), "voci")
    return 0


if __name__ == '__main__':
    sys.exit(main())
