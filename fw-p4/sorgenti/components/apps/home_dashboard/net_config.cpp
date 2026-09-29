#include "net_config.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_sntp.h"
#include "nvs.h"
#include "lwip/ip4_addr.h"

static const char *TAG = "net_cfg";
static const char *NS  = "netcfg";

static char s_ntp[64];

static void get_str(nvs_handle_t h, const char *k, char *out, size_t sz, const char *def)
{
    size_t len = sz;
    if (nvs_get_str(h, k, out, &len) != ESP_OK) strlcpy(out, def, sz);
}

void net_config_load(net_config_t *c)
{
    memset(c, 0, sizeof(*c));
    c->dhcp = true;
    strlcpy(c->mask, "255.255.255.0", sizeof(c->mask));
    strlcpy(c->hostname, "pannello-ha", sizeof(c->hostname));
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t d = 1;
    nvs_get_u8(h, "dhcp", &d);
    c->dhcp = d != 0;
    get_str(h, "ip",   c->ip,   sizeof(c->ip),   "");
    get_str(h, "mask", c->mask, sizeof(c->mask), "255.255.255.0");
    get_str(h, "gw",   c->gw,   sizeof(c->gw),   "");
    get_str(h, "dns1", c->dns1, sizeof(c->dns1), "");
    get_str(h, "dns2", c->dns2, sizeof(c->dns2), "");
    get_str(h, "host", c->hostname, sizeof(c->hostname), "pannello-ha");
    get_str(h, "room", c->room, sizeof(c->room), "");
    get_str(h, "ntp",  c->ntp,  sizeof(c->ntp),  "");
    nvs_close(h);
}

bool net_config_save(const net_config_t *c)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e = nvs_set_u8(h, "dhcp", c->dhcp ? 1 : 0);
    e |= nvs_set_str(h, "ip",   c->ip);
    e |= nvs_set_str(h, "mask", c->mask);
    e |= nvs_set_str(h, "gw",   c->gw);
    e |= nvs_set_str(h, "dns1", c->dns1);
    e |= nvs_set_str(h, "dns2", c->dns2);
    e |= nvs_set_str(h, "host", c->hostname);
    e |= nvs_set_str(h, "room", c->room);
    e |= nvs_set_str(h, "ntp",  c->ntp);
    e |= nvs_commit(h);
    nvs_close(h);
    strlcpy(s_ntp, c->ntp, sizeof(s_ntp));
    return e == ESP_OK;
}

static bool valid_ip(const char *s, bool allow_empty)
{
    if (!s[0]) return allow_empty;
    ip4_addr_t a;
    return ip4addr_aton(s, &a) != 0;
}

bool net_config_validate(const net_config_t *c, char *err, size_t err_sz)
{
    if (!c->hostname[0]) { snprintf(err, err_sz, "Il nome del pannello non puo' essere vuoto"); return false; }
    for (const char *p = c->hostname; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-')) {
            snprintf(err, err_sz, "Nome del pannello: solo lettere, cifre e '-'");
            return false;
        }
    }
    if (c->dhcp) return true;
    if (!valid_ip(c->ip, false))    { snprintf(err, err_sz, "Indirizzo IP non valido");  return false; }
    if (!valid_ip(c->mask, false))  { snprintf(err, err_sz, "Maschera non valida");      return false; }
    if (!valid_ip(c->gw, false))    { snprintf(err, err_sz, "Gateway non valido");       return false; }
    if (!valid_ip(c->dns1, true))   { snprintf(err, err_sz, "DNS primario non valido");  return false; }
    if (!valid_ip(c->dns2, true))   { snprintf(err, err_sz, "DNS secondario non valido"); return false; }
    ip4_addr_t ip, mask, gw;
    ip4addr_aton(c->ip, &ip); ip4addr_aton(c->mask, &mask); ip4addr_aton(c->gw, &gw);
    if (!ip4_addr_netcmp(&ip, &gw, &mask)) {
        snprintf(err, err_sz, "Il gateway non e' nella stessa rete dell'IP");
        return false;
    }
    return true;
}

static void set_dns(esp_netif_t *nif, esp_netif_dns_type_t type, const char *s)
{
    if (!s[0]) return;
    esp_netif_dns_info_t d = {};
    d.ip.type = ESP_IPADDR_TYPE_V4;
    d.ip.u_addr.ip4.addr = ipaddr_addr(s);
    esp_netif_set_dns_info(nif, type, &d);
}

void net_config_apply(esp_netif_t *nif)
{
    if (!nif) nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!nif) return;
    net_config_t c;
    net_config_load(&c);
    strlcpy(s_ntp, c.ntp, sizeof(s_ntp));

    esp_netif_set_hostname(nif, c.hostname);
    if (c.dhcp) {
        esp_netif_dhcpc_start(nif);          // gia' avviato: nessun effetto
        ESP_LOGI(TAG, "DHCP, nome '%s'", c.hostname);
        return;
    }
    esp_netif_dhcpc_stop(nif);
    esp_netif_ip_info_t ip = {};
    ip.ip.addr      = ipaddr_addr(c.ip);
    ip.netmask.addr = ipaddr_addr(c.mask);
    ip.gw.addr      = ipaddr_addr(c.gw);
    esp_err_t e = esp_netif_set_ip_info(nif, &ip);
    /* Senza DNS espliciti uso il gateway, come farebbe quasi ogni router. */
    set_dns(nif, ESP_NETIF_DNS_MAIN,   c.dns1[0] ? c.dns1 : c.gw);
    set_dns(nif, ESP_NETIF_DNS_BACKUP, c.dns2);
    ESP_LOGI(TAG, "IP statico %s/%s gw %s, nome '%s' (%s)", c.ip, c.mask, c.gw, c.hostname,
             e == ESP_OK ? "ok" : esp_err_to_name(e));
}

void net_config_apply_now(void)
{
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!nif) return;
    /* Il nome e l'eventuale DHCP si rinegoziano solo con una nuova associazione. */
    esp_wifi_disconnect();
    net_config_apply(nif);
    esp_wifi_connect();

    if (s_ntp[0] && esp_sntp_enabled()) {
        esp_sntp_stop();
        esp_sntp_setservername(0, s_ntp);
        esp_sntp_init();
    }
}

const char *net_config_ntp(void)
{
    if (!s_ntp[0]) {
        net_config_t c;
        net_config_load(&c);
        strlcpy(s_ntp, c.ntp, sizeof(s_ntp));
    }
    return s_ntp[0] ? s_ntp : NULL;
}
