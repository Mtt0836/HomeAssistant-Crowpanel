#include "avviso_ui.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"

static const char *TAG = "avviso";

#define DURATA_MS 12000          // quanto resta scritto prima di sparire
#define TESTO_MAX 240

static lv_obj_t  *s_box   = NULL;
static lv_timer_t *s_timer = NULL;

/* Cancellare l'oggetto da dentro l'evento di un suo figlio vuol dire liberarlo
   mentre LVGL ci sta ancora lavorando sopra: il pannello se ne va in "block
   already marked as free". Si fa un attimo dopo, a evento finito. */
static void chiudi_poi(void *ud)
{
    (void)ud;
    if (s_timer) { lv_timer_del(s_timer); s_timer = NULL; }
    if (s_box)   { lv_obj_del(s_box);     s_box   = NULL; }
}

static void scaduto(lv_timer_t *t)
{
    (void)t;
    lv_async_call(chiudi_poi, NULL);
}

static void toccato(lv_event_t *e)
{
    (void)e;
    lv_async_call(chiudi_poi, NULL);
}

void avviso_ui_mostra(const char *testo)
{
    if (!testo || !*testo) return;

    char msg[TESTO_MAX];
    strlcpy(msg, testo, sizeof(msg));

    bsp_display_lock(0);

    /* Un avviso alla volta: il secondo prende il posto del primo invece di
       accavallarcisi sopra. Chi legge vuole l'ultimo, non una pila. */
    if (s_timer) { lv_timer_del(s_timer); s_timer = NULL; }
    if (s_box)   { lv_obj_del(s_box);     s_box   = NULL; }

    /* Su lv_layer_top: cosi' si vede anche sopra lo slideshow e sopra la
       configurazione guidata, che e' proprio quando serve di piu'. */
    s_box = lv_obj_create(lv_layer_top());
    lv_obj_set_width(s_box, lv_pct(72));
    lv_obj_set_height(s_box, LV_SIZE_CONTENT);
    lv_obj_align(s_box, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_set_style_bg_color(s_box, lv_color_hex(0x1b2030), 0);
    lv_obj_set_style_bg_opa(s_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_box, lv_color_hex(0x0b7dda), 0);
    lv_obj_set_style_border_width(s_box, 2, 0);
    lv_obj_set_style_radius(s_box, 12, 0);
    lv_obj_set_style_pad_all(s_box, 18, 0);
    lv_obj_set_style_shadow_width(s_box, 24, 0);
    lv_obj_set_style_shadow_opa(s_box, LV_OPA_50, 0);
    lv_obj_clear_flag(s_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_box, toccato, LV_EVENT_CLICKED, NULL);

    lv_obj_t *l = lv_label_create(s_box);
    lv_label_set_text(l, msg);
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xf0f2f5), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);

    s_timer = lv_timer_create(scaduto, DURATA_MS, NULL);
    lv_timer_set_repeat_count(s_timer, 1);

    bsp_display_unlock();
    ESP_LOGI(TAG, "avviso sullo schermo: %.60s", msg);
}
