#include "web_config.h"
#include "ha_config.h"
#include "ha_http.h"
#include "dash_override.h"
#include "ha_ws.h"
#include "web_auth.h"
#include "web_cert.h"
#include "ha_oauth.h"
#include "ha_token.h"
#include "ha_registry.h"
#include "standby_show.h"
#include "idle_manager.h"
#include "ha_plugin.h"
#include "net_config.h"      // il nome di rete del pannello, per controllare l'intestazione Host
#include "backup.h"
#include "esplora.h"

#include <string.h>
#include <strings.h>         // strcasecmp: i nomi di rete non badano alle maiuscole
#include <stdlib.h>
#include <sys/stat.h>
#include <ctype.h>          // isxdigit: scioglie i %XX dei percorsi
#include <errno.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_https_server.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "cJSON.h"

static const char *TAG = "web_config";
static web_cfg_changed_cb_t     s_on_cfg    = NULL;
static web_layout_changed_cb_t  s_on_layout = NULL;

// Copia di riserva della pagina, compilata nel firmware (vedi CMakeLists).
extern const uint8_t panel_html_gz_start[] asm("_binary_panel_html_gz_start");
extern const uint8_t panel_html_gz_end[]   asm("_binary_panel_html_gz_end");

#define SD_PAGE     "/sdcard/web/panel.html"
#define SD_PAGE_GZ  "/sdcard/web/panel.html.gz"

// ------------------------------------------------------------------ utilita'

static char *recv_body(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 96 * 1024) return NULL;
    char *buf = (char *)heap_caps_malloc(total + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = (char *)malloc(total + 1);
    if (!buf) return NULL;
    int off = 0;
    while (off < total) {
        int r = httpd_req_recv(req, buf + off, total - off);
        if (r <= 0) { free(buf); return NULL; }
        off += r;
    }
    buf[total] = 0;
    return buf;
}

/* Manda un cJSON e lo libera. */
static esp_err_t send_json(httpd_req_t *r, cJSON *j)
{
    httpd_resp_set_type(r, "application/json");
    if (!j) return httpd_resp_sendstr(r, "null");
    char *txt = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!txt) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "memoria");
    esp_err_t e = httpd_resp_sendstr(r, txt);
    cJSON_free(txt);
    return e;
}

static const char *jstr(const cJSON *o, const char *k)
{
    const cJSON *i = cJSON_GetObjectItem(o, k);
    return cJSON_IsString(i) ? i->valuestring : NULL;
}

// ------------------------------------------------------------------ sessione

static void get_sid(httpd_req_t *r, char *sid, size_t sz)
{
    sid[0] = 0;
    size_t n = sz;
    if (httpd_req_get_cookie_val(r, "sid", sid, &n) != ESP_OK) sid[0] = 0;
}

static web_role_t role_of(httpd_req_t *r)
{
    char sid[WEB_SID_LEN + 2];
    get_sid(r, sid, sizeof(sid));
    return web_auth_role_of(sid);
}

/* Cancello: true se si puo' proseguire, altrimenti ha gia' risposto lui. */
static bool allow(httpd_req_t *r, web_role_t min)
{
    web_role_t have = role_of(r);
    if (have >= min && have != WEB_ROLE_NONE) return true;
    httpd_resp_set_status(r, have == WEB_ROLE_NONE ? "401 Unauthorized" : "403 Forbidden");
    httpd_resp_set_type(r, "application/json");
    httpd_resp_sendstr(r, have == WEB_ROLE_NONE
        ? "{\"error\":\"Serve l'accesso.\"}"
        : "{\"error\":\"Questa parte e' riservata all'amministratore.\"}");
    return false;
}

/* httpd_resp_set_hdr non copia il valore: si tiene il puntatore e lo legge
   quando manda la risposta. Il testo del cookie deve quindi restare fermo
   fino ad allora, e una variabile locale non basta nemmeno se sta nella
   funzione che chiama.

   Il motivo e' meno ovvio di "la variabile muore al ritorno". Il compilatore
   vede che dopo set_cookie() quel pezzo di pila non lo legge piu' nessuno -
   il puntatore che se l'e' portato via sta dentro una struttura di ESP-IDF,
   e lui li' non guarda - e allora e' libero di darlo a un'altra variabile
   della stessa funzione. Nell'intestazione ci finisce quello che ci viene
   scritto dopo.

   Non e' un ragionamento teorico: e' successo. Nel browser si e' trovato un
   cookie che si chiamava "/?auth_ok=Mario Rossi", cioe' pari pari
   l'indirizzo di ritorno costruito subito dopo il cookie. Chi tornava
   dall'accesso con l'account di Home Assistant si vedeva dire "benvenuto" e
   restava fuori, perche' la sessione non gliel'aveva data nessuno.

   Qui il testo sta in un posto legato alla connessione, che vive finche'
   vive la risposta. Le prese aperte insieme sono 5 piu' 2, quindi otto
   posti bastano e avanzano. */
#define COOKIE_POSTI 8
#define COOKIE_LEN   160
static char s_cookie[COOKIE_POSTI][COOKIE_LEN];

static void set_cookie(httpd_req_t *r, const char *sid)
{
    int fd = httpd_req_to_sockfd(r);
    char *c = s_cookie[(fd < 0 ? 0 : fd) % COOKIE_POSTI];
    /* HttpOnly: il cookie non e' leggibile dagli script della pagina.
       Secure: solo su HTTPS.
       SameSite=Lax e non Strict: tornando dall'autorizzazione di Home
       Assistant il browser arriva qui da un altro sito, e con Strict non
       rimandava il cookie appena dato. Risultato: l'accesso riusciva ma la
       pagina si ritrovava sconosciuta, senza nessun errore da mostrare.
       Con Lax il cookie viaggia sulle aperture di pagina come questa, ma non
       su richieste che cambiano qualcosa partite da altri siti. */
    snprintf(c, COOKIE_LEN,
             "sid=%s; Path=/; Max-Age=28800; HttpOnly; SameSite=Lax; Secure", sid);
    httpd_resp_set_hdr(r, "Set-Cookie", c);
}

// ------------------------------------------------------------------ pagina

static esp_err_t send_file(httpd_req_t *r, const char *path, bool gz)
{
    FILE *f = fopen(path, "rb");
    if (!f) return ESP_FAIL;
    httpd_resp_set_type(r, "text/html");
    if (gz) httpd_resp_set_hdr(r, "Content-Encoding", "gzip");
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        // se la connessione cade a meta' non si puo' piu' ripiegare su un'altra
        // copia: la risposta e' gia' partita, si chiude e basta.
        if (httpd_resp_send_chunk(r, buf, n) != ESP_OK) { fclose(f); return ESP_OK; }
    }
    fclose(f);
    httpd_resp_send_chunk(r, NULL, 0);
    return ESP_OK;
}

static esp_err_t h_page(httpd_req_t *r)
{
    struct stat st;
    if (stat(SD_PAGE, &st) == 0    && send_file(r, SD_PAGE, false)  == ESP_OK) return ESP_OK;
    if (stat(SD_PAGE_GZ, &st) == 0 && send_file(r, SD_PAGE_GZ, true) == ESP_OK) return ESP_OK;
    httpd_resp_set_type(r, "text/html");
    httpd_resp_set_hdr(r, "Content-Encoding", "gzip");
    return httpd_resp_send(r, (const char *)panel_html_gz_start,
                           panel_html_gz_end - panel_html_gz_start);
}

// ------------------------------------------------------------------ accesso

static esp_err_t h_session(httpd_req_t *r)
{
    cJSON *j = cJSON_CreateObject();
    web_role_t role = role_of(r);
    cJSON_AddBoolToObject(j, "configured", web_auth_configured());
    cJSON_AddStringToObject(j, "role", role == WEB_ROLE_ADMIN ? "admin" :
                                        role == WEB_ROLE_GUEST ? "guest" : "none");
    cJSON_AddBoolToObject(j, "guest_enabled", web_auth_guest_enabled());
    cJSON_AddNumberToObject(j, "wait", web_auth_wait_s());
    cJSON_AddBoolToObject(j, "ha_set", ha_config_is_set());
    cJSON_AddBoolToObject(j, "ha_login", ha_oauth_available());
    cJSON_AddBoolToObject(j, "ha_paired", ha_token_have_refresh());
    return send_json(r, j);
}

/* Primo avvio: chi arriva per primo crea la password di amministratore. */
static esp_err_t h_setup(httpd_req_t *r)
{
    if (web_auth_configured()) {
        httpd_resp_set_status(r, "409 Conflict");
        return httpd_resp_sendstr(r, "La password esiste gia'.");
    }
    char *body = recv_body(r);
    if (!body) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "body");
    cJSON *j = cJSON_Parse(body);
    free(body);
    const char *pw = jstr(j, "password");
    bool ok = pw && web_auth_set_password(WEB_ROLE_ADMIN, pw);
    if (ok) {
        char sid[WEB_SID_LEN + 1];
        if (web_auth_login(pw, sid, sizeof(sid)) != WEB_ROLE_NONE)
            set_cookie(r, sid);
    }
    cJSON_Delete(j);
    if (!ok) {
        httpd_resp_set_status(r, "400 Bad Request");
        return httpd_resp_sendstr(r, "Password troppo corta.");
    }
    return httpd_resp_sendstr(r, "ok");
}

static esp_err_t h_login(httpd_req_t *r)
{
    uint32_t wait = web_auth_wait_s();
    if (wait) {
        httpd_resp_set_status(r, "429 Too Many Requests");
        char msg[96];
        snprintf(msg, sizeof(msg), "{\"error\":\"Troppi tentativi: riprova fra %u s.\"}",
                 (unsigned)wait);
        httpd_resp_set_type(r, "application/json");
        return httpd_resp_sendstr(r, msg);
    }
    char *body = recv_body(r);
    if (!body) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "body");
    cJSON *j = cJSON_Parse(body);
    free(body);
    const char *pw = jstr(j, "password");
    char sid[WEB_SID_LEN + 1] = "";
    web_role_t role = pw ? web_auth_login(pw, sid, sizeof(sid)) : WEB_ROLE_NONE;
    cJSON_Delete(j);
    if (role == WEB_ROLE_NONE) {
        httpd_resp_set_status(r, "401 Unauthorized");
        httpd_resp_set_type(r, "application/json");
        return httpd_resp_sendstr(r, "{\"error\":\"Password non valida.\"}");
    }
    set_cookie(r, sid);
    /* Non posso riusare h_session: leggerebbe il cookie della richiesta, che
       e' ancora quello di prima (vuoto), e risponderebbe "nessun ruolo". */
    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "configured", true);
    cJSON_AddStringToObject(out, "role", role == WEB_ROLE_ADMIN ? "admin" : "guest");
    cJSON_AddBoolToObject(out, "guest_enabled", web_auth_guest_enabled());
    cJSON_AddNumberToObject(out, "wait", 0);
    cJSON_AddBoolToObject(out, "ha_set", ha_config_is_set());
    cJSON_AddBoolToObject(out, "ha_login", ha_oauth_available());
    cJSON_AddBoolToObject(out, "ha_paired", ha_token_have_refresh());
    return send_json(r, out);
}

static esp_err_t h_logout(httpd_req_t *r)
{
    char sid[WEB_SID_LEN + 2];
    get_sid(r, sid, sizeof(sid));
    web_auth_logout(sid);
    httpd_resp_set_hdr(r, "Set-Cookie", "sid=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax; Secure");
    return httpd_resp_sendstr(r, "ok");
}

/* Cambio password: la propria (amministratore) e quella dell'ospite. */
static esp_err_t h_passwords(httpd_req_t *r)
{
    if (!allow(r, WEB_ROLE_ADMIN)) return ESP_OK;
    char *body = recv_body(r);
    if (!body) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "body");
    cJSON *j = cJSON_Parse(body);
    free(body);
    bool ok = true, touched = false;
    const cJSON *a = cJSON_GetObjectItem(j, "admin");
    const cJSON *g = cJSON_GetObjectItem(j, "guest");
    if (cJSON_IsString(a) && a->valuestring[0]) {
        ok = web_auth_set_password(WEB_ROLE_ADMIN, a->valuestring);
        touched = true;
    }
    if (ok && cJSON_IsString(g)) {          // stringa vuota = ospite disattivato
        ok = web_auth_set_password(WEB_ROLE_GUEST, g->valuestring);
        touched = true;
    }
    cJSON_Delete(j);
    if (!ok) {
        httpd_resp_set_status(r, "400 Bad Request");
        return httpd_resp_sendstr(r, "Password troppo corta.");
    }
    if (touched) web_auth_logout_all();     // chi era dentro rientra con la nuova
    return httpd_resp_sendstr(r, "ok");
}

/* Accesso con l'account di Home Assistant: mando il browser da HA. */
static esp_err_t h_auth_start(httpd_req_t *r)
{
    char url[400];
    if (!ha_oauth_start_url(url, sizeof(url))) {
        httpd_resp_set_status(r, "503 Service Unavailable");
        return httpd_resp_sendstr(r, "Home Assistant non e' configurato.");
    }
    httpd_resp_set_status(r, "302 Found");
    httpd_resp_set_hdr(r, "Location", url);
    return httpd_resp_send(r, NULL, 0);
}

/* Ritorno da HA con il codice: lo scambio, scopro chi e', apro la sessione. */
static esp_err_t h_auth_callback(httpd_req_t *r)
{
    char query[512] = "", code[300] = "", state[80] = "", err[160] = "";
    httpd_req_get_url_query_str(r, query, sizeof(query));
    httpd_query_key_value(query, "code", code, sizeof(code));
    httpd_query_key_value(query, "state", state, sizeof(state));

    char sid[WEB_SID_LEN + 1] = "", name[64] = "";
    bool ok = code[0] && ha_oauth_finish(code, state, sid, sizeof(sid),
                                         name, sizeof(name), err, sizeof(err));
    if (ok) set_cookie(r, sid);
    else if (!err[0]) strlcpy(err, "Autorizzazione non riuscita.", sizeof(err));

    /* Torno alla pagina: se e' andata, il cookie e' gia' nel browser. */
    char loc[256];
    if (ok) {
        char n[120];
        size_t o = 0;
        for (const char *c = name; *c && o + 4 < sizeof(n); c++) {
            if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
                (*c >= '0' && *c <= '9')) { n[o++] = *c; }
            else { o += snprintf(n + o, sizeof(n) - o, "%%%02X", (unsigned char)*c); }
        }
        n[o] = 0;
        snprintf(loc, sizeof(loc), "/?auth_ok=%s", n);
    } else {
        char e[240];
        size_t o = 0;
        for (const char *c = err; *c && o + 4 < sizeof(e); c++) {
            if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
                (*c >= '0' && *c <= '9')) { e[o++] = *c; }
            else { o += snprintf(e + o, sizeof(e) - o, "%%%02X", (unsigned char)*c); }
        }
        e[o] = 0;
        snprintf(loc, sizeof(loc), "/?auth_error=%s", e);
    }
    httpd_resp_set_status(r, "302 Found");
    httpd_resp_set_hdr(r, "Location", loc);
    return httpd_resp_send(r, NULL, 0);
}

// ------------------------------------------------------------------ Home Assistant

static esp_err_t h_get_config(httpd_req_t *r)
{
    if (!allow(r, WEB_ROLE_ADMIN)) return ESP_OK;
    char url[HA_URL_MAX] = "", token[HA_TOKEN_MAX] = "", dash[HA_DASH_MAX] = "";
    int view = 0;
    ha_config_load(url, sizeof(url), token, sizeof(token));
    bool has_token = token[0] != 0;
    memset(token, 0, sizeof(token));        // il token non esce da qui, mai
    ha_config_load_dash(dash, sizeof(dash), &view);
    char tts[HA_TTS_MAX] = "";
    ha_config_load_tts(tts, sizeof(tts));

    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "url", url);
    cJSON_AddStringToObject(j, "dashboard", dash);
    cJSON_AddNumberToObject(j, "view", view);
    cJSON_AddBoolToObject(j, "token_set", has_token);
    cJSON_AddStringToObject(j, "tts", tts);
    /* Il certificato non si rimanda indietro: chi apre la pagina deve sapere
       SE c'e', non rileggerselo. */
    cJSON_AddBoolToObject(j, "ca_set", ha_config_has_ca());
    return send_json(r, j);
}

static esp_err_t h_post_config(httpd_req_t *r)
{
    if (!allow(r, WEB_ROLE_ADMIN)) return ESP_OK;
    char *body = recv_body(r);
    if (!body) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "body");
    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "json");

    const char *url   = jstr(j, "url");
    const char *token = jstr(j, "token");          // assente o vuoto: lascia quello che c'e'
    const char *dash  = jstr(j, "dashboard");
    const cJSON *view = cJSON_GetObjectItem(j, "view");

    bool ok = true;
    if (url && url[0]) {
        if (strncmp(url, "ws://", 5) && strncmp(url, "wss://", 6)) {
            cJSON_Delete(j);
            httpd_resp_set_status(r, "400 Bad Request");
            return httpd_resp_sendstr(r, "L'indirizzo deve iniziare con ws:// o wss://");
        }
        ok = (token && token[0]) ? ha_config_save(url, token) : ha_config_save_url(url);
    } else if (token && token[0]) {
        char cur[HA_URL_MAX] = "", tok[HA_TOKEN_MAX] = "";
        ha_config_load(cur, sizeof(cur), tok, sizeof(tok));
        memset(tok, 0, sizeof(tok));
        ok = ha_config_save(cur, token);
    }
    if (ok && dash) ok = ha_config_save_dash(dash[0] ? dash : "lovelace",
                                             cJSON_IsNumber(view) ? view->valueint : 0);
    const char *tts = jstr(j, "tts");
    if (ok && tts) ha_config_save_tts(tts);      // vuoto = torna al predefinito

    /* Il certificato di Home Assistant. Si manda solo quando si cambia: la
       pagina non lo rilegge, quindi un salvataggio normale non lo tocca.
       La stringa "-" vuol dire "toglilo". */
    const cJSON *ca = cJSON_GetObjectItem(j, "ca");
    if (ok && cJSON_IsString(ca)) {
        const char *v = ca->valuestring;
        bool togli = !v[0] || (v[0] == '-' && !v[1]);
        ok = ha_config_save_ca(togli ? NULL : v);
        ha_tls_ricarica();
        ESP_LOGW(TAG, "certificato di Home Assistant %s: si applica alla prossima "
                 "connessione", togli ? "rimosso" : "aggiornato");
    }
    cJSON_Delete(j);
    if (!ok) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "save");
    if (s_on_cfg) s_on_cfg();
    return httpd_resp_sendstr(r, "ok");
}

/* Fotografia dello schermo: intestazione di 4 byte (larghezza e altezza,
   16 bit) e poi i pixel RGB565. Il browser la disegna su una canvas: cosi'
   nell'editor si vede il pannello vero invece di una ricostruzione. */
static esp_err_t h_screenshot(httpd_req_t *r)
{
    if (!allow(r, WEB_ROLE_GUEST)) return ESP_OK;
    int scale = 2;
    char q[32];
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) == ESP_OK) {
        char v[8];
        if (httpd_query_key_value(q, "scale", v, sizeof(v)) == ESP_OK) scale = atoi(v);
    }
    if (scale < 1) scale = 1;
    if (scale > 8) scale = 8;

    bsp_display_lock(0);
    lv_img_dsc_t *snap = (lv_img_dsc_t *)lv_snapshot_take(lv_scr_act(), LV_IMG_CF_TRUE_COLOR);
    bsp_display_unlock();
    if (!snap) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "memoria");

    const int w = snap->header.w, h = snap->header.h;
    const int ow = w / scale, oh = h / scale;
    const uint16_t *src = (const uint16_t *)snap->data;

    httpd_resp_set_type(r, "application/octet-stream");
    uint8_t head[4] = { (uint8_t)(ow & 0xff), (uint8_t)(ow >> 8),
                        (uint8_t)(oh & 0xff), (uint8_t)(oh >> 8) };
    esp_err_t e = httpd_resp_send_chunk(r, (const char *)head, sizeof(head));

    uint16_t *row = (uint16_t *)malloc(ow * 2);
    for (int y = 0; y < oh && e == ESP_OK && row; y++) {
        const uint16_t *sr = src + (size_t)(y * scale) * w;
        for (int x = 0; x < ow; x++) row[x] = sr[x * scale];
        e = httpd_resp_send_chunk(r, (const char *)row, ow * 2);
    }
    free(row);
    lv_snapshot_free(snap);
    httpd_resp_send_chunk(r, NULL, 0);
    return ESP_OK;
}

// ------------------------------------------------------------------ dashboard

static esp_err_t h_get_layout(httpd_req_t *r)
{
    if (!allow(r, WEB_ROLE_GUEST)) return ESP_OK;
    bsp_display_lock(0);
    cJSON *j = dash_override_layout();
    bsp_display_unlock();
    if (!j) {
        j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "error",
            "Dashboard non ancora ricevuta da Home Assistant.");
    }
    return send_json(r, j);
}

static esp_err_t h_get_ovr(httpd_req_t *r)
{
    if (!allow(r, WEB_ROLE_GUEST)) return ESP_OK;
    return send_json(r, dash_override_current());
}

static esp_err_t h_put_ovr(httpd_req_t *r)
{
    if (!allow(r, WEB_ROLE_GUEST)) return ESP_OK;
    char *body = recv_body(r);
    if (!body) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "body");
    bool ok = dash_override_save(body);
    free(body);
    if (ok && s_on_layout) s_on_layout();
    return ok ? httpd_resp_sendstr(r, "ok") : httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "invalid");
}

// ------------------------------------------------------- slideshow (standby)

/* Elenco delle entita' di HA, per dispositivo: serve a scegliere i valori in
   sovrimpressione senza andare a caccia degli identificativi. La risposta e'
   grossa (decine di kB) e ha_registry_json ci mette qualche secondo la prima
   volta, poi la tiene da parte. */
static esp_err_t h_entities(httpd_req_t *r)
{
    if (!allow(r, WEB_ROLE_GUEST)) return ESP_OK;
    char *txt = ha_registry_json();
    if (!txt) {
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "error",
            "Elenco non disponibile: Home Assistant non ha risposto.");
        return send_json(r, j);
    }
    httpd_resp_set_type(r, "application/json");
    esp_err_t e = httpd_resp_sendstr(r, txt);
    free(txt);
    return e;
}

static esp_err_t h_get_standby(httpd_req_t *r)
{
    if (!allow(r, WEB_ROLE_GUEST)) return ESP_OK;
    standby_cfg_t c;
    standby_config_load(&c);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "slideshow_dopo_min", c.slideshow_after_s / 60.0);
    cJSON_AddNumberToObject(j, "spegni_dopo_min",    c.screen_off_after_s / 60.0);
    cJSON_AddNumberToObject(j, "secondi_foto",       c.photo_seconds);
    cJSON_AddBoolToObject(j,   "casuale",            c.shuffle);
    cJSON_AddStringToObject(j, "cartella",           c.folder);
    cJSON_AddNumberToObject(j, "max_valori",         STANDBY_MAX_OVERLAY);
    /* Quando lo slideshow lo decide l'integrazione di Home Assistant, qui si
       guarda e basta: due padroni per la stessa configurazione vorrebbe dire
       che l'ultimo che salva cancella l'altro senza dirlo a nessuno. */
    cJSON_AddBoolToObject(j, "gestito_da_ha", ha_plugin_owns_slideshow());
    /* Sempre nella forma lunga: la pagina deve poter mostrare il nome scelto
       in una casella modificabile, e "" significa "usa quello di HA". */
    cJSON *ov = cJSON_AddArrayToObject(j, "sovrimpressione");
    for (int i = 0; i < c.n_overlay; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "id",   c.overlay[i]);
        cJSON_AddStringToObject(e, "nome", c.overlay_label[i]);
        cJSON_AddItemToArray(ov, e);
    }
    return send_json(r, j);
}

static esp_err_t h_post_standby(httpd_req_t *r)
{
    if (!allow(r, WEB_ROLE_ADMIN)) return ESP_OK;
    if (ha_plugin_owns_slideshow()) {
        httpd_resp_set_status(r, "409 Conflict");
        httpd_resp_set_type(r, "application/json");
        return httpd_resp_sendstr(r,
            "{\"error\":\"Lo slideshow lo gestisce Home Assistant. "
            "Cambialo da li', oppure riprendi il comando da qui.\"}");
    }
    char *body = recv_body(r);
    if (!body) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "body");
    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "json");

    /* Parto da quella attuale: la pagina manda solo quello che cambia. */
    standby_cfg_t c;
    standby_config_load(&c);
    const cJSON *v;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(j, "slideshow_dopo_min"))) c.slideshow_after_s  = (int)(v->valuedouble * 60);
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(j, "spegni_dopo_min")))    c.screen_off_after_s = (int)(v->valuedouble * 60);
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(j, "secondi_foto")))       c.photo_seconds      = v->valueint;
    if (cJSON_IsBool(v = cJSON_GetObjectItem(j, "casuale")))              c.shuffle            = cJSON_IsTrue(v);
    if (cJSON_IsString(v = cJSON_GetObjectItem(j, "cartella")))           strlcpy(c.folder, v->valuestring, sizeof(c.folder));
    standby_overlay_from_json(cJSON_GetObjectItem(j, "sovrimpressione"), &c);
    cJSON_Delete(j);

    if (!standby_config_save(&c)) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "save");
    idle_manager_reload_config();      // chiede subito a HA i valori nuovi
    return httpd_resp_sendstr(r, "ok");
}

/* "Riprendi il comando da qui": serve se l'integrazione viene tolta da HA e
   il pannello non ha ancora avuto modo di accorgersene. */
static esp_err_t h_standby_release(httpd_req_t *r)
{
    if (!allow(r, WEB_ROLE_ADMIN)) return ESP_OK;
    ha_plugin_release_slideshow();
    return httpd_resp_sendstr(r, "ok");
}

static esp_err_t h_reload(httpd_req_t *r)
{
    if (!allow(r, WEB_ROLE_GUEST)) return ESP_OK;
    ha_ws_request_lovelace();
    return httpd_resp_sendstr(r, "ok");
}

// ------------------------------------------------------------------ avvio

/* ---------------------------------------------------------------- salvataggio

   Tre cose sole: prendere la chiave di recupero, scaricare la configurazione,
   rimetterla. Tutte e tre da amministratore, perche' il salvataggio cifrato
   contiene il permesso di entrare in Home Assistant e la chiave privata del
   certificato: chi lo scarica si porta via le chiavi di casa. */

static esp_err_t h_chiave(httpd_req_t *r)
{
    if (!allow(r, WEB_ROLE_ADMIN)) return ESP_OK;
    char k[BACKUP_CHIAVE_MAX];
    if (!backup_chiave_testo(k, sizeof(k)))
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "chiave");
    backup_chiave_segna_presa();

    /* Come file da salvare, non come pagina: una chiave di recupero letta a
       schermo e poi dimenticata non ha protetto nessuno. */
    char nome[96];
    net_config_t nc;
    net_config_load(&nc);
    snprintf(nome, sizeof(nome), "attachment; filename=\"chiave-%s.txt\"",
             nc.hostname[0] ? nc.hostname : "pannello");
    httpd_resp_set_type(r, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(r, "Content-Disposition", nome);

    char testo[BACKUP_CHIAVE_MAX + 420];
    int n = snprintf(testo, sizeof(testo),
        "Chiave di recupero del pannello %s\r\n\r\n"
        "    %s\r\n\r\n"
        "Serve per riaprire un salvataggio cifrato della configurazione.\r\n"
        "Tienila da parte, fuori dal pannello: se il pannello si guasta o lo\r\n"
        "azzeri, questa e' l'unica cosa che permette di rimettere tutto com'era.\r\n"
        "Chi ha questa chiave e un salvataggio cifrato ha anche il permesso di\r\n"
        "entrare nel tuo Home Assistant: trattala come una password.\r\n",
        nc.hostname[0] ? nc.hostname : "pannello", k);
    return httpd_resp_send(r, testo, n);
}

static esp_err_t h_backup_get(httpd_req_t *r)
{
    if (!allow(r, WEB_ROLE_ADMIN)) return ESP_OK;

    bool cifrato = true;
    char q[48];
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) == ESP_OK) {
        char v[8];
        if (httpd_query_key_value(q, "cifrato", v, sizeof(v)) == ESP_OK)
            cifrato = !(v[0] == '0' || v[0] == 'n');
    }

    char *j = backup_esporta(cifrato);
    if (!j) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "esportazione");

    net_config_t nc;
    net_config_load(&nc);
    char nome[128];
    snprintf(nome, sizeof(nome), "attachment; filename=\"%s-%s.json\"",
             nc.hostname[0] ? nc.hostname : "pannello", cifrato ? "cifrato" : "chiaro");
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Content-Disposition", nome);
    esp_err_t e = httpd_resp_send(r, j, strlen(j));
    free(j);
    return e;
}

static esp_err_t h_backup_post(httpd_req_t *r)
{
    if (!allow(r, WEB_ROLE_ADMIN)) return ESP_OK;
    char *body = recv_body(r);
    if (!body) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "body");

    /* Il corpo e' {"file": <testo del salvataggio>, "chiave": "..."}: la
       chiave si manda solo se il file viene da un altro pannello. */
    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "json");
    const char *file   = jstr(j, "file");
    const char *chiave = jstr(j, "chiave");

    char msg[160] = "";
    bool ok = file && backup_importa(file, chiave, msg, sizeof(msg));
    if (!file) snprintf(msg, sizeof(msg), "Nessun file.");
    cJSON_Delete(j);

    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", ok);
    cJSON_AddStringToObject(out, "messaggio", msg);
    if (!ok) httpd_resp_set_status(r, "400 Bad Request");
    return send_json(r, out);
}

/* Manda un file come allegato, a fette.

   A fette e non in un colpo solo per due motivi diversi. Il primo e' la
   memoria: un file sulla SD puo' essere di megabyte e in RAM non ci sta. Il
   secondo e' il collegamento verso il C6: una raffica grossa sull'SDIO e'
   esattamente quello che lo fa cadere, e la fetta da due kilobyte lascia
   respirare il bus fra una scrittura e l'altra.

   La fetta sta in PSRAM: questo gestore gira sul task del server web, che di
   stack non ne ha da sprecare. */
static esp_err_t manda_file(httpd_req_t *r, const char *path, const char *nome,
                            const char *tipo)
{
    FILE *f = fopen(path, "rb");
    if (!f) return httpd_resp_send_err(r, HTTPD_404_NOT_FOUND, strerror(errno));

    const size_t FETTA = 2048;
    char *buf = (char *)heap_caps_malloc(FETTA, MALLOC_CAP_SPIRAM);
    if (!buf) {
        fclose(f);
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "memoria");
    }

    char disp[160];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", nome);
    httpd_resp_set_type(r, tipo);
    httpd_resp_set_hdr(r, "Content-Disposition", disp);

    bool rotto = false;
    size_t n;
    while ((n = fread(buf, 1, FETTA, f)) > 0) {
        if (httpd_resp_send_chunk(r, buf, n) != ESP_OK) { rotto = true; break; }
    }
    fclose(f);
    heap_caps_free(buf);
    if (!rotto) httpd_resp_send_chunk(r, NULL, 0);     // fine della risposta
    return ESP_OK;
}

/* Il registro della batteria. Resta un indirizzo suo, anche se ora l'esploratore
   lo troverebbe da solo: e' quello che si scrive in un appunto o in un comando
   curl, e un indirizzo stabile vale piu' di un percorso da cercare. */
static esp_err_t h_batteria_csv(httpd_req_t *r)
{
    if (!allow(r, WEB_ROLE_ADMIN)) return ESP_OK;
    bool prec = false;
    char q[48] = "", v[8];
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "prec", v, sizeof(v)) == ESP_OK)
        prec = (v[0] == '1');
    return manda_file(r, prec ? "/sdcard/batteria.1.csv" : "/sdcard/batteria.csv",
                      prec ? "batteria-precedente.csv" : "batteria.csv",
                      "text/csv; charset=utf-8");
}

/* Il percorso arriva dentro un indirizzo web, quindi con gli spazi e le lettere
   accentate codificati in %XX. httpd_query_key_value non li scioglie, e un
   percorso lasciato codificato semplicemente non si apre: un file che si chiama
   "foto di casa.jpg" arriverebbe come "foto%20di%20casa.jpg". */
static void sciogli_url(char *s)
{
    char *w = s;
    for (char *p = s; *p; p++) {
        if (*p == '+') { *w++ = ' '; continue; }
        if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char due[3] = { p[1], p[2], 0 };
            *w++ = (char)strtol(due, nullptr, 16);
            p += 2;
            continue;
        }
        *w++ = *p;
    }
    *w = 0;
}

/* Legge il parametro "percorso" e lo verifica. Ritorna false - avendo gia'
   risposto con l'errore - se manca o se non e' ammesso. Ogni gestore che tocca
   il filesystem passa da qui: e' l'unico punto in cui un percorso venuto da
   fuori diventa un percorso vero. */
static bool prendi_percorso(httpd_req_t *r, char *out, size_t sz)
{
    char q[420] = "";
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "percorso", out, sz) != ESP_OK) {
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "manca percorso");
        return false;
    }
    sciogli_url(out);
    if (!esplora_permesso(out)) {
        /* Non si dice perche': che sia ".." o una radice non consentita non
           interessa a chi ha diritto di stare qui, e a chi non ce l'ha non si
           regalano indizi. */
        httpd_resp_send_err(r, HTTPD_403_FORBIDDEN, "percorso non ammesso");
        return false;
    }
    return true;
}

#define ESPLORA_VOCI 250

// L'elenco di una cartella, piu' lo spazio della radice che la contiene.
static esp_err_t h_file_elenco(httpd_req_t *r)
{
    if (!allow(r, WEB_ROLE_ADMIN)) return ESP_OK;
    char percorso[300];
    if (!prendi_percorso(r, percorso, sizeof(percorso))) return ESP_OK;

    esplora_voce_t *v = (esplora_voce_t *)heap_caps_malloc(
        sizeof(esplora_voce_t) * ESPLORA_VOCI, MALLOC_CAP_SPIRAM);
    if (!v) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "memoria");

    bool troncato = false;
    int n = esplora_elenca(percorso, v, ESPLORA_VOCI, &troncato);
    if (n < 0) {
        heap_caps_free(v);
        return httpd_resp_send_err(r, HTTPD_404_NOT_FOUND, "cartella non apribile");
    }

    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "percorso", percorso);
    cJSON_AddBoolToObject(out, "troncato", troncato);
    uint64_t tot = 0, lib = 0;
    if (esplora_spazio(percorso, &tot, &lib)) {
        cJSON_AddNumberToObject(out, "spazio_totale", (double)tot);
        cJSON_AddNumberToObject(out, "spazio_libero", (double)lib);
    }
    cJSON *radici = cJSON_AddArrayToObject(out, "radici");
    for (int i = 0; esplora_radice(i); i++)
        cJSON_AddItemToArray(radici, cJSON_CreateString(esplora_radice(i)));

    cJSON *voci = cJSON_AddArrayToObject(out, "voci");
    for (int i = 0; i < n; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "nome", v[i].nome);
        cJSON_AddBoolToObject(e, "cartella", v[i].cartella);
        cJSON_AddNumberToObject(e, "byte", v[i].byte);
        cJSON_AddNumberToObject(e, "quando", (double)v[i].quando);
        cJSON_AddItemToArray(voci, e);
    }
    heap_caps_free(v);
    return send_json(r, out);
}

static esp_err_t h_file_scarica(httpd_req_t *r)
{
    if (!allow(r, WEB_ROLE_ADMIN)) return ESP_OK;
    char percorso[300];
    if (!prendi_percorso(r, percorso, sizeof(percorso))) return ESP_OK;

    struct stat st = {};
    if (stat(percorso, &st) != 0)
        return httpd_resp_send_err(r, HTTPD_404_NOT_FOUND, "non c'e'");
    if (S_ISDIR(st.st_mode))
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "e' una cartella");

    const char *nome = strrchr(percorso, '/');
    nome = nome ? nome + 1 : percorso;
    /* Tipo generico: il browser lo salva invece di provare a mostrarlo, che
       per un esploratore di file e' quello che si vuole. */
    return manda_file(r, percorso, nome, "application/octet-stream");
}

/* Mettere un file sul pannello, sulla SD o in /spiffs.

   Sul /spiffs ci avevo messo un divieto, ragionando che li' dentro c'e' la
   configurazione del pannello e sovrascriverla con un clic vorrebbe dire
   perdere l'indirizzo di Home Assistant. Il divieto e' caduto quando e'
   arrivato l'editor di testo: il suo mestiere e' proprio aprire un file di
   configurazione, correggerlo e rimetterlo a posto, e un editor che non puo'
   salvare dove il file sta non serve a niente.

   Quello che protegge davvero e' altro: ci arriva solo un amministratore, che
   da questa stessa pagina puo' gia' rimettere un intero salvataggio, e il
   percorso lo scrive per esteso. La riga che vietava /spiffs non aggiungeva
   sicurezza, toglieva solo un uso legittimo.

   Il corpo si scrive a fette direttamente sul file: recv_body tiene tutto in
   memoria e si ferma a 96 kB, che per una foto non basta. */
static esp_err_t h_file_carica(httpd_req_t *r)
{
    if (!allow(r, WEB_ROLE_ADMIN)) return ESP_OK;
    char percorso[300];
    if (!prendi_percorso(r, percorso, sizeof(percorso))) return ESP_OK;

    size_t lung = strlen(percorso);
    if (lung == 0 || percorso[lung - 1] == '/')
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "manca il nome del file");

    /* Non si comincia a scrivere se non ci sta: meglio un rifiuto subito che una
       scheda piena e un file mozzato. Il margine di 256 kB lascia respiro al
       registro della batteria e alla cache delle immagini. */
    uint64_t tot = 0, lib = 0;
    int atteso = r->content_len;
    if (atteso <= 0) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "corpo vuoto");
    if (esplora_spazio(percorso, &tot, &lib) &&
        (uint64_t)atteso + 256 * 1024 > lib) {
        char msg[96];
        snprintf(msg, sizeof(msg), "non ci sta: %u kB liberi", (unsigned)(lib / 1024));
        return httpd_resp_send_err(r, HTTPD_413_CONTENT_TOO_LARGE, msg);
    }

    /* Si scrive su un nome provvisorio e si rinomina alla fine: una connessione
       che cade a meta' non lascia al suo posto un file mozzato che sembra buono.
       E' la stessa precauzione della cache delle immagini. */
    char tmp[320];
    snprintf(tmp, sizeof(tmp), "%s.tmp", percorso);
    FILE *f = fopen(tmp, "wb");
    if (!f) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, strerror(errno));

    const size_t FETTA = 2048;
    char *buf = (char *)heap_caps_malloc(FETTA, MALLOC_CAP_SPIRAM);
    if (!buf) {
        fclose(f); unlink(tmp);
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "memoria");
    }

    int restano = atteso;
    bool ok = true;
    while (restano > 0) {
        int quanti = restano < (int)FETTA ? restano : (int)FETTA;
        int letti = httpd_req_recv(r, buf, quanti);
        if (letti <= 0) { ok = false; break; }
        if (fwrite(buf, 1, letti, f) != (size_t)letti) { ok = false; break; }
        restano -= letti;
    }
    fclose(f);
    heap_caps_free(buf);

    if (ok) {
        unlink(percorso);
        ok = rename(tmp, percorso) == 0;
    }
    if (!ok) unlink(tmp);

    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", ok);
    cJSON_AddStringToObject(out, "messaggio",
                            ok ? "caricato" : "caricamento non riuscito");
    if (ok) ESP_LOGI(TAG, "caricato %s (%d byte)", percorso, atteso);
    else    httpd_resp_set_status(r, "500 Internal Server Error");
    return send_json(r, out);
}

/* Cancellare un file.

   La conferma non sta qui ma nella pagina, dove si puo' far vedere il nome di
   quello che sta per sparire: una domanda che non dice su cosa si sta
   decidendo non fa decidere niente. Qui si controlla solo che il percorso sia
   ammesso e si esegue.

   Solo file e cartelle vuote: una cartella piena non si svuota per sbaglio con
   un clic. */
static esp_err_t h_file_elimina(httpd_req_t *r)
{
    if (!allow(r, WEB_ROLE_ADMIN)) return ESP_OK;
    char percorso[300];
    if (!prendi_percorso(r, percorso, sizeof(percorso))) return ESP_OK;

    bool ok = esplora_elimina(percorso);
    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", ok);
    cJSON_AddStringToObject(out, "messaggio",
                            ok ? "cancellato"
                               : "non si cancella (non c'e', oppure e' una cartella piena)");
    if (!ok) httpd_resp_set_status(r, "409 Conflict");
    return send_json(r, out);
}

/* La memoria NVS. Sola lettura, e dei segreti si vede che esistono e non il
   valore: il permesso di Home Assistant apre tutta la casa, su una pagina web
   non ci va. Chi e' segreto lo decide backup_e_segreto(), lo stesso elenco del
   salvataggio in chiaro. */
static esp_err_t h_nvs(httpd_req_t *r)
{
    if (!allow(r, WEB_ROLE_ADMIN)) return ESP_OK;

    const int MAX = 220;
    esplora_nvs_t *v = (esplora_nvs_t *)heap_caps_malloc(
        sizeof(esplora_nvs_t) * MAX, MALLOC_CAP_SPIRAM);
    if (!v) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "memoria");

    bool troncato = false;
    int n = esplora_nvs(v, MAX, &troncato);
    if (n < 0) {
        heap_caps_free(v);
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs");
    }

    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "troncato", troncato);
    cJSON *voci = cJSON_AddArrayToObject(out, "voci");
    for (int i = 0; i < n; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "spazio", v[i].spazio);
        cJSON_AddStringToObject(e, "chiave", v[i].chiave);
        cJSON_AddStringToObject(e, "tipo",   v[i].tipo);
        cJSON_AddBoolToObject(e,   "segreto", v[i].segreto);
        cJSON_AddNumberToObject(e, "byte",   v[i].byte);
        cJSON_AddStringToObject(e, "valore", v[i].valore);
        cJSON_AddItemToArray(voci, e);
    }
    heap_caps_free(v);
    return send_json(r, out);
}

static const httpd_uri_t ROUTES[] = {
    {"/",              HTTP_GET,  h_page,        NULL},
    {"/setup",         HTTP_GET,  h_page,        NULL},
    {"/api/session",   HTTP_GET,  h_session,     NULL},
    {"/api/setup",     HTTP_POST, h_setup,       NULL},
    {"/api/login",     HTTP_POST, h_login,       NULL},
    {"/auth/start",    HTTP_GET,  h_auth_start,  NULL},
    {"/auth/callback", HTTP_GET,  h_auth_callback, NULL},
    {"/api/logout",    HTTP_POST, h_logout,      NULL},
    {"/api/passwords", HTTP_POST, h_passwords,   NULL},
    {"/api/config",    HTTP_GET,  h_get_config,  NULL},
    {"/api/config",    HTTP_POST, h_post_config, NULL},
    {"/api/layout",    HTTP_GET,  h_get_layout,  NULL},
    {"/api/screenshot",HTTP_GET,  h_screenshot,  NULL},
    {"/api/overrides", HTTP_GET,  h_get_ovr,     NULL},
    {"/api/overrides", HTTP_PUT,  h_put_ovr,     NULL},
    {"/api/reload",    HTTP_POST, h_reload,      NULL},
    {"/api/entities",  HTTP_GET,  h_entities,    NULL},
    {"/api/standby",   HTTP_GET,  h_get_standby, NULL},
    {"/api/standby",   HTTP_POST, h_post_standby,NULL},
    {"/api/standby/locale", HTTP_POST, h_standby_release, NULL},
    {"/api/chiave",    HTTP_GET,  h_chiave,      NULL},
    {"/api/backup",    HTTP_GET,  h_backup_get,  NULL},
    {"/api/backup",    HTTP_POST, h_backup_post, NULL},
    {"/api/batteria.csv", HTTP_GET, h_batteria_csv, NULL},
    {"/api/file",      HTTP_GET,  h_file_elenco,  NULL},
    {"/api/scarica",   HTTP_GET,  h_file_scarica, NULL},
    {"/api/carica",    HTTP_POST, h_file_carica,  NULL},
    {"/api/nvs",       HTTP_GET,  h_nvs,          NULL},
    {"/api/elimina",   HTTP_POST, h_file_elimina, NULL},
};

/* Il nostro indirizzo IP in forma di testo, stringa vuota se non ce l'abbiamo. */
static void mio_ip(char *out, size_t sz)
{
    esp_netif_ip_info_t ip = {};
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (nif && esp_netif_get_ip_info(nif, &ip) == ESP_OK && ip.ip.addr)
        snprintf(out, sz, IPSTR, IP2STR(&ip.ip));
    else
        out[0] = 0;
}

/* Questo nome porta davvero a noi?

   L'intestazione "Host" la scrive chi chiama, non il pannello: riscriverla
   dentro "Location" senza guardarla vuol dire lasciare che un estraneo si
   faccia emettere dal pannello un rimando verso dove gli pare. Si accettano
   soltanto i tre nomi con cui si arriva davvero qui: il nostro indirizzo IP,
   il nostro nome di rete e lo stesso nome con .local (quello che risponde a
   mDNS). Qualunque altra cosa viene ignorata e si rimanda all'IP. */
static bool nome_nostro(const char *host)
{
    if (!host || !host[0]) return false;

    char ip[16];
    mio_ip(ip, sizeof(ip));
    if (ip[0] && !strcmp(host, ip)) return true;

    net_config_t nc;
    net_config_load(&nc);
    const char *nome = nc.hostname[0] ? nc.hostname : "pannello";
    if (!strcasecmp(host, nome)) return true;

    char locale[sizeof(nc.hostname) + 8];
    snprintf(locale, sizeof(locale), "%s.local", nome);
    return strcasecmp(host, locale) == 0;
}

/* Chi arriva in chiaro sulla porta 80 viene mandato all'indirizzo sicuro. */
static esp_err_t h_redirect(httpd_req_t *r)
{
    char host[80] = "";
    if (httpd_req_get_hdr_value_str(r, "Host", host, sizeof(host)) != ESP_OK)
        host[0] = 0;

    char *colon = strchr(host, ':');
    if (colon) *colon = 0;                     // via la porta 80

    if (!nome_nostro(host)) {
        /* Nome sconosciuto (o assente): si rimanda al nostro indirizzo, che e'
           l'unica cosa che sappiamo per certo essere noi. */
        mio_ip(host, sizeof(host));
        if (!host[0]) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR,
                                                 "Il pannello non ha ancora un indirizzo.");
    }

    char loc[128];
    snprintf(loc, sizeof(loc), "https://%s/", host);
    httpd_resp_set_status(r, "301 Moved Permanently");
    httpd_resp_set_hdr(r, "Location", loc);
    return httpd_resp_send(r, NULL, 0);
}

static bool have_ip(void)
{
    esp_netif_ip_info_t ip = {};
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    return nif && esp_netif_get_ip_info(nif, &ip) == ESP_OK && ip.ip.addr != 0;
}

static void start_task(void *arg)
{
    /* Il certificato porta dentro l'indirizzo del pannello: aspetto che la rete
       sia in piedi, altrimenti lo genererei per 0.0.0.0 e poi da rifare. */
    for (int i = 0; i < 120 && !have_ip(); i++) vTaskDelay(pdMS_TO_TICKS(500));

    const char *cert, *key;
    size_t cert_len, key_len;
    if (!web_cert_get(&cert, &cert_len, &key, &key_len)) {
        ESP_LOGE(TAG, "senza certificato non parte l'HTTPS");
        vTaskDelete(NULL);
        return;
    }

    httpd_ssl_config_t cfg = HTTPD_SSL_CONFIG_DEFAULT();
    cfg.servercert     = (const uint8_t *)cert;
    cfg.servercert_len = cert_len;
    cfg.prvtkey_pem    = (const uint8_t *)key;
    cfg.prvtkey_len    = key_len;
    cfg.httpd.max_uri_handlers  = sizeof(ROUTES) / sizeof(ROUTES[0]) + 2;
    cfg.httpd.stack_size        = 10240;     // cJSON, TLS e il lock del display
    /* Un browser apre piu' connessioni in parallelo per la stessa pagina: con
       troppo poche prese, le richieste in coda sembrano non rispondere. */
    cfg.httpd.max_open_sockets  = 5;
    cfg.httpd.recv_wait_timeout = 8;
    cfg.httpd.send_wait_timeout = 8;
    cfg.httpd.lru_purge_enable  = true;

    httpd_handle_t srv = NULL;
    if (httpd_ssl_start(&srv, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ssl_start fallito");
        vTaskDelete(NULL);
        return;
    }
    for (const httpd_uri_t &u : ROUTES) httpd_register_uri_handler(srv, &u);

    // porta 80: solo il rimando
    httpd_config_t plain = HTTPD_DEFAULT_CONFIG();
    plain.server_port      = 80;
    /* Porta di controllo diversa da quelle dei server gia' in piedi: il server
       HTTPS usa la 32768 e la 32769, e sovrapponendosi httpd_start fallisce
       con "error in creating ctrl socket". */
    plain.ctrl_port        = 32770;
    plain.max_uri_handlers = 2;
    plain.max_open_sockets = 2;      // risponde solo "vai su https", non serve altro
    plain.uri_match_fn     = httpd_uri_match_wildcard;
    httpd_handle_t plain_srv = NULL;
    if (httpd_start(&plain_srv, &plain) == ESP_OK) {
        httpd_uri_t any = {"/*", HTTP_GET, h_redirect, NULL};
        httpd_register_uri_handler(plain_srv, &any);
    } else {
        ESP_LOGW(TAG, "porta 80 non attiva: chi scrive http:// non verra' rimandato");
    }

    char fp[128];
    web_cert_fingerprint(fp, sizeof(fp));
    ESP_LOGI(TAG, "pagina su https, impronta del certificato %s", fp);
    vTaskDelete(NULL);
}

void web_config_start(web_cfg_changed_cb_t on_cfg, web_layout_changed_cb_t on_layout)
{
    s_on_cfg = on_cfg; s_on_layout = on_layout;
    web_auth_init();
    xTaskCreate(start_task, "web_start", 8192, NULL, 3, NULL);
}
