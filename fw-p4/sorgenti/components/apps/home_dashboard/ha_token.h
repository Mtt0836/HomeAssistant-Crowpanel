#pragma once
#include <stdbool.h>
#include <stddef.h>

// Il permesso del pannello di parlare con Home Assistant.
//
// Due modi, in ordine di preferenza:
//
//   1. abbinamento (come l'app ufficiale di HA): al primo avvio si autorizza
//      il pannello dal telefono e HA gli lascia un "refresh token". Con quello
//      il pannello si genera da solo i token di accesso, che durano poco e si
//      rinnovano: non c'e' niente da incollare a mano.
//
//   2. long-lived token: quello generato nel profilo di HA e incollato nella
//      pagina. Resta valido per chi ce l'ha gia' e come via di riserva.
//
// Il refresh token si puo' revocare da Home Assistant, in Profilo > Sessioni:
// e' il modo giusto per togliere l'accesso a un pannello che non si usa piu'.

#ifdef __cplusplus
extern "C" {
#endif

bool ha_token_have_refresh(void);

// Salva il permesso ottenuto con l'abbinamento (NULL o "" lo cancella).
bool ha_token_set_refresh(const char *refresh_token);

// Token da usare per autenticarsi: quello rinnovato se il pannello e'
// abbinato, altrimenti il long-lived. Rinnova da solo quando serve; puo'
// bloccare per qualche secondo (chiamata HTTP a HA).
bool ha_token_get_access(char *out, size_t out_sz);

// Il prossimo ha_token_get_access rifara' il giro: da chiamare se HA ha
// risposto "auth_invalid".
void ha_token_invalidate(void);

// Restituisce l'abbinamento a HA e lo cancella (ritorno alle impostazioni
// iniziali). Puo' bloccare per qualche secondo.
void ha_token_revoke_refresh(void);

#ifdef __cplusplus
}
#endif
