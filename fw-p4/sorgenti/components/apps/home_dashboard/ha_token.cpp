#include "ha_token.h"
#include "ha_http.h"
#include "ha_config.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "cJSON.h"

static const char *TAG = "ha_token";
static const char *NS  = "hapanel";
static const char *K_REFRESH = "ha_refresh";

static char    s_access[HA_TOKEN_MAX];
static int64_t s_access_until = 0;

static bool load_refresh(char *out, size_t sz)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = sz;
    esp_err_t err = nvs_get_str(h, K_REFRESH, out, &len);
    nvs_close(h);
    return err == ESP_OK && len > 1;
}

bool ha_token_have_refresh(void)
{
    char rt[HA_TOKEN_MAX];
    return load_refresh(rt, sizeof(rt));
}

bool ha_token_set_refresh(const char *refresh_token)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err;
    if (!refresh_token || !refresh_token[0]) {
        err = nvs_erase_key(h, K_REFRESH);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    } else {
        err = nvs_set_str(h, K_REFRESH, refresh_token);
    }
    err |= nvs_commit(h);
    nvs_close(h);
    s_access[0] = 0;
    s_access_until = 0;
    ESP_LOGI(TAG, "abbinamento a Home Assistant %s",
             refresh_token && refresh_token[0] ? "salvato" : "rimosso");
    return err == ESP_OK;
}

void ha_token_invalidate(void)
{
    s_access[0] = 0;
    s_access_until = 0;
}

/* Restituisce il permesso a Home Assistant, cosi' il pannello sparisce da
   Profilo > Sessioni invece di restarci come voce morta. Se HA non risponde
   il permesso lo cancelliamo comunque: e' un ritorno alle impostazioni
   iniziali, la memoria locale va svuotata in ogni caso. */
void ha_token_revoke_refresh(void)
{
    char rt[HA_TOKEN_MAX];
    if (!load_refresh(rt, sizeof(rt))) return;

    char base[160], url[200], rt_e[600];
    if (ha_http_base(base, sizeof(base))) {
        snprintf(url, sizeof(url), "%s/auth/token", base);
        ha_http_url_encode(rt, rt_e, sizeof(rt_e));
        char *body = (char *)malloc(1024);
        char *resp = (char *)malloc(512);
        if (body && resp) {
            snprintf(body, 1024, "action=revoke&token=%s", rt_e);
            int status = ha_http_post_form(url, body, resp, 512);
            ESP_LOGI(TAG, "abbinamento revocato in Home Assistant (codice %d)", status);
        }
        free(body);
        free(resp);
    }
    ha_token_set_refresh(NULL);
}

/* Chiede a HA un token di accesso nuovo partendo dal permesso salvato. */
static bool renew(void)
{
    char rt[HA_TOKEN_MAX];
    if (!load_refresh(rt, sizeof(rt))) return false;

    char base[160], me[64], me_e[192], rt_e[600];
    if (!ha_http_base(base, sizeof(base)) || !ha_http_panel_url(me, sizeof(me), "/")) return false;
    ha_http_url_encode(me, me_e, sizeof(me_e));
    ha_http_url_encode(rt, rt_e, sizeof(rt_e));

    char url[200];
    snprintf(url, sizeof(url), "%s/auth/token", base);
    char *body = (char *)malloc(1024);
    char *resp = (char *)malloc(2048);
    bool ok = false;
    if (body && resp) {
        snprintf(body, 1024, "grant_type=refresh_token&refresh_token=%s&client_id=%s", rt_e, me_e);
        int status = ha_http_post_form(url, body, resp, 2048);
        if (status == 200) {
            cJSON *j = cJSON_Parse(resp);
            const cJSON *at = cJSON_GetObjectItem(j, "access_token");
            const cJSON *ex = cJSON_GetObjectItem(j, "expires_in");
            if (cJSON_IsString(at)) {
                strlcpy(s_access, at->valuestring, sizeof(s_access));
                int secs = cJSON_IsNumber(ex) ? ex->valueint : 1800;
                if (secs > 120) secs -= 60;          // rinnovo un minuto prima
                s_access_until = esp_timer_get_time() + (int64_t)secs * 1000000LL;
                ok = true;
                ESP_LOGI(TAG, "token di accesso rinnovato (vale %d s)", secs);
            }
            cJSON_Delete(j);
        } else {
            ESP_LOGW(TAG, "rinnovo non riuscito: stato %d", status);
            /* Un 400 qui vuol dire che il permesso e' stato revocato in HA
               (Profilo > Sessioni): inutile ritentare col vecchio. */
            if (status == 400) ha_token_set_refresh(NULL);
        }
    }
    free(body);
    free(resp);
    return ok;
}

bool ha_token_get_access(char *out, size_t out_sz)
{
    if (ha_token_have_refresh()) {
        if (s_access[0] && esp_timer_get_time() < s_access_until) {
            strlcpy(out, s_access, out_sz);
            return true;
        }
        if (renew()) {
            strlcpy(out, s_access, out_sz);
            return true;
        }
    }
    // via di riserva: il long-lived token incollato a mano
    char url[HA_URL_MAX], tok[HA_TOKEN_MAX];
    if (ha_config_load(url, sizeof(url), tok, sizeof(tok)) && tok[0]) {
        strlcpy(out, tok, out_sz);
        memset(tok, 0, sizeof(tok));
        return true;
    }
    return false;
}
