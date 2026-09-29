#include "mdi_icon.h"

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "assets/mdi_nomi.h"

static const char *TAG = "mdi";

LV_FONT_DECLARE(lv_font_mdi_20);
LV_FONT_DECLARE(lv_font_mdi_32);

static int confronta(const void *chiave, const void *voce)
{
    return strcmp((const char *)chiave, ((const mdi_voce_t *)voce)->nome);
}

/* I codici delle icone stanno oltre 0xFFFF, quindi in UTF-8 sono quattro
   byte. LVGL li regge: la sua decodifica arriva fino a quattro. */
static int utf8(uint32_t c, char *out)
{
    if (c < 0x80) {
        out[0] = (char)c;
        return 1;
    }
    if (c < 0x800) {
        out[0] = (char)(0xC0 | (c >> 6));
        out[1] = (char)(0x80 | (c & 0x3F));
        return 2;
    }
    if (c < 0x10000) {
        out[0] = (char)(0xE0 | (c >> 12));
        out[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        out[2] = (char)(0x80 | (c & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (c >> 18));
    out[1] = (char)(0x80 | ((c >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((c >> 6) & 0x3F));
    out[3] = (char)(0x80 | (c & 0x3F));
    return 4;
}

bool mdi_icon_text(const char *name, char *out, size_t out_sz)
{
    if (!name || !out || out_sz < 5) return false;
    if (!strncmp(name, "mdi:", 4)) name += 4;
    if (!*name) return false;

    const mdi_voce_t *v = (const mdi_voce_t *)bsearch(
        name, MDI_NOMI, sizeof(MDI_NOMI) / sizeof(MDI_NOMI[0]),
        sizeof(MDI_NOMI[0]), confronta);
    if (!v) {
        /* Una sola volta per nome sarebbe piu' pulito, ma tenere un elenco
           dei gia' detti costa piu' di quello che vale: il log dettagliato
           si accende solo quando si va a caccia di icone mancanti. */
        ESP_LOGD(TAG, "icona non compilata: %s (aggiungila a strumenti/icone/elenco.txt)", name);
        return false;
    }
    out[utf8(v->codice, out)] = 0;
    return true;
}

const lv_font_t *mdi_icon_font(int size_px)
{
    return size_px >= 26 ? &lv_font_mdi_32 : &lv_font_mdi_20;
}
