#include "arp_pin.h"

#include <string.h>
#include "esp_log.h"
#include "lwip/tcpip.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/ip4_addr.h"

static const char *TAG = "arp_pin";

static ip4_addr_t s_host;

/* Gira nel thread di lwIP (nessun core locking in questo progetto). */
static void pin_in_tcpip(void *arg)
{
    ip4_addr_t target = s_host;
    struct netif *nif = nullptr;
    struct netif *n;
    NETIF_FOREACH(n) {
        if (!netif_is_up(n) || ip4_addr_isany_val(*netif_ip4_addr(n))) continue;
        nif = n;
        break;
    }
    if (nif == nullptr) {
        ESP_LOGW(TAG, "nessuna interfaccia attiva");
        return;
    }
    /* Fuori dalla sottorete i pacchetti per HA passano dal gateway: e' quella
       la voce che lwIP rinnova. */
    if (!ip4_addr_netcmp(&target, netif_ip4_addr(nif), netif_ip4_netmask(nif))) {
        target = *netif_ip4_gw(nif);
    }

    struct eth_addr *mac = nullptr;
    const ip4_addr_t *ip = nullptr;
    if (etharp_find_addr(nif, &target, &mac, &ip) < 0 || mac == nullptr) {
        ESP_LOGW(TAG, "%s non ancora nella tabella ARP: voce non fissata", ip4addr_ntoa(&target));
        return;
    }
    struct eth_addr copy = *mac;
    err_t e = etharp_add_static_entry(&target, &copy);
    ESP_LOGI(TAG, "voce ARP statica %s -> %02x:%02x:%02x:%02x:%02x:%02x (%s)",
             ip4addr_ntoa(&target), copy.addr[0], copy.addr[1], copy.addr[2],
             copy.addr[3], copy.addr[4], copy.addr[5], e == ERR_OK ? "ok" : "errore");
}

void arp_pin_host(const char *host)
{
    ip4_addr_t a;
    if (host == nullptr || !ip4addr_aton(host, &a)) {
        ESP_LOGW(TAG, "'%s' non e' un IP letterale: voce ARP statica non usata", host ? host : "");
        return;
    }
    s_host = a;
    tcpip_callback(pin_in_tcpip, nullptr);
}
