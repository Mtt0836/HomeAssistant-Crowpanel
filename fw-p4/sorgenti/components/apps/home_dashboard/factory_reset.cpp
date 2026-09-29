#include "factory_reset.h"
#include "ha_token.h"

#include <stdio.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_spiffs.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "factory";

/* Cancellare la NVS a blocchi (una chiave alla volta, un namespace alla volta)
   vuol dire tenere un elenco aggiornato di tutto quello che il pannello
   salva: prima o poi qualcuno aggiunge una preferenza e si dimentica di
   aggiungerla qui, e il "reset totale" non lo e' piu'. Qui si cancella la
   partizione intera: l'elenco non serve e non puo' invecchiare. */
void factory_reset_now(void)
{
    ESP_LOGW(TAG, "ritorno alle impostazioni iniziali: cancello tutto");

    /* Finche' la rete c'e' ancora: restituisco a HA il permesso del pannello. */
    ha_token_revoke_refresh();

    /* Il Wi-Fi lo tiene il driver in una sua area della NVS. Glielo faccio
       dimenticare da lui, cosi' non lo riscrive mentre cancello. */
    esp_err_t err = esp_wifi_restore();
    if (err != ESP_OK) ESP_LOGW(TAG, "Wi-Fi non azzerato dal driver: %s", esp_err_to_name(err));

    /* Ritocchi della dashboard e log: la partizione dati e' tutta roba
       nostra, creata mentre il pannello gira (l'immagine di partenza e'
       vuota), quindi si puo' formattare. */
    err = esp_spiffs_format(CONFIG_BSP_SPIFFS_PARTITION_LABEL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "formattazione dati non riuscita (%s): cancello i file",
                 esp_err_to_name(err));
        unlink("/spiffs/overrides.json");
        unlink("/spiffs/ramlog.csv");
    }

    err = nvs_flash_erase();
    ESP_LOGW(TAG, "memoria impostazioni cancellata: %s", esp_err_to_name(err));

    /* Nessun nvs_flash_init(): da qui in poi non si salva piu' niente, si
       riavvia subito. La pausa e' corta apposta: con la memoria gia'
       cancellata ogni altro task che va a leggersi le sue impostazioni trova
       il vuoto, e non ha senso lasciarlo girare. Il messaggio a schermo e'
       comparso prima, all'inizio di tutto il giro. */
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGW(TAG, "riavvio");
    esp_restart();
}

static void reset_task(void *arg)
{
    (void)arg;
    factory_reset_now();
    vTaskDelete(NULL);            // non ci arriva mai
}

void factory_reset_start(void)
{
    /* Task a parte: la revoca parla con HA e la formattazione tocca la flash.
       Dal task di LVGL bloccherebbe lo schermo (e il lock del display). */
    xTaskCreate(reset_task, "factory", 6144, NULL, 4, NULL);
}
