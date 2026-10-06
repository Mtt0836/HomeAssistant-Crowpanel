#include "ha_config.h"
#include "ha_token.h"
#include <string.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"

static const char *TAG = "ha_config";
static const char *NS  = "hapanel";

static bool nvs_get(const char *key, char *out, size_t sz)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = sz;
    esp_err_t err = nvs_get_str(h, key, out, &len);
    nvs_close(h);
    return err == ESP_OK && len > 1;
}

bool ha_config_is_set(void)
{
    /* Basta l'indirizzo piu' un permesso qualunque: il long-lived token
       incollato oppure l'abbinamento fatto dal telefono (ha_token). */
    char u[HA_URL_MAX], t[HA_TOKEN_MAX];
    if (!nvs_get("ha_url", u, sizeof(u))) return false;
    if (nvs_get("ha_token", t, sizeof(t))) return true;
    return ha_token_have_refresh();
}

bool ha_config_load(char *url, size_t url_sz, char *token, size_t token_sz)
{
    return nvs_get("ha_url", url, url_sz) && nvs_get("ha_token", token, token_sz);
}

/* Solo l'indirizzo: serve a chi vuole sapere con quale Home Assistant sta
   parlando il pannello senza tirare fuori anche il token. */
bool ha_config_load_url(char *url, size_t sz)
{
    return nvs_get("ha_url", url, sz);
}

void ha_config_load_tts(char *id, size_t sz)
{
    if (!nvs_get("ha_tts", id, sz)) strlcpy(id, "tts.piper", sz);
}

bool ha_config_save_tts(const char *id)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = (id && id[0]) ? nvs_set_str(h, "ha_tts", id) : nvs_erase_key(h, "ha_tts");
    nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND;
}

bool ha_config_load_uuid(char *uuid, size_t sz)
{
    return nvs_get("ha_uuid", uuid, sz);
}

bool ha_config_save_uuid(const char *uuid)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_str(h, "ha_uuid", uuid ? uuid : "");
    err |= nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

bool ha_config_save(const char *url, const char *token)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_str(h, "ha_url", url);
    err |= nvs_set_str(h, "ha_token", token);
    err |= nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "config salvata (%s)", err == ESP_OK ? "ok" : "errore");
    return err == ESP_OK;
}

/* Aggiorna solo l'indirizzo, lasciando il token dov'e'. Serve a correggere
   l'URL (es. porta sbagliata) senza dover rimaneggiare le credenziali. */
bool ha_config_save_url(const char *url)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_str(h, "ha_url", url);
    err |= nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

void ha_config_clear(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, "ha_url");
    nvs_erase_key(h, "ha_token");
    nvs_erase_key(h, "ha_uuid");
    nvs_commit(h);
    nvs_close(h);
}

void ha_config_load_dash(char *path, size_t sz, int *view)
{
    if (!nvs_get("ha_dash", path, sz)) strlcpy(path, "lovelace", sz);
    int32_t v = 0;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i32(h, "ha_view", &v);
        nvs_close(h);
    }
    *view = (int)v;
}

bool ha_config_save_dash(const char *path, int view)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_str(h, "ha_dash", path);
    err |= nvs_set_i32(h, "ha_view", view);
    err |= nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

// ------------------------------------------------------------------ certificato

#define K_CA "ha_ca"

bool ha_config_load_ca(char *pem, size_t sz)
{
    if (!pem || sz < 2) return false;
    pem[0] = 0;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t n = sz;
    bool ok = nvs_get_str(h, K_CA, pem, &n) == ESP_OK && pem[0];
    nvs_close(h);
    if (!ok) pem[0] = 0;
    return ok;
}

bool ha_config_has_ca(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t n = 0;
    bool ok = nvs_get_str(h, K_CA, NULL, &n) == ESP_OK && n > 1;
    nvs_close(h);
    return ok;
}

bool ha_config_save_ca(const char *pem)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e;
    if (!pem || !pem[0]) {
        e = nvs_erase_key(h, K_CA);
        if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;   // non c'era: va bene lo stesso
        ESP_LOGI(TAG, "certificato di Home Assistant rimosso: torno alle autorita' pubbliche");
    } else {
        e = nvs_set_str(h, K_CA, pem);
        ESP_LOGI(TAG, "certificato di Home Assistant salvato (%u byte)", (unsigned)strlen(pem));
    }
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK;
}
