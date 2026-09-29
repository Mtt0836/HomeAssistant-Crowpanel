#include "ha_http.h"
#include "ha_config.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

static const char *TAG = "ha_http";

bool ha_http_base(char *out, size_t sz)
{
    /* Solo l'indirizzo: ha_config_load vorrebbe anche il long-lived token e
       senza quello direbbe di no, ma qui si tratta solo di cambiare lo schema
       dell'indirizzo. Un pannello abbinato dal telefono il long-lived non ce
       l'ha, e si ritrovava senza indirizzo di base: niente accesso con
       l'account di HA, niente rinnovo del permesso, niente voce. */
    char url[HA_URL_MAX];
    if (!ha_config_load_url(url, sizeof(url)) || !url[0]) return false;

    const char *rest, *scheme;
    if (!strncmp(url, "wss://", 6))     { scheme = "https"; rest = url + 6; }
    else if (!strncmp(url, "ws://", 5)) { scheme = "http";  rest = url + 5; }
    else return false;

    char host[128];
    strlcpy(host, rest, sizeof(host));
    char *slash = strchr(host, '/');
    if (slash) *slash = 0;
    if (!host[0]) return false;
    snprintf(out, sz, "%s://%s", scheme, host);
    return true;
}

/* Il pannello si presenta a HA con il proprio indirizzo. HA accetta gli
   indirizzi IP di rete locale (si discosta apposta dalla specifica IndieAuth,
   che li vieterebbe), quindi non serve un nome di dominio. */
bool ha_http_panel_url(char *out, size_t sz, const char *path)
{
    esp_netif_ip_info_t ip = {};
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!nif || esp_netif_get_ip_info(nif, &ip) != ESP_OK || !ip.ip.addr) return false;
    snprintf(out, sz, "https://" IPSTR "%s", IP2STR(&ip.ip), path);
    return true;
}

void ha_http_url_encode(const char *in, char *out, size_t sz)
{
    static const char *hex = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < sz; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || strchr("-_.~", *p)) {
            out[o++] = *p;
        } else {
            out[o++] = '%';
            out[o++] = hex[*p >> 4];
            out[o++] = hex[*p & 0xf];
        }
    }
    out[o] = 0;
}

int ha_http_post_form(const char *url, const char *body, char *resp, size_t resp_sz)
{
    esp_http_client_config_t c = {};
    c.url = url;
    c.method = HTTP_METHOD_POST;
    c.timeout_ms = 8000;
    if (!strncmp(url, "https://", 8)) c.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t h = esp_http_client_init(&c);
    if (!h) return -1;
    esp_http_client_set_header(h, "Content-Type", "application/x-www-form-urlencoded");

    int status = -1;
    esp_err_t err = esp_http_client_open(h, strlen(body));
    if (err != ESP_OK) ESP_LOGE(TAG, "%s non raggiungibile: %s", url, esp_err_to_name(err));
    if (err == ESP_OK) {
        if (esp_http_client_write(h, body, strlen(body)) >= 0 &&
            esp_http_client_fetch_headers(h) >= 0) {
            int n = esp_http_client_read_response(h, resp, resp_sz - 1);
            resp[n > 0 ? n : 0] = 0;
            status = esp_http_client_get_status_code(h);
        }
    }
    esp_http_client_cleanup(h);
    return status;
}
