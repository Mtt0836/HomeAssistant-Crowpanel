# Modifiche al firmware del C6

Il firmware del C6 è l'esempio **slave** di
[esp-hosted-mcu](https://github.com/espressif/esp-hosted-mcu) di Espressif,
versione **2.12.13**, con due cambiamenti. Qui ci sono solo quelli: il
progetto intero si scarica da Espressif, e ridistribuirlo tutto vorrebbe
dire portarsi dietro una copia che invecchia.

> Perché proprio la 2.12.13: la documentazione di ESP-Hosted dice che host e
> slave devono essere lo *stesso* codice. Il P4 scarica la 2.12.13, quindi
> il C6 deve avere quella. Il pannello usciva di fabbrica con la 2.12.3, e
> il disallineamento produceva cadute sporadiche del bus.

## Cosa cambia

**1. Ripristino automatico (`rollback_guard.c`, `rollback_guard.h`)**

Un'immagine appena installata resta in prova. Viene confermata solo quando
il canale col P4 è rimasto aperto due minuti con del traffico vero; se dopo
sei avvii (cinque minuti l'uno) non ci è riuscita, il C6 torna all'altro
slot. Un'immagine già confermata non viene più toccata.

Serve perché verso il C6 si passa solo dall'SDIO: un firmware che rompe il
bus non sarebbe più raggiungibile, se non aprendo il pannello e attaccandosi
ai contatti seriali.

Il bootloader di fabbrica del C6 **non** ha `BOOTLOADER_APP_ROLLBACK_ENABLE`
e non conviene riscriverlo via SDIO: il ripristino è quindi fatto
dall'applicazione, non dal bootloader.

**2. Bus in Default Speed (`sdkconfig.defaults.esp32c6`)**

`CONFIG_ESP_SDIO_DEFAULT_SPEED=y`. Il file è quello completo, commentato
dove serve.

## Come applicarle

```bash
git clone https://github.com/espressif/esp-hosted-mcu
cd esp-hosted-mcu/slave          # l'esempio slave
git checkout <la 2.12.13>

cp /percorso/modifiche/rollback_guard.[ch] main/
cp /percorso/modifiche/sdkconfig.defaults.esp32c6 .
git apply /percorso/modifiche/richiami.patch

idf.py set-target esp32c6
idf.py build
```

Il file da caricare sul pannello è `build/network_adapter.bin`.

`richiami.patch` sono le poche righe che richiamano il ripristino: tre
chiamate dentro `esp_hosted_coprocessor.c` (all'avvio, all'apertura e alla
chiusura del canale dati, e a ogni pacchetto che arriva dal P4), il file
nuovo aggiunto a `main/CMakeLists.txt`, e il numero di versione riportato
nella risposta che il P4 legge col comando `fw`.

Se la patch non si applica perché la versione è cambiata, sono modifiche
abbastanza piccole da rifare a mano guardando il diff.

> [!WARNING]
> Ogni nuovo firmware per il C6 deve contenere il ripristino automatico.
> Senza, un errore si paga aprendo il pannello.
