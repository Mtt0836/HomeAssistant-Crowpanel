#pragma once
/* Allocatore di LVGL (LV_MEM_CUSTOM): sceglie RAM interna o PSRAM secondo
   Impostazioni > Debug e tiene il conto dei byte in uso in ciascuna.
   Compilato dentro la libreria LVGL (vedi main/CMakeLists.txt). */
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void *lvgl_mem_alloc(size_t size);
void *lvgl_mem_realloc(void *ptr, size_t size);
void  lvgl_mem_free(void *ptr);

// true = PSRAM (con ripiego in RAM interna se esaurita). Va scelto prima di
// lv_init(): i blocchi gia' allocati restano dove sono.
void lvgl_mem_set_psram(bool psram);
bool lvgl_mem_get_psram(void);

// Byte attualmente allocati da LVGL in RAM interna e in PSRAM.
void lvgl_mem_stats(size_t *internal_bytes, size_t *psram_bytes);

#ifdef __cplusplus
}
#endif
