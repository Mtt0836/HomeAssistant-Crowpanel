# Note tecniche

Le cose che hanno fatto perdere giorni, scritte perché non le perda anche
qualcun altro. Valgono per il CrowPanel Advance 10.1" V1.1/V1.2 con
ESP-IDF 5.4.4.

## La scheda SD alimenta il bus I2C

`bsp_sdcard_mount()` è l'**unico** punto del progetto che accende il canale 4
del regolatore LDO, e da quel canale dipende l'alimentazione di tutti i
dispositivi I2C: touch GT911, codec audio, il controllore STC8.

Togliere `CONFIG_EXAMPLE_ENABLE_SD_CARD` per alleggerire la compilazione
rende muto l'intero bus: i probe I2C vanno in *timeout* (non in NACK, il che
manda l'indagine su una pista sbagliata) e l'avvio si ferma dentro
l'inizializzazione del touch, prima ancora di accendere la retroilluminazione.
Senza scheda inserita il montaggio fallisce lo stesso, ma l'LDO viene acceso
*prima* del fallimento — ed è quello che conta.

Non disattivarla. Se un giorno si vuole togliere la SD, prima va spostata
l'accensione dell'LDO 4 fuori da `bsp_sdcard_mount()`.

## SDIO verso il C6

Bus a **4 linee**: D0=17, D1=16, D2=15, D3=14, CLK=18, CMD=19, reset su
GPIO32. Il sorgente di fabbrica è per la V1.0 (D0=14, una linea sola) e va
corretto — è in `sdkconfig.defaults`.

Serve anche `CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM=y`: con esp_hosted
2.12.x la RAM interna non basta e l'avvio muore su
`assert failed: sdio_mempool_create`.

## Host e slave ESP-Hosted devono essere la stessa versione

La documentazione del componente lo dice: *slave e host devono usare lo
stesso codice*. Il pannello esce di fabbrica con lo slave **2.12.3**, mentre
il manifest tira sull'host la **2.12.13**. Il disallineamento produce cadute
sporadiche del bus difficili da attribuire.

Non si può allineare l'host verso il basso: la 2.12.3 alloca i buffer SDIO
con `MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA` cablato, senza opzione PSRAM, e
riporta il crash d'avvio. Va aggiornato il C6 (vedi `fw-c6/`).

## `Failed to send data: 258` non è un timeout

258 = 0x102 = `ESP_ERR_INVALID_ARG`; il timeout sarebbe 0x107. È il rifiuto
per **allineamento DMA** descritto in
[esp-hosted-mcu#219](https://github.com/espressif/esp-hosted-mcu/issues/219):
con i buffer TX in PSRAM e `CONFIG_CACHE_L2_CACHE_LINE_128B=y` (che la
configurazione di fabbrica imposta) metà dei blocchi non è allineata a 128 e
`sdmmc_io_write_blocks()` li rifiuta prima ancora del bus.

Si risolve con **`CONFIG_CACHE_L2_CACHE_LINE_64B=y`**, che però richiede di
ricaricare anche il bootloader. Misure prima e dopo: scaricamenti da 1,2 MB
riusciti da 7 su 10 a 10 su 10, e i recuperi del collegamento a ogni avvio
spariti.

Firma distintiva del guaio: i due tentativi falliti hanno lo stesso
timestamp.

## La richiesta ARP di lwIP inchiodava il bus

Con ESP-Hosted lo stack TCP/IP gira sul P4. lwIP non rinfresca le voci ARP
col traffico normale: a `ARP_MAXAGE - 30` secondi (cioè 270 con i valori
predefiniti) manda una richiesta ARP *unicast* al MAC già noto, e in quel
preciso momento una scrittura SDIO verso il C6 falliva.

La prova: abbassando `ARP_MAXAGE` a 120 i guasti si sono spostati da ~270 s
a ~90 s dopo la connessione, esattamente come previsto.

Rimedio in `home_dashboard/arp_pin.cpp`: il MAC di Home Assistant imparato al
collegamento viene fissato con `etharp_add_static_entry`, e il rinnovo non
parte più. Va rifatto a ogni connessione, perché il recupero dell'SDIO
ricrea l'interfaccia di rete e svuota la tabella.

## Le raffiche in ingresso

Lo stesso guasto scattava quando Home Assistant risponde con qualcosa di
grosso (le statistiche per i grafici): a fallire era l'ACK del P4.
`CONFIG_LWIP_TCP_WND_DEFAULT=2880` (due segmenti invece di quattro) ha
ridotto i recuperi da cinque a due in sette minuti di prova.

Da non fare: mettere l'host SDIO in modalità *packet*
(`ESP_HOSTED_SDIO_OPTIMIZATION_RX_NONE`). Il C6 è in streaming e l'host va in
assert all'avvio, in ciclo di riavvii. Per passare a packet va cambiato anche
lo slave.

## Il pannello si rialza da solo

`hosted_recovery.cpp` intercetta `ESP_HOSTED_EVENT_TRANSPORT_FAILURE` e
rialza il trasporto in circa 3,2 secondi senza riavviare il pannello
(`TRANSPORT_RESTART_ON_FAILURE` è disattivato apposta). Il conteggio dei
recuperi finisce in un sensore di Home Assistant.

Nel recupero, `esp_hosted_deinit()` va chiamato **prima** di chiudere
l'interfaccia di rete, e non si devono fare chiamate RPC mentre il recupero è
in corso: sono due modi documentati di far andare il pannello in panico.

## Memoria di LVGL

L'allocatore sta in `main/lvgl_mem.c`, compilato dentro la libreria LVGL, e
copre `alloc`, `realloc` e `free`; sceglie RAM interna o PSRAM prima di
`lv_init`. **Va lasciato in PSRAM**: con le Impostazioni aperte la RAM
interna libera resta a 133 KB in PSRAM e scende a 41 KB (minimo 34) in RAM
interna.

Il guaio originale era più sottile: deviando in PSRAM solo `alloc`, i blocchi
ridimensionati tornavano in RAM interna, e si vedeva un calo di ~40 KB
aprendo le Impostazioni.

## Le foto dello slideshow

Tre trappole del decoder JPEG hardware del P4:

- vuole il buffer di uscita con larghezza e altezza arrotondate a multipli di
  16, se no rifiuta; le righe e colonne in più si ritagliano col PPA;
- LVGL tiene una cache delle immagini: riusando lo stesso `lv_img_dsc_t` per
  ogni foto serve `lv_img_cache_invalidate_src()` prima di
  `lv_img_set_src()`, altrimenti ridisegna la voce vecchia con un puntatore
  già liberato e si vede rumore;
- **non sa rimpicciolire mentre decodifica** (IDF 5.4): una foto da 12
  megapixel vorrebbe 23 MB di PSRAM e viene saltata. Le foto vanno ridotte
  prima, sul PC — `strumenti/converti_foto.bat`.

## Home Assistant: le previsioni non sono attributi

La documentazione di HA lo dice esplicitamente: le previsioni del meteo non
stanno nello stato dell'entità, arrivano solo a chi si iscrive con
`weather/subscribe_forecast`. Stessa storia per i template della card
markdown, che si espandono con `render_template` e restano aggiornati.

Nel formato compresso di `subscribe_entities`, poi, gli attributi che
**spariscono** (la luminosità quando una luce si spegne) non vengono
segnalati: chi disegna deve guardare prima lo stato.

## Fermare e riavviare il WebSocket in due

Il client WebSocket lo fermano e lo riavviano due parti diverse: il watchdog
della riconnessione quando il collegamento cade, e `ha_ws_restart` quando
cambia l'indirizzo o la dashboard. Se capitano insieme,
`esp_websocket_client_set_uri` libera e rifà le stringhe dentro il client
mentre l'altro le sta ancora usando, e il pannello muore con
`assert failed: tlsf_free ... block already marked as free`.

Succede davvero: basta toccare "cambia dashboard" mentre la connessione si
sta rialzando. Adesso i due passano uno alla volta da un mutex.

Nota per chi legge il dump: nel panico non c'è il backtrace, ma gli
indirizzi nello stack decodificati con `riscv32-esp-elf-addr2line` portano
dritti al punto (`reconnect_now` → `ha_ws_restart` →
`esp_websocket_client_set_uri`).

## Cambiare schermata da dentro un evento di LVGL

Un pulsante che nella sua callback ricostruisce la schermata sta cancellando
sé stesso mentre l'evento è ancora in corso, e LVGL subito dopo ci torna
sopra. Il cambio va rimandato di un attimo con `lv_async_call`, che lo
esegue appena finito il giro.

## Date e fusi orari

Nella libreria C del pannello `timegm` non esiste, e `mktime` applica il fuso
locale a un istante che è già UTC. Le date ISO di Home Assistant vanno
convertite contando i giorni dall'epoca a mano, se no le previsioni serali
finiscono nel giorno sbagliato.
