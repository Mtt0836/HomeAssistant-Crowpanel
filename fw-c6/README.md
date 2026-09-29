# Firmware della radio (ESP32-C6)

Il Wi-Fi non è nel P4. Sul CrowPanel c'è un secondo chip, un **ESP32-C6**,
che fa solo da scheda di rete: è collegato al P4 con un bus SDIO a quattro
linee e programmato con **ESP-Hosted**. Lo stack TCP/IP gira sul P4, il C6
porta i pacchetti e basta.

> [!IMPORTANT]
> **Di solito non serve aggiornarlo.** Il C6 esce di fabbrica con un
> firmware che funziona, e il pannello ci va. Questa cartella serve se il
> collegamento radio si interrompe spesso.

## Quando ha senso aggiornarlo

Il sensore **Recuperi del collegamento radio** (in Home Assistant, sulla
scheda del pannello) conta quante volte il bus verso il C6 è caduto ed è
stato rialzato. Qualche recupero al giorno non è un problema. Se invece ne
conti diversi ogni pochi minuti, o se i grafici non finiscono mai di
caricarsi, questa versione risolve due cose:

- **velocità del bus**. Il firmware di fabbrica è compilato in *High Speed*
  mentre il P4 pilota il bus a 20 MHz: in quel modo uno scaricamento da
  1,2 MB non arrivava mai in fondo, cadeva fra i 200 KB e il mega. Con
  *Default Speed* arriva in fondo 7 volte su 10, a circa 400 KB/s.
- **ripristino automatico**. Un'immagine appena installata resta *in prova*
  finché il canale col P4 non è rimasto aperto due minuti con del traffico.
  Se non ci riesce, dopo sei avvii falliti torna da sola all'immagine di
  prima. Senza questo, un firmware sbagliato renderebbe il pannello muto per
  sempre: verso il C6 si passa solo dall'SDIO, e se l'SDIO è rotto non c'è
  più strada.

## Come si aggiorna: via SDIO, dal pannello stesso

Non serve smontare niente né collegare fili: il P4 scarica l'immagine e la
scrive nel C6 attraverso il collegamento che hanno già.

1. Sul PC, dalla cartella `binari/`, avvia il server che serve i file a
   pezzi:

   ```bash
   python rangeserver.py 8000
   ```

   Serve questo e non `python -m http.server`: il pannello scarica a blocchi
   di 16 KB con richieste *Range*, così una caduta del collegamento costa
   solo il blocco in corso invece di tutto lo scaricamento.

   > Il server si mette in ascolto su tutte le interfacce e serve la cartella
   > da cui lo lanci: aprilo nella cartella `binari/` e non in una che
   > contenga altro, e chiudilo appena finito. Lo scaricamento è in chiaro e
   > senza firma, quindi va fatto su una rete di cui ti fidi: chi si mettesse
   > in mezzo deciderebbe cosa finisce dentro il C6.

2. Dalla console seriale del pannello (115200 baud):

   ```
   slaveota http://INDIRIZZO-DEL-PC:8000/network_adapter.bin
   ```

   Ci mette qualche minuto. Alla fine il C6 riparte con l'immagine nuova.

3. Controlla con il comando `fw`. Il campo *build* dice quale immagine è
   attiva:

   | build | immagine |
   |---|---|
   | -1 | quella di fabbrica |
   | 1 | ripristino automatico, bus in High Speed |
   | 2 | ripristino automatico, bus in Default Speed — **questa** |

Se qualcosa va storto non hai rotto niente: il C6 ha due slot e torna da
solo a quello di prima. Per provarlo apposta c'è `slaveota <url> prova`, che
spegne l'SDIO subito dopo l'attivazione in modo che l'immagine nuova non
riesca a confermarsi; dopo mezz'ora il C6 è tornato indietro.

## Come si aggiorna: con un adattatore seriale

Solo se il C6 è già morto e l'SDIO non risponde più. Bisogna aprire il
pannello: le piazzole stanno nella fila vicino al modulo C6, lato
componenti — **P31** massa, **P25** TX, **P21** RX, **P36** IO9 (da tenere a
massa all'accensione per entrare in caricamento). Serve un adattatore a
3,3 V.

Poi, con tutti e quattro i file di `binari/`:

```bash
esptool.py --chip esp32c6 -p PORTA write_flash --flash_mode dio --flash_freq 80m --flash_size 4MB \
  0x0 bootloader.bin 0x8000 partition-table.bin 0xd000 ota_data_initial.bin 0x10000 network_adapter.bin
```

## Compilare

I sorgenti completi non sono qui: sono l'esempio *slave* di
[esp-hosted-mcu](https://github.com/espressif/esp-hosted-mcu) di Espressif,
che si scarica da lì. In [`modifiche/`](modifiche) ci sono solo le
differenze, che sono poche.

Vedi [`modifiche/README.md`](modifiche/README.md).
