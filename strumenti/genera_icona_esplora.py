# Genera l'icona dell'app Esplora: una cartella, 112x112, nello stesso formato
# di img_app_home.c (LV_IMG_CF_TRUE_COLOR_ALPHA: RGB565 little-endian + 1 byte
# di alpha). Niente librerie: le forme sono due rettangoli arrotondati e una
# linguetta, disegnati a risoluzione quadrupla e poi rimpiccioliti - e' il
# rimpicciolimento che fa i bordi morbidi e l'alpha dei contorni.
import io

N = 112
S = 4                      # supercampionamento
W = N * S

def dentro_rr(x, y, x0, y0, x1, y1, r):
    """Punto dentro un rettangolo con gli angoli arrotondati."""
    if x < x0 or x > x1 or y < y0 or y > y1:
        return False
    cx = min(max(x, x0 + r), x1 - r)
    cy = min(max(y, y0 + r), y1 - r)
    dx, dy = x - cx, y - cy
    return dx * dx + dy * dy <= r * r

# colori della cartella (dietro piu' scuro, davanti piu' chiaro)
DIETRO  = (0xD9, 0x8E, 0x1F)
DAVANTI = (0xF7, 0xB7, 0x33)

def colore(x, y):
    """Ritorna (r,g,b) oppure None se li' non c'e' niente."""
    # linguetta + corpo di dietro
    ling = dentro_rr(x, y, 14 * S, 26 * S, 54 * S, 44 * S, 5 * S)
    retro = dentro_rr(x, y, 14 * S, 34 * S, 98 * S, 88 * S, 7 * S)
    # falda davanti, leggermente piu' bassa: e' lei a dare l'idea di cartella
    fronte = dentro_rr(x, y, 14 * S, 44 * S, 98 * S, 88 * S, 7 * S)
    if fronte:
        return DAVANTI
    if ling or retro:
        return DIETRO
    return None

# accumulo a risoluzione piena e media su ogni blocco SxS
acc = [[[0, 0, 0, 0] for _ in range(N)] for _ in range(N)]
for yy in range(W):
    for xx in range(W):
        c = colore(xx, yy)
        if c is None:
            continue
        a = acc[yy // S][xx // S]
        a[0] += c[0]; a[1] += c[1]; a[2] += c[2]; a[3] += 1

byte = bytearray()
for y in range(N):
    for x in range(N):
        r, g, b, n = acc[y][x]
        if n == 0:
            byte += b'\x00\x00\x00'
            continue
        r //= n; g //= n; b //= n
        alpha = int(255 * n / (S * S))
        rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        byte += bytes([rgb565 & 0xFF, (rgb565 >> 8) & 0xFF, alpha])

righe = []
for i in range(0, len(byte), 12):
    righe.append('  ' + ' '.join('0x%02x,' % v for v in byte[i:i + 12]))

testa = '''#ifdef __has_include
    #if __has_include("lvgl.h")
        #ifndef LV_LVGL_H_INCLUDE_SIMPLE
            #define LV_LVGL_H_INCLUDE_SIMPLE
        #endif
    #endif
#endif

#if defined(LV_LVGL_H_INCLUDE_SIMPLE)
    #include "lvgl.h"
#else
    #include "lvgl/lvgl.h"
#endif

#ifndef LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_MEM_ALIGN
#endif

#ifndef LV_ATTRIBUTE_IMG_APP_ESPLORA
#define LV_ATTRIBUTE_IMG_APP_ESPLORA
#endif

/* Icona dell'app Esplora, 112x112: una cartella.
   Disegnata da strumenti/genera_icona_esplora.py, non da un programma di
   grafica, cosi' si puo' rigenerare cambiando due colori. Formato
   LV_IMG_CF_TRUE_COLOR_ALPHA (RGB565 little-endian + 1 byte di alpha), lo
   stesso di img_app_home: sfondo trasparente, sul tema scuro del launcher
   resta la sola sagoma. */
const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST LV_ATTRIBUTE_IMG_APP_ESPLORA
uint8_t img_app_esplora_map[] = {
'''

coda = '''};

const lv_img_dsc_t img_app_esplora = {
  .header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA,
  .header.always_zero = 0,
  .header.reserved = 0,
  .header.w = 112,
  .header.h = 112,
  .data_size = 112 * 112 * 3,
  .data = img_app_esplora_map,
};
'''

dest = r'D:\HA_DISPLAY\V0.3\components\apps\esplora_app\img_app_esplora.c'
import os
os.makedirs(os.path.dirname(dest), exist_ok=True)
io.open(dest, 'w', encoding='utf-8', newline='\n').write(testa + '\n'.join(righe) + '\n' + coda)
print('scritta', dest, len(byte), 'byte di pixel')
