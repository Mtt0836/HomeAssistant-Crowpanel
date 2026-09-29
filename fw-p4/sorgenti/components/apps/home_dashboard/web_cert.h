#pragma once
#include <stdbool.h>
#include <stddef.h>

// Certificato del pannello per l'HTTPS.
//
// Nessuno puo' firmarci un certificato (il pannello non ha un nome su
// internet), quindi se lo firma da solo: il traffico e' cifrato, ma il browser
// mostra comunque un avviso perche' non conosce chi l'ha firmato. Per sapere
// che si sta parlando davvero col pannello e non con qualcun altro, l'impronta
// del certificato e' scritta in Impostazioni > Sicurezza: si confronta con
// quella che mostra il browser.
//
// Chiave ellittica P-256 generata al primo avvio e tenuta in NVS. Se cambia
// l'indirizzo IP il certificato viene rifatto, cosi' resta coerente.

#ifdef __cplusplus
extern "C" {
#endif

// PEM del certificato e della chiave (validi finche' non si richiama).
// Restituisce false se la generazione non riesce.
bool web_cert_get(const char **cert_pem, size_t *cert_len,
                  const char **key_pem,  size_t *key_len);

// "AB:CD:..." (SHA-256 del certificato), stessa forma che mostra il browser.
// Stringa vuota se il certificato non c'e' ancora.
void web_cert_fingerprint(char *out, size_t out_sz);

#ifdef __cplusplus
}
#endif
