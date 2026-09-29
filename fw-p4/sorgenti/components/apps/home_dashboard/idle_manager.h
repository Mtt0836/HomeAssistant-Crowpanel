#pragma once
#include "esp_brookesia.hpp"

/* Inattivita' del pannello: attivo -> slideshow -> schermo spento.
   Un tocco in slideshow o a schermo spento riaccende e mostra la dashboard di
   HA; quel primo tocco non preme niente (lo assorbe l'overlay dello standby).
   Sostituisce il vecchio task di main.cpp che spegneva lo schermo dopo 60 s. */
void idle_manager_start(ESP_Brookesia_Phone *phone, int ha_app_id);

/* Forza subito lo slideshow (comando UART "standby", per le prove). */
void idle_manager_force_standby(void);

/* Rilegge standby.json: tempi, cartella e valori in sovrimpressione. La chiama
   la pagina web dopo aver salvato. */
extern "C" void idle_manager_reload_config(void);

/* Schermo e luminosita' comandati da fuori (Home Assistant). Si possono
   chiamare da qualunque task: lasciano una richiesta che la macchina a stati
   raccoglie entro un quinto di secondo. Luminosita' da 1 a 100; per spegnere
   c'e' set_screen(false), cosi' la luminosita' scelta non si perde. */
extern "C" {
void idle_manager_set_screen(bool on);
bool idle_manager_screen_on(void);
void idle_manager_set_brightness(int pct);
int  idle_manager_brightness(void);
}
