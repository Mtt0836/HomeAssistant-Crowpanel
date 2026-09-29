#include "setup_wizard.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "nvs.h"
#include "bsp/esp-bsp.h"
#include "esp_brookesia.hpp"

#include "home_dashboard/net_config.h"
#include "home_dashboard/ha_config.h"
#include "home_dashboard/ha_discover.h"
#include "home_dashboard/ha_ws.h"
#include "home_dashboard/ha_token.h"
#include "home_dashboard/ha_http.h"
#include "home_dashboard/web_auth.h"
#include "home_dashboard/HomeDashboard.hpp"
#include "pin_lock.h"

static const char *TAG = "wizard";
#define NS_SETUP  "setup"
#define K_DONE    "done"

#define C_BG     lv_color_hex(0x10131a)
#define C_CARD   lv_color_hex(0x1b2030)
#define C_TXT    lv_color_hex(0xf0f2f5)
#define C_SOFT   lv_color_hex(0x9aa3ad)
#define C_ACC    lv_color_hex(0x0b7dda)
#define C_OK     lv_color_hex(0x2e7d32)
#define C_ERR    lv_color_hex(0xef5350)

enum {
    ST_WELCOME = 0, ST_WIFI, ST_NAME, ST_HA, ST_AUTH, ST_DASH, ST_PIN, ST_PASS, ST_DONE,
    ST_COUNT
};

static const char *STEP_TITLE[ST_COUNT] = {
    "Benvenuto", "Rete Wi-Fi", "Nome e stanza", "Home Assistant",
    "Autorizzazione", "Dashboard", "PIN dello schermo", "Password della pagina web",
    "Tutto pronto"
};

struct Wz {
    lv_obj_t  *win, *body, *title, *counter, *hint, *kb;
    lv_obj_t  *btn_back, *btn_skip, *btn_next;
    lv_timer_t *tick;
    int   step;
    bool  hidden_for_wifi;
    // dati raccolti
    char  name[32], room[32], ha_url[HA_URL_MAX], dash[HA_DASH_MAX];
    int   view;
    char  pin_first[PIN_MAX_LEN + 1];
    // campi della schermata corrente
    lv_obj_t *ta_a, *ta_b, *state_lab, *list_box;
    // passo di Home Assistant: la scheda per scrivere l'indirizzo a mano sta
    // li' pronta ma nascosta, e si apre solo se la si chiede
    lv_obj_t *box_manuale;
    bool  ha_manuale;
    bool  ha_lista_fatta;
};

static Wz *s_w = nullptr;
static ESP_Brookesia_Phone *s_phone = nullptr;
static int s_settings_id = -1;

/* Risposte di HA raccolte nel task del WebSocket e lette dal timer LVGL. */
static char  *s_reply = nullptr;         // JSON grezzo, in PSRAM
static volatile bool s_reply_ready = false;
static volatile bool s_reply_ok = false;

static void show_step(int step);

// ------------------------------------------------------------------ memoria

bool setup_wizard_needed(void)
{
    nvs_handle_t h;
    uint8_t done = 0;
    if (nvs_open(NS_SETUP, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, K_DONE, &done);
        nvs_close(h);
    }
    return done == 0;
}

static void mark_done(bool done)
{
    nvs_handle_t h;
    if (nvs_open(NS_SETUP, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, K_DONE, done ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

void setup_wizard_reset(void) { mark_done(false); }

bool setup_wizard_active(void) { return s_w != nullptr; }

// ------------------------------------------------------------------ mattoni

static lv_obj_t *wz_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t col)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, col, 0);
    return l;
}

static lv_obj_t *wz_btn(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *ud, bool primary)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_height(b, 56);
    lv_obj_set_style_bg_color(b, primary ? C_ACC : lv_color_hex(0x2c3242), 0);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_22, 0);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

static void ta_focus(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    lv_obj_t *ta = lv_event_get_target(e);
    if (c == LV_EVENT_FOCUSED) {
        lv_keyboard_set_textarea(s_w->kb, ta);
        lv_obj_clear_flag(s_w->kb, LV_OBJ_FLAG_HIDDEN);
    } else if (c == LV_EVENT_DEFOCUSED || c == LV_EVENT_READY) {
        lv_obj_add_flag(s_w->kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_state(ta, LV_STATE_FOCUSED);
    }
}

static lv_obj_t *wz_field(lv_obj_t *parent, const char *label, const char *value,
                          const char *placeholder)
{
    wz_label(parent, label, &lv_font_montserrat_20, C_SOFT);
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_text(ta, value ? value : "");
    if (placeholder) lv_textarea_set_placeholder_text(ta, placeholder);
    lv_obj_set_width(ta, lv_pct(100));
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_22, 0);
    lv_obj_add_event_cb(ta, ta_focus, LV_EVENT_ALL, nullptr);
    return ta;
}

static lv_obj_t *wz_card(lv_obj_t *parent)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(c, C_CARD, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_radius(c, 12, 0);
    lv_obj_set_style_pad_all(c, 16, 0);
    lv_obj_set_style_pad_row(c, 10, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

// ------------------------------------------------------------------ Wi-Fi

static bool wifi_ok(void)
{
    wifi_ap_record_t ap = {};
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return false;
    esp_netif_ip_info_t ip = {};
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    return nif && esp_netif_get_ip_info(nif, &ip) == ESP_OK && ip.ip.addr != 0;
}

static void wifi_open(lv_event_t *e)
{
    (void)e;
    if (!s_phone || s_settings_id < 0) return;
    /* Mi tolgo di mezzo: la schermata Wi-Fi e' quella dell'app Impostazioni.
       Quando la rete sara' collegata il timer mi rimette in primo piano. */
    lv_obj_add_flag(s_w->win, LV_OBJ_FLAG_HIDDEN);
    s_w->hidden_for_wifi = true;
    ESP_Brookesia_CoreAppEventData_t ev = {};
    ev.id = s_settings_id;
    ev.type = ESP_BROOKESIA_CORE_APP_EVENT_TYPE_START;
    s_phone->sendAppEvent(&ev);
}

// --------------------------------------------- HA: trovarlo sulla rete

/* Home Assistant si annuncia da solo, quindi l'indirizzo non c'e' bisogno di
   scriverlo: il pannello chiede alla rete chi c'e' e mostra quello che
   trova. La ricerca sta in un task a parte perche' blocca un secondo e
   mezzo, e bloccare LVGL vorrebbe dire una schermata congelata. */
static ha_found_t s_ha[HA_FOUND_MAX];
static volatile int s_ha_n = -1;          // -1 = sto ancora cercando

static void ha_scan_task(void *arg)
{
    (void)arg;
    int n = ha_discover_scan(s_ha, HA_FOUND_MAX, 1500);
    s_ha_n = n;                            // per ultimo: e' lui a dire che l'array e' pronto
    vTaskDelete(nullptr);
}

static void ha_mostra_manuale(void)
{
    if (!s_w || s_w->ha_manuale) return;
    s_w->ha_manuale = true;
    if (s_w->box_manuale) lv_obj_clear_flag(s_w->box_manuale, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_w->btn_next, LV_OBJ_FLAG_HIDDEN);
}

static void ha_manuale_clicked(lv_event_t *e) { (void)e; ha_mostra_manuale(); }

static void pick_ha_poi(void *ud)
{
    int i = (int)(intptr_t)ud;
    if (i < 0 || i >= HA_FOUND_MAX || !s_ha[i].url[0]) return;
    strlcpy(s_w->ha_url, s_ha[i].url, sizeof(s_w->ha_url));
    ha_config_save_url(s_ha[i].url);
    /* L'identificativo dell'istanza serve dopo: se un giorno il router da'
       a Home Assistant un altro indirizzo, e' il modo di riconoscerlo. */
    ha_config_save_uuid(s_ha[i].uuid);
    home_dashboard_reconnect();
    show_step(ST_AUTH);
}

/* Il passo successivo non si apre da dentro l'evento del pulsante: cambiare
   schermata vuol dire ripulire il corpo della finestra, cioe' cancellare il
   pulsante che l'evento lo sta ancora ricevendo, e LVGL subito dopo ci
   torna sopra. Il pannello se ne andava in "block already marked as free".
   lv_async_call fa la stessa cosa un attimo dopo, a evento finito. */
static void pick_ha(lv_event_t *e)
{
    lv_async_call(pick_ha_poi, lv_event_get_user_data(e));
}

static void build_ha_list(void)
{
    if (!s_w || !s_w->list_box) return;
    lv_obj_clean(s_w->list_box);
    int n = s_ha_n;

    if (n <= 0) {
        lv_label_set_text(s_w->state_lab,
                          "Non l'ho trovato da solo: scrivi qui sotto l'indirizzo.");
        ha_mostra_manuale();
        lv_obj_add_flag(s_w->list_box, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_label_set_text(s_w->state_lab, n == 1 ? "Ne ho trovato uno sulla rete:"
                                             : "Ne ho trovati piu' di uno sulla rete:");
    for (int i = 0; i < n; i++) {
        char label[96];
        if (s_ha[i].versione[0]) snprintf(label, sizeof(label), "%s  (%s)", s_ha[i].nome, s_ha[i].versione);
        else                     snprintf(label, sizeof(label), "%s", s_ha[i].nome);

        lv_obj_t *b = wz_btn(s_w->list_box, label, pick_ha, (void *)(intptr_t)i, i == 0);
        lv_obj_set_width(b, lv_pct(100));

        /* Sotto al nome, l'indirizzo. Il nome se lo sceglie chi installa Home
           Assistant e non prova niente: due case vicine possono chiamarlo
           tutte e due "Casa", e chi volesse farsi passare per il tuo lo
           scriverebbe uguale al tuo. L'indirizzo invece dice da che parte si
           sta andando, ed e' l'unica cosa che qui si puo' riconoscere. */
        char dove[80];
        ha_discover_host(s_ha[i].url, dove, sizeof(dove));
        if (dove[0]) {
            lv_obj_set_height(b, 78);
            lv_obj_t *nome = lv_obj_get_child(b, 0);
            lv_obj_align(nome, LV_ALIGN_TOP_MID, 0, 2);
            lv_obj_t *sotto = lv_label_create(b);
            lv_label_set_text(sotto, dove);
            lv_obj_set_style_text_font(sotto, &lv_font_montserrat_16, 0);
            // sul pulsante blu il grigio si legge male
            lv_obj_set_style_text_color(sotto, i == 0 ? lv_color_hex(0xd7e6f5) : C_SOFT, 0);
            lv_obj_align(sotto, LV_ALIGN_BOTTOM_MID, 0, -2);
        }
    }
    lv_obj_set_width(wz_btn(s_w->list_box, "Scrivi l'indirizzo a mano",
                            ha_manuale_clicked, nullptr, false), lv_pct(100));
}

// ------------------------------------------------------------------ HA: elenchi

static void reply_cb(bool success, cJSON *result, const char *error, void *ctx)
{
    (void)ctx;
    if (s_reply) { free(s_reply); s_reply = nullptr; }
    s_reply_ok = success;
    if (success && result) s_reply = cJSON_PrintUnformatted(result);
    else if (error)        s_reply = strdup(error);
    s_reply_ready = true;
}

static void ask_dashboards(void)
{
    s_reply_ready = false;
    s_reply_ok = false;
    ha_ws_request("\"type\":\"lovelace/dashboards/list\"", reply_cb, nullptr);
}

static void pick_dash_poi(void *ud)
{
    const char *path = (const char *)ud;
    strlcpy(s_w->dash, path ? path : "lovelace", sizeof(s_w->dash));
    s_w->view = 0;
    ha_config_save_dash(s_w->dash, 0);
    home_dashboard_reconnect();
    show_step(ST_PIN);
}

// come pick_ha: il cambio di schermata aspetta la fine dell'evento
static void pick_dash(lv_event_t *e)
{
    lv_async_call(pick_dash_poi, lv_event_get_user_data(e));
}

static void build_dash_list(cJSON *arr)
{
    lv_obj_clean(s_w->list_box);
    /* La dashboard predefinita non compare nell'elenco di HA: la metto io. */
    static char paths[12][HA_DASH_MAX];
    int n = 0;
    strlcpy(paths[n], "lovelace", HA_DASH_MAX);
    wz_btn(s_w->list_box, "Predefinita (Panoramica)", pick_dash, paths[n], false);
    n++;

    const cJSON *d;
    cJSON_ArrayForEach(d, arr) {
        if (n >= 12) break;
        const cJSON *up = cJSON_GetObjectItem(d, "url_path");
        const cJSON *ti = cJSON_GetObjectItem(d, "title");
        if (!cJSON_IsString(up)) continue;
        strlcpy(paths[n], up->valuestring, HA_DASH_MAX);
        char label[96];
        snprintf(label, sizeof(label), "%s", cJSON_IsString(ti) ? ti->valuestring : up->valuestring);
        wz_btn(s_w->list_box, label, pick_dash, paths[n], false);
        n++;
    }
    if (n == 1) wz_label(s_w->list_box, "Home Assistant non ha dashboard aggiuntive.",
                         &lv_font_montserrat_20, C_SOFT);
}

// ------------------------------------------------------------------ passi

static void next_clicked(lv_event_t *e);
static void back_clicked(lv_event_t *e);
static void skip_clicked(lv_event_t *e);

static void save_step_data(void)
{
    switch (s_w->step) {
    case ST_NAME: {
        net_config_t c;
        net_config_load(&c);
        if (s_w->ta_a) strlcpy(c.hostname, lv_textarea_get_text(s_w->ta_a), sizeof(c.hostname));
        if (s_w->ta_b) strlcpy(c.room, lv_textarea_get_text(s_w->ta_b), sizeof(c.room));
        char err[96];
        if (net_config_validate(&c, err, sizeof(err))) net_config_save(&c);
        break;
    }
    case ST_HA: {
        /* Se l'indirizzo e' stato scelto dall'elenco si e' gia' salvato da
           se': qui si passa solo quando lo si e' scritto a mano. */
        if (!s_w->ha_manuale || !s_w->ta_a) break;
        const char *u = lv_textarea_get_text(s_w->ta_a);
        if (!strncmp(u, "ws://", 5) || !strncmp(u, "wss://", 6)) {
            strlcpy(s_w->ha_url, u, sizeof(s_w->ha_url));
            ha_config_save_url(u);
            /* Se l'indirizzo scritto a mano e' comunque uno di quelli che si
               annunciano, mi segno anche l'identificativo: cosi' il pannello
               sa ritrovarlo se cambia indirizzo. Se non lo e', lo dimentico,
               perche' un identificativo vecchio farebbe inseguire l'istanza
               sbagliata. */
            const char *uu = "";
            for (int i = 0; i < s_ha_n && i < HA_FOUND_MAX; i++)
                if (!strcmp(s_ha[i].url, u)) { uu = s_ha[i].uuid; break; }
            ha_config_save_uuid(uu);
            home_dashboard_reconnect();
        }
        break;
    }
    case ST_PASS: {
        if (!s_w->ta_a || !s_w->ta_b) break;
        const char *a = lv_textarea_get_text(s_w->ta_a);
        const char *b = lv_textarea_get_text(s_w->ta_b);
        if (a[0] && !strcmp(a, b)) web_auth_set_password(WEB_ROLE_ADMIN, a);
        break;
    }
    default: break;
    }
}

/* PIN: prima le cifre nuove, poi la conferma. */
static void pin_confirm(const char *pin, void *ctx)
{
    (void)ctx;
    if (!pin) return;
    if (strcmp(pin, s_w->pin_first)) {
        lv_label_set_text(s_w->state_lab, "I due PIN non coincidono: riprova.");
        lv_obj_set_style_text_color(s_w->state_lab, C_ERR, 0);
        return;
    }
    if (pin_lock_set(pin)) {
        lv_label_set_text(s_w->state_lab, "PIN impostato.");
        lv_obj_set_style_text_color(s_w->state_lab, C_OK, 0);
    }
}

static void pin_new(const char *pin, void *ctx)
{
    (void)ctx;
    if (!pin) return;
    strlcpy(s_w->pin_first, pin, sizeof(s_w->pin_first));
    pin_lock_ask_digits("Ripeti il PIN", "Le stesse cifre di prima", pin_confirm, nullptr);
}

static void pin_clicked(lv_event_t *e)
{
    (void)e;
    pin_lock_ask_digits("Nuovo PIN", "Da 4 a 8 cifre", pin_new, nullptr);
}

static void finish_clicked(lv_event_t *e)
{
    (void)e;
    setup_wizard_close(true);
    ESP_LOGI(TAG, "configurazione guidata completata");
}

static void show_step(int step)
{
    Wz *w = s_w;
    w->step = step;
    w->ta_a = w->ta_b = w->state_lab = w->list_box = nullptr;
    w->box_manuale = nullptr;          // sta nel corpo: fra un attimo non c'e' piu'
    w->ha_manuale = w->ha_lista_fatta = false;
    lv_obj_clean(w->body);
    lv_obj_add_flag(w->kb, LV_OBJ_FLAG_HIDDEN);

    lv_label_set_text(w->title, STEP_TITLE[step]);
    lv_label_set_text_fmt(w->counter, "passo %d di %d", step + 1, ST_COUNT);
    lv_obj_clear_flag(w->btn_next, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(w->btn_skip, LV_OBJ_FLAG_HIDDEN);
    if (step == 0) lv_obj_add_flag(w->btn_back, LV_OBJ_FLAG_HIDDEN);
    else           lv_obj_clear_flag(w->btn_back, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *c = wz_card(w->body);
    net_config_t nc;
    net_config_load(&nc);

    switch (step) {
    case ST_WELCOME:
        wz_label(c, "Questo pannello mostra una dashboard di Home Assistant.\n\n"
                    "Ci vogliono pochi minuti: rete Wi-Fi, collegamento a Home "
                    "Assistant, scelta della dashboard. PIN e password si possono "
                    "anche saltare e impostare dopo.", &lv_font_montserrat_22, C_TXT);
        break;

    case ST_WIFI:
        w->state_lab = wz_label(c, "", &lv_font_montserrat_22, C_TXT);
        wz_label(c, "La rete serve per parlare con Home Assistant e per aprire la "
                    "pagina di configurazione dal telefono.", &lv_font_montserrat_20, C_SOFT);
        wz_btn(c, "Scegli la rete", wifi_open, nullptr, true);
        break;

    case ST_NAME:
        w->ta_a = wz_field(c, "Nome del dispositivo", nc.hostname, "pannello-ha");
        w->ta_b = wz_field(c, "Stanza", nc.room, "es. Cucina");
        wz_label(c, "Il nome e' quello con cui il pannello si presenta alla rete. "
                    "La stanza serve solo a riconoscerlo.", &lv_font_montserrat_20, C_SOFT);
        lv_obj_add_flag(w->btn_skip, LV_OBJ_FLAG_HIDDEN);
        break;

    case ST_HA: {
        char url[HA_URL_MAX] = "", tok[HA_TOKEN_MAX] = "";
        ha_config_load(url, sizeof(url), tok, sizeof(tok));
        memset(tok, 0, sizeof(tok));

        w->state_lab = wz_label(c, "Cerco Home Assistant sulla rete...",
                                &lv_font_montserrat_22, C_TXT);
        w->list_box = wz_card(w->body);

        /* La scheda per scriverlo a mano c'e' gia', ma sta nascosta: serve a
           chi ha Home Assistant su un'altra sottorete o dietro un indirizzo
           esterno, dove l'annuncio non arriva. */
        w->box_manuale = wz_card(w->body);
        lv_obj_add_flag(w->box_manuale, LV_OBJ_FLAG_HIDDEN);
        w->ta_a = wz_field(w->box_manuale, "Indirizzo di Home Assistant", url,
                           "ws://192.168.1.20:8123/api/websocket");
        wz_label(w->box_manuale, "E' l'indirizzo che usi nel browser, con ws:// davanti e "
                                 "/api/websocket in fondo.", &lv_font_montserrat_20, C_SOFT);

        w->ha_manuale = false;
        w->ha_lista_fatta = false;
        lv_obj_add_flag(w->btn_next, LV_OBJ_FLAG_HIDDEN);   // si prosegue scegliendo
        s_ha_n = -1;
        xTaskCreate(ha_scan_task, "wz_ha", 4096, nullptr, 3, nullptr);
        break;
    }

    case ST_AUTH: {
        char me[64] = "";
        ha_http_panel_url(me, sizeof(me), "/");
        if (ha_token_have_refresh()) {
            wz_label(c, "Il pannello e' gia' autorizzato su Home Assistant.",
                     &lv_font_montserrat_22, C_OK);
        } else {
            wz_label(c, "Inquadra il codice col telefono, apri la pagina e premi "
                        "\"Entra con Home Assistant\": autorizzi una volta sola e il "
                        "pannello resta collegato.", &lv_font_montserrat_20, C_TXT);
#if defined(LV_USE_QRCODE) && LV_USE_QRCODE
            lv_obj_t *qr = lv_qrcode_create(c, 200, lv_color_black(), lv_color_white());
            lv_qrcode_update(qr, me, strlen(me));
            lv_obj_set_style_border_width(qr, 6, 0);
            lv_obj_set_style_border_color(qr, lv_color_white(), 0);
#endif
            wz_label(c, me, &lv_font_montserrat_20, C_SOFT);
            w->state_lab = wz_label(c, "In attesa dell'autorizzazione...",
                                    &lv_font_montserrat_20, C_SOFT);
            lv_obj_clear_flag(w->btn_skip, LV_OBJ_FLAG_HIDDEN);
        }
        break;
    }

    case ST_DASH:
        wz_label(c, "Quale dashboard deve mostrare il pannello?",
                 &lv_font_montserrat_22, C_TXT);
        w->state_lab = wz_label(c, "Chiedo l'elenco a Home Assistant...",
                                &lv_font_montserrat_20, C_SOFT);
        w->list_box = wz_card(w->body);
        ask_dashboards();
        lv_obj_add_flag(w->btn_next, LV_OBJ_FLAG_HIDDEN);   // si prosegue scegliendo
        break;

    case ST_PIN:
        wz_label(c, "Un PIN protegge le schermate che possono scollegare il pannello "
                    "o mostrare le credenziali di casa: rete, Home Assistant, debug, Wi-Fi.",
                 &lv_font_montserrat_20, C_TXT);
        w->state_lab = wz_label(c, pin_lock_is_set() ? "PIN gia' impostato." : "",
                                &lv_font_montserrat_20, C_SOFT);
        wz_btn(c, pin_lock_is_set() ? "Cambia PIN" : "Imposta PIN", pin_clicked, nullptr, true);
        lv_obj_clear_flag(w->btn_skip, LV_OBJ_FLAG_HIDDEN);
        break;

    case ST_PASS:
        if (web_auth_configured()) {
            wz_label(c, "La password della pagina web e' gia' impostata.",
                     &lv_font_montserrat_22, C_OK);
        } else {
            wz_label(c, "Serve per aprire la pagina di configurazione dal telefono. "
                        "Almeno 6 caratteri.", &lv_font_montserrat_20, C_TXT);
            w->ta_a = wz_field(c, "Password", "", nullptr);
            w->ta_b = wz_field(c, "Ripetila", "", nullptr);
            lv_textarea_set_password_mode(w->ta_a, true);
            lv_textarea_set_password_mode(w->ta_b, true);
            lv_obj_clear_flag(w->btn_skip, LV_OBJ_FLAG_HIDDEN);
        }
        break;

    case ST_DONE: {
        char txt[320];
        snprintf(txt, sizeof(txt),
                 "Nome: %s\nStanza: %s\nDashboard: %s\nPIN: %s\nPagina web: %s",
                 nc.hostname, nc.room[0] ? nc.room : "-",
                 s_w->dash[0] ? s_w->dash : "predefinita",
                 pin_lock_is_set() ? "impostato" : "non impostato",
                 web_auth_configured() ? "protetta" : "da configurare");
        wz_label(c, txt, &lv_font_montserrat_22, C_TXT);
        lv_obj_add_flag(w->btn_next, LV_OBJ_FLAG_HIDDEN);
        wz_btn(c, "Inizia a usare il pannello", finish_clicked, nullptr, true);
        break;
    }
    }
}

static void next_clicked(lv_event_t *e)
{
    (void)e;
    save_step_data();
    int next = s_w->step + 1;
    if (next >= ST_COUNT) next = ST_DONE;
    show_step(next);
}

static void back_clicked(lv_event_t *e)
{
    (void)e;
    if (s_w->step > 0) show_step(s_w->step - 1);
}

static void skip_clicked(lv_event_t *e)
{
    (void)e;
    show_step(s_w->step + 1 >= ST_COUNT ? ST_DONE : s_w->step + 1);
}

// ------------------------------------------------------------------ battito

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    Wz *w = s_w;
    if (!w) return;

    bool net = wifi_ok();

    if (w->hidden_for_wifi) {
        if (net) {                       // rete fatta: torno in primo piano
            w->hidden_for_wifi = false;
            lv_obj_clear_flag(w->win, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(w->win);
            show_step(ST_NAME);
        }
        return;
    }

    if (w->step == ST_WIFI) {
        wifi_ap_record_t ap = {};
        bool ok = net && esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
        if (w->state_lab) {
            lv_label_set_text_fmt(w->state_lab, ok ? "Connesso a %s" : "Nessuna rete collegata",
                                  (const char *)ap.ssid);
            lv_obj_set_style_text_color(w->state_lab, ok ? C_OK : C_ERR, 0);
        }
        if (ok) lv_obj_clear_state(w->btn_next, LV_STATE_DISABLED);
        else    lv_obj_add_state(w->btn_next, LV_STATE_DISABLED);
    }

    if (w->step == ST_HA && !w->ha_lista_fatta && s_ha_n >= 0) {
        w->ha_lista_fatta = true;
        build_ha_list();
    }

    if (w->step == ST_AUTH && !ha_token_have_refresh() && w->state_lab) {
        /* niente: l'attesa la mostra gia' l'etichetta */
    } else if (w->step == ST_AUTH && ha_token_have_refresh()) {
        home_dashboard_reconnect();
        show_step(ST_DASH);
    }

    if (w->step == ST_DASH && s_reply_ready) {
        s_reply_ready = false;
        if (s_reply_ok && s_reply) {
            cJSON *arr = cJSON_Parse(s_reply);
            lv_label_set_text(w->state_lab, "Tocca quella che vuoi vedere:");
            build_dash_list(arr);
            cJSON_Delete(arr);
        } else {
            lv_label_set_text(w->state_lab,
                "Non sono riuscito a leggere l'elenco. Vai avanti: userai la dashboard predefinita.");
            lv_obj_set_style_text_color(w->state_lab, C_ERR, 0);
            lv_obj_clear_flag(w->btn_next, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// ------------------------------------------------------------------ avvio

extern "C" void setup_wizard_start_with(void *phone, int settings_app_id)
{
    if (s_w) return;
    s_phone = (ESP_Brookesia_Phone *)phone;
    s_settings_id = settings_app_id;

    Wz *w = (Wz *)calloc(1, sizeof(Wz));
    if (!w) return;
    s_w = w;

    w->win = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(w->win);
    lv_obj_set_size(w->win, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(w->win, C_BG, 0);
    lv_obj_set_style_bg_opa(w->win, LV_OPA_COVER, 0);
    lv_obj_add_flag(w->win, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(w->win, LV_OBJ_FLAG_SCROLLABLE);

    w->title = lv_label_create(w->win);
    lv_obj_set_style_text_font(w->title, &lv_font_montserrat_34, 0);
    lv_obj_set_style_text_color(w->title, C_TXT, 0);
    lv_obj_align(w->title, LV_ALIGN_TOP_LEFT, 30, 24);

    w->counter = lv_label_create(w->win);
    lv_obj_set_style_text_font(w->counter, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(w->counter, C_SOFT, 0);
    lv_obj_align(w->counter, LV_ALIGN_TOP_RIGHT, -30, 34);

    w->body = lv_obj_create(w->win);
    lv_obj_remove_style_all(w->body);
    lv_obj_set_size(w->body, LV_HOR_RES - 60, LV_VER_RES - 170);
    lv_obj_align(w->body, LV_ALIGN_TOP_MID, 0, 80);
    lv_obj_set_flex_flow(w->body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(w->body, 12, 0);
    lv_obj_set_scroll_dir(w->body, LV_DIR_VER);

    lv_obj_t *bar = lv_obj_create(w->win);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_HOR_RES - 60, 70);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, 12, 0);
    w->btn_back = wz_btn(bar, "Indietro", back_clicked, nullptr, false);
    w->btn_skip = wz_btn(bar, "Salta",    skip_clicked, nullptr, false);
    w->btn_next = wz_btn(bar, "Avanti",   next_clicked, nullptr, true);
    lv_obj_set_width(w->btn_back, 150);
    lv_obj_set_width(w->btn_skip, 150);
    lv_obj_set_width(w->btn_next, 180);

    w->kb = lv_keyboard_create(w->win);
    lv_obj_set_size(w->kb, LV_HOR_RES, 250);
    lv_obj_align(w->kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(w->kb, LV_OBJ_FLAG_HIDDEN);

    w->tick = lv_timer_create(tick_cb, 800, nullptr);
    show_step(ST_WELCOME);
    ESP_LOGI(TAG, "configurazione guidata avviata");
}

void setup_wizard_start(void) { setup_wizard_start_with(nullptr, -1); }

/* Come premere "Avanti": salva il passo corrente e va al successivo. Serve
   per provare dal PC la stessa strada che fa il dito. */
void setup_wizard_next(void)
{
    if (!s_w) return;
    save_step_data();
    show_step(s_w->step + 1 >= ST_COUNT ? ST_DONE : s_w->step + 1);
}

void setup_wizard_goto(int step)
{
    if (!s_w || step < 0 || step >= ST_COUNT) return;
    show_step(step);
}

void setup_wizard_close(bool done)
{
    if (!s_w) { mark_done(done); return; }
    mark_done(done);
    if (s_w->tick) lv_timer_del(s_w->tick);
    lv_obj_del(s_w->win);
    free(s_w);
    s_w = nullptr;
    if (s_reply) { free(s_reply); s_reply = nullptr; }
}
