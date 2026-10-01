#include "web_auth.h"
#include "secret_hash.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs.h"

static const char *TAG = "web_auth";

#define NVS_NS  "webauth"
#define SESSIONS 4
#define SESSION_US (8 * 3600 * 1000000LL)   // otto ore
#define FREE_TRIES 4

struct Account {
    bool    set;
    uint8_t salt[SECRET_SALT_LEN];
    uint8_t hash[SECRET_HASH_LEN];
};

struct Session {
    char       sid[WEB_SID_LEN + 1];
    web_role_t role;
    int64_t    expires;
};

static Account  s_admin, s_guest;
static Session  s_sess[SESSIONS];
static uint32_t s_fails = 0;
static int64_t  s_locked_until = 0;

// ------------------------------------------------------------------ memoria

static const char *key_salt(web_role_t r) { return r == WEB_ROLE_ADMIN ? "a_salt" : "g_salt"; }
static const char *key_hash(web_role_t r) { return r == WEB_ROLE_ADMIN ? "a_hash" : "g_hash"; }

static void load_account(nvs_handle_t h, web_role_t r, Account *a)
{
    size_t n = SECRET_SALT_LEN;
    bool ok = nvs_get_blob(h, key_salt(r), a->salt, &n) == ESP_OK && n == SECRET_SALT_LEN;
    n = SECRET_HASH_LEN;
    ok = ok && nvs_get_blob(h, key_hash(r), a->hash, &n) == ESP_OK && n == SECRET_HASH_LEN;
    a->set = ok;
}

void web_auth_init(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        load_account(h, WEB_ROLE_ADMIN, &s_admin);
        load_account(h, WEB_ROLE_GUEST, &s_guest);
        nvs_get_u32(h, "fails", &s_fails);
        nvs_close(h);
    }
    uint32_t w = secret_backoff_s(s_fails, FREE_TRIES);
    if (w) s_locked_until = esp_timer_get_time() + (int64_t)w * 1000000LL;
    ESP_LOGI(TAG, "password amministratore %s, ospite %s",
             s_admin.set ? "impostata" : "da creare",
             s_guest.set ? "attivo" : "non attivo");
}

bool web_auth_configured(void)    { return s_admin.set; }
bool web_auth_guest_enabled(void) { return s_guest.set; }

static void save_fails(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, "fails", s_fails);
    nvs_commit(h);
    nvs_close(h);
}

bool web_auth_set_password(web_role_t role, const char *pw)
{
    if (role != WEB_ROLE_ADMIN && role != WEB_ROLE_GUEST) return false;
    Account *a = role == WEB_ROLE_ADMIN ? &s_admin : &s_guest;

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok;
    if (!pw || !pw[0]) {
        // l'amministratore non puo' restare senza password
        if (role == WEB_ROLE_ADMIN) { nvs_close(h); return false; }
        nvs_erase_key(h, key_salt(role));
        nvs_erase_key(h, key_hash(role));
        a->set = false;
        ok = true;
    } else if (strlen(pw) < WEB_PW_MIN) {
        nvs_close(h);
        return false;
    } else {
        secret_hash_new_salt(a->salt);
        secret_hash_make(pw, a->salt, a->hash);
        ok = nvs_set_blob(h, key_salt(role), a->salt, SECRET_SALT_LEN) == ESP_OK &&
             nvs_set_blob(h, key_hash(role), a->hash, SECRET_HASH_LEN) == ESP_OK;
        a->set = ok;
    }
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "password %s %s", role == WEB_ROLE_ADMIN ? "amministratore" : "ospite",
             a->set ? "aggiornata" : "rimossa");
    return ok;
}

// ------------------------------------------------------------------ sessioni

uint32_t web_auth_wait_s(void)
{
    int64_t now = esp_timer_get_time();
    if (s_locked_until <= now) return 0;
    return (uint32_t)((s_locked_until - now + 999999) / 1000000);
}

static void new_sid(char *sid)
{
    static const char hex[] = "0123456789abcdef";
    uint8_t raw[WEB_SID_LEN / 2];
    esp_fill_random(raw, sizeof(raw));
    for (size_t i = 0; i < sizeof(raw); i++) {
        sid[i * 2]     = hex[raw[i] >> 4];
        sid[i * 2 + 1] = hex[raw[i] & 0xf];
    }
    sid[WEB_SID_LEN] = 0;
}

static bool match(const Account *a, const char *pw)
{
    if (!a->set) return false;
    uint8_t calc[SECRET_HASH_LEN];
    secret_hash_make(pw, a->salt, calc);
    return secret_hash_equal(calc, a->hash);
}

web_role_t web_auth_login(const char *pw, char *sid, size_t sid_sz)
{
    if (!pw || sid_sz <= WEB_SID_LEN || web_auth_wait_s()) return WEB_ROLE_NONE;

    web_role_t role = WEB_ROLE_NONE;
    if      (match(&s_admin, pw)) role = WEB_ROLE_ADMIN;
    else if (match(&s_guest, pw)) role = WEB_ROLE_GUEST;

    if (role == WEB_ROLE_NONE) {
        s_fails++;
        save_fails();
        uint32_t w = secret_backoff_s(s_fails, FREE_TRIES);
        if (w) s_locked_until = esp_timer_get_time() + (int64_t)w * 1000000LL;
        ESP_LOGW(TAG, "password sbagliata (%u di fila)", (unsigned)s_fails);
        return WEB_ROLE_NONE;
    }
    if (s_fails) { s_fails = 0; save_fails(); }
    s_locked_until = 0;

    if (!web_auth_grant(role, sid, sid_sz)) return WEB_ROLE_NONE;
    ESP_LOGI(TAG, "accesso come %s", role == WEB_ROLE_ADMIN ? "amministratore" : "ospite");
    return role;
}

bool web_auth_grant(web_role_t role, char *sid, size_t sid_sz)
{
    if (role == WEB_ROLE_NONE || sid_sz <= WEB_SID_LEN) return false;
    /* Prima un posto libero, e solo se non ce n'e' quello che scade prima.
       Guardando la sola scadenza si buttava fuori una sessione viva mentre un
       posto lasciato da chi era uscito restava li': la scadenza di quel posto
       e' rimasta quella vecchia, grande, e nel confronto perdeva. */
    int64_t now = esp_timer_get_time();
    int slot = -1;
    for (int i = 0; i < SESSIONS; i++)
        if (s_sess[i].role == WEB_ROLE_NONE) { slot = i; break; }
    if (slot < 0) {
        slot = 0;
        for (int i = 1; i < SESSIONS; i++)
            if (s_sess[i].expires < s_sess[slot].expires) slot = i;
    }

    new_sid(s_sess[slot].sid);
    s_sess[slot].role = role;
    s_sess[slot].expires = now + SESSION_US;
    strlcpy(sid, s_sess[slot].sid, sid_sz);
    return true;
}

web_role_t web_auth_role_of(const char *sid)
{
    if (!sid || !sid[0]) return WEB_ROLE_NONE;
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < SESSIONS; i++) {
        if (s_sess[i].role == WEB_ROLE_NONE) continue;
        if (s_sess[i].expires < now) { s_sess[i].role = WEB_ROLE_NONE; continue; }
        if (secret_str_equal(s_sess[i].sid, sid)) {
            s_sess[i].expires = now + SESSION_US;    // finche' si usa, non scade
            return s_sess[i].role;
        }
    }
    return WEB_ROLE_NONE;
}

void web_auth_logout(const char *sid)
{
    if (!sid) return;
    for (int i = 0; i < SESSIONS; i++)
        if (s_sess[i].role != WEB_ROLE_NONE && secret_str_equal(s_sess[i].sid, sid))
            s_sess[i].role = WEB_ROLE_NONE;
}

void web_auth_logout_all(void)
{
    for (int i = 0; i < SESSIONS; i++) s_sess[i].role = WEB_ROLE_NONE;
}

void web_auth_reset(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    s_admin.set = s_guest.set = false;
    s_fails = 0;
    s_locked_until = 0;
    web_auth_logout_all();
    ESP_LOGW(TAG, "accesso web azzerato dal pannello: la pagina torna al primo avvio");
}
