# Pannello Home Assistant per CrowPanel Advance 10.1"

Un pannello a muro che mostra le dashboard di **Home Assistant** su un
Elecrow CrowPanel Advance 10.1" (ESP32-P4 + ESP32-C6), le disegna con LVGL e
le comanda al tocco. Quando nessuno lo usa diventa una cornice digitale con
le foto della microSD, l'ora e i valori che scegli tu; al primo tocco torna
la dashboard.

Non è un browser: non c'è una pagina web dentro. Il pannello parla il
protocollo WebSocket di Home Assistant, si legge la dashboard che hai già
fatto e la ridisegna con i widget nativi. Per questo va fluido su un
microcontrollore e si riaccende in un istante.

Nella cartella [`custom_components/crowpanel`](custom_components/crowpanel)
c'è anche l'**integrazione per Home Assistant**: fa comparire il pannello
dentro HA come un dispositivo vero, con lo schermo che si accende e si
spegne, la luminosità, la diagnostica e le impostazioni dello slideshow —
anche da fuori casa.

---

**Versione 0.3.5 — 10 ottobre 2026.** Cosa e' cambiato da una versione
all'altra sta in [CHANGELOG.md](CHANGELOG.md).

Provata su hardware vero: un CrowPanel Advance 10.1" V1.2 con ESP-IDF 6.1,
collegato a un Home Assistant di casa. Il pannello ha retto una veglia
continua con il registro della seriale sotto osservazione, attraversando piu'
volte il ciclo completo dello standby — slideshow, schermo spento, risveglio
— senza un riavvio; l'aggiornamento via rete e' stato provato da capo a
fondo, compresa un'immagine con l'impronta sbagliata di proposito, che il
pannello ha rifiutato; e un ciclo completo di batteria, 17 ore di scarica e
13 di ricarica, ha prodotto la prima curva misurata sulla cella vera.

Quello che non e' ancora stato provato e' scritto dove serve, senza
nasconderlo. La voce degli avvisi richiede un altoparlante collegato al
connettore della scheda, che non e' montato di serie, e non ha ancora prodotto
suono. L'autotaratura della batteria e' stata misurata su una cella e una
scheda: la curva che impara vale per la tua, non per quella di qualcun altro,
ed e' il motivo per cui la impara invece di averla scritta dentro.

---

## Indice

- [Cosa serve](#cosa-serve)
- [Installazione](#installazione)
  - [1. Firmware del P4](#1-firmware-del-p4)
  - [2. Firmware del C6](#2-firmware-del-c6-di-solito-non-serve)
  - [3. Prima accensione](#3-prima-accensione)
  - [4. Integrazione per Home Assistant](#4-integrazione-per-home-assistant)
- [Come si usa](#come-si-usa)
  - [La dashboard](#la-dashboard)
  - [Lo slideshow](#lo-slideshow)
  - [La pagina web del pannello](#la-pagina-web-del-pannello)
  - [Le impostazioni sul pannello](#le-impostazioni-sul-pannello)
  - [Aggiornare il firmware da Home Assistant](#aggiornare-il-firmware-da-home-assistant)
  - [Esplora: file, testi e foto](#esplora-file-testi-e-foto)
  - [La batteria](#la-batteria)
  - [La console seriale](#la-console-seriale)
- [Cosa sa disegnare](#cosa-sa-disegnare)
- [Compilare da sorgenti](#compilare-da-sorgenti)
- [Se qualcosa non va](#se-qualcosa-non-va)
- [Cosa manca](#cosa-manca)
- [Com'è fatto dentro](#comè-fatto-dentro)
- [Licenza e ringraziamenti](#licenza-e-ringraziamenti)

---

## Cosa serve

| | |
|---|---|
| **Pannello** | Elecrow CrowPanel Advance 10.1", **hardware V1.1 o V1.2** (sulla V1.0 i pin dell'SDIO sono diversi e non va). Schermo 1024×600, ESP32-P4 con 32 MB di PSRAM e 16 MB di flash, ESP32-C6 per il Wi-Fi. Provato sulla V1.2. |
| **microSD** | Una qualunque, formattata FAT32. **Va lasciata inserita** anche se non ci metti le foto: vedi l'avvertenza qui sotto. |
| **Alimentazione** | USB-C, almeno 2 A. Lo schermo da solo si mangia buona parte di un alimentatore da telefono. |
| **Home Assistant** | 2024.11 o più recente, raggiungibile dal pannello. Serve un account **amministratore** per l'abbinamento. |
| **Un PC** | Per il primo caricamento del firmware, con un cavo USB-C dati (non uno di quelli da sola ricarica). |

> [!IMPORTANT]
> **La microSD va lasciata dentro.** Sul CrowPanel è la stessa funzione che
> monta la scheda ad accendere il regolatore LDO canale 4, da cui dipende
> l'alimentazione di tutto il bus I2C: touch, codec audio e il resto. Senza
> scheda il pannello parte lo stesso, ma è un equilibrio che conviene non
> mettere alla prova. Se il touch non risponde, la prima cosa da guardare è
> se la scheda è al suo posto.

---

## Installazione

### 1. Firmware del P4

I binari già compilati stanno in [`fw-p4/binari`](fw-p4/binari). Serve
**esptool**, che si installa con `pip install esptool` (oppure arriva già con
ESP-IDF).

Collega il pannello al PC con il cavo USB-C. Sul PC compare una porta seriale
(`COM5`, `/dev/ttyUSB0`, `/dev/cu.usbserial-…`): la scheda si presenta come
**USB-SERIAL CH340**. Poi, dalla cartella `fw-p4/binari`:

```bash
esptool.py --chip esp32p4 -p PORTA -b 460800 --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m \
  0x2000 bootloader.bin 0x8000 partition-table.bin 0x10000 ota_data_initial.bin 0x20000 HA_Display.bin
```

Su Windows con PowerShell è la stessa riga, con `` ` `` al posto di `\` per
andare a capo, oppure tutto su una riga sola.

Dura un paio di minuti. Alla fine il pannello riparte da solo e mostra la
configurazione guidata.

> [!NOTE]
> Il caricamento **non cancella** le impostazioni (Wi-Fi, abbinamento a Home
> Assistant, foto, configurazione dello slideshow): quelle stanno in
> partizioni che l'aggiornamento non tocca. Per ripartire davvero da zero c'è
> *Impostazioni → Sicurezza → Impostazioni iniziali* sul pannello, oppure il
> comando `factory cancella` dalla seriale.

Istruzioni per esteso, con le varianti: [`fw-p4/README.md`](fw-p4/README.md).

### 2. Firmware del C6 (di solito non serve)

Il Wi-Fi non è nel P4: sta in un **ESP32-C6** che fa da radio, collegato
all'altro chip via SDIO e programmato con ESP-Hosted. Il C6 esce di fabbrica
con un firmware che funziona, e **non va aggiornato per far andare il
pannello**.

Serve aggiornarlo solo se il collegamento radio si interrompe spesso: la
versione qui dentro risolve due guai concreti (il bus SDIO tarato a una
velocità che il P4 non regge, e un'immagine che si ripristina da sola se
rompe il collegamento). Come e quando farlo:
[`fw-c6/README.md`](fw-c6/README.md).

### 3. Prima accensione

Alla prima accensione parte una configurazione guidata di nove passi. Si
tocca sullo schermo, con la tastiera a video.

1. **Benvenuto**
2. **Rete Wi-Fi** — cerca le reti e chiede la password.
3. **Nome e stanza** — come vuoi che il pannello si chiami in Home Assistant
   e in che stanza sta.
4. **Home Assistant** — non c'è niente da scrivere: Home Assistant si
   annuncia da solo sulla rete e il pannello lo trova, con il nome che gli
   hai dato tu. Tocchi quello giusto. Se sta su un'altra sottorete o dietro
   un indirizzo esterno, l'annuncio non arriva e lo si scrive a mano, con il
   pulsante sotto l'elenco.
5. **Autorizzazione** — il pannello mostra un QR code. Lo inquadri col
   telefono, si apre la sua pagina web, premi *Entra con Home Assistant* e
   autorizzi lì, con la solita schermata di HA. **Non devi generare né
   incollare nessun token**: il permesso il pannello se lo rinnova da solo.
   Fallo con un account **amministratore**, se no i comandi da HA non
   arriveranno.
6. **Dashboard** — quale dashboard e quale vista mostrare.
7. **PIN dello schermo** — facoltativo, per bloccare le impostazioni.
8. **Password della pagina web** — per la pagina di configurazione via
   browser.
9. **Tutto pronto**

La guidata si può rifare in qualunque momento da *Impostazioni → Sicurezza*,
o con `wizard reset` dalla seriale.

### 4. Integrazione per Home Assistant

**Con HACS** (si aggiorna da sola):

1. In HACS, menu in alto a destra → **Custom repositories**.
2. Incolla l'indirizzo di questo repository, categoria **Integration**.
3. Cerca **Pannello CrowPanel** e installalo.
4. Riavvia Home Assistant.

**A mano**: copia la cartella `custom_components/crowpanel` dentro la
cartella di configurazione di Home Assistant, accanto a
`configuration.yaml`, così da avere
`config/custom_components/crowpanel/manifest.json`. Poi riavvia HA.

In tutti e due i casi, alla fine: **Impostazioni → Dispositivi e servizi →
Aggiungi integrazione → Pannello CrowPanel**. Non chiede niente, conferma e
basta. Entro un minuto il pannello compare da solo come dispositivo.

Tutto il resto — entità, servizi, impostazioni dello slideshow da HA — sta in
[`docs/integrazione-home-assistant.md`](docs/integrazione-home-assistant.md).

---

## Come si usa

### La dashboard

Il pannello mostra una vista della tua dashboard di Home Assistant, disegnata
con i widget di LVGL. Il tocco su una card fa quello che ti aspetti: accende
e spegne una luce, regola la luminosità sul cerchio, alza e abbassa il
termostato, mette in pausa la musica.

Cambia da solo appena cambia in Home Assistant: se aggiungi una card, entro
pochi secondi compare anche sul pannello, senza riavviare niente.

### Lo slideshow

Dopo due minuti senza tocchi il pannello passa alle foto che trova in
`/foto` sulla microSD (o nella radice della scheda, se quella cartella non
c'è), con sopra l'ora, la data e fino a quattro valori di Home Assistant
scelti da te. Dopo altri dieci minuti spegne lo schermo. Un tocco riaccende
tutto. I tempi si cambiano dalla pagina web o da Home Assistant.

> [!TIP]
> Il decoder JPEG del P4 non sa rimpicciolire mentre decodifica: una foto da
> 12 megapixel vorrebbe 23 MB di memoria e viene saltata. Le foto vanno
> ridotte prima, sul PC. Nel repository c'è
> [`strumenti/converti_foto.bat`](strumenti) che lo fa per tutta una
> cartella, tenendo conto anche dell'orientamento.

### La pagina web del pannello

Il pannello ha una sua pagina web, all'indirizzo `https://` più il nome che
gli hai dato, con `.local` in fondo: per esempio
**`https://pannello-ha.local`**. Se il tuo computer non risolve i nomi
`.local`, l'indirizzo IP è scritto in *Impostazioni → Rete*. Da lì:

- **Dashboard** — riordinare le card e nasconderne, senza toccare la
  dashboard vera di Home Assistant: i ritocchi valgono solo per il pannello.
- **Slideshow** — scegliere i valori da mostrare sulle foto, pescandoli
  dall'elenco delle entità di HA raggruppate per dispositivo, e dare a
  ognuno un nome corto.
- **Home Assistant** — cambiare indirizzo e dashboard.
- **Accessi** — password di amministratore e accesso ospite in sola lettura.

Il certificato è generato dal pannello stesso, quindi il browser avvisa la
prima volta: è normale e riguarda solo la tua rete di casa.

### Le impostazioni sul pannello

*Impostazioni* sullo schermo, oltre a quelle di Elecrow:

- **Rete** — DHCP o indirizzo fisso, DNS, nome, server dell'ora.
- **Debug** — livello del log, console seriale, contatori, log dettagliato
  del dialogo con Home Assistant.
- **Sicurezza** — PIN, password della pagina web, e *Impostazioni iniziali*
  (cancella tutto e riparte come appena uscito dalla scatola).

### Aggiornare il firmware da Home Assistant

Dalla seconda volta in poi il pannello si aggiorna **senza cavo**. Il `.bin` si
carica dal browser in *Impostazioni → Dispositivi e servizi → CrowPanel →
Configura → Archivio dei firmware*, e da lì si manda al pannello.

Le versioni restano dentro Home Assistant, e questo è il punto: la domanda che
viene dopo un aggiornamento non è "come aggiorno" ma "come torno indietro".
Con le versioni di prima ancora lì, tornare è una scelta.

Nella pagina del dispositivo compare un **Firmware** con "installata 0.3.2,
disponibile 0.3.3" e il pulsante *Installa*, con la barra di avanzamento
durante lo scaricamento. È lo stesso posto e lo stesso aspetto degli
aggiornamenti di Home Assistant: non c'è niente da imparare.

**Due reti di sicurezza, e vale la pena sapere che ci sono.**

Di ogni firmware si calcola un'impronta SHA-256 quando lo si carica, e la si
ricalcola quando parte verso il pannello; il pannello la ricalcola una terza
volta sui byte man mano che arrivano. Se non coincidono butta l'immagine e il
bootloader non la vede nemmeno.

Un firmware appena installato parte **in prova** e diventa definitivo solo dopo
due minuti in piedi e collegato a Home Assistant. Se si riavvia prima — crash,
avvii a vuoto, rete che non sale — il pannello rimette da solo quello di prima.
Un aggiornamento via rete che può murare un dispositivo appeso al muro non vale
la pena di esistere.

### Esplora: file, testi e foto

Un'app sul pannello e una sezione nella pagina web per vedere cosa c'è sulla
scheda SD e in memoria.

Sul **pannello** si guarda: elenco, spazio libero, i file di testo (csv, txt,
ini, cfg, json, log, yaml…) si leggono a pagine, le foto si aprono. C'è un
tasto per passare a un font a larghezza fissa, che serve quando le colonne di
un csv devono incolonnarsi.

Nella **pagina web** si fa: scaricare un file sul computer, caricarne uno,
correggere un file di testo e salvarlo. Un pannello appeso al muro non ha dove
mettere le cose né da dove prenderle — quelle operazioni hanno senso solo dove
c'è un computer dall'altra parte.

Si può anche cancellare, con la X in fondo alla riga, che chiede conferma
mostrando il nome di quello che sta per sparire. Solo file e cartelle vuote.

La pagina web mostra anche la memoria di configurazione (NVS). I segreti — il
permesso di Home Assistant, la chiave privata del certificato, le impronte di
password e PIN — si vede che esistono e quanto sono grandi, mai il valore.

### La batteria

Se al pannello si collega una batteria al litio, la percentuale che arriva in
Home Assistant **non** è quella del coprocessore sulla scheda. Quella fa una
mappa lineare da 3,5 a 4,2 V, e sbaglia due volte: la curva di una litio non è
una retta — fra 3,9 e 3,6 V ci sta l'80% della capacità — e 4,2 V è la tensione
di *fine carica*, non quella di una cella piena a riposo. Risultato: una cella
piena resta all'87% per sempre.

Il pannello se la calcola su una curva a 21 punti, e **la curva se la impara
da solo**. Manca un sensore di corrente, ma non serve: a riposo il consumo è
costante, e con consumo costante il tempo *è* la misura della carica
consumata. Una scarica completa registrata minuto per minuto dà quindi la
curva vera di quella cella su quella scheda. Finché non l'ha imparata usa una
tabella standard.

Misura da sé anche due cose che falsano la lettura: di quanto la carica gonfia
la tensione, e di quanto la abbassa lo schermo acceso (su questo pannello sono
163 mV).

Il registro finisce in `/sdcard/batteria.csv` e si scarica dalla pagina web
(`/api/batteria.csv`): l'autotaratura non è una scatola chiusa, i conti si
possono rifare a mano.

### La console seriale

A 115200 baud sulla stessa porta da cui esce il log. `help` elenca tutto. I
più utili:

| Comando | Cosa fa |
|---|---|
| `info` | stato, memoria, a quale Home Assistant è collegato, motivo dell'ultimo riavvio |
| `open` | apre l'app della dashboard |
| `shot [1-8]` | scarica una fotografia dello schermo (base64) |
| `url <ws://…>` | cambia l'indirizzo di Home Assistant senza toccare il permesso |
| `dash <nome> [vista]` | sceglie la dashboard da mostrare |
| `fw` | versione del firmware del C6 |
| `tap x y` · `drag x1 y1 x2 y2` | tocco finto, per provare l'interfaccia dal PC |
| `mostra <file>` | stampa un file sulla seriale (per portarselo sul computer) |
| `batteria` | tensione, stima, stato della taratura |
| `ota <url> <sha256>` | aggiorna il firmware da un indirizzo, senza Home Assistant |
| `factory cancella` | cancella tutto e riparte da zero |

---

## Cosa sa disegnare

**Viste**: a sezioni (quella nuova di HA), a colonne (*masonry*) e a pannello.

**Card**:

| Card | Note |
|---|---|
| `tile` | icona, nome, stato; il tocco comanda |
| `button` | |
| `entity`, `sensor` | |
| `entities` | con l'interruttore nelle righe che lo prevedono |
| `glance` | |
| `gauge` | con le soglie colorate e l'ago |
| `heading` | |
| `markdown` | i template vengono espansi da Home Assistant e restano aggiornati |
| `weather-forecast` | condizione di adesso e fino a cinque righe di previsioni, a giorni o a ore |
| `light` | cerchio della luminosità, la lampadina al centro accende e spegne |
| `thermostat` | cerchio della temperatura, più e meno, e cosa sta facendo l'impianto |
| `media-control` | titolo, artista, i tasti e il volume |
| `statistics-graph`, `history-graph` | dati veri dal registratore di HA; la finestra giornaliera è ancorata alla mezzanotte |
| `vertical-stack`, `horizontal-stack`, `grid` | |

Le icone sono quelle scelte in Home Assistant: nel firmware c'è un
sottoinsieme di Material Design Icons (244 glifi in due misure). Quando ne
serve una che non c'è, il pannello ripiega sull'icona del tipo di entità e lo
scrive nel log, così si sa cosa aggiungere — si rigenera con
[`strumenti/costruisci_icone.ps1`](strumenti).

Le card che non conosce le disegna come un riquadro con scritto il tipo,
invece di sparire in silenzio.

---

## Compilare da sorgenti

Non serve per usare il pannello: i binari pronti sono in `fw-p4/binari`.

Serve **ESP-IDF 6.1** (altre versioni non sono state provate). Il progetto
del P4 sta in [`fw-p4/sorgenti`](fw-p4/sorgenti):

```bash
cd fw-p4/sorgenti
idf.py set-target esp32p4
idf.py build
idf.py -p PORTA flash monitor
```

La prima compilazione scarica i componenti gestiti e dura parecchio. La
configurazione che conta sta in `sdkconfig.defaults`, commentata riga per
riga dove non è ovvia: `sdkconfig` è generato e non è nel repository.

Per il C6 (raramente serve): [`fw-c6/README.md`](fw-c6/README.md).

---

## Se qualcosa non va

**Lo schermo resta nero.** Controlla l'alimentatore: sotto i 2 A il pannello
riparte in continuazione. Poi guarda il log dalla seriale.

**Il touch non risponde.** Quasi sempre è la microSD non inserita — vedi
l'avvertenza in cima.

**Il pannello non si collega a Home Assistant.** Dalla seriale, `info` dice
`ha_ws=connesso` o `disconnesso` e a quale indirizzo sta provando. Se
l'indirizzo è sbagliato si cambia con `url ws://…/api/websocket` senza
rifare l'abbinamento.

**Home Assistant ha cambiato indirizzo IP.** Non devi fare niente: se
l'avevi scelto dall'elenco nella configurazione guidata, il pannello si è
segnato quale istanza è, e dopo un paio di minuti di silenzio lo ricerca
sulla rete e riparte da solo. Se invece l'indirizzo l'avevi scritto a mano e
non corrisponde a nessuna istanza che si annuncia, va corretto a mano.

**Il pannello non compare in Home Assistant.** L'integrazione non c'è o HA
non è stato riavviato dopo averla copiata. `info` dice
`integrazione=assente` oppure `installata`. Se il pannello era già acceso,
aspetta un minuto: riprova a presentarsi ogni mezzo minuto.

**I comandi da Home Assistant non arrivano.** Il pannello ascolta gli eventi
di HA, e HA lo permette solo agli amministratori: se l'abbinamento è stato
fatto con un utente normale, i comandi non passano. Si rifà l'abbinamento
dalla configurazione guidata.

**Il collegamento radio si interrompe.** Il sensore *Recuperi del
collegamento radio* in Home Assistant conta quante volte è successo. Qualche
recupero al giorno è nella norma; qualcuno ogni pochi minuti no, e allora
vale la pena aggiornare il firmware del C6 ([`fw-c6/README.md`](fw-c6/README.md)).

**Il pannello si è riavviato da solo.** Il sensore *Motivo dell'ultimo
riavvio* lo dice: `crash` vuol dire che è ripartito dopo un guaio. Il log
dalla seriale, ripreso subito dopo, dice dove.

---

## Cosa manca

Detto chiaro, così nessuno ci perde tempo sopra:

- le card **Energia** di Home Assistant non ci sono;
- `crowpanel.avviso` per ora **solo parla**: il messaggio scritto sullo
  schermo non è ancora implementato;
- i valori in sovrimpressione sullo slideshow sono al massimo quattro;
- niente video, niente telecamere;
- le viste generate automaticamente da HA (*strategy*) non sono supportate:
  vanno aperte in HA e "prese in carico".

---

## Com'è fatto dentro

Due chip. L'**ESP32-P4** fa tutto — schermo, touch, LVGL, il dialogo con Home
Assistant, la pagina web — e non ha radio. L'**ESP32-C6** fa da scheda di
rete, collegato via SDIO a 4 bit e pilotato con ESP-Hosted: lo stack TCP/IP
gira sul P4, il C6 porta solo i pacchetti.

Il dialogo con Home Assistant è lo stesso che usa il frontend di HA nel
browser: un WebSocket, `subscribe_entities` per ricevere solo le entità che
servono e solo quando cambiano, `lovelace/config` per la dashboard,
`call_service` per comandare. Le previsioni del meteo e i template della card
markdown arrivano da sottoscrizioni che restano aperte.

L'integrazione per Home Assistant gira al contrario di quasi tutte: non è HA
che va a cercare il pannello sulla rete, è il pannello che si presenta sul
WebSocket che ha già aperto. Perciò niente indirizzo IP da scrivere, niente
porte da aprire, niente avvisi di certificato — e funziona da fuori casa come
da dentro.

Le cose che hanno fatto perdere giorni — la scheda SD che alimenta il bus
I2C, il bus verso la radio che cadeva per una richiesta ARP, i buffer in
PSRAM disallineati rispetto alla cache — sono scritte in
[`docs/note-tecniche.md`](docs/note-tecniche.md), perché non le perda anche
qualcun altro.

---

## Licenza e ringraziamenti

Questo progetto è distribuito con licenza **Apache 2.0** — vedi
[`LICENSE`](LICENSE) e [`NOTICE`](NOTICE).

È costruito sul lavoro di altri:

- **Elecrow**, per la scheda e per la demo da cui parte il firmware;
- **Espressif**, per ESP-IDF, ESP-Brookesia, ESP-Hosted e il BSP della
  scheda;
- **LVGL**, la libreria grafica;
- **Material Design Icons** (Pictogrammers), le icone, licenza Apache 2.0;
- **Home Assistant**, che è il motivo per cui tutto questo esiste.

I dettagli di chi ha fatto cosa stanno in [`NOTICE`](NOTICE).

Non è un prodotto Elecrow, né Espressif, né Home Assistant, e non è
approvato da nessuno di loro.
