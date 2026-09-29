# Firmware del P4

L'ESP32-P4 è il chip che fa tutto: schermo, touch, grafica, il dialogo con
Home Assistant, la pagina web. Questo è il firmware che gli si carica.

- [`binari/`](binari) — già compilato, pronto da caricare
- [`sorgenti/`](sorgenti) — il progetto ESP-IDF, per chi vuole modificarlo

## Caricare i binari

Serve **esptool** (`pip install esptool`, oppure arriva con ESP-IDF) e un
cavo USB-C **dati**: quelli da sola ricarica non fanno comparire nessuna
porta seriale.

Collega il pannello e guarda che porta è comparsa. La scheda si presenta
come *USB-SERIAL CH340*: su Windows è tipo `COM5` (si vede in Gestione
dispositivi), su Linux `/dev/ttyUSB0`, su macOS `/dev/cu.usbserial-…`.

Poi, da dentro `binari/`:

```bash
esptool.py --chip esp32p4 -p PORTA -b 460800 --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m \
  0x2000 bootloader.bin 0x8000 partition-table.bin 0x10000 esp_brookesia_demo.bin
```

Su Windows, in PowerShell, la stessa cosa su una riga sola:

```powershell
esptool.py --chip esp32p4 -p COM5 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m 0x2000 bootloader.bin 0x8000 partition-table.bin 0x10000 esp_brookesia_demo.bin
```

Se `esptool.py` non viene trovato, prova `python -m esptool` al suo posto.

Dura un paio di minuti. Alla fine il pannello riparte da solo.

Se il caricamento non parte (`Failed to connect`), tieni premuto il tasto
BOOT mentre dai il comando, oppure abbassa la velocità con `-b 115200`.

## Cosa c'è nei tre file

| File | Indirizzo | Cos'è |
|---|---|---|
| `bootloader.bin` | `0x2000` | l'avvio |
| `partition-table.bin` | `0x8000` | come è divisa la flash |
| `esp_brookesia_demo.bin` | `0x10000` | l'applicazione (il nome è quello della demo da cui parte) |

`flash_args` è lo stesso elenco nel formato di esptool: con
`esptool.py --chip esp32p4 -p PORTA write_flash "@flash_args"` si ottiene la
stessa cosa senza scrivere gli indirizzi a mano.

## Cosa NON viene cancellato

Le impostazioni — Wi-Fi, abbinamento a Home Assistant, PIN, password della
pagina web, configurazione dello slideshow, registro della memoria — stanno
in due partizioni (`nvs` e `storage`) che il caricamento non tocca. Un
aggiornamento non fa perdere niente.

Per cancellare davvero tutto: *Impostazioni → Sicurezza → Impostazioni
iniziali* sul pannello, oppure `factory cancella` dalla console seriale. Con
`esptool.py erase_flash` si cancella tutto compreso il firmware, e bisogna
ricaricare.

## Compilare

Serve **ESP-IDF 5.4.4**. Altre versioni non sono state provate; la 5.5 in
particolare cambia cose nel BSP.

```bash
cd sorgenti
idf.py set-target esp32p4
idf.py build
idf.py -p PORTA flash monitor
```

La prima compilazione scarica i componenti gestiti (ESP-Brookesia,
ESP-Hosted, LVGL, il BSP) e ci mette parecchio.

Note:

- la configurazione che conta sta in `sdkconfig.defaults`, commentata dove
  non è ovvia. `sdkconfig` è generato e non sta nel repository: se lo
  cancelli si rifà da solo;
- l'immagine SPIFFS **non** viene caricata insieme al firmware, apposta: se
  lo facesse, ogni `idf.py flash` cancellerebbe la configurazione del
  pannello;
- le icone sono già generate e compilate. Per aggiungerne una si mette il
  nome in `strumenti/icone/elenco.txt` e si lancia
  `strumenti/costruisci_icone.ps1`: serve Node solo per quello, non per
  compilare;
- la pagina web è compressa e inclusa nel firmware. Dopo averla modificata
  si rigenera con `strumenti/costruisci_web.ps1`.
