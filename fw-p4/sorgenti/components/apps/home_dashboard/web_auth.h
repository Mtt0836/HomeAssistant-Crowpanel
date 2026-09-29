#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Chi puo' fare cosa sulla pagina del pannello.
//
// Due utenti, nessun nome da ricordare: si digita solo la password e il
// pannello capisce chi sei da quale delle due impronte corrisponde.
//
//   amministratore  tutto: Home Assistant, password, ritocchi della dashboard
//   ospite          solo i ritocchi della dashboard (ordine e card nascoste)
//
// L'ospite serve a chi abita la casa o ci sta in affitto: puo' adattare quello
// che vede sullo schermo, ma non arriva alle credenziali di Home Assistant, che
// comandano tutto l'impianto.
//
// La sessione vive nella memoria del pannello: dopo un riavvio si rientra.

#ifdef __cplusplus
extern "C" {
#endif

#define WEB_PW_MIN  6
#define WEB_SID_LEN 32

typedef enum {
    WEB_ROLE_NONE = 0,
    WEB_ROLE_GUEST,
    WEB_ROLE_ADMIN,
} web_role_t;

void web_auth_init(void);

bool web_auth_configured(void);        // la password di amministratore esiste
bool web_auth_guest_enabled(void);

// pw NULL o "" toglie la password (permesso solo per l'ospite).
bool web_auth_set_password(web_role_t role, const char *pw);

// Secondi di attesa dopo troppi tentativi sbagliati (0 = si puo' provare).
uint32_t web_auth_wait_s(void);

// Verifica la password e apre una sessione; sid deve tenere WEB_SID_LEN+1 byte.
web_role_t web_auth_login(const char *pw, char *sid, size_t sid_sz);

// Apre una sessione senza password: la usa l'accesso con l'account di Home
// Assistant, dove a garantire chi sei e' stato HA.
bool web_auth_grant(web_role_t role, char *sid, size_t sid_sz);

web_role_t web_auth_role_of(const char *sid);
void web_auth_logout(const char *sid);
void web_auth_logout_all(void);        // dopo un cambio di password

// Cancella entrambe le password: la pagina torna al primo avvio. E' il
// recupero per chi la dimentica, e si fa solo dal pannello (protetto dal PIN).
void web_auth_reset(void);

#ifdef __cplusplus
}
#endif
