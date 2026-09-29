#pragma once
#include <stdbool.h>
#include "esp_netif.h"

/* Impostazioni di rete del pannello (NVS, namespace "netcfg"): DHCP o IP
   statico, DNS, nome del pannello nel router e server NTP.
   Vanno applicate a ogni creazione dell'interfaccia WiFi, prima della
   connessione: all'avvio (app Impostazioni) e dopo ogni recupero SDIO. */

typedef struct {
    bool dhcp;
    char ip[16], mask[16], gw[16], dns1[16], dns2[16];
    char hostname[32];
    char room[32];         // stanza in cui e' installato (solo descrittiva)
    char ntp[64];          // vuoto = server predefiniti
} net_config_t;

#ifdef __cplusplus
extern "C" {
#endif

void net_config_load(net_config_t *c);
bool net_config_save(const net_config_t *c);

// Controlla gli indirizzi prima di salvarli; in caso di errore scrive il motivo.
bool net_config_validate(const net_config_t *c, char *err, size_t err_sz);

// Applica la configurazione salvata all'interfaccia (NULL = WIFI_STA_DEF).
void net_config_apply(esp_netif_t *nif);

// Applica subito e riconnette il WiFi, per le modifiche fatte dal pannello.
void net_config_apply_now(void);

// Server NTP scelto dall'utente, o NULL per i predefiniti.
const char *net_config_ntp(void);

#ifdef __cplusplus
}
#endif
