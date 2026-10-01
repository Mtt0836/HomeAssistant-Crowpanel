#include "ha_ws.h"
#include "ha_token.h"
#include <string.h>
#include <stdio.h>
#include <string>
#include <vector>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "esp_websocket_client.h"
#include "arp_pin.h"
#include "debug_config.h"

static const char *TAG = "ha_ws";

static esp_websocket_client_handle_t s_client = NULL;
static char           s_url[160];
static char           s_token[400];
static char           s_dash[64];
static ha_ws_callbacks_t s_cb = {};

static volatile bool  s_connected     = false;
static volatile bool  s_authenticated = false;
static volatile int64_t s_last_ok_us  = 0;      // ultimo momento "vivo"

/* HA vuole id strettamente crescenti sulla connessione. Assegnazione e invio
   stanno sotto lo stesso mutex: il ping del watchdog parte da un altro task e,
   se superasse un messaggio con id piu' basso, HA rifiuterebbe quest'ultimo. */
static SemaphoreHandle_t s_send_mtx = NULL;
static int  s_next_id          = 1;
static int  s_lovelace_req_id  = -1;
static int  s_entities_sub_id  = -1;
static int  s_tts_sub_id       = -1;
static int  s_ll_upd_sub_id    = -1;

/* Sottoscrizioni verso HA chieste da altri moduli. Due specie:

   - gli eventi del bus ("subscribe_events"), che usa l'integrazione;
   - i comandi che restano aperti e continuano a mandare aggiornamenti, come
     "weather/subscribe_forecast": le previsioni del tempo non stanno negli
     attributi dell'entita', HA le manda solo a chi si iscrive.

   Stanno qui insieme perche' hanno lo stesso problema: vanno rifatte a ogni
   riconnessione. Chi le chiede se ne dimentica e basta. */
#define MAX_EVENT_SUBS 12
struct EventSub {
    std::string body;      // il comando da rimandare a ogni riconnessione
    ha_event_cb_t cb;
    void *ctx;
    int sub_id;            // id del messaggio: gli aggiornamenti arrivano con questo
    int handle;            // nome stabile per chi ha chiesto l'iscrizione
    bool bus;              // evento del bus: passo "data", non tutto l'evento
    bool viva;
};
static EventSub s_evsub[MAX_EVENT_SUBS];
static int      s_next_handle = 1;

#define MAX_READY_CBS 2
static ha_ready_cb_t s_ready_cb[MAX_READY_CBS];
static int           s_n_ready = 0;

/* Richieste generiche in attesa di risposta. */
struct Pending { int id; ha_result_cb_t cb; void *ctx; };
#define MAX_PENDING 8
static Pending s_pending[MAX_PENDING];
static SemaphoreHandle_t s_pend_mtx = NULL;

static SemaphoreHandle_t s_follow_mtx = NULL;
/* Fermare e riavviare il client lo fanno in due: il watchdog qui sotto quando
   la connessione cade, e ha_ws_restart quando cambia l'indirizzo o la
   dashboard. Se capitano insieme si liberano a vicenda le stringhe dentro il
   client (esp_websocket_client_set_uri le rifa' da capo) e il pannello va in
   "block already marked as free". Qui si passa uno alla volta. */
static SemaphoreHandle_t s_life_mtx = NULL;
static std::vector<std::string> s_follow;         // entita' seguite

/* Il messaggio in arrivo si ricompone qui, in PSRAM: la configurazione di una
   dashboard puo' pesare decine di KB e la RAM interna serve a SDIO e WiFi. */
static char  *s_rx     = NULL;
static size_t s_rx_len = 0;
static size_t s_rx_cap = 0;

static void *psram_malloc(size_t sz)
{
    void *p = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(sz);
}

static bool rx_append(const char *data, size_t len)
{
    if (s_rx_len + len + 1 > s_rx_cap) {
        size_t cap = s_rx_cap ? s_rx_cap : 8192;
        while (cap < s_rx_len + len + 1) cap *= 2;
        if (cap > 512 * 1024) return false;       // messaggio assurdo: scarto
        char *n = (char *)heap_caps_realloc(s_rx, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!n) return false;
        s_rx = n; s_rx_cap = cap;
    }
    memcpy(s_rx + s_rx_len, data, len);
    s_rx_len += len;
    s_rx[s_rx_len] = 0;
    return true;
}

// ---- invio ----
static void ws_send_raw(const char *json)
{
    if (s_client && esp_websocket_client_is_connected(s_client))
        esp_websocket_client_send_text(s_client, json, strlen(json), pdMS_TO_TICKS(5000));
}

/* body: campi del messaggio senza graffe e senza "id", es. "\"type\":\"ping\"".
   Ritorna l'id usato (-1 se non connesso). */
static int ws_send_cmd(const std::string &body)
{
    if (!s_client || !esp_websocket_client_is_connected(s_client)) return -1;
    xSemaphoreTakeRecursive(s_send_mtx, portMAX_DELAY);
    int id = s_next_id++;
    std::string msg = "{\"id\":" + std::to_string(id) + "," + body + "}";
    /* L'autenticazione (col token) non passa da qui: send_auth usa ws_send_raw. */
    if (debug_config_get()->ha_verbose) ESP_LOGI(TAG, "TX %.300s", msg.c_str());
    ws_send_raw(msg.c_str());
    xSemaphoreGiveRecursive(s_send_mtx);
    return id;
}

static void send_auth(void)
{
    size_t n = strlen(s_token) + 48;
    char *msg = (char *)malloc(n);
    if (!msg) { ESP_LOGE(TAG, "memoria finita: autenticazione saltata"); return; }
    snprintf(msg, n, "{\"type\":\"auth\",\"access_token\":\"%s\"}", s_token);
    ws_send_raw(msg);
    /* Il token non deve restare in giro nell'heap dopo l'uso: il prossimo che
       si prende questo blocco se lo ritroverebbe dentro. */
    memset(msg, 0, n);
    free(msg);
}

void ha_ws_request_lovelace(void)
{
    if (!s_authenticated) return;
    std::string body = "\"type\":\"lovelace/config\",\"force\":false";
    if (s_dash[0] && strcmp(s_dash, "lovelace") != 0)
        body += std::string(",\"url_path\":\"") + s_dash + "\"";
    s_lovelace_req_id = ws_send_cmd(body);
    ESP_LOGI(TAG, "richiesta dashboard '%s'", s_dash);
}

/* subscribe_entities e' il meccanismo del frontend di HA: il server filtra
   sulle entita' indicate, manda subito lo stato completo e poi solo le
   differenze. Rispetto a subscribe_events globale il traffico sul
   collegamento SDIO verso il C6 resta minimo. */
static void send_follow(void)
{
    if (!s_authenticated) return;
    xSemaphoreTakeRecursive(s_send_mtx, portMAX_DELAY);
    if (s_entities_sub_id > 0) {
        ws_send_cmd("\"type\":\"unsubscribe_events\",\"subscription\":" +
                    std::to_string(s_entities_sub_id));
        s_entities_sub_id = -1;
    }
    std::string ids;
    xSemaphoreTake(s_follow_mtx, portMAX_DELAY);
    for (size_t i = 0; i < s_follow.size(); i++) {
        if (i) ids += ",";
        ids += "\"" + s_follow[i] + "\"";
    }
    size_t n = s_follow.size();
    xSemaphoreGive(s_follow_mtx);
    if (n) {
        s_entities_sub_id = ws_send_cmd("\"type\":\"subscribe_entities\",\"entity_ids\":[" + ids + "]");
        ESP_LOGI(TAG, "seguo %u entita'", (unsigned)n);
    }
    xSemaphoreGiveRecursive(s_send_mtx);
}

void ha_ws_follow_entities(const char *const *ids)
{
    xSemaphoreTake(s_follow_mtx, portMAX_DELAY);
    s_follow.clear();
    for (int i = 0; ids && ids[i]; i++) s_follow.push_back(ids[i]);
    xSemaphoreGive(s_follow_mtx);
    send_follow();
}

void ha_ws_call_service_data(const char *domain, const char *service,
                             const char *entity_id, const char *extra)
{
    if (!s_authenticated) return;
    char body[320];
    /* Il servizio senza dati e quello con i dati si scrivono uguali a parte
       "service_data": un servizio come light.turn_on vuole anche quanto
       accenderla, climate.set_temperature a che temperatura andare. */
    if (extra && *extra)
        snprintf(body, sizeof(body),
            "\"type\":\"call_service\",\"domain\":\"%s\",\"service\":\"%s\","
            "\"service_data\":{%s},\"target\":{\"entity_id\":\"%s\"}",
            domain, service, extra, entity_id);
    else
        snprintf(body, sizeof(body),
            "\"type\":\"call_service\",\"domain\":\"%s\",\"service\":\"%s\","
            "\"target\":{\"entity_id\":\"%s\"}", domain, service, entity_id);
    ws_send_cmd(body);
}

void ha_ws_call_service(const char *domain, const char *service, const char *entity_id)
{
    ha_ws_call_service_data(domain, service, entity_id, NULL);
}

void ha_ws_call_service_many(const char *domain, const char *service, const char *entity_ids)
{
    if (!s_authenticated || !entity_ids || !*entity_ids) return;
    /* Un solo messaggio per tutte le entita' invece di uno ciascuna. Non e'
       una raffinatezza: le raffiche di messaggi sono proprio cio' che il
       collegamento SDIO verso il C6 regge peggio, e una card che accende otto
       luci ne manderebbe otto di fila. */
    std::string b = std::string("\"type\":\"call_service\",\"domain\":\"") + domain +
                    "\",\"service\":\"" + service +
                    "\",\"target\":{\"entity_id\":[" + entity_ids + "]}";
    ws_send_cmd(b);
}

bool ha_ws_connected(void) { return s_connected && s_authenticated; }

int ha_ws_request(const char *body, ha_result_cb_t cb, void *ctx)
{
    if (!s_authenticated) return -1;
    xSemaphoreTakeRecursive(s_send_mtx, portMAX_DELAY);   // id e registrazione insieme
    xSemaphoreTake(s_pend_mtx, portMAX_DELAY);
    int slot = -1;
    for (int i = 0; i < MAX_PENDING; i++) if (s_pending[i].id <= 0) { slot = i; break; }
    int id = -1;
    if (slot >= 0) {
        s_pending[slot] = { s_next_id, cb, ctx };          // prima dell'invio: la risposta puo' arrivare subito
        xSemaphoreGive(s_pend_mtx);
        id = ws_send_cmd(body);
        xSemaphoreTake(s_pend_mtx, portMAX_DELAY);
        if (id < 0) s_pending[slot].id = 0;
    }
    xSemaphoreGive(s_pend_mtx);
    xSemaphoreGiveRecursive(s_send_mtx);
    return id;
}

// Estrae la richiesta in attesa con quell'id (false se non e' nostra).
static bool take_pending(int id, Pending *out)
{
    bool found = false;
    xSemaphoreTake(s_pend_mtx, portMAX_DELAY);
    for (int i = 0; i < MAX_PENDING; i++) {
        if (s_pending[i].id > 0 && s_pending[i].id == id) {
            *out = s_pending[i];
            s_pending[i].id = 0;
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_pend_mtx);
    return found;
}

bool ha_ws_cancel(int id)
{
    Pending pd;
    return take_pending(id, &pd);
}

// Connessione persa: chi aspettava una risposta viene avvisato, cosi' non resta appeso.
static void fail_all_pending(void)
{
    Pending list[MAX_PENDING];
    int n = 0;
    xSemaphoreTake(s_pend_mtx, portMAX_DELAY);
    for (int i = 0; i < MAX_PENDING; i++) {
        if (s_pending[i].id > 0) { list[n++] = s_pending[i]; s_pending[i].id = 0; }
    }
    xSemaphoreGive(s_pend_mtx);
    for (int i = 0; i < n; i++) if (list[i].cb) list[i].cb(false, NULL, "connessione persa", list[i].ctx);
}

static void on_auth_ok(void)
{
    /* Host dell'URL (ws://HOST[:porta]/...): la sua voce ARP diventa statica. */
    const char *p = strstr(s_url, "://");
    if (p) {
        p += 3;
        char host[64];
        size_t n = strcspn(p, ":/");
        if (n < sizeof(host)) {
            memcpy(host, p, n);
            host[n] = 0;
            arp_pin_host(host);
        }
    }

    s_tts_sub_id    = ws_send_cmd("\"type\":\"subscribe_events\",\"event_type\":\"panel_tts\"");
    // Se modifichi la dashboard in HA, il pannello la ricarica da solo.
    s_ll_upd_sub_id = ws_send_cmd("\"type\":\"subscribe_events\",\"event_type\":\"lovelace_updated\"");
    ha_ws_request_lovelace();
    s_entities_sub_id = -1;
    send_follow();           // riprende le entita' gia' note dopo una riconnessione

    for (int i = 0; i < MAX_EVENT_SUBS; i++)
        if (s_evsub[i].viva) s_evsub[i].sub_id = ws_send_cmd(s_evsub[i].body);
    for (int i = 0; i < s_n_ready; i++) s_ready_cb[i]();
}

bool ha_ws_on_ready(ha_ready_cb_t cb)
{
    if (!cb || s_n_ready >= MAX_READY_CBS) return false;
    s_ready_cb[s_n_ready++] = cb;
    if (ha_ws_connected()) cb();        // collegamento gia' pronto
    return true;
}

static int sub_add(const std::string &body, ha_event_cb_t cb, void *ctx, bool bus)
{
    if (!cb) return -1;
    int slot = -1;
    for (int i = 0; i < MAX_EVENT_SUBS; i++) if (!s_evsub[i].viva) { slot = i; break; }
    if (slot < 0) {
        ESP_LOGW(TAG, "troppe iscrizioni aperte, questa la salto");
        return -1;
    }
    EventSub &e = s_evsub[slot];
    e.body = body;
    e.cb = cb;
    e.ctx = ctx;
    e.bus = bus;
    e.viva = true;
    e.handle = s_next_handle++;
    /* Se il collegamento c'e' gia', l'iscrizione parte subito; altrimenti la
       fara' on_auth_ok insieme alle altre. */
    e.sub_id = ha_ws_connected() ? ws_send_cmd(e.body) : -1;
    return e.handle;
}

bool ha_ws_subscribe_event(const char *event_type, ha_event_cb_t cb, void *ctx)
{
    if (!event_type) return false;
    std::string b = "\"type\":\"subscribe_events\",\"event_type\":\"";
    return sub_add(b + event_type + "\"", cb, ctx, true) > 0;
}

int ha_ws_subscribe(const char *body, ha_event_cb_t cb, void *ctx)
{
    return body ? sub_add(body, cb, ctx, false) : -1;
}

void ha_ws_unsubscribe(int handle)
{
    if (handle <= 0) return;
    for (int i = 0; i < MAX_EVENT_SUBS; i++) {
        EventSub &e = s_evsub[i];
        if (!e.viva || e.handle != handle) continue;
        if (e.sub_id > 0 && ha_ws_connected())
            ws_send_cmd("\"type\":\"unsubscribe_events\",\"subscription\":" + std::to_string(e.sub_id));
        e.viva = false;
        e.cb = NULL;
        e.ctx = NULL;
        e.sub_id = -1;
        e.body.clear();
        return;
    }
}

// ---- parsing messaggi ----

/* Formato compresso di subscribe_entities:
     {"a": {eid: {"s": stato, "a": {attributi}, ...}}}           aggiunte/stato iniziale
     {"c": {eid: {"+": {"s": stato, "a": {...}}, "-": {...}}}}    variazioni
     {"r": [eid, ...]}                                             rimosse         */
/* true se l'evento apparteneva a un'iscrizione chiesta da un altro modulo. */
static bool event_sub_dispatch(int id, cJSON *ev)
{
    for (int i = 0; i < MAX_EVENT_SUBS; i++) {
        EventSub &e = s_evsub[i];
        if (!e.viva || e.sub_id != id) continue;
        /* Un evento del bus porta il contenuto dentro "data"; una
           sottoscrizione come quella del meteo manda direttamente il suo. */
        if (e.cb) e.cb(e.bus ? cJSON_GetObjectItem(ev, "data") : ev, e.ctx);
        return true;
    }
    return false;
}

static void handle_entities_event(cJSON *ev)
{
    if (!s_cb.on_entity) return;
    cJSON *e;
    cJSON *add = cJSON_GetObjectItem(ev, "a");
    cJSON_ArrayForEach(e, add) {
        cJSON *s = cJSON_GetObjectItem(e, "s");
        s_cb.on_entity(e->string, cJSON_IsString(s) ? s->valuestring : NULL,
                       cJSON_GetObjectItem(e, "a"));
    }
    cJSON *chg = cJSON_GetObjectItem(ev, "c");
    cJSON_ArrayForEach(e, chg) {
        cJSON *plus = cJSON_GetObjectItem(e, "+");
        if (!plus) continue;
        cJSON *s = cJSON_GetObjectItem(plus, "s");
        s_cb.on_entity(e->string, cJSON_IsString(s) ? s->valuestring : NULL,
                       cJSON_GetObjectItem(plus, "a"));
    }
    cJSON *rem = cJSON_GetObjectItem(ev, "r");
    cJSON_ArrayForEach(e, rem) {
        if (cJSON_IsString(e)) s_cb.on_entity(e->valuestring, "unavailable", NULL);
    }
}

static void process_message(const char *json)
{
    if (debug_config_get()->ha_verbose) ESP_LOGI(TAG, "RX (%u byte) %.300s", (unsigned)strlen(json), json);
    cJSON *root = cJSON_Parse(json);
    if (!root) { ESP_LOGW(TAG, "JSON non valido (%u byte)", (unsigned)strlen(json)); return; }
    /* Qualsiasi messaggio valido prova che il collegamento e' vivo. */
    s_last_ok_us = esp_timer_get_time();
    cJSON *type = cJSON_GetObjectItem(root, "type");
    cJSON *idj  = cJSON_GetObjectItem(root, "id");
    int id = cJSON_IsNumber(idj) ? idj->valueint : -1;
    if (cJSON_IsString(type)) {
        const char *t = type->valuestring;
        if (!strcmp(t, "auth_required")) {
            send_auth();
        } else if (!strcmp(t, "auth_ok")) {
            ESP_LOGI(TAG, "auth OK");
            s_authenticated = true;
            on_auth_ok();
            if (s_cb.on_status) s_cb.on_status(true);
        } else if (!strcmp(t, "auth_invalid")) {
            /* Col pannello abbinato puo' voler dire solo che il token di
               accesso e' scaduto: al prossimo tentativo se ne fa uno nuovo. */
            ha_token_invalidate();
            ESP_LOGE(TAG, "TOKEN NON VALIDO");
        } else if (!strcmp(t, "event")) {
            cJSON *ev = cJSON_GetObjectItem(root, "event");
            if (id > 0 && id == s_entities_sub_id) {
                handle_entities_event(ev);
            } else if (id > 0 && id == s_ll_upd_sub_id) {
                cJSON *data = cJSON_GetObjectItem(ev, "data");
                cJSON *up   = data ? cJSON_GetObjectItem(data, "url_path") : NULL;
                const char *p = cJSON_IsString(up) ? up->valuestring : "lovelace";
                if (!strcmp(p, s_dash)) {
                    ESP_LOGI(TAG, "dashboard modificata in HA, ricarico");
                    ha_ws_request_lovelace();
                }
            } else if (id > 0 && event_sub_dispatch(id, ev)) {
                /* gestito da un modulo iscritto */
            } else if (id > 0 && id == s_tts_sub_id && s_cb.on_tts) {
                cJSON *data = cJSON_GetObjectItem(ev, "data");
                cJSON *m = data ? cJSON_GetObjectItem(data, "message") : NULL;
                if (cJSON_IsString(m)) s_cb.on_tts(m->valuestring);
            }
        } else if (!strcmp(t, "result")) {
            Pending pd;
            if (id > 0 && take_pending(id, &pd)) {
                bool ok = cJSON_IsTrue(cJSON_GetObjectItem(root, "success"));
                cJSON *err = cJSON_GetObjectItem(root, "error");
                cJSON *msg = err ? cJSON_GetObjectItem(err, "message") : NULL;
                if (pd.cb) pd.cb(ok, cJSON_GetObjectItem(root, "result"),
                                 cJSON_IsString(msg) ? msg->valuestring : (ok ? NULL : "errore"), pd.ctx);
            } else if (id > 0 && id == s_lovelace_req_id) {
                s_lovelace_req_id = -1;
                cJSON *ok = cJSON_GetObjectItem(root, "success");
                if (cJSON_IsTrue(ok)) {
                    ESP_LOGI(TAG, "dashboard ricevuta (%u byte)", (unsigned)strlen(json));
                    if (s_cb.on_lovelace) s_cb.on_lovelace(cJSON_GetObjectItem(root, "result"), NULL);
                } else {
                    cJSON *err = cJSON_GetObjectItem(root, "error");
                    cJSON *msg = err ? cJSON_GetObjectItem(err, "message") : NULL;
                    const char *m = cJSON_IsString(msg) ? msg->valuestring : "errore sconosciuto";
                    ESP_LOGW(TAG, "dashboard non disponibile: %s", m);
                    if (s_cb.on_lovelace) s_cb.on_lovelace(NULL, m);
                }
            } else if (!cJSON_IsTrue(cJSON_GetObjectItem(root, "success"))) {
                cJSON *err = cJSON_GetObjectItem(root, "error");
                cJSON *msg = err ? cJSON_GetObjectItem(err, "message") : NULL;
                ESP_LOGW(TAG, "comando %d rifiutato: %s", id,
                         cJSON_IsString(msg) ? msg->valuestring : "?");
            }
        }
    }
    cJSON_Delete(root);
}

// ---- event handler ----
static void ws_event(void *args, esp_event_base_t base, int32_t id, void *event_data)
{
    esp_websocket_event_data_t *d = (esp_websocket_event_data_t *)event_data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connesso");
        xSemaphoreTakeRecursive(s_send_mtx, portMAX_DELAY);
        s_next_id = 1;
        s_lovelace_req_id = s_entities_sub_id = s_tts_sub_id = s_ll_upd_sub_id = -1;
        xSemaphoreGiveRecursive(s_send_mtx);
        s_connected = true; s_authenticated = false; s_rx_len = 0;
        s_last_ok_us = esp_timer_get_time();
        break;
    case WEBSOCKET_EVENT_DATA:
        if (d->op_code == 0x01 || d->op_code == 0x00) {   // text / continuation
            if (d->payload_offset == 0) s_rx_len = 0;
            bool ok = rx_append(d->data_ptr, d->data_len);
            if (d->payload_offset + d->data_len >= d->payload_len) {
                if (ok) process_message(s_rx);
                else ESP_LOGE(TAG, "messaggio scartato: %d byte, memoria insufficiente", d->payload_len);
                s_rx_len = 0;
            }
        }
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "disconnesso/errore");
        s_connected = false; s_authenticated = false; s_rx_len = 0;
        fail_all_pending();
        if (s_cb.on_status) s_cb.on_status(false);
        break;
    default: break;
    }
}

// ---- watchdog: interviene SOLO su connessione stabilita ma muta ----
//
// La versione precedente considerava "non vivo" anche il caso normale in cui il
// client sta gia' ritentando da solo, e ogni 5s faceva stop()+start(). Con un
// Home Assistant irraggiungibile i due meccanismi di riconnessione si
// ostacolavano a vicenda e dopo ~90s la coda SDIO verso il C6 andava in
// "Unrecoverable host sdio state": ESP-Hosted riavviava l'intero pannello.
// Un indirizzo sbagliato deve solo mostrare "disconnesso", mai riavviare.
static void watchdog_task(void *arg)
{
    const int64_t STALE_US = 30LL * 1000 * 1000;   // connesso ma zitto da 30s
    static const int delays_s[] = { 5, 10, 20, 40, 60 };
    int     backoff = 0;
    int64_t last_attempt_us = 0;
    int64_t last_ping_us = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (!s_client) continue;

        const int64_t now = esp_timer_get_time();
        const bool link_up = esp_websocket_client_is_connected(s_client);

        if (link_up && s_authenticated) {
            if ((now - s_last_ok_us) < STALE_US) {
                backoff = 0;
                /* Battito applicativo: HA risponde con un pong, che rinnova
                   s_last_ok_us anche quando nessuna entita' cambia stato. */
                if ((now - last_ping_us) > 15LL * 1000 * 1000) {
                    ws_send_cmd("\"type\":\"ping\"");
                    last_ping_us = now;
                }
                continue;
            }
            ESP_LOGW(TAG, "connessione muta da %ds, riconnetto", (int)((now - s_last_ok_us) / 1000000));
        } else if (link_up) {
            continue;                       // handshake/auth in corso
        }

        const int64_t wait_us = (int64_t)delays_s[backoff] * 1000 * 1000;
        if (last_attempt_us != 0 && (now - last_attempt_us) < wait_us) continue;

        ESP_LOGI(TAG, "tentativo di connessione (prossimo fra %ds se fallisce)",
                 delays_s[backoff < 4 ? backoff + 1 : 4]);
        /* Il token si prende qui, in questo task: se il pannello e' abbinato
           serve una chiamata HTTP a HA, che nel task del WebSocket bloccherebbe
           anche i ping. */
        ha_token_get_access(s_token, sizeof(s_token));
        xSemaphoreTake(s_life_mtx, portMAX_DELAY);
        esp_websocket_client_stop(s_client);
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_websocket_client_start(s_client);
        xSemaphoreGive(s_life_mtx);

        last_attempt_us = esp_timer_get_time();
        s_last_ok_us    = last_attempt_us;
        if (backoff < 4) backoff++;
    }
}

void ha_ws_start(const char *url, const char *token, const char *dash_path,
                 const ha_ws_callbacks_t *cbs)
{
    strlcpy(s_url, url, sizeof(s_url));
    strlcpy(s_token, token, sizeof(s_token));
    strlcpy(s_dash, dash_path ? dash_path : "lovelace", sizeof(s_dash));
    if (cbs) s_cb = *cbs;
    if (!s_token[0]) ha_token_get_access(s_token, sizeof(s_token));

    if (!s_send_mtx)   s_send_mtx   = xSemaphoreCreateRecursiveMutex();
    if (!s_follow_mtx) s_follow_mtx = xSemaphoreCreateMutex();
    if (!s_pend_mtx)   s_pend_mtx   = xSemaphoreCreateMutex();
    if (!s_life_mtx)   s_life_mtx   = xSemaphoreCreateMutex();

    /* Gli alberi cJSON (configurazione dashboard, stati) vanno in PSRAM.
       free() di ESP-IDF libera qualunque heap, quindi i blocchi allocati
       prima di questo punto restano gestiti correttamente. */
    cJSON_Hooks hooks = { psram_malloc, free };
    cJSON_InitHooks(&hooks);

    esp_websocket_client_config_t cfg = {};
    cfg.uri                 = s_url;
    /* Riconnessione affidata al solo watchdog qui sotto: con quella interna
       attiva i due meccanismi si sovrapponevano e, con HA irraggiungibile,
       martellavano il trasporto SDIO verso il C6 fino a bloccarlo. */
    cfg.disable_auto_reconnect = true;
    cfg.reconnect_timeout_ms = 5000;
    cfg.network_timeout_ms   = 10000;
    cfg.pingpong_timeout_sec = 8;      // reconnect se manca il pong per 8s
    cfg.buffer_size          = 4096;   // i messaggi grandi arrivano a frammenti
    cfg.task_stack           = 12288;  // parsing della dashboard + costruzione UI

    s_client = esp_websocket_client_init(&cfg);
    esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, ws_event, NULL);
    esp_websocket_client_start(s_client);

    xTaskCreate(watchdog_task, "ha_ws_wd", 4096, NULL, 4, NULL);
}

void ha_ws_restart(const char *url, const char *token, const char *dash_path)
{
    if (!s_client) { ha_ws_start(url, token, dash_path, &s_cb); return; }
    xSemaphoreTake(s_life_mtx, portMAX_DELAY);
    strlcpy(s_url, url, sizeof(s_url));
    strlcpy(s_token, token, sizeof(s_token));
    strlcpy(s_dash, dash_path ? dash_path : "lovelace", sizeof(s_dash));
    esp_websocket_client_stop(s_client);
    esp_websocket_client_set_uri(s_client, s_url);
    esp_websocket_client_start(s_client);
    xSemaphoreGive(s_life_mtx);
}
