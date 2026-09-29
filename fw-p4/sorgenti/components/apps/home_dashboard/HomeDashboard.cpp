#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "bsp/esp-bsp.h"
#include "HomeDashboard.hpp"
#include "ha_config.h"
#include "lovelace_ui.h"
#include "dash_override.h"
#include "ha_plugin.h"
#include <string>
#include "ha_ws.h"
#include "web_config.h"
#include "tts_player.h"

static const char *TAG = "HomeDash";
static HomeDashboard *s_self = nullptr;   // singleton per le callback C

LV_IMG_DECLARE(img_app_home);

#define COLOR_ON   lv_color_make(0x2e,0xa0,0x43)
#define COLOR_OFF  lv_color_make(0x3a,0x3a,0x3a)

HomeDashboard::HomeDashboard():
    ESP_Brookesia_PhoneApp("Home", &img_app_home, true) {}
HomeDashboard::~HomeDashboard() {}

bool HomeDashboard::init(void)
{
    s_self = this;
    tts_player_init();
    dash_override_init();

    if (ha_config_is_set()) {
        char url[HA_URL_MAX], token[HA_TOKEN_MAX], dash[HA_DASH_MAX];
        int view = 0;
        ha_config_load(url,sizeof(url),token,sizeof(token));
        ha_config_load_dash(dash,sizeof(dash),&view);
        ha_ws_callbacks_t cb = {};
        cb.on_status   = on_status;
        cb.on_lovelace = on_lovelace;
        cb.on_entity   = on_entity;
        cb.on_tts      = on_tts;
        ha_ws_start(url, token, dash, &cb);
        ll_charts_start();
        /* Si presenta a HA e si mette in ascolto dei suoi comandi. Se
           l'integrazione non c'e', se ne accorge e non insiste. */
        ha_plugin_start();
    }
    // il server web e' sempre attivo: setup token
    web_config_start(on_config_changed, on_layout_changed);
    return true;
}

bool HomeDashboard::run(void)
{
    if (ha_config_is_set()) build_dashboard();
    else                    build_setup_screen();
    _ui_ready = true;
    return true;
}

bool HomeDashboard::back(void)  { return close(); }

void HomeDashboard::destroy_ui(void)
{
    ll_unbind();
    if (_root){ lv_obj_del(_root); _root=nullptr; }
    _status_dot=nullptr;
}

bool HomeDashboard::close(void)
{
    _ui_ready = false;
    destroy_ui();
    return true;
}

// ---- IP corrente (per QR/URL di setup) ----
static void get_ip(char *out, size_t sz)
{
    esp_netif_ip_info_t ip = {};
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (nif && esp_netif_get_ip_info(nif,&ip)==ESP_OK)
        snprintf(out,sz,IPSTR,IP2STR(&ip.ip));
    else
        snprintf(out,sz,"0.0.0.0");
}

void HomeDashboard::build_setup_screen(void)
{
    lv_area_t a=getVisualArea(); int w=a.x2-a.x1,h=a.y2-a.y1;
    _root=lv_obj_create(lv_scr_act());
    lv_obj_set_size(_root,w,h);
    lv_obj_align(_root,LV_ALIGN_TOP_MID,0,0);
    lv_obj_set_flex_flow(_root,LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(_root,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_border_width(_root,0,0);

    char ip[16]; get_ip(ip,sizeof(ip));
    char link[48]; snprintf(link,sizeof(link),"http://%s/setup",ip);

    lv_obj_t *t=lv_label_create(_root);
    lv_label_set_text(t,"Configura il pannello");
    lv_obj_set_style_text_font(t,&lv_font_montserrat_34,0);

#if defined(LV_USE_QRCODE) && LV_USE_QRCODE
    lv_obj_t *qr=lv_qrcode_create(_root,240,lv_color_black(),lv_color_white());
    lv_qrcode_update(qr,link,strlen(link));
#endif

    lv_obj_t *l=lv_label_create(_root);
    lv_label_set_text_fmt(l,"Apri dal telefono:\n%s",link);
    lv_obj_set_style_text_align(l,LV_TEXT_ALIGN_CENTER,0);
}

/* Messaggio mostrato finche' la dashboard non e' arrivata (o se HA la rifiuta). */
static std::string s_msg = "Connessione a Home Assistant...";

void HomeDashboard::build_dashboard(void)
{
    lv_area_t a=getVisualArea(); int w=a.x2-a.x1+1,h=a.y2-a.y1+1;
    _root=lv_obj_create(lv_scr_act());
    lv_obj_set_size(_root,w,h);
    lv_obj_align(_root,LV_ALIGN_TOP_MID,0,0);
    lv_obj_set_style_border_width(_root,0,0); lv_obj_set_style_radius(_root,0,0);
    lv_obj_set_style_pad_all(_root,12,0);
    lv_obj_set_style_bg_color(_root,lv_color_hex(0x111318),0);
    lv_obj_set_scroll_dir(_root,LV_DIR_VER);

    if (ll_has_view()) {
        ll_build(_root, w-24-8);          // 8 px lasciati alla barra di scorrimento
    } else {
        lv_obj_t *l=lv_label_create(_root);
        lv_label_set_text(l,s_msg.c_str());
        lv_obj_set_style_text_color(l,lv_color_hex(0xe1e1e1),0);
        lv_obj_set_style_text_font(l,&lv_font_montserrat_24,0);
        lv_obj_set_style_text_align(l,LV_TEXT_ALIGN_CENTER,0);
        lv_obj_center(l);
    }

    // pallino stato connessione HA, sopra al contenuto
    _status_dot=lv_obj_create(_root);
    lv_obj_add_flag(_status_dot,LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(_status_dot,12,12);
    lv_obj_set_style_radius(_status_dot,6,0);
    lv_obj_set_style_border_width(_status_dot,0,0);
    lv_obj_align(_status_dot,LV_ALIGN_TOP_RIGHT,0,-6);
    lv_obj_set_style_bg_color(_status_dot, ha_ws_connected()?COLOR_ON:lv_color_make(0xc0,0x30,0x30),0);
}

void HomeDashboard::rebuild(void)
{
    if (!_ui_ready) return;
    destroy_ui();
    if (ha_config_is_set()) build_dashboard(); else build_setup_screen();
}

// ---- callback dai moduli (girano nel task WebSocket: prendono il lock LVGL) ----
void HomeDashboard::on_lovelace(cJSON *config, const char *error)
{
    char dash[HA_DASH_MAX]; int view=0;
    ha_config_load_dash(dash,sizeof(dash),&view);
    bool ok=false;
    bsp_display_lock(0);
    if (config) {
        char err[160];
        // i ritocchi locali (ordine, card nascoste) si applicano qui sopra
        cJSON *tweaked = dash_override_prepare(config, view, dash);
        ok = tweaked && ll_set_config(tweaked, 0, err, sizeof(err));
        if (tweaked) cJSON_Delete(tweaked);
        else         snprintf(err,sizeof(err),"La vista %d non esiste in '%s'",view,dash);
        if (!ok) s_msg = err;
    } else {
        s_msg = std::string("Dashboard '") + dash + "' non disponibile:\n" + (error?error:"");
    }
    if (s_self) s_self->rebuild();
    bsp_display_unlock();
    // l'iscrizione agli stati parte fuori dal lock: gli stati arrivano via on_entity
    if (ok) ha_ws_follow_entities(ll_entity_ids());
}

void HomeDashboard::on_entity(const char *entity_id, const char *state, cJSON *attrs)
{
    bsp_display_lock(0);
    ll_entity_update(entity_id, state, attrs);
    bsp_display_unlock();
}

void HomeDashboard::on_status(bool connected)
{
    if (!s_self || !s_self->_ui_ready || !s_self->_status_dot) return;
    if (bsp_display_lock(0)) {
        lv_obj_set_style_bg_color(s_self->_status_dot,
            connected?COLOR_ON:lv_color_make(0xc0,0x30,0x30),0);
        bsp_display_unlock();
    }
}

/* Ricollegarsi vuol dire fermare il WebSocket, e il task del WebSocket puo'
   essere fermo in attesa del lock del display (lo prende per disegnare la
   dashboard appena arrivata). Se chi chiede il ricollegamento e' il task di
   LVGL, che quel lock ce l'ha in mano, i due si aspettano a vicenda e lo
   schermo si blocca: successo davvero, premendo "Avanti" nella configurazione
   guidata. Percio' il lavoro vero lo fa un task a parte. */
void HomeDashboard::reconnect_now(void)
{
    char url[HA_URL_MAX], token[HA_TOKEN_MAX], dash[HA_DASH_MAX]; int view=0;
    if (ha_config_load(url,sizeof(url),token,sizeof(token))) {
        ha_config_load_dash(dash,sizeof(dash),&view);
        ha_ws_restart(url,token,dash);
        if (s_self) { bsp_display_lock(0); s_self->rebuild(); bsp_display_unlock(); }
    }
}

void HomeDashboard::on_config_changed(void)
{
    xTaskCreate([](void *) { HomeDashboard::reconnect_now(); vTaskDelete(NULL); },
                "ha_recfg", 6144, nullptr, 4, nullptr);
}

/* L'editor web ha salvato i ritocchi: ridisegno dalla copia della dashboard
   che abbiamo gia' in memoria, senza ridisturbare Home Assistant. */
void HomeDashboard::on_layout_changed(void)
{
    if (!dash_override_has_source()) { ha_ws_request_lovelace(); return; }
    bool ok=false;
    bsp_display_lock(0);
    cJSON *tweaked = dash_override_rebuild();
    if (tweaked) {
        char err[160];
        ok = ll_set_config(tweaked, 0, err, sizeof(err));
        if (!ok) s_msg = err;
        cJSON_Delete(tweaked);
    }
    if (s_self) s_self->rebuild();
    bsp_display_unlock();
    if (ok) { ha_ws_follow_entities(ll_entity_ids()); ll_charts_refresh(); }
}

void HomeDashboard::on_tts(const char *message){ tts_player_say(message); }

extern "C" void home_dashboard_reconnect(void) { HomeDashboard::on_config_changed(); }
