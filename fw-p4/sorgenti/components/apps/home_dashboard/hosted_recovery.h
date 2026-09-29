#pragma once

/* Recupero del collegamento SDIO verso il C6 senza riavviare il pannello.
   Richiede CONFIG_ESP_HOSTED_TRANSPORT_RESTART_ON_FAILURE disattivato. */

#ifdef __cplusplus
extern "C" {
#endif

void     hosted_recovery_start(void);
unsigned hosted_recovery_count(void);   // quanti recuperi dall'avvio
void     hosted_recovery_suspend(void); // spegne SDIO e recupero fino al riavvio (test)

#ifdef __cplusplus
}
#endif
