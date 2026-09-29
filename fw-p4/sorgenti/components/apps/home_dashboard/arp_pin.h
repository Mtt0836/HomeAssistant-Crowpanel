#pragma once

/* Voce ARP statica verso Home Assistant.

   lwIP rinnova le voci ARP in uso a ARP_MAXAGE-30 s (270 s) con una richiesta
   unicast, e in quell'istante la scrittura SDIO verso il C6 va in timeout
   (vedi crowpanel-v12-vincoli, punto 5). Fissando come statica la voce di HA,
   imparata normalmente al primo collegamento, il rinnovo non parte piu'.
   Se HA cambiasse scheda di rete basta riavviare il pannello: la voce si
   reimpara da zero. */

#ifdef __cplusplus
extern "C" {
#endif

/* host: IP letterale di HA (es. "192.168.1.20"). Se HA e' su un'altra
   sottorete si fissa invece la voce del gateway. Da chiamare quando la
   connessione verso HA e' appena riuscita: la voce dinamica e' gia' presente. */
void arp_pin_host(const char *host);

#ifdef __cplusplus
}
#endif
