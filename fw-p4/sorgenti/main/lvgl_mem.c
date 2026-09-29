#include "lvgl_mem.h"

#include <stdlib.h>
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"

/* Predefinito PSRAM: e' quello che faceva gia' la configurazione Elecrow
   (LV_MEM_CUSTOM_ALLOC su MALLOC_CAP_SPIRAM), ma solo per le allocazioni
   nuove: realloc() e free() restavano quelli di sistema, e i blocchi
   ridimensionati potevano finire in RAM interna. Qui passano tutti di qui. */
static bool s_psram = true;
static size_t s_int = 0, s_ext = 0;

static inline void account(void *p, bool add)
{
    if (!p) return;
    size_t n = heap_caps_get_allocated_size(p);
    size_t *c = esp_ptr_external_ram(p) ? &s_ext : &s_int;
    if (add) __atomic_fetch_add(c, n, __ATOMIC_RELAXED);
    else     __atomic_fetch_sub(c, n, __ATOMIC_RELAXED);
}

static inline uint32_t first_caps(void)
{
    return s_psram ? (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static inline uint32_t second_caps(void)
{
    return s_psram ? (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) : (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void *lvgl_mem_alloc(size_t size)
{
    void *p = heap_caps_malloc_prefer(size, 2, first_caps(), second_caps());
    account(p, true);
    return p;
}

void *lvgl_mem_realloc(void *ptr, size_t size)
{
    if (!ptr) return lvgl_mem_alloc(size);
    account(ptr, false);
    void *p = heap_caps_realloc_prefer(ptr, size, 2, first_caps(), second_caps());
    account(p ? p : ptr, true);        // se fallisce, il vecchio blocco resta valido
    return p;
}

void lvgl_mem_free(void *ptr)
{
    account(ptr, false);
    free(ptr);
}

void lvgl_mem_set_psram(bool psram) { s_psram = psram; }
bool lvgl_mem_get_psram(void) { return s_psram; }

void lvgl_mem_stats(size_t *internal_bytes, size_t *psram_bytes)
{
    if (internal_bytes) *internal_bytes = s_int;
    if (psram_bytes)    *psram_bytes = s_ext;
}
