#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_memory_utils.h"
#include "esp_heap_caps.h"
#include "esp_ldo_regulator.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp_board_extra.h"

#include "esp_brookesia.hpp"
#include "apps.h"
#include "home_dashboard/uart_console.h"
#include "home_dashboard/hosted_recovery.h"
#include "home_dashboard/ha_discover.h"
#include "home_dashboard/idle_manager.h"
#include "home_dashboard/ram_monitor.h"
#include "home_dashboard/batteria.h"
#include "home_dashboard/ota_update.h"
#include "home_dashboard/debug_config.h"
#include "setting/setup_wizard.h"
#include "../components/espressif__esp32_p4_function_ev_board/bsp_stc8h1kxx.h"
#include "esp_sleep.h"
#include "driver/rtc_io.h"
#include "esp_timer.h"

static const char *TAG = "main";

extern esp_lcd_touch_handle_t tp;
static int s_prev_brightness = 0;
static bool s_enter_sleep_flag = false;
void touch_detect_task(void *param)
{
    lv_indev_t *active_indev = lv_indev_get_act();  // Get the currently active input device
    while (1)
    {
        static uint32_t prev_boot_time_s = 0;
        uint32_t boot_time_s = esp_timer_get_time() / 1000 / 1000;

        uint16_t touch_x[1];
        uint16_t touch_y[1];
        uint8_t touch_cnt = 0;

        bool touchpad_pressed = esp_lcd_touch_get_coordinates(tp, touch_x, touch_y, NULL, &touch_cnt, 1);
        /*There are clicks on the touchscreen.*/
        if (touch_cnt) {
            prev_boot_time_s = boot_time_s;
            // If the screen was off before, restore the brightness
            if (s_enter_sleep_flag) {
                s_enter_sleep_flag = !s_enter_sleep_flag;
                bsp_display_brightness_set(s_prev_brightness);
            }
        }
        else {
            if (!s_enter_sleep_flag) {
                /* If there is no touch and it is in the non-screen-off state, and it 
                    has not been touched for more than a certain period of time, it enters the screen-off state*/
                if (60 < boot_time_s-prev_boot_time_s) {
                    s_enter_sleep_flag = !s_enter_sleep_flag;
                    s_prev_brightness = bsp_display_brightness_get();
                    bsp_display_brightness_set(0);
                }
            }
        }
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}


extern "C" void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(bsp_spiffs_mount());
    ESP_LOGI(TAG, "SPIFFS mount successfully");

#if CONFIG_EXAMPLE_ENABLE_SD_CARD
    esp_err_t sd_err = bsp_sdcard_mount();
    if (sd_err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_sdcard_mount failed: %s", esp_err_to_name(sd_err));
    }
    ESP_LOGI(TAG, "SD card mount successfully");
#endif

    ESP_ERROR_CHECK(bsp_extra_codec_init());

    stc8_i2c_init();
    /* Una lettura sola, per il registro di avvio. Lo spegnimento di
       protezione NON si decide qui: lo decide batteria.cpp, che per farlo
       pretende dieci letture riuscite di fila sotto soglia.

       Prima era il contrario, e la differenza non e' teorica. Qui la
       struttura veniva azzerata e stc8_battery_info_get esce in anticipo se
       l'I2C da' errore: bastava un errore su quel bus - lo stesso del touch,
       che sotto pressione ce ne ha dati 1205 in 71 secondi - per lasciare
       bat_voltage a zero, e zero e' minore di 3500. Il pannello se ne andava
       in deep sleep durante l'avvio e sembrava morto. */
    Battery_info_t battery_info = {0};
    esp_err_t bat_err = stc8_battery_info_get(&battery_info);
    ESP_LOGI(TAG, "alimentazione all'avvio: %s adc=%lu mV bat=%lu mV %d%% stato=%d led=%d",
             esp_err_to_name(bat_err), battery_info.adc_voltage, battery_info.bat_voltage,
             battery_info.bat_level, battery_info.bat_state, battery_info.led_state);
    batteria_avvia();

    /* Se questa immagine e' appena arrivata via rete, da qui parte la prova:
       viene confermata solo dopo che il pannello ha retto collegato a Home
       Assistant, altrimenti al prossimo riavvio il bootloader rimette quella
       di prima. */
    ota_update_init();

    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_H_RES * BSP_LCD_V_RES,
        .double_buffer = BSP_LCD_DRAW_BUFF_DOUBLE,
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,
            .sw_rotate = false,
        }
    };
    /* Prima di lv_init(): RAM interna o PSRAM per gli oggetti LVGL. */
    debug_config_early();
    bsp_display_start_with_config(&cfg);
    bsp_display_backlight_on();

    /* Lo spegnimento dello schermo dopo 60 s (touch_detect_task) e' sostituito
       da idle_manager: slideshow, poi schermo spento, risveglio sulla dashboard. */

    /* Schermata di avvio. Quella di Elecrow erano due bitmap RGB888 da 1,8 MB
       l'una (3,7 MB di firmware) con qualche secondo di animazione da
       aspettare: qui bastano due scritte, e il pannello parte prima. */
    bsp_display_lock(0);
    lv_obj_t *boot = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(boot, lv_color_hex(0x111318), 0);
    lv_obj_set_style_bg_opa(boot, LV_OPA_COVER, 0);
    lv_obj_t *boot_t = lv_label_create(boot);
    lv_label_set_text(boot_t, "Pannello Home Assistant");
    lv_obj_set_style_text_color(boot_t, lv_color_hex(0xE1E1E1), 0);
    lv_obj_set_style_text_font(boot_t, &lv_font_montserrat_34, 0);
    lv_obj_align(boot_t, LV_ALIGN_CENTER, 0, -20);
    lv_obj_t *boot_s = lv_label_create(boot);
    lv_label_set_text(boot_s, "avvio...");
    lv_obj_set_style_text_color(boot_s, lv_color_hex(0x8A8F98), 0);
    lv_obj_set_style_text_font(boot_s, &lv_font_montserrat_24, 0);
    lv_obj_align(boot_s, LV_ALIGN_CENTER, 0, 30);
    lv_scr_load(boot);

    ESP_Brookesia_Phone *phone = new ESP_Brookesia_Phone();
    assert(phone != nullptr && "Failed to create phone");

    ESP_Brookesia_PhoneStylesheet_t *phone_stylesheet = new ESP_Brookesia_PhoneStylesheet_t ESP_BROOKESIA_PHONE_1024_600_DARK_STYLESHEET();
    ESP_BROOKESIA_CHECK_NULL_EXIT(phone_stylesheet, "Create phone stylesheet failed");
    ESP_BROOKESIA_CHECK_FALSE_EXIT(phone->addStylesheet(*phone_stylesheet), "Add phone stylesheet failed");
    ESP_BROOKESIA_CHECK_FALSE_EXIT(phone->activateStylesheet(*phone_stylesheet), "Activate phone stylesheet failed");

    /* Brookesia costruisce la sua interfaccia sulla schermata attiva, che e'
       questa: cancellarla qui gli lascia in mano un puntatore morto (lo
       avevamo provato e il pannello moriva creando le icone del launcher).
       Resta li' sotto, coperta, come faceva quella di Elecrow. */
    assert(phone->begin() && "Failed to begin phone");

    AppSettings *app_settings = new AppSettings();
    assert(app_settings != nullptr && "Failed to create app_settings");
    int settings_app_id = phone->installApp(app_settings);
    assert((settings_app_id >= 0) && "Failed to begin app_settings");

    HomeDashboard *home_dashboard = new HomeDashboard();
    assert(home_dashboard != nullptr && "Failed to create home_dashboard");
    int ha_app_id = phone->installApp(home_dashboard);
    assert((ha_app_id >= 0) && "Failed to install home_dashboard");

    /* Esplora: cosa c'e' sulla SD, guardato stando davanti al pannello. Scarica
       e carica stanno solo nella pagina web, dove c'e' un computer dall'altra
       parte; qui si guarda e basta. Se non si installa non e' un guaio da
       fermare l'avvio - il pannello serve a mostrare Home Assistant. */
    EsploraApp *esplora = new EsploraApp();
    if (esplora && phone->installApp(esplora) < 0)
        ESP_LOGW(TAG, "l'app Esplora non si e' installata: vado avanti senza");

    /* Console di servizio sulla UART di debug: permette di aprire l'app e di
       scaricare uno screenshot dal PC, senza dover toccare il pannello. */
    uart_console_start(phone, ha_app_id);

    /* Un timeout SDIO verso il C6 non riavvia piu' il pannello: il trasporto
       viene rialzato in background e interfaccia e touch restano attivi. */
    hosted_recovery_start();

    /* mDNS: serve alla configurazione guidata per trovare Home Assistant da
       sola, e intanto fa rispondere il pannello al proprio nome (la sua
       pagina web si apre con <nome>.local invece che con l'indirizzo IP). */
    ha_discover_start();
    ha_discover_guard_start();

    /* Primo avvio: configurazione guidata sopra a tutto (rete, Home Assistant,
       dashboard, PIN, password). Si rifa' da Impostazioni > Sicurezza. */
    if (setup_wizard_needed()) setup_wizard_start_with(phone, settings_app_id);

    bsp_display_unlock();

    idle_manager_start(phone, ha_app_id);

    /* Interruttori di debug salvati (livello log, monitor prestazioni...). */
    debug_config_init();

    /* Un campione di memoria ogni ora in /spiffs/ramlog.csv (comando UART "ramlog"). */
    ram_monitor_start();
}
