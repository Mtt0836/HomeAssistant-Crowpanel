#pragma once
#include "lvgl.h"

/* Sezioni aggiunte all'app Impostazioni di Elecrow: Rete, Home Assistant,
   Debug e Sicurezza. Crea le voci nel menu principale e le schermate (da
   chiamare in extraUiInit, a ogni apertura dell'app, come le schermate
   SquareLine). Le schermate restituite vanno registrate da AppSettings per il
   tasto indietro. Le voci protette chiedono il PIN (pin_lock.h). */

enum {
    SETTINGS_EXTRA_NET = 0,
    SETTINGS_EXTRA_HA,
    SETTINGS_EXTRA_DEBUG,
    SETTINGS_EXTRA_SEC,
    SETTINGS_EXTRA_COUNT
};

void settings_extra_build(lv_obj_t *main_container, lv_obj_t **screens);

/* Mostra una delle schermate sopra (comando UART "setscr", per le prove).
   Chiamare col lock del display. */
bool settings_extra_show(int which);
