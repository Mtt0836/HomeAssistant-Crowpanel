#include "hosted_recovery.h"
#include "net_config.h"

#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "nvs.h"
#include "esp_hosted.h"
#include "esp_hosted_event.h"

/* Quando una scrittura SDIO verso il C6 fallisce, ESP-Hosted dichiara il
   trasporto "irrecuperabile". Di serie reagisce riavviando tutto il P4: per un
   display a parete significa schermo nero e ~26 s di buio. Qui seguiamo invece
   la procedura dell'esempio ufficiale host_hosted_events: chiudiamo il netif,
   deinizializziamo ESP-Hosted, lo rialziamo (con reset hardware del C6 via
   GPIO32) e riconnettiamo il Wi-Fi. Interfaccia e touch restano vivi. */

static const char *TAG = "hosted_rec";

#define BIT_FAILURE BIT0
#define BIT_UP      BIT1

static EventGroupHandle_t s_ev         = nullptr;
static volatile bool      s_recovering = false;
static unsigned           s_count      = 0;

static void hosted_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == ESP_HOSTED_EVENT_TRANSPORT_FAILURE) {
        if (!s_recovering) {
            xEventGroupSetBits(s_ev, BIT_FAILURE);
        }
    } else if (id == ESP_HOSTED_EVENT_TRANSPORT_UP) {
        xEventGroupSetBits(s_ev, BIT_UP);
    }
}

/* Con il trasporto rotto non si possono chiamare esp_wifi_stop()/deinit(): sono
   RPC verso il C6 e fallirebbero. Si simulano invece gli eventi, cosi' lo stack
   IP chiude la connessione in modo ordinato. */
static void netif_close(void)
{
    wifi_event_sta_disconnected_t ev = {};
    ev.reason = WIFI_REASON_CONNECTION_FAIL;
    esp_event_post(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &ev, sizeof(ev), portMAX_DELAY);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_event_post(WIFI_EVENT, WIFI_EVENT_STA_STOP, nullptr, 0, portMAX_DELAY);
    vTaskDelay(pdMS_TO_TICKS(100));

    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta) {
        esp_netif_destroy_default_wifi(sta);
    }
}

/* Credenziali: prima quelle salvate dall'app Settings nella NVS del P4, poi
   quelle che il C6 conserva nella sua flash. */
static bool load_saved_creds(wifi_config_t *cfg)
{
    nvs_handle_t h;
    if (nvs_open("storage", NVS_READONLY, &h) != ESP_OK) return false;
    size_t sl = sizeof(cfg->sta.ssid), pl = sizeof(cfg->sta.password);
    bool ok = nvs_get_str(h, "wifi_ssid", (char *)cfg->sta.ssid, &sl) == ESP_OK && cfg->sta.ssid[0] != '\0';
    if (ok && nvs_get_str(h, "wifi_pass", (char *)cfg->sta.password, &pl) != ESP_OK) {
        cfg->sta.password[0] = '\0';
    }
    nvs_close(h);
    return ok;
}

static bool wifi_bringup(void)
{
    esp_netif_t *nif = esp_netif_create_default_wifi_sta();
    if (nif == nullptr) {
        ESP_LOGE(TAG, "creazione netif fallita");
        return false;
    }
    net_config_apply(nif);            // la netif e' nuova: IP statico e nome vanno riapplicati
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&init) != ESP_OK)            return false;
    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) return false;
    if (esp_wifi_start() != ESP_OK)                return false;

    wifi_config_t cfg = {};
    if (load_saved_creds(&cfg)) {
        esp_wifi_set_config(WIFI_IF_STA, &cfg);
    } else if (esp_wifi_get_config(WIFI_IF_STA, &cfg) != ESP_OK || cfg.sta.ssid[0] == '\0') {
        ESP_LOGW(TAG, "nessuna credenziale Wi-Fi disponibile dopo il recupero");
        return true;   // trasporto su, ma senza rete da riprendere
    }
    ESP_LOGI(TAG, "riconnessione Wi-Fi a %s", (const char *)cfg.sta.ssid);
    esp_wifi_connect();
    return true;
}

static void recovery_task(void *arg)
{
    while (true) {
        xEventGroupWaitBits(s_ev, BIT_FAILURE, pdTRUE, pdTRUE, portMAX_DELAY);
        s_recovering = true;
        s_count++;
        const int64_t t0 = esp_timer_get_time();
        ESP_LOGW(TAG, "trasporto SDIO caduto (evento #%u): recupero senza riavvio", s_count);

        /* Prima si ferma ESP-Hosted (e con lui il task di ricezione SDIO), poi
           si chiude la netif: nell'ordine inverso un pacchetto in arrivo veniva
           consegnato alla netif gia' distrutta (panic in esp_netif_receive del
           21/09, emerso con decine di recuperi di fila). */
        esp_hosted_deinit();
        netif_close();
        vTaskDelay(pdMS_TO_TICKS(500));

        xEventGroupClearBits(s_ev, BIT_UP);
        esp_hosted_init();
        esp_hosted_connect_to_slave();

        EventBits_t b = xEventGroupWaitBits(s_ev, BIT_UP, pdTRUE, pdTRUE, pdMS_TO_TICKS(20000));
        esp_hosted_coprocessor_fwver_t ver = {};
        if (!(b & BIT_UP) || esp_hosted_get_coprocessor_fwversion(&ver) != ESP_OK) {
            /* Ultima risorsa: se il trasporto non torna, il riavvio resta
               l'unico modo di non lasciare il pannello offline per sempre. */
            ESP_LOGE(TAG, "trasporto non tornato su: riavvio come ultima risorsa");
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        }
        ESP_LOGI(TAG, "C6 di nuovo raggiungibile, firmware %u.%u.%u",
                 (unsigned)ver.major1, (unsigned)ver.minor1, (unsigned)ver.patch1);

        if (!wifi_bringup()) {
            ESP_LOGE(TAG, "reinizializzazione Wi-Fi fallita: riavvio come ultima risorsa");
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        }

        ESP_LOGW(TAG, "recupero #%u completato in %lld ms, nessun riavvio",
                 s_count, (long long)((esp_timer_get_time() - t0) / 1000));
        s_recovering = false;
    }
}

/* Spegne ESP-Hosted e sospende il recupero, lasciando il C6 acceso (EN alto).
   Per il C6 e' come un P4 muto: serve a provare il suo ripristino automatico
   dell'immagine senza caricare un firmware rotto. Si esce solo riavviando. */
void hosted_recovery_suspend(void)
{
    s_recovering = true;
    ESP_LOGW(TAG, "SDIO spenta su richiesta: nessun recupero fino al prossimo riavvio");
    esp_hosted_deinit();
    netif_close();
}

void hosted_recovery_start(void)
{
    s_ev = xEventGroupCreate();
    esp_event_handler_register(ESP_HOSTED_EVENT, ESP_EVENT_ANY_ID, hosted_event_handler, nullptr);
    xTaskCreate(recovery_task, "hosted_rec", 6144, nullptr, 5, nullptr);
}

unsigned hosted_recovery_count(void)
{
    return s_count;
}
