#pragma once
#include "lvgl.h"
#include "esp_brookesia.hpp"
#include "cJSON.h"

/*
 * App "Home" per il launcher ESP-Brookesia (CrowPanel ESP32-P4 10.1").
 * Mette insieme i moduli:
 *   - ha_config      : URL + token in NVS
 *   - lovelace_ui    : vista Lovelace letta da HA (lovelace/config) resa in LVGL
 *   - ha_ws          : WebSocket verso HA (con watchdog reconnect)
 *   - web_config     : /setup (token) e / (editor dashboard)
 *   - tts_player     : TTS via Piper (trigger anche da evento HA "panel_tts")
 *
 * Primo avvio (config assente): mostra schermata con IP + QR verso http://<ip>/setup.
 * Config presente: si connette a HA, scarica la dashboard e la disegna.
 */
class HomeDashboard : public ESP_Brookesia_PhoneApp {
public:
    HomeDashboard();
    ~HomeDashboard();
    bool run(void);
    bool back(void);
    bool close(void);
    bool init(void) override;

    /* Rilegge la configurazione e si ricollega a Home Assistant: la usa la
       configurazione guidata dopo aver cambiato indirizzo o abbinamento. */
    static void on_config_changed(void);
    static void reconnect_now(void);   // il lavoro vero, nel task dedicato

private:
    bool _ui_ready = false;
    lv_obj_t *_root = nullptr;
    lv_obj_t *_status_dot = nullptr;

    void build_setup_screen(void);   // primo avvio: QR + IP
    void build_dashboard(void);      // vista Lovelace (o messaggio di attesa/errore)
    void rebuild(void);              // ricostruzione, chiamare col lock del display
    void destroy_ui(void);

    // callback dei moduli (statici, con singleton interno)
    static void on_status(bool connected);
    static void on_lovelace(cJSON *config, const char *error);
    static void on_entity(const char *entity_id, const char *state, cJSON *attrs);
    static void on_layout_changed(void);
    static void on_tts(const char *message);
};

extern "C" void home_dashboard_reconnect(void);
