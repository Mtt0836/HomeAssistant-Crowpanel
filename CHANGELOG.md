# Cosa è cambiato

Elenca i rilasci pubblicati qui, dal più recente. I numeri che mancano
nella serie non sono mai usciti da questo repository.

Il numero è quello del **firmware** (`HA_Display`, lo mostra il comando
`info` e il sensore *Firmware* in Home Assistant). Da 0.3.5 l'integrazione
per Home Assistant porta lo stesso numero: prima erano due serie separate e
si erano allontanate, con le correzioni di 0.3.3 uscite sotto il numero
0.3.0 del manifest.

---

## 0.3.5 — 10 ottobre 2026

Un ciclo completo di batteria, scarica e ricarica, ha prodotto la prima
taratura misurata sulla cella vera: 15 ore e 21 minuti di scarica, 919
punti. E ha fatto vedere tre cose che non andavano.

**Il registro della batteria scriveva due colonne in meno.** La riga del CSV
aveva dodici segnaposti per quattordici valori: i secondi della corsa e la
carica consumata — le due colonne con cui si ricontrolla la curva sul
computer — non uscivano, e al loro posto comparivano altri due numeri. La
taratura non era intaccata, perché lavora in memoria, ma il registro esiste
proprio per poter rifare i conti a mano. Ora le colonne ci sono, e
l'intestazione le dichiara.

Il registro si accorge anche di avere un'intestazione scritta da un firmware
precedente: la mette da parte in `batteria.1.csv` e ricomincia il file, così
le colonne corrispondono sempre a quello che c'è scritto in cima.

**La caduta di tensione dello schermo acceso era sottostimata**, 22 mV
contro i circa 40 che il registro stesso misura. Si leggeva troppo presto,
trenta secondi dopo lo spegnimento, quando la cella non ha finito di
rilassarsi. Ora si misura dopo due minuti, e solo se lo schermo è stato
acceso per almeno un minuto. Conta perché la stima della carica si confronta
sempre con una tensione a riposo, e lo schermo acceso la abbassa.

**L'aggiornamento via rete poteva fallire con un indirizzo lungo.** Lo spazio
per l'indirizzo era la metà di quello che serve a tenere insieme l'indirizzo
di Home Assistant e il percorso che manda: veniva tagliato a metà, e
l'aggiornamento si fermava dando la colpa alla rete.

Sotto il cofano: `-Wformat` era disattivato su tutto il progetto, eredità del
progetto di esempio da cui questo è partito. È il controllo che avrebbe
trovato subito il difetto del registro, quindi è stato riacceso sui sorgenti
del pannello. Ha trovato altri dieci punti, nessuno dei quali è un difetto
vero: sono elencati in [`docs/formato-troncamento.md`](docs/formato-troncamento.md).

Quello che non è ancora stato provato, detto chiaro: le due colonne nuove
girano da due giorni ma **nessuna scarica di taratura le ha ancora
attraversate per intero**. La prossima lo farà.

## 0.3.3 — 6 ottobre 2026

Due cose che il controllo automatico di Home Assistant (hassfest) rifiutava.

Il componente `file_upload`, che serve a caricare un firmware dal browser,
andava dichiarato fra le dipendenze dell'integrazione.

Lo stato del sensore dell'alimentazione mandava le parole italiane `in
carica` e `a batteria`. Gli stati di un sensore a scelta fissa devono essere
chiavi macchina, perché il nome visibile lo danno le traduzioni: chi ha Home
Assistant in inglese si ritrovava l'italiano. Ora sono `in_carica` e
`a_batteria`, nel firmware e nell'integrazione.

## 0.3.2 — 6 ottobre 2026

**Aggiornamento del firmware via rete.** Dalla seconda volta in poi il
pannello si aggiorna senza cavo. Il `.bin` si carica dal browser dentro Home
Assistant, resta in archivio e da lì si manda al pannello; nella pagina del
dispositivo compare un *Firmware* con versione installata e disponibile, il
pulsante *Installa* e la barra di avanzamento, come per gli aggiornamenti di
Home Assistant.

Due reti di sicurezza. Di ogni immagine si calcola un'impronta SHA-256 quando
si carica e si ricalcola quando parte; il pannello la ricalcola una terza
volta sui byte che arrivano, e se non coincide butta tutto. E un firmware
appena installato parte **in prova**: diventa definitivo solo dopo due minuti
in piedi e collegato a Home Assistant, altrimenti il pannello rimette da solo
quello di prima. Un aggiornamento via rete che può murare un dispositivo
appeso al muro non vale la pena di esistere.

Le versioni precedenti restano in archivio e si possono rimettere o
cancellare: la domanda che viene dopo un aggiornamento non è "come aggiorno"
ma "come torno indietro".

**La batteria si tara da sola.** Se al pannello si collega una batteria al
litio, la percentuale non è più quella del coprocessore sulla scheda — che fa
una mappa lineare da 3,5 a 4,2 V e sbaglia due volte, perché la curva di una
litio non è una retta e 4,2 V è la tensione di fine carica, non quella di una
cella piena a riposo. Il pannello usa una curva a 21 punti e **la impara da
sé** su una scarica completa: manca un sensore di corrente ma non serve,
perché a riposo il consumo è costante e con consumo costante il tempo è la
misura della carica consumata. Misura anche di quanto la carica gonfia la
tensione e di quanto la abbassa lo schermo acceso. Tutto finisce in un
registro su microSD che si scarica dalla pagina web: l'autotaratura non è una
scatola chiusa.

**Esplora: file, testi e foto.** Un'app sul pannello e una sezione nella
pagina web per vedere cosa c'è sulla microSD. Sul pannello si guarda: elenco,
spazio libero, i file di testo a pagine, le foto si aprono, e c'è un tasto
per passare a un font a larghezza fissa quando le colonne di un CSV devono
incolonnarsi. Dalla pagina web si scarica un file sul computer, se ne carica
uno, si corregge un testo e si salva. Si cancella con conferma, solo file e
cartelle vuote. La pagina web mostra anche la memoria di configurazione: dei
segreti si vede che esistono e quanto sono grandi, mai il valore.

**Home Assistant dietro HTTPS.** Il collegamento `wss://` funziona: al
WebSocket mancava l'elenco dei certificati.

## 0.2.0 — 1 ottobre 2026

Il pannello passa a **ESP-IDF 6.1**.

La card **Energia**, e il conto delle card riconosciute arriva a sedici.

L'integrazione sa **salvare la configurazione** del pannello e mandargli le
**foto dello slideshow**.

## 0.1.0 — 29 settembre 2026

Prima pubblicazione.

Il pannello legge la dashboard che hai già fatto in Home Assistant e la
ridisegna con i widget di LVGL, parlando il protocollo WebSocket: non c'è una
pagina web dentro. Si comanda al tocco — luci, termostato, lettore
multimediale — e quando nessuno lo usa diventa una cornice digitale con le
foto della microSD, l'ora e i valori che scegli tu.

Configurazione guidata al primo avvio, e una pagina web sul pannello in
HTTPS, con password o con l'account di Home Assistant. Editor a griglia per
spostare e ridimensionare le card. PIN per bloccare lo schermo.

L'**integrazione per Home Assistant** fa comparire il pannello come un
dispositivo vero — schermo, luminosità, diagnostica, impostazioni dello
slideshow — e si installa da HACS. Il pannello si fa trovare da solo, senza
scrivere indirizzi.
