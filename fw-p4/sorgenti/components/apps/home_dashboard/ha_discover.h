#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ha_config.h"

/* Trovare Home Assistant senza farsi scrivere l'indirizzo.

   Home Assistant si annuncia da solo sulla rete locale: pubblica in mDNS un
   servizio "_home-assistant._tcp" con dentro il nome dell'istanza, la
   versione e l'indirizzo con cui la si raggiunge in casa. E' la stessa
   strada che usa l'app ufficiale sul telefono.

   Scandire la rete indirizzo per indirizzo sarebbe la scelta sbagliata: ci
   vorrebbe un minuto, busserebbe a ogni macchina, e non basterebbe sapere
   quale porta provare (la 8123 e' solo quella predefinita: dietro un
   reverse proxy Home Assistant sta sulla 80 o sulla 443). L'annuncio invece
   dice tutto in mezzo secondo. */

#ifdef __cplusplus
extern "C" {
#endif

#define HA_FOUND_MAX 4

typedef struct {
    char nome[40];              // "Casa": il nome che gli hai dato tu
    char url[HA_URL_MAX];       // ws://... gia' pronto da salvare
    char uuid[40];              // identifica l'istanza anche se cambia indirizzo
    char versione[16];
    /* L'indirizzo IPv4 dell'annuncio, che non sempre coincide con quello
       dentro l'url (HA puo' dichiarare un nome, o un reverse proxy). Serve a
       dire se l'istanza trovata sta sulla nostra stessa rete. 0 = non c'era. */
    uint32_t ip4;
} ha_found_t;

/* Avvia mDNS e pubblica il pannello, cosi' la sua pagina web si apre con
   <nome>.local invece che con l'indirizzo IP. Da chiamare una volta sola,
   quando la rete e' su. */
void ha_discover_start(void);

/* Chiede alla rete chi e' Home Assistant. Ritorna quanti ne ha trovati,
   riempiendo out. Blocca per al massimo attesa_ms. */
int ha_discover_scan(ha_found_t *out, int max, int attesa_ms);

/* Da "ws://192.168.1.7:8123/api/websocket" tira fuori "192.168.1.7:8123".
   E' quello che si mostra a chi guarda lo schermo: il nome dell'istanza lo
   sceglie chi la installa e puo' essere qualunque cosa, l'indirizzo no. */
void ha_discover_host(const char *url, char *out, size_t sz);

/* Tiene d'occhio il collegamento: se Home Assistant non risponde piu' da un
   po' e nel frattempo si e' spostato a un altro indirizzo, lo ritrova e lo
   propone.

   Proporre, non spostarsi da solo. L'annuncio mDNS non e' firmato da
   nessuno: chiunque sia sulla rete puo' dire "Home Assistant sono io", uuid
   compreso, visto che l'uuid lo leggono tutti. Se il pannello ci andasse
   senza chiedere, al primo messaggio gli consegnerebbe il token, cioe' il
   comando di tutta la casa. Percio' l'indirizzo nuovo compare sullo schermo
   e si muove solo se qualcuno, li' davanti, conferma. */
void ha_discover_guard_start(void);

#ifdef __cplusplus
}
#endif
