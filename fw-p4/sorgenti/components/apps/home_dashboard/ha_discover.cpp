#include "ha_discover.h"
#include "ha_ws.h"
#include "net_config.h"

#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "mdns.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"

static const char *TAG = "ha_disc";

static bool s_pronto = false;

extern "C" void home_dashboard_reconnect(void);

// ------------------------------------------------------------------ utilita'

/* "http://192.168.1.167" -> "ws://192.168.1.167/api/websocket"

   L'indirizzo che Home Assistant annuncia e' quello del browser: per il
   WebSocket cambia lo schema e si aggiunge la coda. E' l'unica cosa che
   l'utente doveva scrivere a mano, ed e' anche quella che sbagliava piu'
   spesso. */
static bool url_ws(const char *base, char *out, size_t sz)
{
    if (!base || !*base) return false;
    const char *schema, *resto;
    if (!strncmp(base, "https://", 8))     { schema = "wss://"; resto = base + 8; }
    else if (!strncmp(base, "http://", 7)) { schema = "ws://";  resto = base + 7; }
    else return false;

    char host[96];
    strlcpy(host, resto, sizeof(host));
    size_t n = strlen(host);
    while (n && host[n - 1] == '/') host[--n] = 0;
    if (!n) return false;
    snprintf(out, sz, "%s%s/api/websocket", schema, host);
    return true;
}

void ha_discover_host(const char *url, char *out, size_t sz)
{
    if (!out || !sz) return;
    out[0] = 0;
    if (!url || !*url) return;
    const char *p = url;
    if (!strncmp(p, "wss://", 6))     p += 6;
    else if (!strncmp(p, "ws://", 5)) p += 5;
    size_t j = 0;
    while (*p && *p != '/' && j + 1 < sz) out[j++] = *p++;
    out[j] = 0;
}

static const char *txt_get(const mdns_result_t *r, const char *chiave)
{
    for (size_t i = 0; i < r->txt_count; i++)
        if (r->txt[i].key && !strcmp(r->txt[i].key, chiave)) return r->txt[i].value;
    return NULL;
}

/* Il nome pubblicato deve essere un'etichetta di rete: lettere, cifre e
   trattini. "Pannello Cucina" diventa "pannello-cucina". */
static void nome_rete(const char *in, char *out, size_t sz)
{
    size_t j = 0;
    for (const char *p = in; *p && j + 1 < sz; p++) {
        char c = (char)tolower((unsigned char)*p);
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out[j++] = c;
        else if (j && out[j - 1] != '-')                      out[j++] = '-';
    }
    while (j && out[j - 1] == '-') j--;
    out[j] = 0;
    if (!j) strlcpy(out, "pannello-ha", sz);
}

// ------------------------------------------------------------------ avvio

void ha_discover_start(void)
{
    if (s_pronto) return;
    if (mdns_init() != ESP_OK) {
        ESP_LOGW(TAG, "mDNS non parte: il pannello si fara' scrivere l'indirizzo");
        return;
    }
    s_pronto = true;

    /* Gia' che il mDNS e' acceso, il pannello si annuncia a sua volta: la
       sua pagina web si apre con <nome>.local, senza dover andare a leggere
       l'indirizzo IP nelle Impostazioni. */
    net_config_t n;
    net_config_load(&n);
    char host[32];
    nome_rete(n.hostname, host, sizeof(host));
    mdns_hostname_set(host);
    mdns_instance_name_set(n.hostname[0] ? n.hostname : host);
    mdns_service_add(NULL, "_https", "_tcp", 443, NULL, 0);
    mdns_service_add(NULL, "_http",  "_tcp", 80,  NULL, 0);
    ESP_LOGI(TAG, "mDNS attivo, il pannello risponde a %s.local", host);
}

// ------------------------------------------------------------------ ricerca

int ha_discover_scan(ha_found_t *out, int max, int attesa_ms)
{
    if (!s_pronto || !out || max <= 0) return 0;

    mdns_result_t *res = NULL;
    esp_err_t err = mdns_query_ptr("_home-assistant", "_tcp", attesa_ms, max, &res);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ricerca fallita: %s", esp_err_to_name(err));
        return 0;
    }

    int n = 0;
    for (mdns_result_t *r = res; r && n < max; r = r->next) {
        ha_found_t &f = out[n];
        memset(&f, 0, sizeof(f));

        const char *nome = txt_get(r, "location_name");
        strlcpy(f.nome, nome && *nome ? nome
                      : (r->instance_name ? r->instance_name : "Home Assistant"),
                sizeof(f.nome));
        const char *u = txt_get(r, "uuid");
        if (u) strlcpy(f.uuid, u, sizeof(f.uuid));
        const char *v = txt_get(r, "version");
        if (v) strlcpy(f.versione, v, sizeof(f.versione));

        /* L'indirizzo dell'annuncio serve comunque, anche quando poi useremo
           quello dichiarato da HA: e' l'unico modo di sapere da che parte
           della rete arriva la risposta. */
        esp_ip4_addr_t ip4 = {};
        for (mdns_ip_addr_t *a = r->addr; a; a = a->next) {
            if (a->addr.type != ESP_IPADDR_TYPE_V4) continue;
            ip4 = a->addr.u_addr.ip4;
            break;
        }
        f.ip4 = ip4.addr;

        /* Prima l'indirizzo che HA dichiara per l'uso in casa: sa lui se sta
           dietro un reverse proxy, su che porta e se in https. Solo se non
           lo dice ripieghiamo sull'indirizzo e sulla porta dell'annuncio. */
        if (!url_ws(txt_get(r, "internal_url"), f.url, sizeof(f.url)) &&
            !url_ws(txt_get(r, "base_url"), f.url, sizeof(f.url)) && ip4.addr) {
            snprintf(f.url, sizeof(f.url), "ws://" IPSTR ":%u/api/websocket",
                     IP2STR(&ip4), (unsigned)r->port);
        }
        if (!f.url[0]) continue;              // senza indirizzo non ce ne facciamo niente
        ESP_LOGI(TAG, "trovato '%s' (%s) %s", f.nome, f.versione, f.url);
        n++;
    }
    mdns_query_results_free(res);
    return n;
}

// ------------------------------------------------- se HA cambia indirizzo

/* Un indirizzo IP scritto in memoria invecchia: basta che il router ne
   assegni un altro a Home Assistant e il pannello resta muto finche'
   qualcuno non lo corregge a mano. Se pero' HA continua ad annunciarsi, il
   pannello puo' accorgersene da solo.

   Accorgersene, e chiedere. Non spostarsi. L'annuncio mDNS e' una frase
   detta ad alta voce sulla rete: non la firma nessuno e chiunque puo'
   ripeterla. Nemmeno l'uuid dell'istanza e' un segreto, lo legge chiunque
   sia collegato. Quindi uno che sia sulla rete di casa - il portatile
   dell'ospite, la telecamera comprata a poco, la lampadina con il firmware
   di qualcun altro - puo' aspettare che il pannello resti scollegato e poi
   dire "l'Home Assistant che cerchi sono io". Il pannello ci andrebbe, e
   alla prima richiesta di autenticarsi gli consegnerebbe il token: cioe' il
   comando di tutta la casa, luci serrature e allarme compresi.

   Non esiste un controllo da aggiungere qui che chiuda il buco, perche' il
   difetto e' del mezzo: mDNS non sa dire chi parla. L'unica cosa che
   distingue davvero e' qualcuno che guarda lo schermo e riconosce
   l'indirizzo. Percio' l'indirizzo nuovo si mostra e si aspetta. Se non c'e'
   nessuno in casa il pannello resta scollegato, che e' il modo giusto di
   sbagliare. */
#define GIRO_S      30
#define MUTO_PRIMA_S 120

static lv_obj_t *s_dlg = NULL;             // la domanda sullo schermo, se aperta
static char s_cand[HA_URL_MAX] = "";       // indirizzo proposto, in attesa di risposta
static char s_chiesto[HA_URL_MAX] = "";    // gia' chiesto: non si insiste

/* Sulla nostra stessa rete? Non e' una prova di identita' - chi e' dentro la
   rete resta dentro - ma separa il caso normale (il router ha cambiato idea
   sull'indirizzo di HA) da quello che merita un occhio in piu'. */
static bool stessa_rete(uint32_t ip4)
{
    if (!ip4) return true;                 // non lo sappiamo: non allarmo a vuoto
    esp_netif_ip_info_t ip = {};
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!nif || esp_netif_get_ip_info(nif, &ip) != ESP_OK || !ip.ip.addr) return true;
    return (ip4 & ip.netmask.addr) == (ip.ip.addr & ip.netmask.addr);
}

/* Cancellare un oggetto da dentro l'evento di un suo figlio vuol dire
   liberare il pulsante mentre LVGL ci sta ancora lavorando sopra, e il
   pannello se ne va in "block already marked as free". lv_async_call fa la
   stessa cosa un attimo dopo, a evento finito. */
static void dlg_chiudi_poi(void *ud)
{
    (void)ud;
    if (!s_dlg) return;
    lv_obj_del(s_dlg);
    s_dlg = NULL;
}

static void dlg_si_poi(void *ud)
{
    (void)ud;
    if (s_dlg) { lv_obj_del(s_dlg); s_dlg = NULL; }
    if (!s_cand[0]) return;
    ESP_LOGW(TAG, "spostamento confermato dallo schermo: %s", s_cand);
    if (ha_config_save_url(s_cand)) home_dashboard_reconnect();
}

static void dlg_si(lv_event_t *e) { (void)e; lv_async_call(dlg_si_poi, NULL); }

static void dlg_no(lv_event_t *e)
{
    (void)e;
    ESP_LOGW(TAG, "spostamento rifiutato dallo schermo: %s", s_cand);
    lv_async_call(dlg_chiudi_poi, NULL);
}

static void riga_testo(lv_obj_t *parent, const char *txt,
                       const lv_font_t *font, uint32_t col)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
}

static void chiedi_conferma(const char *vecchio, const ha_found_t *f)
{
    char da[80], a[80];
    ha_discover_host(vecchio, da, sizeof(da));
    ha_discover_host(f->url, a, sizeof(a));
    const bool vicino = stessa_rete(f->ip4);

    bsp_display_lock(0);
    if (s_dlg) { bsp_display_unlock(); return; }

    strlcpy(s_cand, f->url, sizeof(s_cand));
    strlcpy(s_chiesto, f->url, sizeof(s_chiesto));

    /* Sfondo scuro a tutto schermo: un lv_obj e' cliccabile di suo, quindi si
       prende i tocchi e nessuno preme per sbaglio quello che c'e' sotto. */
    s_dlg = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_dlg);
    lv_obj_set_size(s_dlg, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_dlg, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_dlg, LV_OPA_70, 0);
    lv_obj_clear_flag(s_dlg, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *c = lv_obj_create(s_dlg);
    lv_obj_set_size(c, 660, LV_SIZE_CONTENT);
    lv_obj_center(c);
    lv_obj_set_style_bg_color(c, lv_color_hex(0x1b2030), 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_radius(c, 14, 0);
    lv_obj_set_style_pad_all(c, 22, 0);
    lv_obj_set_style_pad_row(c, 12, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);

    riga_testo(c, "Home Assistant si e' spostato?", &lv_font_montserrat_26, 0xf0f2f5);

    char testo[320];
    snprintf(testo, sizeof(testo),
             "Non risponde piu' su %s.\n"
             "Adesso sulla rete c'e' un Home Assistant che dice di essere lo "
             "stesso, su %s.",
             da[0] ? da : "(indirizzo sconosciuto)", a);
    riga_testo(c, testo, &lv_font_montserrat_20, 0xf0f2f5);

    if (!vicino) {
        riga_testo(c, LV_SYMBOL_WARNING "  Si trova su una rete diversa da quella "
                      "del pannello.", &lv_font_montserrat_20, 0xe8a33d);
    }

    riga_testo(c, "Conferma solo se quell'indirizzo lo riconosci. Chiunque sia "
                  "collegato alla rete puo' farsi passare per Home Assistant, e "
                  "al pannello basta un collegamento per consegnargli le chiavi "
                  "di casa.", &lv_font_montserrat_18, 0x9aa3ad);

    lv_obj_t *riga = lv_obj_create(c);
    lv_obj_remove_style_all(riga);
    lv_obj_set_size(riga, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(riga, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(riga, 12, 0);
    lv_obj_clear_flag(riga, LV_OBJ_FLAG_SCROLLABLE);

    struct Tasto { const char *testo; lv_event_cb_t cb; uint32_t colore; };
    static const Tasto TASTI[] = {
        { "No, lascia stare",       dlg_no, 0x2c3242 },
        { "Vai a questo indirizzo", dlg_si, 0x0b7dda },
    };
    for (const Tasto &t : TASTI) {
        lv_obj_t *b = lv_btn_create(riga);
        lv_obj_set_height(b, 56);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_style_bg_color(b, lv_color_hex(t.colore), 0);
        lv_obj_set_style_radius(b, 10, 0);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, t.testo);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
        lv_obj_center(l);
        lv_obj_add_event_cb(b, t.cb, LV_EVENT_CLICKED, NULL);
    }

    bsp_display_unlock();
    ESP_LOGW(TAG, "Home Assistant forse spostato: %s -> %s, chiedo sullo schermo",
             vecchio, f->url);
}

static void guard_task(void *arg)
{
    (void)arg;
    int64_t muto_da = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(GIRO_S * 1000));
        int64_t adesso = esp_timer_get_time() / 1000000;

        if (ha_ws_connected()) {
            muto_da = 0;
            /* E' tornato da solo sul vecchio indirizzo: la domanda sullo
               schermo non ha piu' senso, e lasciarla li' vorrebbe dire far
               confermare a freddo uno spostamento che non serve piu'. */
            if (s_dlg) {
                bsp_display_lock(0);
                dlg_chiudi_poi(NULL);
                bsp_display_unlock();
                s_chiesto[0] = 0;
            }
            continue;
        }
        if (s_dlg) continue;                     // sto gia' aspettando una risposta
        if (!muto_da) { muto_da = adesso; continue; }
        if (adesso - muto_da < MUTO_PRIMA_S) continue;

        char uuid[40] = "", url[HA_URL_MAX] = "";
        if (!ha_config_load_uuid(uuid, sizeof(uuid)) || !uuid[0]) continue;
        ha_config_load_url(url, sizeof(url));

        ha_found_t trovati[HA_FOUND_MAX];
        int n = ha_discover_scan(trovati, HA_FOUND_MAX, 2000);
        for (int i = 0; i < n; i++) {
            if (strcmp(trovati[i].uuid, uuid)) continue;
            if (!strcmp(trovati[i].url, url)) break;   // e' li' dov'era: il guasto e' altrove
            if (!strcmp(trovati[i].url, s_chiesto)) break;  // gia' chiesto, non insisto
            chiedi_conferma(url, &trovati[i]);
            break;
        }
    }
}

void ha_discover_guard_start(void)
{
    static bool avviato = false;
    if (avviato || !s_pronto) return;
    avviato = true;
    xTaskCreate(guard_task, "ha_disc", 4096, NULL, 3, NULL);
}
