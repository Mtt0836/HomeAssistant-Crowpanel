#pragma once

#include "esp_brookesia.hpp"

/* Console di servizio sulla UART di debug (la stessa del log, 115200 8N1).
   Serve a pilotare il pannello da PC senza toccarlo fisicamente: aprire l'app,
   catturare uno screenshot, leggere lo stato. Comandi disponibili: "help".   */
void uart_console_start(ESP_Brookesia_Phone *phone, int ha_app_id);

/* Esegue un comando della console (gli stessi della seriale) e ne copia
   l'uscita in out. Blocca finche' il comando termina: chiamarla da un task,
   non dal task LVGL. "shot" non e' disponibile per questa via. */
void uart_console_exec(const char *cmd, char *out, size_t out_sz);
