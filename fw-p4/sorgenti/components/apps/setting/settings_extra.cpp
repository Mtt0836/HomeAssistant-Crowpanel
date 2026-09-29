#include "settings_extra.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <initializer_list>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_hosted.h"
#include "esp_hosted_misc.h"
#include "bsp/esp-bsp.h"
#include "ui/ui.h"
#include "home_dashboard/net_config.h"
#include "home_dashboard/debug_config.h"
#include "home_dashboard/ha_config.h"
#include "home_dashboard/ha_ws.h"
#include "home_dashboard/hosted_recovery.h"
#include "home_dashboard/uart_console.h"
#include "home_dashboard/web_auth.h"
#include "home_dashboard/web_cert.h"
#include "home_dashboard/factory_reset.h"
#include "pin_lock.h"
#include "setup_wizard.h"

static const char *TAG = "set_extra";

/* In main/lvgl_mem.c, compilato dentro la libreria LVGL. */
extern "C" void lvgl_mem_stats(size_t *internal_bytes, size_t *psram_bytes);

// Stile delle schermate Elecrow (ui_ScreenSetting*)
#define C_SCREEN   lv_color_hex(0xE5F3FF)
#define C_ROW      lv_color_white()
#define C_PRESSED  lv_color_hex(0xCBCBCB)
#define C_TEXT2    lv_color_hex(0x606060)
#define C_ERR      lv_color_hex(0xC62828)
#define C_OK       lv_color_hex(0x2E7D32)
#define CONTENT_W  900
#define CONTENT_Y  86
#define KB_H       250

// ------------------------------------------------------------------ dati di stato
/* Raccolti da un task (le letture WiFi sono RPC verso il C6 e non devono
   bloccare il task LVGL); le schermate li leggono con un lv_timer. */
struct Status {
    char ip[16], gw[16], mask[16], dns[16];
    char mac[18], ssid[33];
    int  rssi, channel;
    bool wifi_ok;
    char c6fw[32];
};
static Status s_st;
static volatile bool s_info_run = false;
static int s_open_screens = 0;

static void info_task(void *arg)
{
    bool fw_read = false;
    while (s_info_run) {
        Status st = {};
        esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t ip = {};
        if (nif && esp_netif_get_ip_info(nif, &ip) == ESP_OK) {
            snprintf(st.ip, sizeof(st.ip), IPSTR, IP2STR(&ip.ip));
            snprintf(st.gw, sizeof(st.gw), IPSTR, IP2STR(&ip.gw));
            snprintf(st.mask, sizeof(st.mask), IPSTR, IP2STR(&ip.netmask));
            esp_netif_dns_info_t d = {};
            if (esp_netif_get_dns_info(nif, ESP_NETIF_DNS_MAIN, &d) == ESP_OK)
                snprintf(st.dns, sizeof(st.dns), IPSTR, IP2STR(&d.ip.u_addr.ip4));
            uint8_t m[6];
            if (esp_netif_get_mac(nif, m) == ESP_OK)
                snprintf(st.mac, sizeof(st.mac), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
        }
        wifi_ap_record_t ap = {};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            st.wifi_ok = true;
            strlcpy(st.ssid, (const char *)ap.ssid, sizeof(st.ssid));
            st.rssi = ap.rssi;
            st.channel = ap.primary;
        }
        if (!fw_read) {
            esp_hosted_coprocessor_fwver_t v = {};
            if (esp_hosted_get_coprocessor_fwversion(&v) == ESP_OK) {
                snprintf(s_st.c6fw, sizeof(s_st.c6fw), "%u.%u.%u build %d",
                         (unsigned)v.major1, (unsigned)v.minor1, (unsigned)v.patch1, (int)v.build);
                fw_read = true;
            }
        }
        strlcpy(st.c6fw, s_st.c6fw, sizeof(st.c6fw));
        bsp_display_lock(0);
        s_st = st;
        bsp_display_unlock();
        for (int i = 0; i < 20 && s_info_run; i++) vTaskDelay(pdMS_TO_TICKS(100));
    }
    vTaskDelete(NULL);
}

static void screen_deleted(lv_event_t *e)
{
    if (--s_open_screens <= 0) {
        s_open_screens = 0;
        s_info_run = false;
    }
}

static void start_info(void)
{
    if (s_open_screens++ == 0 && !s_info_run) {
        s_info_run = true;
        xTaskCreate(info_task, "set_info", 4096, nullptr, 2, nullptr);
    }
}

// ------------------------------------------------------------------ mattoni UI

static lv_obj_t *mk_screen(const char *title, lv_obj_t **content)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, C_SCREEN, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(scr);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_30, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 40);

    /* Stesso pulsante di ritorno delle schermate Elecrow (60x60 in alto a
       sinistra, icona return.png): riporta al menu delle Impostazioni. */
    lv_obj_t *back = lv_btn_create(scr);
    lv_obj_set_size(back, 60, 60);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 22, 30);
    lv_obj_set_style_bg_color(back, lv_color_hex(0xF6F6F6), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
    lv_obj_clear_flag(back, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *back_img = lv_img_create(back);
    lv_img_set_src(back_img, &ui_img_return_png);
    lv_obj_center(back_img);
    lv_obj_set_style_img_recolor(back_img, lv_color_black(), 0);
    lv_obj_set_style_img_recolor_opa(back_img, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(back, [](lv_event_t *e) { lv_scr_load(ui_ScreenSettingMain); },
                        LV_EVENT_CLICKED, nullptr);

    lv_obj_t *c = lv_obj_create(scr);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, CONTENT_W, LV_VER_RES - CONTENT_Y - 10);
    lv_obj_align(c, LV_ALIGN_TOP_MID, 0, CONTENT_Y);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 8, 0);
    lv_obj_set_style_pad_bottom(c, 20, 0);
    lv_obj_set_scroll_dir(c, LV_DIR_VER);
    *content = c;
    lv_obj_add_event_cb(scr, screen_deleted, LV_EVENT_DELETE, nullptr);
    start_info();
    return scr;
}

static lv_obj_t *mk_section(lv_obj_t *parent, const char *title)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, title);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(l, C_TEXT2, 0);
    lv_obj_set_style_pad_top(l, 10, 0);
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_size(box, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(box, C_ROW, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_radius(box, 12, 0);
    lv_obj_set_style_pad_all(box, 14, 0);
    lv_obj_set_style_pad_row(box, 10, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    return box;
}

// Riga "etichetta ........ widget": ritorna il contenitore a destra.
static lv_obj_t *mk_row(lv_obj_t *box, const char *label)
{
    lv_obj_t *r = lv_obj_create(box);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *l = lv_label_create(r);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_22, 0);
    return r;
}

static lv_obj_t *mk_value(lv_obj_t *row)
{
    lv_obj_t *v = lv_label_create(row);
    lv_label_set_text(v, "-");
    lv_obj_set_style_text_font(v, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(v, C_TEXT2, 0);
    return v;
}

static lv_obj_t *mk_btn(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_height(b, 56);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

// ---- tastiera condivisa della schermata ----
struct Kb { lv_obj_t *kb; lv_obj_t *content; };

static void kb_hide(Kb *k)
{
    lv_obj_add_flag(k->kb, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(k->kb, nullptr);
    lv_obj_set_height(k->content, LV_VER_RES - CONTENT_Y - 10);
}

static void kb_event(lv_event_t *e)
{
    Kb *k = (Kb *)lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) kb_hide(k);
}

static void ta_event(lv_event_t *e)
{
    Kb *k = (Kb *)lv_event_get_user_data(e);
    lv_obj_t *ta = lv_event_get_target(e);
    if (lv_event_get_code(e) == LV_EVENT_FOCUSED) {
        bool numeric = lv_obj_get_user_data(ta) != nullptr;
        lv_keyboard_set_mode(k->kb, numeric ? LV_KEYBOARD_MODE_NUMBER : LV_KEYBOARD_MODE_TEXT_LOWER);
        lv_keyboard_set_textarea(k->kb, ta);
        lv_obj_clear_flag(k->kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(k->content, LV_VER_RES - CONTENT_Y - KB_H - 10);
        lv_obj_scroll_to_view_recursive(ta, LV_ANIM_ON);
    } else if (lv_event_get_code(e) == LV_EVENT_DEFOCUSED) {
        kb_hide(k);
    }
}

static Kb *mk_keyboard(lv_obj_t *scr, lv_obj_t *content)
{
    Kb *k = new Kb{lv_keyboard_create(scr), content};
    lv_obj_set_size(k->kb, LV_HOR_RES, KB_H);
    lv_obj_align(k->kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(k->kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(k->kb, kb_event, LV_EVENT_ALL, k);
    lv_obj_add_event_cb(scr, [](lv_event_t *e) { delete (Kb *)lv_event_get_user_data(e); }, LV_EVENT_DELETE, k);
    return k;
}

static lv_obj_t *mk_ta(lv_obj_t *row, Kb *k, const char *text, int w, bool numeric)
{
    lv_obj_t *ta = lv_textarea_create(row);
    lv_textarea_set_one_line(ta, true);
    lv_obj_set_width(ta, w);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_22, 0);
    lv_textarea_set_text(ta, text);
    if (numeric) {
        lv_textarea_set_accepted_chars(ta, "0123456789.");
        lv_textarea_set_max_length(ta, 15);
        lv_obj_set_user_data(ta, (void *)1);
    }
    lv_obj_add_event_cb(ta, ta_event, LV_EVENT_ALL, k);
    return ta;
}

// ------------------------------------------------------------------ Rete

struct NetUi {
    lv_obj_t *v_ip, *v_mac, *v_ssid, *v_rssi, *v_gw, *v_dns;
    lv_obj_t *sw_dhcp, *ta_ip, *ta_mask, *ta_gw, *ta_dns1, *ta_dns2, *ta_ntp;
    lv_obj_t *ta_url, *ta_dash, *ta_view;
    lv_obj_t *msg;
    lv_timer_t *timer;
};

static void net_refresh(lv_timer_t *t)
{
    NetUi *u = (NetUi *)t->user_data;
    lv_label_set_text(u->v_ip, s_st.ip[0] ? s_st.ip : "-");
    lv_label_set_text(u->v_mac, s_st.mac[0] ? s_st.mac : "-");
    lv_label_set_text(u->v_gw, s_st.gw[0] ? s_st.gw : "-");
    lv_label_set_text(u->v_dns, s_st.dns[0] ? s_st.dns : "-");
    if (s_st.wifi_ok) {
        lv_label_set_text(u->v_ssid, s_st.ssid);
        const char *q = s_st.rssi > -55 ? "ottimo" : s_st.rssi > -67 ? "buono" : s_st.rssi > -75 ? "discreto" : "debole";
        lv_label_set_text_fmt(u->v_rssi, "%d dBm (%s), canale %d", s_st.rssi, q, s_st.channel);
    } else {
        lv_label_set_text(u->v_ssid, "non connesso");
        lv_label_set_text(u->v_rssi, "-");
    }
}

static void net_dhcp_changed(lv_event_t *e)
{
    NetUi *u = (NetUi *)lv_event_get_user_data(e);
    bool dhcp = lv_obj_has_state(u->sw_dhcp, LV_STATE_CHECKED);
    for (lv_obj_t *ta : {u->ta_ip, u->ta_mask, u->ta_gw, u->ta_dns1, u->ta_dns2}) {
        if (dhcp) lv_obj_add_state(ta, LV_STATE_DISABLED);
        else      lv_obj_clear_state(ta, LV_STATE_DISABLED);
    }
}

static void set_msg(lv_obj_t *l, bool ok, const char *text)
{
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, ok ? C_OK : C_ERR, 0);
}

static void apply_task(void *arg)
{
    net_config_apply_now();
    vTaskDelete(NULL);
}

static void net_save(lv_event_t *e)
{
    NetUi *u = (NetUi *)lv_event_get_user_data(e);
    net_config_t c;
    net_config_load(&c);
    c.dhcp = lv_obj_has_state(u->sw_dhcp, LV_STATE_CHECKED);
    strlcpy(c.ip,   lv_textarea_get_text(u->ta_ip),   sizeof(c.ip));
    strlcpy(c.mask, lv_textarea_get_text(u->ta_mask), sizeof(c.mask));
    strlcpy(c.gw,   lv_textarea_get_text(u->ta_gw),   sizeof(c.gw));
    strlcpy(c.dns1, lv_textarea_get_text(u->ta_dns1), sizeof(c.dns1));
    strlcpy(c.dns2, lv_textarea_get_text(u->ta_dns2), sizeof(c.dns2));
    strlcpy(c.ntp,  lv_textarea_get_text(u->ta_ntp),  sizeof(c.ntp));
    char err[96];
    if (!net_config_validate(&c, err, sizeof(err))) { set_msg(u->msg, false, err); return; }
    if (!net_config_save(&c)) { set_msg(u->msg, false, "Salvataggio non riuscito"); return; }
    set_msg(u->msg, true, "Salvato: riconnessione WiFi in corso...");
    // disconnect/connect sono RPC verso il C6: fuori dal task LVGL
    xTaskCreate(apply_task, "net_apply", 4096, nullptr, 3, nullptr);
}

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

/* Schermata Home Assistant: indirizzo del server, dashboard e vista. */
struct HaUi { lv_obj_t *ta_url, *ta_dash, *ta_view, *msg; };

static void net_save_ha(lv_event_t *e)
{
    HaUi *u = (HaUi *)lv_event_get_user_data(e);
    const char *url = lv_textarea_get_text(u->ta_url);
    if (strncmp(url, "ws://", 5) != 0 && strncmp(url, "wss://", 6) != 0) {
        set_msg(u->msg, false, "L'indirizzo di HA deve iniziare con ws:// o wss://");
        return;
    }
    const char *dash = lv_textarea_get_text(u->ta_dash);
    int view = atoi(lv_textarea_get_text(u->ta_view));
    if (!ha_config_save_url(url) || !ha_config_save_dash(dash[0] ? dash : "lovelace", view)) {
        set_msg(u->msg, false, "Salvataggio non riuscito");
        return;
    }
    set_msg(u->msg, true, "Salvato: il pannello si riavvia...");
    xTaskCreate(restart_task, "restart", 2048, nullptr, 3, nullptr);
}

static lv_obj_t *build_network(void)
{
    lv_obj_t *c;
    lv_obj_t *scr = mk_screen("Rete", &c);
    NetUi *u = new NetUi{};
    lv_obj_add_event_cb(scr, [](lv_event_t *e) {
        NetUi *x = (NetUi *)lv_event_get_user_data(e);
        lv_timer_del(x->timer);
        delete x;
    }, LV_EVENT_DELETE, u);
    Kb *k = mk_keyboard(scr, c);

    net_config_t nc;
    net_config_load(&nc);

    lv_obj_t *b = mk_section(c, "Connessione attuale");
    u->v_ssid = mk_value(mk_row(b, "Rete WiFi"));
    u->v_rssi = mk_value(mk_row(b, "Segnale"));
    u->v_ip   = mk_value(mk_row(b, "Indirizzo IP"));
    u->v_gw   = mk_value(mk_row(b, "Gateway"));
    u->v_dns  = mk_value(mk_row(b, "DNS"));
    u->v_mac  = mk_value(mk_row(b, "MAC"));

    b = mk_section(c, "Indirizzo del pannello");
    lv_obj_t *r = mk_row(b, "Automatico (DHCP)");
    u->sw_dhcp = lv_switch_create(r);
    if (nc.dhcp) lv_obj_add_state(u->sw_dhcp, LV_STATE_CHECKED);
    lv_obj_add_event_cb(u->sw_dhcp, net_dhcp_changed, LV_EVENT_VALUE_CHANGED, u);
    u->ta_ip   = mk_ta(mk_row(b, "Indirizzo IP"), k, nc.ip, 300, true);
    u->ta_mask = mk_ta(mk_row(b, "Maschera"), k, nc.mask, 300, true);
    u->ta_gw   = mk_ta(mk_row(b, "Gateway"), k, nc.gw, 300, true);
    u->ta_dns1 = mk_ta(mk_row(b, "DNS primario"), k, nc.dns1, 300, true);
    u->ta_dns2 = mk_ta(mk_row(b, "DNS secondario"), k, nc.dns2, 300, true);
    lv_obj_t *dns_note = lv_label_create(b);
    lv_label_set_text(dns_note, "DNS vuoti: si usa il gateway.");
    lv_obj_set_style_text_color(dns_note, C_TEXT2, 0);

    b = mk_section(c, "Nome e ora");
    /* Il nome si cambia dalle informazioni sul dispositivo: qui si vede e
       basta, per non avere due campi che scrivono lo stesso valore. */
    lv_obj_t *vname = mk_value(mk_row(b, "Nome del dispositivo"));
    lv_label_set_text(vname, nc.hostname[0] ? nc.hostname : "pannello");
    u->ta_ntp  = mk_ta(mk_row(b, "Server NTP"), k, nc.ntp, 360, false);
    lv_textarea_set_placeholder_text(u->ta_ntp, "predefiniti");
    lv_obj_t *btns = lv_obj_create(b);
    lv_obj_remove_style_all(btns);
    lv_obj_set_size(btns, lv_pct(100), LV_SIZE_CONTENT);
    mk_btn(btns, "Salva e applica", net_save, u);

    u->msg = lv_label_create(c);
    lv_label_set_text(u->msg, "");
    lv_obj_set_style_text_font(u->msg, &lv_font_montserrat_22, 0);

    // stato iniziale dei campi statici
    lv_event_send(u->sw_dhcp, LV_EVENT_VALUE_CHANGED, nullptr);
    u->timer = lv_timer_create(net_refresh, 1000, u);
    net_refresh(u->timer);
    return scr;
}

static lv_obj_t *build_ha(void)
{
    lv_obj_t *c;
    lv_obj_t *scr = mk_screen("Home Assistant", &c);
    HaUi *u = new HaUi{};
    lv_obj_add_event_cb(scr, [](lv_event_t *e) { delete (HaUi *)lv_event_get_user_data(e); },
                        LV_EVENT_DELETE, u);
    Kb *k = mk_keyboard(scr, c);

    char url[HA_URL_MAX] = "", tok[HA_TOKEN_MAX], dash[HA_DASH_MAX] = "";
    int view = 0;
    ha_config_load(url, sizeof(url), tok, sizeof(tok));
    memset(tok, 0, sizeof(tok));                 // il token non si mostra e non resta in memoria
    ha_config_load_dash(dash, sizeof(dash), &view);
    char vbuf[8];
    snprintf(vbuf, sizeof(vbuf), "%d", view);

    lv_obj_t *b = mk_section(c, "Server");
    u->ta_url  = mk_ta(mk_row(b, "Indirizzo"), k, url, 520, false);
    u->ta_dash = mk_ta(mk_row(b, "Dashboard"), k, dash, 360, false);
    u->ta_view = mk_ta(mk_row(b, "Vista (numero)"), k, vbuf, 120, true);
    lv_obj_t *note = lv_label_create(b);
    lv_label_set_text(note, "Il token di accesso non viene mostrato: si cambia dalla pagina web.");
    lv_obj_set_style_text_color(note, C_TEXT2, 0);
    lv_obj_t *btns = lv_obj_create(b);
    lv_obj_remove_style_all(btns);
    lv_obj_set_size(btns, lv_pct(100), LV_SIZE_CONTENT);
    mk_btn(btns, "Salva e riavvia", net_save_ha, u);

    u->msg = lv_label_create(c);
    lv_label_set_text(u->msg, "");
    lv_obj_set_style_text_font(u->msg, &lv_font_montserrat_22, 0);
    return scr;
}

// ------------------------------------------------------------------ Debug

struct DbgUi {
    lv_obj_t *status, *dd_log, *sw_uart, *sw_perf, *sw_ha, *sw_lvps, *ta_cmd, *out, *btn_run;
    lv_timer_t *timer;
};

static char *s_cmd_out = nullptr;           // uscita del comando (PSRAM)
static volatile bool s_cmd_busy = false, s_cmd_done = false;
static char s_cmd_line[160];

static void cmd_task(void *arg)
{
    uart_console_exec(s_cmd_line, s_cmd_out, 4096);
    s_cmd_done = true;
    vTaskDelete(NULL);
}

static void run_cmd(DbgUi *u, const char *cmd)
{
    if (s_cmd_busy) return;
    if (!s_cmd_out) s_cmd_out = (char *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    if (!s_cmd_out) return;
    strlcpy(s_cmd_line, cmd, sizeof(s_cmd_line));
    s_cmd_busy = true;
    s_cmd_done = false;
    lv_textarea_set_text(u->out, "...");
    xTaskCreate(cmd_task, "dbg_cmd", 8192, nullptr, 3, nullptr);
}

static void dbg_refresh(lv_timer_t *t)
{
    DbgUi *u = (DbgUi *)t->user_data;
    if (s_cmd_done) {
        s_cmd_done = false;
        s_cmd_busy = false;
        lv_textarea_set_text(u->out, s_cmd_out[0] ? s_cmd_out : "(nessuna uscita)");
    }
    char dash[HA_DASH_MAX] = "";
    int view = 0;
    ha_config_load_dash(dash, sizeof(dash), &view);
    const unsigned up = (unsigned)(esp_timer_get_time() / 1000000);
    size_t lv_int = 0, lv_ext = 0;
    lvgl_mem_stats(&lv_int, &lv_ext);
    lv_label_set_text_fmt(u->status,
        "Acceso da: %ud %02uh %02um\n"
        "RAM interna: %u KB liberi (minimo %u KB, blocco max %u KB)\n"
        "PSRAM: %u KB liberi\n"
        "Interfaccia LVGL: %u KB in RAM interna, %u KB in PSRAM\n"
        "Coprocessore C6: %s\n"
        "Recuperi SDIO dall'avvio: %u\n"
        "Home Assistant: %s (dashboard %s/%d)\n"
        "WiFi: %s  %d dBm  IP %s",
        up / 86400, (up / 3600) % 24, (up / 60) % 60,
        (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
        (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
        (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
        (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
        (unsigned)(lv_int / 1024), (unsigned)(lv_ext / 1024),
        s_st.c6fw[0] ? s_st.c6fw : "-",
        hosted_recovery_count(),
        ha_ws_connected() ? "connesso" : "non connesso", dash, view,
        s_st.wifi_ok ? s_st.ssid : "non connesso", s_st.rssi, s_st.ip[0] ? s_st.ip : "-");
}

static void dbg_toggle(lv_event_t *e)
{
    DbgUi *u = (DbgUi *)lv_event_get_user_data(e);
    debug_config_t c = *debug_config_get();
    c.log_level    = (dbg_log_level_t)lv_dropdown_get_selected(u->dd_log);
    c.uart_console = lv_obj_has_state(u->sw_uart, LV_STATE_CHECKED);
    c.perf_monitor = lv_obj_has_state(u->sw_perf, LV_STATE_CHECKED);
    c.ha_verbose   = lv_obj_has_state(u->sw_ha, LV_STATE_CHECKED);
    c.lvgl_psram   = lv_obj_has_state(u->sw_lvps, LV_STATE_CHECKED);
    debug_config_set(&c);
}

struct QuickCmd { DbgUi *u; const char *cmd; };

static void quick_cb(lv_event_t *e)
{
    QuickCmd *q = (QuickCmd *)lv_event_get_user_data(e);
    if (!strcmp(q->cmd, "@dashboard")) {
        ha_ws_request_lovelace();
        lv_textarea_set_text(q->u->out, "Dashboard richiesta di nuovo a Home Assistant.");
        return;
    }
    run_cmd(q->u, q->cmd);
}

static void run_typed(lv_event_t *e)
{
    DbgUi *u = (DbgUi *)lv_event_get_user_data(e);
    const char *t = lv_textarea_get_text(u->ta_cmd);
    if (t[0]) run_cmd(u, t);
}

static lv_obj_t *build_debug(void)
{
    lv_obj_t *c;
    lv_obj_t *scr = mk_screen("Debug", &c);
    DbgUi *u = new DbgUi{};
    static QuickCmd quick[9];
    lv_obj_add_event_cb(scr, [](lv_event_t *e) {
        DbgUi *x = (DbgUi *)lv_event_get_user_data(e);
        lv_timer_del(x->timer);
        delete x;
    }, LV_EVENT_DELETE, u);
    Kb *k = mk_keyboard(scr, c);
    const debug_config_t *dc = debug_config_get();

    lv_obj_t *b = mk_section(c, "Stato del dispositivo");
    u->status = lv_label_create(b);
    lv_obj_set_style_text_font(u->status, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_line_space(u->status, 6, 0);

    b = mk_section(c, "Interruttori");
    lv_obj_t *r = mk_row(b, "Livello del log");
    u->dd_log = lv_dropdown_create(r);
    lv_dropdown_set_options(u->dd_log, "Nessuno\nErrori\nAvvisi\nInfo\nDebug");
    lv_dropdown_set_selected(u->dd_log, dc->log_level);
    lv_obj_set_width(u->dd_log, 220);
    lv_obj_add_event_cb(u->dd_log, dbg_toggle, LV_EVENT_VALUE_CHANGED, u);
    struct { const char *label; lv_obj_t **sw; bool on; } sws[] = {
        {"Console UART (comandi da seriale)", &u->sw_uart, dc->uart_console},
        {"Monitor prestazioni (FPS, CPU)",    &u->sw_perf, dc->perf_monitor},
        {"Log dettagliato Home Assistant",    &u->sw_ha,   dc->ha_verbose},
        {"Interfaccia in PSRAM (dal riavvio)", &u->sw_lvps, dc->lvgl_psram},
    };
    for (auto &s : sws) {
        *s.sw = lv_switch_create(mk_row(b, s.label));
        if (s.on) lv_obj_add_state(*s.sw, LV_STATE_CHECKED);
        lv_obj_add_event_cb(*s.sw, dbg_toggle, LV_EVENT_VALUE_CHANGED, u);
    }

    b = mk_section(c, "Comandi rapidi");
    lv_obj_t *grid = lv_obj_create(b);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(grid, 10, 0);
    lv_obj_set_style_pad_row(grid, 10, 0);
    static const char *const labels[][2] = {
        {"Stato", "info"}, {"Versione C6", "fw"}, {"Memoria C6", "cpmem"},
        {"Campione RAM", "ramlog ora"}, {"Aggiorna grafici", "refresh"},
        {"Ricarica dashboard", "@dashboard"}, {"Standby", "standby"}, {"Task", "tasks"},
        {"Riavvia", "reboot"},
    };
    for (int i = 0; i < 9; i++) {
        quick[i] = {u, labels[i][1]};
        mk_btn(grid, labels[i][0], quick_cb, &quick[i]);
    }

    b = mk_section(c, "Console");
    r = mk_row(b, "Comando");
    u->ta_cmd = mk_ta(r, k, "", 520, false);
    lv_textarea_set_placeholder_text(u->ta_cmd, "help");
    u->btn_run = mk_btn(b, "Esegui", run_typed, u);
    u->out = lv_textarea_create(b);
    lv_obj_set_size(u->out, lv_pct(100), 220);
    lv_obj_set_style_text_font(u->out, &lv_font_montserrat_16, 0);
    lv_textarea_set_text(u->out, "L'uscita dei comandi appare qui.");
    lv_obj_clear_flag(u->out, LV_OBJ_FLAG_CLICK_FOCUSABLE);   // sola lettura: niente tastiera

    u->timer = lv_timer_create(dbg_refresh, 1000, u);
    dbg_refresh(u->timer);
    return scr;
}

// ------------------------------------------------------------------ Sicurezza

struct SecUi {
    lv_obj_t *state, *btn_set, *btn_del, *note;
    lv_obj_t *sw[PIN_AREA_COUNT];
    lv_obj_t *web_state, *web_fp;
    char first[PIN_MAX_LEN + 1];       // primo inserimento, in attesa della conferma
};

static void sec_refresh(SecUi *u)
{
    bool on = pin_lock_is_set();
    lv_label_set_text(u->state, on ? "impostato" : "non impostato");
    lv_label_set_text(lv_obj_get_child(u->btn_set, 0), on ? "Cambia PIN" : "Imposta PIN");
    if (on) lv_obj_clear_state(u->btn_del, LV_STATE_DISABLED);
    else    lv_obj_add_state(u->btn_del, LV_STATE_DISABLED);
    for (int i = 0; i < PIN_AREA_COUNT; i++) {
        if (!u->sw[i]) continue;
        if (on) lv_obj_clear_state(u->sw[i], LV_STATE_DISABLED);
        else    lv_obj_add_state(u->sw[i], LV_STATE_DISABLED);
        if (pin_lock_required((pin_area_t)i)) lv_obj_add_state(u->sw[i], LV_STATE_CHECKED);
    }
    if (u->web_state) {
        lv_label_set_text_fmt(u->web_state, "%s%s",
            web_auth_configured() ? "password impostata" : "da creare al primo accesso",
            web_auth_guest_enabled() ? ", ospite attivo" : "");
    }
    lv_label_set_text(u->note, on
        ? "Dopo tre tentativi sbagliati il pannello aspetta mezzo minuto, poi\n"
          "sempre di piu'. Il PIN vale solo qui: la pagina web ha una sua password."
        : "Senza PIN chiunque tocchi lo schermo puo' cambiare queste impostazioni.");
}

static void sec_area_changed(lv_event_t *e)
{
    SecUi *u = (SecUi *)lv_event_get_user_data(e);
    lv_obj_t *sw = lv_event_get_target(e);
    for (int i = 0; i < PIN_AREA_COUNT; i++)
        if (u->sw[i] == sw)
            pin_lock_set_required((pin_area_t)i, lv_obj_has_state(sw, LV_STATE_CHECKED));
}

/* Terzo passo: le due cifre coincidono, salvo. */
static void sec_confirm(const char *pin, void *ctx)
{
    SecUi *u = (SecUi *)ctx;
    if (!pin) return;
    if (strcmp(pin, u->first) != 0) {
        lv_label_set_text(u->note, "I due PIN non coincidono: riprova.");
        return;
    }
    if (!pin_lock_set(pin)) {
        lv_label_set_text(u->note, "Salvataggio del PIN non riuscito.");
        return;
    }
    sec_refresh(u);
}

/* Secondo passo: chiedo la ripetizione. */
static void sec_new(const char *pin, void *ctx)
{
    SecUi *u = (SecUi *)ctx;
    if (!pin) return;
    strlcpy(u->first, pin, sizeof(u->first));
    pin_lock_ask_digits("Ripeti il PIN", "Le stesse cifre di prima", sec_confirm, u);
}

/* Primo passo: se un PIN c'e' gia', prima va dimostrato di conoscerlo. */
static void sec_set_clicked(lv_event_t *e)
{
    SecUi *u = (SecUi *)lv_event_get_user_data(e);
    auto chiedi_nuovo = [](bool ok, void *ctx) {
        if (ok) pin_lock_ask_digits("Nuovo PIN", "Da 4 a 8 cifre", sec_new, ctx);
    };
    if (pin_lock_is_set()) pin_lock_ask_verify("PIN attuale", chiedi_nuovo, u);
    else                   chiedi_nuovo(true, u);
}

static void sec_del_clicked(lv_event_t *e)
{
    SecUi *u = (SecUi *)lv_event_get_user_data(e);
    pin_lock_ask_verify("PIN attuale", [](bool ok, void *ctx) {
        SecUi *x = (SecUi *)ctx;
        if (!ok) return;
        pin_lock_set(NULL);
        sec_refresh(x);
    }, u);
}

static void sec_web_reset(lv_event_t *e)
{
    SecUi *u = (SecUi *)lv_event_get_user_data(e);
    /* Chiede il PIN se c'e': chi ha il pannello in mano puo' riprendersi
       l'accesso alla pagina, chi e' solo sulla rete no. */
    pin_lock_ask_verify("PIN attuale", [](bool ok, void *ctx) {
        SecUi *x = (SecUi *)ctx;
        if (!ok) return;
        web_auth_reset();
        sec_refresh(x);
        lv_label_set_text(x->note,
            "Accesso web azzerato: alla prossima apertura la pagina chiede di\n"
            "creare una password nuova.");
    }, u);
}

/* Ritorno alle impostazioni iniziali. Due cancelli prima di cancellare: il
   PIN (se c'e') e una finestra che dice per esteso cosa sparisce. Non e'
   un'operazione da confermare per sbaglio sfiorando lo schermo. */
static void reset_do(lv_event_t *e)
{
    lv_obj_t *overlay = (lv_obj_t *)lv_event_get_user_data(e);
    lv_obj_clean(overlay);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_t *l = lv_label_create(overlay);
    lv_label_set_text(l, "Sto cancellando tutto.\n\nIl pannello si riavvia da solo.");
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(l);
    factory_reset_start();
}

static void reset_cancel(lv_event_t *e)
{
    lv_obj_del((lv_obj_t *)lv_event_get_user_data(e));
}

static void reset_ask(void *ctx)
{
    (void)ctx;
    lv_obj_t *overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_50, 0);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(overlay);
    lv_obj_set_size(card, 700, 420);
    lv_obj_center(card);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 20, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(card, 12, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(card);
    lv_label_set_text(t, "Torno alle impostazioni iniziali?");
    lv_obj_set_style_text_font(t, &lv_font_montserrat_24, 0);

    lv_obj_t *d = lv_label_create(card);
    lv_obj_set_width(d, lv_pct(100));
    lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);
    lv_label_set_text(d,
        "Il pannello dimentica il Wi-Fi, Home Assistant e l'abbinamento, il nome "
        "e la stanza, il PIN, la password della pagina web e i ritocchi della "
        "dashboard. Il certificato viene rifatto, quindi il browser avvisera' di "
        "nuovo.\n\n"
        "Al riavvio riparte la configurazione guidata, come alla prima accensione. "
        "Non si torna indietro.");
    lv_obj_set_style_text_color(d, C_TEXT2, 0);

    lv_obj_t *btns = lv_obj_create(card);
    lv_obj_remove_style_all(btns);
    lv_obj_set_size(btns, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btns, 12, 0);
    mk_btn(btns, "Annulla", reset_cancel, overlay);
    lv_obj_t *go = mk_btn(btns, "Cancella tutto", reset_do, overlay);
    lv_obj_set_style_bg_color(go, lv_color_hex(0xc62828), 0);
}

static void sec_factory_reset(lv_event_t *e)
{
    (void)e;
    if (pin_lock_is_set())
        pin_lock_ask_verify("PIN attuale", [](bool ok, void *ctx) { if (ok) reset_ask(ctx); }, nullptr);
    else
        reset_ask(nullptr);
}

static lv_obj_t *build_security(void)
{
    lv_obj_t *c;
    lv_obj_t *scr = mk_screen("Sicurezza", &c);
    SecUi *u = new SecUi{};
    lv_obj_add_event_cb(scr, [](lv_event_t *e) { delete (SecUi *)lv_event_get_user_data(e); },
                        LV_EVENT_DELETE, u);

    lv_obj_t *b = mk_section(c, "Codice PIN dello schermo");
    u->state = mk_value(mk_row(b, "Stato"));
    lv_obj_t *btns = lv_obj_create(b);
    lv_obj_remove_style_all(btns);
    lv_obj_set_size(btns, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btns, 10, 0);
    u->btn_set = mk_btn(btns, "Imposta PIN", sec_set_clicked, u);
    u->btn_del = mk_btn(btns, "Rimuovi PIN", sec_del_clicked, u);

    b = mk_section(c, "Chiedi il PIN per");
    for (int i = 0; i < PIN_AREA_COUNT; i++) {
        lv_obj_t *r = mk_row(b, pin_lock_area_name((pin_area_t)i));
        u->sw[i] = lv_switch_create(r);
        lv_obj_add_event_cb(u->sw[i], sec_area_changed, LV_EVENT_VALUE_CHANGED, u);
    }

    b = mk_section(c, "Pagina web del pannello");
    u->web_state = mk_value(mk_row(b, "Accesso"));
    lv_obj_t *fp_lab = lv_label_create(b);
    lv_label_set_text(fp_lab, "Impronta del certificato");
    lv_obj_set_style_text_font(fp_lab, &lv_font_montserrat_22, 0);
    u->web_fp = lv_label_create(b);
    lv_obj_set_width(u->web_fp, lv_pct(100));
    lv_label_set_long_mode(u->web_fp, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(u->web_fp, C_TEXT2, 0);
    lv_obj_set_style_text_font(u->web_fp, &lv_font_montserrat_16, 0);
    {
        char fp[160];
        web_cert_fingerprint(fp, sizeof(fp));
        lv_label_set_text(u->web_fp, fp[0] ? fp : "non ancora generato");
    }
    lv_obj_t *fp_note = lv_label_create(b);
    lv_obj_set_width(fp_note, lv_pct(100));
    lv_label_set_long_mode(fp_note, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(fp_note, C_TEXT2, 0);
    lv_label_set_text(fp_note,
        "Il browser avvisa che il certificato non e' firmato da nessuno: e' normale, "
        "se lo fa il pannello. Se l'impronta mostrata dal browser e' questa, stai "
        "parlando col pannello e non con qualcun altro.");
    lv_obj_t *wbtns = lv_obj_create(b);
    lv_obj_remove_style_all(wbtns);
    lv_obj_set_size(wbtns, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(wbtns, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(wbtns, 10, 0);
    mk_btn(wbtns, "Azzera accesso web", sec_web_reset, u);
    mk_btn(wbtns, "Rifai la configurazione guidata", [](lv_event_t *e) {
        (void)e;
        setup_wizard_reset();
        setup_wizard_start();
    }, nullptr);

    b = mk_section(c, "Impostazioni iniziali");
    lv_obj_t *fr_note = lv_label_create(b);
    lv_obj_set_width(fr_note, lv_pct(100));
    lv_label_set_long_mode(fr_note, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(fr_note, C_TEXT2, 0);
    lv_label_set_text(fr_note,
        "Cancella tutto quello che il pannello ricorda e lo riporta a com'era "
        "appena tolto dalla scatola. Da usare prima di regalarlo o rivenderlo, "
        "oppure per ripartire da zero. La configurazione guidata di sopra, "
        "invece, rifa' solo i passi senza dimenticare niente.");
    lv_obj_t *fr_btn = mk_btn(b, "Cancella tutto e riparti", sec_factory_reset, nullptr);
    lv_obj_set_style_bg_color(fr_btn, lv_color_hex(0xc62828), 0);

    u->note = lv_label_create(c);
    lv_obj_set_width(u->note, lv_pct(100));
    lv_label_set_long_mode(u->note, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(u->note, C_TEXT2, 0);
    sec_refresh(u);
    return scr;
}

// ------------------------------------------------------------- nome dispositivo

/* Il nome del pannello e' uno solo: quello di net_config (hostname sulla rete,
   riga "Device Name" nelle informazioni). Si cambia da qui, con una finestra
   sopra alla schermata, per non avere due campi che scrivono lo stesso valore
   e si sovrascrivono a vicenda. */
static lv_obj_t *s_name_label = nullptr;

static void name_apply_task(void *arg)
{
    net_config_apply_now();
    vTaskDelete(NULL);
}

struct NameDlg { lv_obj_t *overlay, *ta, *msg; };

static void name_save(lv_event_t *e)
{
    NameDlg *d = (NameDlg *)lv_event_get_user_data(e);
    net_config_t c;
    net_config_load(&c);
    strlcpy(c.hostname, lv_textarea_get_text(d->ta), sizeof(c.hostname));
    char err[96];
    if (!net_config_validate(&c, err, sizeof(err))) { set_msg(d->msg, false, err); return; }
    if (!net_config_save(&c))                       { set_msg(d->msg, false, "Salvataggio non riuscito"); return; }
    if (s_name_label) lv_label_set_text(s_name_label, c.hostname);
    lv_obj_del(d->overlay);
    delete d;
    // il nome nuovo si presenta alla rete alla prossima connessione
    xTaskCreate(name_apply_task, "net_apply", 4096, nullptr, 3, nullptr);
}

static void name_cancel(lv_event_t *e)
{
    NameDlg *d = (NameDlg *)lv_event_get_user_data(e);
    lv_obj_del(d->overlay);
    delete d;
}

static void name_dialog(lv_event_t *e)
{
    (void)e;
    NameDlg *d = new NameDlg{};
    d->overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(d->overlay);
    lv_obj_set_size(d->overlay, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(d->overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(d->overlay, LV_OPA_50, 0);
    lv_obj_add_flag(d->overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(d->overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(d->overlay);
    lv_obj_set_size(card, 640, 300);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 18, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(card, 10, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(card);
    lv_label_set_text(t, "Nome del dispositivo");
    lv_obj_set_style_text_font(t, &lv_font_montserrat_24, 0);

    net_config_t c;
    net_config_load(&c);
    d->ta = lv_textarea_create(card);
    lv_textarea_set_one_line(d->ta, true);
    lv_textarea_set_max_length(d->ta, sizeof(c.hostname) - 1);
    lv_textarea_set_text(d->ta, c.hostname);
    lv_obj_set_width(d->ta, 560);
    lv_obj_set_style_text_font(d->ta, &lv_font_montserrat_22, 0);

    lv_obj_t *hint = lv_label_create(card);
    lv_label_set_text(hint, "Con questo nome il pannello si presenta alla rete e\n"
                            "compare nelle informazioni. Lettere, numeri e trattino.");
    lv_obj_set_style_text_color(hint, C_TEXT2, 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);

    d->msg = lv_label_create(card);
    lv_label_set_text(d->msg, "");

    lv_obj_t *btns = lv_obj_create(card);
    lv_obj_remove_style_all(btns);
    lv_obj_set_size(btns, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btns, 12, 0);
    mk_btn(btns, "Annulla", name_cancel, d);
    mk_btn(btns, "Salva", name_save, d);

    lv_obj_t *kb = lv_keyboard_create(d->overlay);
    lv_obj_set_size(kb, LV_HOR_RES, KB_H);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kb, d->ta);
}

/* Aggancia la riga "Device Name" della schermata Informazioni di Elecrow:
   mostra il nome vero e, toccandola, apre la finestra per cambiarlo. */
static void bind_about_name(void)
{
    if (!ui_LabelPanelPanelScreenSettingAbout2) return;
    net_config_t c;
    net_config_load(&c);
    s_name_label = ui_LabelPanelPanelScreenSettingAbout2;
    lv_label_set_text(s_name_label, c.hostname[0] ? c.hostname : "pannello");

    lv_obj_t *row = lv_obj_get_parent(s_name_label);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, name_dialog, LV_EVENT_CLICKED, nullptr);
}

// ------------------------------------------------------------------ voci del menu

/* Voce di menu: prima il PIN (se quell'area e' protetta), poi la schermata. */
struct MenuTarget { lv_obj_t *screen; pin_area_t area; };
static MenuTarget s_targets[8];
static int s_ntargets = 0;

static void load_screen_cb(bool ok, void *ctx)
{
    if (ok) lv_scr_load((lv_obj_t *)ctx);
}

static void open_screen(lv_event_t *e)
{
    MenuTarget *t = (MenuTarget *)lv_event_get_user_data(e);
    pin_lock_guard(t->area, load_screen_cb, t->screen);
}

/* La voce WiFi e' di Elecrow: le tolgo il suo gestore e ci metto il mio, che
   chiede il PIN prima di aprire la stessa schermata. */
static void wifi_item_guard(lv_event_t *e)
{
    (void)e;
    pin_lock_guard(PIN_AREA_WIFI, load_screen_cb, ui_ScreenSettingWiFi);
}

static void mk_menu_item(lv_obj_t *container, const char *symbol, const char *text,
                         lv_obj_t *screen, pin_area_t area)
{
    lv_obj_t *it = lv_obj_create(container);
    lv_obj_set_size(it, lv_pct(100), 70);
    lv_obj_clear_flag(it, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(it, C_ROW, 0);
    lv_obj_set_style_border_color(it, C_ROW, 0);
    lv_obj_set_style_bg_color(it, C_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(it, LV_OPA_COVER, LV_STATE_PRESSED);

    lv_obj_t *ic = lv_label_create(it);
    lv_label_set_text(ic, symbol);
    lv_obj_set_style_text_font(ic, &lv_font_montserrat_30, 0);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 6, 0);

    lv_obj_t *l = lv_label_create(it);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_30, 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 62, 2);

    lv_obj_t *ar = lv_img_create(it);
    lv_img_set_src(ar, &ui_img_arrow_png);
    lv_obj_align(ar, LV_ALIGN_RIGHT_MID, 0, 0);

    if (s_ntargets < (int)(sizeof(s_targets) / sizeof(s_targets[0]))) {
        MenuTarget *t = &s_targets[s_ntargets++];
        t->screen = screen;
        t->area = area;
        lv_obj_add_event_cb(it, open_screen, LV_EVENT_CLICKED, t);
    }
}

static lv_obj_t *s_scr[SETTINGS_EXTRA_COUNT];

static void forget_screen(lv_event_t *e)
{
    lv_obj_t *t = lv_event_get_target(e);
    for (int i = 0; i < SETTINGS_EXTRA_COUNT; i++)
        if (s_scr[i] == t) s_scr[i] = nullptr;
}

bool settings_extra_show(int which)
{
    /* L'ultimo numero apre le informazioni di Elecrow: serve per provarle dal
       PC senza toccare lo schermo. */
    if (which == SETTINGS_EXTRA_COUNT && ui_ScreenSettingAbout) {
        lv_scr_load(ui_ScreenSettingAbout);
        return true;
    }
    if (which < 0 || which >= SETTINGS_EXTRA_COUNT || !s_scr[which]) return false;
    lv_scr_load(s_scr[which]);
    return true;
}

void settings_extra_build(lv_obj_t *main_container, lv_obj_t **screens)
{
    s_ntargets = 0;
    screens[SETTINGS_EXTRA_NET]   = s_scr[SETTINGS_EXTRA_NET]   = build_network();
    screens[SETTINGS_EXTRA_HA]    = s_scr[SETTINGS_EXTRA_HA]    = build_ha();
    screens[SETTINGS_EXTRA_DEBUG] = s_scr[SETTINGS_EXTRA_DEBUG] = build_debug();
    screens[SETTINGS_EXTRA_SEC]   = s_scr[SETTINGS_EXTRA_SEC]   = build_security();
    for (int i = 0; i < SETTINGS_EXTRA_COUNT; i++)
        lv_obj_add_event_cb(s_scr[i], forget_screen, LV_EVENT_DELETE, nullptr);

    mk_menu_item(main_container, LV_SYMBOL_WIFI, "Rete",
                 s_scr[SETTINGS_EXTRA_NET], PIN_AREA_NETWORK);
    mk_menu_item(main_container, LV_SYMBOL_HOME, "Home Assistant",
                 s_scr[SETTINGS_EXTRA_HA], PIN_AREA_HA);
    mk_menu_item(main_container, LV_SYMBOL_SETTINGS, "Debug",
                 s_scr[SETTINGS_EXTRA_DEBUG], PIN_AREA_DEBUG);
    mk_menu_item(main_container, LV_SYMBOL_KEYBOARD, "Sicurezza",
                 s_scr[SETTINGS_EXTRA_SEC], PIN_AREA_SECURITY);

    /* "About Device" era in mezzo: va in fondo, dopo le nostre voci. */
    lv_obj_move_to_index(ui_PanelSettingMainContainerItem5, -1);
    bind_about_name();

    /* La voce WiFi di Elecrow passa anche lei dal PIN. */
    lv_obj_remove_event_cb(ui_PanelSettingMainContainerItem1, ui_event_PanelSettingMainContainerItem1);
    lv_obj_add_event_cb(ui_PanelSettingMainContainerItem1, wifi_item_guard, LV_EVENT_CLICKED, nullptr);
    /* Con due voci in piu' il menu non entra piu' nell'altezza originale. */
    lv_obj_set_height(main_container, LV_VER_RES - 98 - 12);
    lv_obj_add_flag(main_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(main_container, LV_DIR_VER);
    ESP_LOGI(TAG, "sezioni Rete, Home Assistant, Debug e Sicurezza pronte");
}
