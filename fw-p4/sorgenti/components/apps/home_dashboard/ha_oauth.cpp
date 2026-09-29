#include "ha_oauth.h"
#include "ha_config.h"
#include "web_auth.h"
#include "ha_http.h"
#include "ha_token.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_websocket_client.h"
#include "cJSON.h"

static const char *TAG = "ha_oauth";

#define STATE_LEN   32
#define STATE_US    (5 * 60 * 1000000LL)      // il codice va usato entro 5 minuti

static char    s_state[STATE_LEN + 1];
static int64_t s_state_until = 0;

// ------------------------------------------------------------------ indirizzi

bool ha_oauth_available(void)
{
    char base[160], me[64];
    return ha_config_is_set() && ha_http_base(base, sizeof(base)) &&
           ha_http_panel_url(me, sizeof(me), "/");
}

bool ha_oauth_start_url(char *out, size_t out_sz)
{
    char base[160], me[64], cb[80];
    if (!ha_http_base(base, sizeof(base))) return false;
    if (!ha_http_panel_url(me, sizeof(me), "/")) return false;
    if (!ha_http_panel_url(cb, sizeof(cb), "/auth/callback")) return false;

    static const char hexd[] = "0123456789abcdef";
    uint8_t raw[STATE_LEN / 2];
    esp_fill_random(raw, sizeof(raw));
    for (size_t i = 0; i < sizeof(raw); i++) {
        s_state[i * 2]     = hexd[raw[i] >> 4];
        s_state[i * 2 + 1] = hexd[raw[i] & 0xf];
    }
    s_state[STATE_LEN] = 0;
    s_state_until = esp_timer_get_time() + STATE_US;

    char me_e[192], cb_e[240];
    ha_http_url_encode(me, me_e, sizeof(me_e));
    ha_http_url_encode(cb, cb_e, sizeof(cb_e));
    snprintf(out, out_sz, "%s/auth/authorize?client_id=%s&redirect_uri=%s&state=%s",
             base, me_e, cb_e, s_state);
    return true;
}

// ------------------------------------------------------------------ chi ha autorizzato

/* Una connessione WebSocket breve col token dell'utente, giusto per chiedere
   "auth/current_user": e' l'unico modo che HA offre per sapere chi e'. */
#define BIT_OK   BIT0
#define BIT_FAIL BIT1
#define BIT_CONN BIT2      // il collegamento c'e': HA ha chiesto di autenticarsi

struct WhoAmI {
    EventGroupHandle_t eg;
    char  token[HA_TOKEN_MAX];
    char  name[64];
    bool  is_admin;
    bool  sent_query;
};

static void who_event(void *arg, esp_event_base_t, int32_t id, void *data)
{
    WhoAmI *w = (WhoAmI *)arg;
    esp_websocket_event_data_t *d = (esp_websocket_event_data_t *)data;

    if (id == WEBSOCKET_EVENT_ERROR || id == WEBSOCKET_EVENT_DISCONNECTED) {
        ESP_LOGW(TAG, "who_am_i: collegamento caduto (evento %d)", (int)id);
        xEventGroupSetBits(w->eg, BIT_FAIL);
        return;
    }
    if (id != WEBSOCKET_EVENT_DATA || d->op_code != 0x01 || d->data_len <= 0) return;

    char *txt = (char *)malloc(d->data_len + 1);
    if (!txt) return;
    memcpy(txt, d->data_ptr, d->data_len);
    txt[d->data_len] = 0;
    cJSON *j = cJSON_Parse(txt);
    free(txt);
    if (!j) return;

    const cJSON *type = cJSON_GetObjectItem(j, "type");
    const char *t = cJSON_IsString(type) ? type->valuestring : "";
    /* Una riga per messaggio: quando l'accesso non riesce, e' questa a dire a
       che punto si e' fermato. Il token non passa di qui. */
    ESP_LOGI(TAG, "who_am_i: HA dice \"%s\"", t);

    if (!strcmp(t, "auth_required")) {
        xEventGroupSetBits(w->eg, BIT_CONN);
        char *msg = (char *)malloc(strlen(w->token) + 48);
        if (msg) {
            sprintf(msg, "{\"type\":\"auth\",\"access_token\":\"%s\"}", w->token);
            esp_websocket_client_send_text(d->client, msg, strlen(msg), portMAX_DELAY);
            free(msg);
        }
    } else if (!strcmp(t, "auth_ok") && !w->sent_query) {
        w->sent_query = true;
        const char *q = "{\"id\":1,\"type\":\"auth/current_user\"}";
        esp_websocket_client_send_text(d->client, q, strlen(q), portMAX_DELAY);
    } else if (!strcmp(t, "auth_invalid")) {
        xEventGroupSetBits(w->eg, BIT_FAIL);
    } else if (!strcmp(t, "result")) {
        const cJSON *res = cJSON_GetObjectItem(j, "result");
        const cJSON *nm  = cJSON_GetObjectItem(res, "name");
        const cJSON *ad  = cJSON_GetObjectItem(res, "is_admin");
        if (cJSON_IsString(nm)) strlcpy(w->name, nm->valuestring, sizeof(w->name));
        w->is_admin = cJSON_IsTrue(ad);
        xEventGroupSetBits(w->eg, cJSON_IsObject(res) ? BIT_OK : BIT_FAIL);
    }
    cJSON_Delete(j);
}

static bool who_am_i(const char *access_token, char *name, size_t name_sz, bool *is_admin)
{
    /* Serve solo l'indirizzo. ha_config_load pretende anche il long-lived
       token e senza quello fallisce, ma qui il token ce l'abbiamo gia' in
       mano: e' quello appena ottenuto dall'autorizzazione. Un pannello
       abbinato dal telefono, che il long-lived non ce l'ha mai avuto, non
       sarebbe mai riuscito ad accedere con l'account di Home Assistant. */
    char url[HA_URL_MAX];
    if (!ha_config_load_url(url, sizeof(url)) || !url[0]) return false;

    WhoAmI w = {};
    w.eg = xEventGroupCreate();
    if (!w.eg) return false;
    strlcpy(w.token, access_token, sizeof(w.token));

    esp_websocket_client_config_t cfg = {};
    cfg.uri = url;
    cfg.disable_auto_reconnect = true;
    cfg.network_timeout_ms = 8000;
    cfg.buffer_size = 2048;
    cfg.task_stack  = 6144;

    esp_websocket_client_handle_t cl = esp_websocket_client_init(&cfg);
    bool ok = false;
    if (cl) {
        esp_websocket_register_events(cl, WEBSOCKET_EVENT_ANY, who_event, &w);
        if (esp_websocket_client_start(cl) == ESP_OK) {
            /* Due tempi separati, e non uno solo per tutto il giro.

               Con un tempo solo da dodici secondi non si entrava mai, e il
               registro diceva perche': aprire il collegamento verso HA ne
               prendeva dieci da solo - il pannello ne ha gia' uno aperto verso
               lo stesso indirizzo, e passare dal C6 via SDIO non e' gratis;
               un paio di volte la connessione e' perfino fallita con "host
               irraggiungibile" e ha dovuto ripetere. Dei dodici secondi ne
               restavano due per l'autenticazione vera, e l'errore che ne usciva
               dava la colpa a Home Assistant, che invece non c'entrava.

               Adesso il collegamento ha il suo tempo e la risposta il suo. */
            int64_t t0 = esp_timer_get_time();
            EventBits_t b1 = xEventGroupWaitBits(w.eg, BIT_CONN | BIT_FAIL, pdFALSE, pdFALSE,
                                                 pdMS_TO_TICKS(20000));
            if (!(b1 & BIT_CONN)) {
                ESP_LOGW(TAG, "who_am_i: %s non si e' collegato entro 20 s", url);
            } else {
                ESP_LOGI(TAG, "who_am_i: collegato in %d ms, chiedo chi e'",
                         (int)((esp_timer_get_time() - t0) / 1000));
                EventBits_t b2 = xEventGroupWaitBits(w.eg, BIT_OK | BIT_FAIL, pdTRUE, pdFALSE,
                                                     pdMS_TO_TICKS(10000));
                ok = (b2 & BIT_OK) != 0;
                if (!b2) ESP_LOGW(TAG, "who_am_i: collegato, ma nessuna risposta "
                                       "all'autenticazione entro 10 s");
            }
        } else {
            ESP_LOGW(TAG, "who_am_i: non sono riuscito ad aprire %s", url);
        }
        esp_websocket_client_stop(cl);
        esp_websocket_client_destroy(cl);
    }
    vEventGroupDelete(w.eg);
    if (ok) {
        strlcpy(name, w.name[0] ? w.name : "utente", name_sz);
        *is_admin = w.is_admin;
    }
    return ok;
}

// ------------------------------------------------------------------ ritorno da HA

bool ha_oauth_finish(const char *code, const char *state,
                     char *sid, size_t sid_sz,
                     char *name, size_t name_sz,
                     char *err, size_t err_sz)
{
    if (!code || !state || !s_state[0] || strcmp(state, s_state) ||
        esp_timer_get_time() > s_state_until) {
        s_state[0] = 0;
        strlcpy(err, "Richiesta scaduta o non valida: riprova dall'inizio.", err_sz);
        return false;
    }
    s_state[0] = 0;                       // monouso

    char base[160], me[64], me_e[192];
    if (!ha_http_base(base, sizeof(base)) || !ha_http_panel_url(me, sizeof(me), "/")) {
        strlcpy(err, "Home Assistant non e' configurato.", err_sz);
        return false;
    }
    ha_http_url_encode(me, me_e, sizeof(me_e));

    char url[200], body[600];
    char code_e[300];
    ha_http_url_encode(code, code_e, sizeof(code_e));
    snprintf(url, sizeof(url), "%s/auth/token", base);
    snprintf(body, sizeof(body), "grant_type=authorization_code&code=%s&client_id=%s",
             code_e, me_e);

    char *resp = (char *)malloc(2048);
    if (!resp) { strlcpy(err, "Memoria insufficiente.", err_sz); return false; }
    int status = ha_http_post_form(url, body, resp, 2048);
    if (status != 200) {
        ESP_LOGW(TAG, "scambio del codice fallito: stato %d", status);
        snprintf(err, err_sz, "Home Assistant ha rifiutato l'accesso (codice %d).", status);
        free(resp);
        return false;
    }

    cJSON *j = cJSON_Parse(resp);
    free(resp);
    const cJSON *at = cJSON_GetObjectItem(j, "access_token");
    const cJSON *rt = cJSON_GetObjectItem(j, "refresh_token");
    if (!cJSON_IsString(at)) {
        cJSON_Delete(j);
        strlcpy(err, "Risposta di Home Assistant non capita.", err_sz);
        return false;
    }

    bool is_admin = false;
    bool ok = who_am_i(at->valuestring, name, name_sz, &is_admin);

    /* Se il pannello non e' ancora abbinato e ad autorizzare e' stato un
       amministratore, il permesso lo teniamo: da li' in poi il pannello si
       genera da solo i token per parlare con HA, senza niente da incollare.
       Si revoca da Home Assistant, in Profilo > Sessioni. */
    bool paired = false;
    if (ok && is_admin && cJSON_IsString(rt) && !ha_token_have_refresh()) {
        paired = ha_token_set_refresh(rt->valuestring);
        if (paired) ESP_LOGI(TAG, "pannello abbinato a Home Assistant");
    }

    /* Altrimenti il token dell'utente non ci serve: lo restituiamo a HA. */
    if (!paired && cJSON_IsString(rt)) {
        char rbody[600], rt_e[400];
        ha_http_url_encode(rt->valuestring, rt_e, sizeof(rt_e));
        snprintf(rbody, sizeof(rbody), "action=revoke&token=%s", rt_e);
        char *dummy = (char *)malloc(512);
        if (dummy) { ha_http_post_form(url, rbody, dummy, 512); free(dummy); }
    }
    cJSON_Delete(j);

    if (!ok) {
        strlcpy(err, "Non sono riuscito a chiedere a Home Assistant chi sei.", err_sz);
        return false;
    }

    web_role_t role = is_admin ? WEB_ROLE_ADMIN : WEB_ROLE_GUEST;
    if (!web_auth_grant(role, sid, sid_sz)) {
        strlcpy(err, "Non sono riuscito ad aprire la sessione.", err_sz);
        return false;
    }
    ESP_LOGI(TAG, "accesso con l'account di Home Assistant: %s (%s)",
             name, is_admin ? "amministratore" : "ospite");
    return true;
}
