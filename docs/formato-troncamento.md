# I dieci `snprintf` che potrebbero tagliare

`-Wformat` è acceso sui nostri sorgenti (`V0.3/components/apps/CMakeLists.txt`)
ed è `-Werror`, quindi quello che segnala va corretto. `-Wformat-truncation`,
che ne fa parte, è invece spento di proposito, e questo file è il motivo per cui
si può farlo senza perdere l'informazione.

**La distinzione.** Un `snprintf` che taglia è comportamento *definito*: scrive
quello che ci sta, chiude la stringa e non sfora mai il buffer. Un numero o un
tipo di argomenti sbagliato è comportamento *indefinito*, e nessuno lo nota
leggendo. Sono due classi diverse di problema, e la seconda è quella che ci ha
morso davvero: in `batteria.cpp` i segnaposti erano dodici per quattordici
argomenti, le due colonne che servivano a ricontrollare l'autotaratura non sono
mai state scritte per un ciclo intero di batteria, e la caduta dello schermo —
un `uint32_t` — finiva in un `%.0f`.

Quindi: `-Wformat` resta, `-Wformat-truncation` no, e i punti che segnalava
stanno qui invece di essere chiusi in fretta per far passare la compilazione.

## L'elenco

Rilevati con ESP-IDF 6.1 / GCC 15.2 il 7 ottobre 2026, firmware 0.3.4.

| File | Riga | Cosa potrebbe tagliare |
|---|---|---|
| `home_dashboard/ota_update.cpp` | 361 | la parentesi finale di `nome versione (data ora)`, se chi chiama dà un buffer piccolo |
| `home_dashboard/esplora.cpp` | 88 | un nome di file fino a 255 byte in 72 |
| `home_dashboard/tts_player.cpp` | 30, 31 | un percorso fino a 121 byte in 120 |
| `home_dashboard/web_image.cpp` | 26 | un nome di file fino a 255 byte in 78 |
| `home_dashboard/web_image.cpp` | 217 | il suffisso `.tmp` quando il nome riempie già il buffer |
| `esplora_app/EsploraApp.cpp` | 222, 247 | l'ultimo carattere di una riga dell'elenco |
| `setting/setup_wizard.cpp` | 265, 266 | un SSID fino a 911 byte in 96 |

## Come guardarli

Due categorie, e si trattano in modo opposto.

**Dove il taglio è la cosa giusta** — l'elenco dei file sul pannello, una riga
dell'interfaccia — non c'è niente da correggere nel codice: c'è da scrivere che
è voluto, perché un nome lunghissimo *deve* essere accorciato per entrare in una
riga dello schermo.

**Dove il taglio produce un errore muto** — un percorso tagliato apre il file
sbagliato o nessuno, un `.tmp` tagliato scrive sopra l'originale — il buffer va
dimensionato sul dato, come si è fatto per l'indirizzo dell'OTA
(`HA_URL_MAX + sizeof(l->url)`, dove c'era `HA_URL_MAX * 2`: base più percorso
arrivano a 527 byte e in 256 non ci stavano, e un indirizzo tagliato faceva
fallire l'aggiornamento lamentando la rete).

Il sospetto principale è `web_image.cpp:217`: lì il nome tagliato e il suffisso
`.tmp` si contendono lo stesso spazio, ed è la scrittura di un file.

Per rivederli tutti, togliere `-Wno-format-truncation` dai due blocchi di
`V0.3/components/apps/CMakeLists.txt` e compilare con `ninja -C build -k 0`,
che li elenca tutti invece di fermarsi al primo.
