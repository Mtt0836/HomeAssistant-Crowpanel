#include "debug_config.h"

#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"

static const char *TAG = "dbg_cfg";
static const char *NS  = "dbgcfg";

static debug_config_t s_cfg = { DBG_LOG_INFO, true, false, false, true };

/* In main/lvgl_mem.c, compilato dentro la libreria LVGL. */
extern "C" void lvgl_mem_set_psram(bool psram);

// ---- monitor prestazioni ------------------------------------------------
/* LVGL 8 senza LV_USE_PERF_MONITOR: conto i refresh col monitor_cb del
   driver del display (chiamato a ogni ridisegno completato) e ricavo la CPU
   dell'interfaccia dal tempo di inattivita' del task LVGL. */
static lv_obj_t *s_perf = nullptr;
static lv_timer_t *s_perf_timer = nullptr;
static volatile uint32_t s_frames = 0;
static void (*s_prev_monitor)(lv_disp_drv_t *, uint32_t, uint32_t) = nullptr;
static bool s_hooked = false;

static void monitor_cb(lv_disp_drv_t *drv, uint32_t time, uint32_t px)
{
    s_frames++;
    if (s_prev_monitor) s_prev_monitor(drv, time, px);
}

static void perf_tick(lv_timer_t *t)
{
    static int64_t last = 0;
    int64_t now = esp_timer_get_time();
    uint32_t frames = s_frames;
    s_frames = 0;
    float secs = last ? (now - last) / 1e6f : 1.0f;
    last = now;
    if (s_perf) {
        lv_label_set_text_fmt(s_perf, "%d FPS  CPU UI %d%%", (int)(frames / secs + 0.5f), 100 - lv_timer_get_idle());
    }
}

static void perf_apply(bool on)
{
    bsp_display_lock(0);
    lv_disp_t *disp = lv_disp_get_default();
    if (on && !s_perf && disp) {
        if (!s_hooked) {
            s_prev_monitor = disp->driver->monitor_cb;
            disp->driver->monitor_cb = monitor_cb;
            s_hooked = true;
        }
        s_perf = lv_label_create(lv_layer_sys());
        lv_obj_set_style_bg_color(s_perf, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_perf, LV_OPA_60, 0);
        lv_obj_set_style_text_color(s_perf, lv_color_hex(0x7CFC00), 0);
        lv_obj_set_style_text_font(s_perf, &lv_font_montserrat_16, 0);
        lv_obj_set_style_pad_all(s_perf, 6, 0);
        lv_obj_align(s_perf, LV_ALIGN_BOTTOM_RIGHT, -4, -4);
        lv_label_set_text(s_perf, "...");
        s_perf_timer = lv_timer_create(perf_tick, 1000, nullptr);
    } else if (!on && s_perf) {
        lv_timer_del(s_perf_timer);
        s_perf_timer = nullptr;
        lv_obj_del(s_perf);
        s_perf = nullptr;
    }
    bsp_display_unlock();
}

// ---- applicazione -------------------------------------------------------

static void apply(void)
{
    static const esp_log_level_t map[] = { ESP_LOG_NONE, ESP_LOG_ERROR, ESP_LOG_WARN, ESP_LOG_INFO, ESP_LOG_DEBUG };
    esp_log_level_set("*", map[s_cfg.log_level]);
    perf_apply(s_cfg.perf_monitor);
}

void debug_config_early(void)
{
    nvs_handle_t h;
    uint8_t v = 1;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "lvps", &v);
        nvs_close(h);
    }
    s_cfg.lvgl_psram = v != 0;
    lvgl_mem_set_psram(s_cfg.lvgl_psram);
    ESP_LOGI(TAG, "oggetti LVGL in %s", s_cfg.lvgl_psram ? "PSRAM" : "RAM interna");
}

void debug_config_init(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, "log", &v) == ESP_OK && v <= DBG_LOG_DEBUG) s_cfg.log_level = (dbg_log_level_t)v;
        if (nvs_get_u8(h, "uart", &v) == ESP_OK) s_cfg.uart_console = v != 0;
        if (nvs_get_u8(h, "perf", &v) == ESP_OK) s_cfg.perf_monitor = v != 0;
        if (nvs_get_u8(h, "hav", &v) == ESP_OK)  s_cfg.ha_verbose   = v != 0;
        nvs_close(h);
    }
    apply();
    ESP_LOGI(TAG, "log=%d console=%d perf=%d ha_verbose=%d", s_cfg.log_level, s_cfg.uart_console,
             s_cfg.perf_monitor, s_cfg.ha_verbose);
}

const debug_config_t *debug_config_get(void) { return &s_cfg; }

void debug_config_set(const debug_config_t *c)
{
    s_cfg = *c;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "log", (uint8_t)s_cfg.log_level);
        nvs_set_u8(h, "uart", s_cfg.uart_console);
        nvs_set_u8(h, "perf", s_cfg.perf_monitor);
        nvs_set_u8(h, "hav", s_cfg.ha_verbose);
        nvs_set_u8(h, "lvps", s_cfg.lvgl_psram);   // letto solo all'avvio
        nvs_commit(h);
        nvs_close(h);
    }
    apply();
}
