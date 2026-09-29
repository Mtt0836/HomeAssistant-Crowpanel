#pragma once
// TTS via Home Assistant (Piper): chiede a HA l'audio del testo, lo scarica e lo
// riproduce sull'altoparlante del pannello. Trigger: chiamata diretta o evento HA "panel_tts".
#ifdef __cplusplus
extern "C" {
#endif
void tts_player_init(void);            // inizializza audio (una volta)
void tts_player_say(const char *msg);  // asincrono: parla senza bloccare
#ifdef __cplusplus
}
#endif
