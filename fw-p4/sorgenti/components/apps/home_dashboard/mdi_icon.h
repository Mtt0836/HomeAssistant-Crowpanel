#pragma once
#include "lvgl.h"
#include <stddef.h>

// Le icone di Home Assistant sul pannello.
//
// In HA ogni entita' e ogni card possono avere un'icona, scritta come
// "mdi:weather-sunny". Sono glifi di un font (Material Design Icons), non
// immagini: qui ne e' compilato dentro un sottoinsieme, in due misure, e
// questo modulo traduce il nome nel carattere da scrivere in una etichetta.
//
// Il font intero ha piu' di settemila glifi e sarebbe decine di megabyte:
// l'elenco di quelli inclusi sta in strumenti/icone/elenco.txt, e aggiungerne
// uno vuol dire scriverlo li' e rilanciare strumenti/costruisci_icone.ps1.
// Se HA chiede un'icona che non c'e', il pannello ripiega su quella generica
// del tipo di entita' e lo annota nel log, cosi' si sa cosa aggiungere.

#ifdef __cplusplus
extern "C" {
#endif

// Riempie out con il carattere dell'icona (UTF-8, breve) e ritorna true.
// name puo' essere "mdi:casa" o "casa"; NULL o sconosciuto -> false.
bool mdi_icon_text(const char *name, char *out, size_t out_sz);

// Il font delle icone della misura piu' vicina a size_px.
const lv_font_t *mdi_icon_font(int size_px);

#ifdef __cplusplus
}
#endif
