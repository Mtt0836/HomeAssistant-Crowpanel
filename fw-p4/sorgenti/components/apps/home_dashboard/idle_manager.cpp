#include "idle_manager.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "standby_show.h"
#include "lovelace_ui.h"
#include "ha_ws.h"

static const char *TAG = "idle";

enum State { ACTIVE, SLIDESHOW, OFF };

static ESP_Brookesia_Phone *s_phone = nullptr;
static int                  s_ha_id = -1;
static standby_cfg_t        s_cfg;
static volatile bool        s_force = false;

/* Richieste che arrivano da fuori (Home Assistant, console). Chi le fa gira
   in un altro task: qui si lascia solo un biglietto e la macchina a stati lo
   raccoglie al giro dopo, cosi' non ci sono due task a comandare lo schermo. */
static volatile int  s_req_screen = -1;      // -1 niente, 0 spegni, 1 accendi
static volatile int  s_req_bright = -1;      // -1 niente, altrimenti 0-100
static volatile bool s_screen_on  = true;    // com'e' adesso, per chi lo chiede
static volatile bool s_req_reload = false;   // rileggi standby.json

static void follow_overlay_entities(void);

static bool pointer_pressed(void)
{
    lv_indev_t *in = nullptr;
    while ((in = lv_indev_get_next(in)) != nullptr) {
        if (lv_indev_get_type(in) == LV_INDEV_TYPE_POINTER && in->proc.state == LV_INDEV_STATE_PRESSED)
            return true;
    }
    return false;
}

static void open_dashboard(void)
{
    if (s_phone == nullptr || s_ha_id < 0) return;
    ESP_Brookesia_CoreAppEventData_t ev = {};
    ev.id   = s_ha_id;
    ev.type = ESP_BROOKESIA_CORE_APP_EVENT_TYPE_START;
    ev.data = nullptr;
    bsp_display_lock(0);
    s_phone->sendAppEvent(&ev);
    bsp_display_unlock();
}

/* Torna alla dashboard: toglie lo slideshow e riazzera l'inattivita'. */
static void wake_up(void)
{
    standby_show_stop();
    open_dashboard();
    bsp_display_lock(0);
    lv_disp_trig_activity(NULL);
    bsp_display_unlock();
}

static void idle_task(void *arg)
{
    State st = ACTIVE;
    int64_t since = 0;
    int brightness = 100;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(200));
        /* Anche solo leggere si fa col lock preso. In pratica qui si legge un
           intero allineato e non e' mai andata storta, ma la regola di LVGL non
           distingue fra letture e scritture: l'eccezione tollerata e' quella
           che un domani qualcuno allarga. */
        bsp_display_lock(0);
        const uint32_t idle_ms = lv_disp_get_inactive_time(NULL);
        bsp_display_unlock();
        const int64_t now = esp_timer_get_time();

        if (s_req_reload) {
            s_req_reload = false;
            standby_config_load(&s_cfg);
            follow_overlay_entities();
        }
        if (s_req_bright >= 0) {
            brightness = s_req_bright;
            s_req_bright = -1;
            /* A schermo spento la luminosita' scelta si mette da parte: la
               ritrova il risveglio, altrimenti riaccenderemmo adesso. */
            if (st != OFF) bsp_display_brightness_set(brightness);
        }
        if (s_req_screen == 0 && st != OFF) {
            ESP_LOGI(TAG, "Home Assistant: schermo spento");
            /* Segnato subito: avviare o togliere lo slideshow richiede
               qualche decimo di secondo, e nel frattempo HA chiede com'e'
               andata. Senza questo si sentiva rispondere il valore vecchio. */
            s_screen_on = false;
            if (st == ACTIVE) {
                brightness = bsp_display_brightness_get();
                if (brightness <= 0) brightness = 100;
                standby_show_start(&s_cfg);
            }
            standby_show_set_dark(true);
            bsp_display_brightness_set(0);
            since = esp_timer_get_time();
            st = OFF;
        } else if (s_req_screen == 1 && st != ACTIVE) {
            ESP_LOGI(TAG, "Home Assistant: schermo acceso");
            bsp_display_brightness_set(brightness);
            s_screen_on = true;
            wake_up();
            st = ACTIVE;
        }
        s_req_screen = -1;

        switch (st) {
        case ACTIVE:
            if (s_force || idle_ms > (uint32_t)s_cfg.slideshow_after_s * 1000) {
                s_force = false;
                brightness = bsp_display_brightness_get();
                if (brightness <= 0) brightness = 100;
                ESP_LOGI(TAG, "inattivo da %u s: slideshow", (unsigned)(idle_ms / 1000));
                standby_show_start(&s_cfg);
                /* Da qui un tocco riporta l'inattivita' sotto la soglia. */
                bsp_display_lock(0);
                lv_disp_trig_activity(NULL);
                bsp_display_unlock();
                vTaskDelay(pdMS_TO_TICKS(300));
                since = esp_timer_get_time();
                st = SLIDESHOW;
            }
            break;

        case SLIDESHOW:
        case OFF:
            /* Un tocco riporta l'inattivita' quasi a zero, mentre senza tocchi
               cresce con il tempo passato dall'avvio dello standby. */
            if ((int64_t)idle_ms + 250 < (now - since) / 1000 && idle_ms < 1000) {
                // tocco: riaccendo, aspetto il rilascio, poi tolgo l'overlay
                ESP_LOGI(TAG, "tocco: risveglio");
                if (st == OFF) bsp_display_brightness_set(brightness);
                for (int i = 0; i < 30 && pointer_pressed(); i++) vTaskDelay(pdMS_TO_TICKS(100));
                wake_up();
                st = ACTIVE;
            } else if (st == SLIDESHOW && s_cfg.screen_off_after_s > 0 &&
                       now - since > (int64_t)s_cfg.screen_off_after_s * 1000000) {
                ESP_LOGI(TAG, "slideshow da %d s: schermo spento", s_cfg.screen_off_after_s);
                standby_show_set_dark(true);
                bsp_display_brightness_set(0);
                st = OFF;
            }
            break;
        }
        s_screen_on = (st != OFF);
    }
}

void idle_manager_force_standby(void) { s_force = true; }

void idle_manager_set_screen(bool on) { s_req_screen = on ? 1 : 0; }
bool idle_manager_screen_on(void)     { return s_screen_on; }

void idle_manager_set_brightness(int pct)
{
    if (pct < 1) pct = 1;                 // 0 spegnerebbe: per quello c'e' set_screen
    if (pct > 100) pct = 100;
    s_req_bright = pct;
}

int idle_manager_brightness(void)
{
    int b = bsp_display_brightness_get();
    return b > 0 ? b : 0;
}

static void follow_overlay_entities(void)
{
    /* Le entita' in sovrimpressione vanno seguite anche se la dashboard non le usa. */
    static const char *ids[STANDBY_MAX_OVERLAY + 1];
    for (int i = 0; i < s_cfg.n_overlay; i++) ids[i] = s_cfg.overlay[i];
    ids[s_cfg.n_overlay] = nullptr;
    bsp_display_lock(0);
    ll_set_extra_entities(ids);
    bool have_view = ll_has_view();
    const char *const *all = ll_entity_ids();
    bsp_display_unlock();
    if (have_view) ha_ws_follow_entities(all);
}

/* Rilegge la configurazione e rimette in ascolto le entita' scelte. La chiama
   la pagina web dopo un salvataggio, e l'integrazione quando la
   configurazione arriva da Home Assistant: senza, le entita' nuove
   resterebbero senza valore fino al riavvio (nessuno le avrebbe chieste).

   Il lavoro vero non si fa qui ma nella macchina a stati, come per lo
   schermo: chi chiama arriva dal server web o dal task del WebSocket, e
   prendere il lock del display da li' e' la strada che ci ha gia' portato a
   un pannello bloccato una volta (la configurazione guidata). */
void idle_manager_reload_config(void)
{
    s_req_reload = true;
}

void idle_manager_start(ESP_Brookesia_Phone *phone, int ha_app_id)
{
    s_phone = phone;
    s_ha_id = ha_app_id;
    standby_config_load(&s_cfg);
    follow_overlay_entities();

    /* 8 KB e non 4: su questo stack non gira solo il conteggio dell'inattivita'.
       Quando parte lo slideshow ci passano dentro, uno dentro l'altro, la
       lettura della cartella su FATFS, il driver della SD, il decodificatore
       JPEG, il ridimensionamento PPA e la creazione dell'immagine in LVGL. Con
       4 KB il pannello e' morto davvero, in "stack overflow in task idle_mgr",
       la prima volta che e' entrato in standby con la RAM interna frammentata:
       ogni lettura della SD falliva e rientrava a ripetere, e ogni giro si
       portava via un altro pezzo di pila. */
    xTaskCreate(idle_task, "idle_mgr", 8192, nullptr, 3, nullptr);
    ESP_LOGI(TAG, "slideshow dopo %d s di inattivita', schermo spento dopo altri %d s",
             s_cfg.slideshow_after_s, s_cfg.screen_off_after_s);
}
