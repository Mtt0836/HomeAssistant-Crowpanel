#pragma once
// Mini server web sul pannello:
//   /       editor della dashboard (riordina e nascondi le card)
//   /setup  URL e token di Home Assistant
// La pagina dell'editor arriva dalla SD (/sdcard/web/panel.html[.gz]) se c'e',
// altrimenti da una copia compilata nel firmware: cosi' si puo' aggiornare
// copiando un file, senza restare mai senza pagina.
#ifdef __cplusplus
extern "C" {
#endif
typedef void (*web_cfg_changed_cb_t)(void);     // dopo salvataggio URL/token
typedef void (*web_layout_changed_cb_t)(void);  // dopo salvataggio dei ritocchi
void web_config_start(web_cfg_changed_cb_t on_cfg, web_layout_changed_cb_t on_layout);
#ifdef __cplusplus
}
#endif
