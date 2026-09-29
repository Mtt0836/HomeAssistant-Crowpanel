#include "pin_lock.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/md.h"

static const char *TAG = "pin_lock";

#define NVS_NS       "pinlock"
#define K_SALT       "salt"
#define K_HASH       "hash"
#define K_AREAS      "areas"
#define K_FAILS      "fails"

#define SALT_LEN     16
#define HASH_LEN     32
#define PBKDF2_ITER  20000
#define GRANT_US     (180 * 1000000LL)   // per 3 minuti non lo richiede di nuovo

static bool     s_set = false;
static uint8_t  s_salt[SALT_LEN];
static uint8_t  s_hash[HASH_LEN];
/* Valori iniziali: tutto cio' che puo' rompere il collegamento o mostrare le
   credenziali di casa. Restano questi finche' non si tocca un interruttore
   (la prima volta la memoria NVS e' vuota e non si puo' nemmeno aprire). */
static uint32_t s_areas = (1u << PIN_AREA_NETWORK) | (1u << PIN_AREA_HA) |
                          (1u << PIN_AREA_DEBUG)   | (1u << PIN_AREA_WIFI) |
                          (1u << PIN_AREA_SECURITY);
static uint8_t  s_fails = 0;
static int64_t  s_locked_until = 0;      // us, orologio monotono
static int64_t  s_granted_until = 0;

// ------------------------------------------------------------------ memoria

static bool nvs_open_rw(nvs_handle_t *h, bool write)
{
    return nvs_open(NVS_NS, write ? NVS_READWRITE : NVS_READONLY, h) == ESP_OK;
}

static uint32_t wait_seconds_for(uint8_t fails)
{
    if (fails < 3) return 0;
    uint32_t s = 30;
    for (uint8_t i = 3; i < fails && s < 3600; i++) s *= 2;
    return s > 3600 ? 3600 : s;
}

void pin_lock_init(void)
{
    nvs_handle_t h;
    if (nvs_open_rw(&h, false)) {
        size_t n = SALT_LEN;
        bool ok = nvs_get_blob(h, K_SALT, s_salt, &n) == ESP_OK && n == SALT_LEN;
        n = HASH_LEN;
        ok = ok && nvs_get_blob(h, K_HASH, s_hash, &n) == ESP_OK && n == HASH_LEN;
        s_set = ok;
        nvs_get_u32(h, K_AREAS, &s_areas);   // se manca, restano i valori iniziali
        nvs_get_u8(h, K_FAILS, &s_fails);
        nvs_close(h);
    }
    // un riavvio non deve azzerare l'attesa: la faccio ripartire da adesso
    uint32_t w = wait_seconds_for(s_fails);
    if (w) s_locked_until = esp_timer_get_time() + (int64_t)w * 1000000LL;
    ESP_LOGI(TAG, "PIN %s, aree protette 0x%02x", s_set ? "impostato" : "assente",
             (unsigned)s_areas);
}

static void save_fails(void)
{
    nvs_handle_t h;
    if (!nvs_open_rw(&h, true)) return;
    nvs_set_u8(h, K_FAILS, s_fails);
    nvs_commit(h);
    nvs_close(h);
}

// ------------------------------------------------------------------ impronta

static void derive(const char *pin, const uint8_t *salt, uint8_t *out)
{
    mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256,
                                  (const unsigned char *)pin, strlen(pin),
                                  salt, SALT_LEN, PBKDF2_ITER, HASH_LEN, out);
}

bool pin_lock_is_set(void) { return s_set; }

bool pin_lock_set(const char *pin)
{
    nvs_handle_t h;
    if (!nvs_open_rw(&h, true)) return false;
    bool ok;
    if (!pin || !pin[0]) {
        nvs_erase_key(h, K_SALT);
        nvs_erase_key(h, K_HASH);
        s_set = false;
        ok = true;
    } else {
        size_t len = strlen(pin);
        if (len < PIN_MIN_LEN || len > PIN_MAX_LEN) { nvs_close(h); return false; }
        esp_fill_random(s_salt, SALT_LEN);
        derive(pin, s_salt, s_hash);
        ok = nvs_set_blob(h, K_SALT, s_salt, SALT_LEN) == ESP_OK &&
             nvs_set_blob(h, K_HASH, s_hash, HASH_LEN) == ESP_OK;
        s_set = ok;
    }
    s_fails = 0;
    s_locked_until = 0;
    nvs_set_u8(h, K_FAILS, 0);
    nvs_commit(h);
    nvs_close(h);
    s_granted_until = ok && s_set ? esp_timer_get_time() + GRANT_US : 0;
    ESP_LOGI(TAG, "PIN %s", s_set ? "aggiornato" : "rimosso");
    return ok;
}

uint32_t pin_lock_wait_s(void)
{
    int64_t now = esp_timer_get_time();
    if (s_locked_until <= now) return 0;
    return (uint32_t)((s_locked_until - now + 999999) / 1000000);
}

bool pin_lock_verify(const char *pin)
{
    if (!s_set || pin_lock_wait_s()) return false;
    uint8_t calc[HASH_LEN];
    derive(pin, s_salt, calc);
    // confronto a tempo costante: non deve trapelare quante cifre sono giuste
    uint8_t diff = 0;
    for (int i = 0; i < HASH_LEN; i++) diff |= calc[i] ^ s_hash[i];
    if (diff) {
        if (s_fails < 255) s_fails++;
        save_fails();
        uint32_t w = wait_seconds_for(s_fails);
        if (w) s_locked_until = esp_timer_get_time() + (int64_t)w * 1000000LL;
        ESP_LOGW(TAG, "PIN errato (%u di fila)", s_fails);
        return false;
    }
    if (s_fails) { s_fails = 0; save_fails(); }
    s_locked_until = 0;
    s_granted_until = esp_timer_get_time() + GRANT_US;
    return true;
}

void pin_lock_forget(void) { s_granted_until = 0; }

// ------------------------------------------------------------------ aree

bool pin_lock_required(pin_area_t area)
{
    if (!s_set || area >= PIN_AREA_COUNT) return false;
    return (s_areas >> area) & 1u;
}

void pin_lock_set_required(pin_area_t area, bool on)
{
    if (area >= PIN_AREA_COUNT) return;
    if (on) s_areas |= (1u << area); else s_areas &= ~(1u << area);
    nvs_handle_t h;
    if (!nvs_open_rw(&h, true)) return;
    nvs_set_u32(h, K_AREAS, s_areas);
    nvs_commit(h);
    nvs_close(h);
}

const char *pin_lock_area_name(pin_area_t area)
{
    switch (area) {
    case PIN_AREA_NETWORK:  return "Rete";
    case PIN_AREA_HA:       return "Home Assistant";
    case PIN_AREA_DEBUG:    return "Debug";
    case PIN_AREA_WIFI:     return "WiFi";
    case PIN_AREA_SECURITY: return "Sicurezza";
    default:                return "?";
    }
}

// ------------------------------------------------------------------ tastierino

static int s_dialogs = 0;

bool pin_lock_dialog_active(void) { return s_dialogs > 0; }

struct Ask {
    lv_obj_t *overlay, *dots, *msg, *pad;
    lv_timer_t *tick;
    char buf[PIN_MAX_LEN + 1];
    bool verify;
    pin_lock_cb_t       cb;
    pin_lock_entry_cb_t ecb;
    void *ctx;
};

static const char *PAD_MAP[] = {
    "1", "2", "3", "\n",
    "4", "5", "6", "\n",
    "7", "8", "9", "\n",
    LV_SYMBOL_BACKSPACE, "0", LV_SYMBOL_OK, ""
};

static void show_dots(Ask *a)
{
    char d[PIN_MAX_LEN * 2 + 1] = "";
    size_t n = strlen(a->buf);
    for (size_t i = 0; i < n; i++) strcat(d, "* ");
    lv_label_set_text(a->dots, n ? d : " ");
}

static void ask_close(Ask *a)
{
    if (s_dialogs > 0) s_dialogs--;
    if (a->tick) lv_timer_del(a->tick);
    lv_obj_del(a->overlay);
    delete a;
}

static void tick_cb(lv_timer_t *t)
{
    Ask *a = (Ask *)t->user_data;
    uint32_t w = pin_lock_wait_s();
    if (w) {
        lv_obj_add_state(a->pad, LV_STATE_DISABLED);
        lv_label_set_text_fmt(a->msg, "Troppi tentativi: riprova fra %u s", (unsigned)w);
    } else if (lv_obj_has_state(a->pad, LV_STATE_DISABLED)) {
        lv_obj_clear_state(a->pad, LV_STATE_DISABLED);
        lv_label_set_text(a->msg, "");
    }
}

static void pad_event(lv_event_t *e)
{
    Ask *a = (Ask *)lv_event_get_user_data(e);
    const char *txt = lv_btnmatrix_get_btn_text(lv_event_get_target(e),
                                                lv_btnmatrix_get_selected_btn(lv_event_get_target(e)));
    if (!txt) return;
    size_t n = strlen(a->buf);

    if (!strcmp(txt, LV_SYMBOL_BACKSPACE)) {
        if (n) a->buf[n - 1] = 0;
        show_dots(a);
        return;
    }
    if (!strcmp(txt, LV_SYMBOL_OK)) {
        if (n < PIN_MIN_LEN) {
            lv_label_set_text_fmt(a->msg, "Almeno %d cifre", PIN_MIN_LEN);
            return;
        }
        if (!a->verify) {
            char pin[PIN_MAX_LEN + 1];
            strlcpy(pin, a->buf, sizeof(pin));
            pin_lock_entry_cb_t cb = a->ecb;
            void *ctx = a->ctx;
            ask_close(a);
            if (cb) cb(pin, ctx);
            return;
        }
        if (pin_lock_verify(a->buf)) {
            pin_lock_cb_t cb = a->cb;
            void *ctx = a->ctx;
            ask_close(a);
            if (cb) cb(true, ctx);
        } else {
            a->buf[0] = 0;
            show_dots(a);
            uint32_t w = pin_lock_wait_s();
            if (w) lv_label_set_text_fmt(a->msg, "Troppi tentativi: riprova fra %u s", (unsigned)w);
            else   lv_label_set_text(a->msg, "PIN errato");
        }
        return;
    }
    if (n < PIN_MAX_LEN && txt[0] >= '0' && txt[0] <= '9') {
        a->buf[n] = txt[0];
        a->buf[n + 1] = 0;
        show_dots(a);
        lv_label_set_text(a->msg, "");
    }
}

static void cancel_event(lv_event_t *e)
{
    Ask *a = (Ask *)lv_event_get_user_data(e);
    pin_lock_cb_t cb = a->cb;
    pin_lock_entry_cb_t ecb = a->ecb;
    void *ctx = a->ctx;
    bool verify = a->verify;
    ask_close(a);
    if (verify) { if (cb) cb(false, ctx); }
    else        { if (ecb) ecb(NULL, ctx); }
}

/* Finestra sopra a tutto (layer_top): funziona da qualunque schermata e non
   entra nell'elenco delle schermate dell'app. */
static void ask_open(const char *title, const char *hint, bool verify,
                     pin_lock_cb_t cb, pin_lock_entry_cb_t ecb, void *ctx)
{
    Ask *a = new Ask{};
    a->verify = verify; a->cb = cb; a->ecb = ecb; a->ctx = ctx;

    a->overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(a->overlay);
    lv_obj_set_size(a->overlay, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(a->overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(a->overlay, LV_OPA_70, 0);
    lv_obj_clear_flag(a->overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(a->overlay, LV_OBJ_FLAG_CLICKABLE);   // blocca cio' che c'e' sotto

    lv_obj_t *card = lv_obj_create(a->overlay);
    lv_obj_set_size(card, 420, 540);
    lv_obj_center(card);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 18, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(card, 6, 0);

    lv_obj_t *t = lv_label_create(card);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_24, 0);

    lv_obj_t *h = lv_label_create(card);
    lv_label_set_text(h, hint ? hint : "");
    lv_obj_set_style_text_font(h, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(h, lv_color_hex(0x707070), 0);

    a->dots = lv_label_create(card);
    lv_obj_set_style_text_font(a->dots, &lv_font_montserrat_30, 0);
    lv_label_set_text(a->dots, " ");

    a->msg = lv_label_create(card);
    lv_obj_set_style_text_font(a->msg, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(a->msg, lv_color_hex(0xC62828), 0);
    lv_label_set_text(a->msg, "");

    a->pad = lv_btnmatrix_create(card);
    lv_btnmatrix_set_map(a->pad, PAD_MAP);
    lv_obj_set_size(a->pad, 360, 300);
    lv_obj_set_style_text_font(a->pad, &lv_font_montserrat_24, 0);
    lv_btnmatrix_set_btn_ctrl_all(a->pad, LV_BTNMATRIX_CTRL_NO_REPEAT);
    lv_obj_add_event_cb(a->pad, pad_event, LV_EVENT_VALUE_CHANGED, a);

    lv_obj_t *cancel = lv_btn_create(card);
    lv_obj_set_size(cancel, 160, 46);
    lv_obj_t *cl = lv_label_create(cancel);
    lv_label_set_text(cl, "Annulla");
    lv_obj_center(cl);
    lv_obj_add_event_cb(cancel, cancel_event, LV_EVENT_CLICKED, a);

    a->tick = lv_timer_create(tick_cb, 500, a);
    tick_cb(a->tick);
    s_dialogs++;
}

void pin_lock_guard(pin_area_t area, pin_lock_cb_t cb, void *ctx)
{
    if (!pin_lock_required(area) || esp_timer_get_time() < s_granted_until) {
        if (cb) cb(true, ctx);
        return;
    }
    char title[64];
    snprintf(title, sizeof(title), "PIN per %s", pin_lock_area_name(area));
    ask_open(title, "Impostazioni protette", true, cb, NULL, ctx);
}

void pin_lock_ask_verify(const char *title, pin_lock_cb_t cb, void *ctx)
{
    if (!pin_lock_is_set()) { if (cb) cb(true, ctx); return; }
    ask_open(title, "Serve il PIN attuale", true, cb, NULL, ctx);
}

void pin_lock_ask_digits(const char *title, const char *hint,
                         pin_lock_entry_cb_t cb, void *ctx)
{
    ask_open(title, hint, false, NULL, cb, ctx);
}
