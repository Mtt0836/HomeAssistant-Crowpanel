# Integrazione per Home Assistant

Fa comparire il pannello dentro Home Assistant come un dispositivo vero:
schermo che si accende e si spegne, luminosità, diagnostica, e le
impostazioni dello slideshow modificabili da HA — anche da fuori casa.

## Come funziona, in due righe

Il verso è l'opposto di quasi tutte le integrazioni: **non è Home Assistant
ad andare a cercare il pannello sulla rete, è il pannello che si presenta.**
Il pannello tiene già aperto un WebSocket verso HA (è così che legge la
dashboard) e appena il collegamento è pronto dice chi è.

Da questo vengono tre cose che sarebbe un peccato perdere di vista:

- **niente da configurare**: nessun indirizzo IP da scrivere, nessuna
  password del pannello da dare a HA. Se il pannello cambia indirizzo non se
  ne accorge nessuno;
- **niente porte aperte e nessun avviso di certificato**: il traffico passa
  dalla connessione che il pannello ha già;
- **funziona da fuori casa** esattamente come da dentro, ovunque arrivi il
  tuo Home Assistant.

## Installazione

### Con HACS (consigliato: si aggiorna da sola)

HACS e' il negozio della comunita' di Home Assistant. Se non ce l'hai, si
installa una volta sola seguendo le istruzioni su hacs.xyz.

1. In HACS, menu in alto a destra → **Custom repositories**.
2. Incolla `https://github.com/Mtt0836/HomeAssistant-Crowpanel`, categoria
   **Integration**, e aggiungi.
3. Cerca **Pannello CrowPanel** in HACS e installalo.
4. Riavvia Home Assistant.

Da li' in poi, quando esce una versione nuova HACS te lo dice e si aggiorna
con un clic.

### A mano

1. Copia la cartella `custom_components/crowpanel` dentro la cartella di
   configurazione di Home Assistant, accanto a `configuration.yaml`. Deve
   risultare `config/custom_components/crowpanel/manifest.json`.
2. Riavvia Home Assistant.
3. **Impostazioni → Dispositivi e servizi → Aggiungi integrazione →
   *Pannello CrowPanel***. Non chiede niente: conferma e basta.
4. Aspetta un minuto. Il pannello riprova a presentarsi da solo ogni mezzo
   minuto, quindi comparirà da sé; se hai fretta, riavvialo.

Il dispositivo compare con il nome che hai dato al pannello e nella stanza
che gli hai indicato nella configurazione guidata.

## Cosa trovi

**Entità**

| Entità | A cosa serve |
|---|---|
| `switch` Schermo | accende e spegne lo schermo (spento = slideshow fermo e retroilluminazione a zero) |
| `number` Luminosità | da 1 a 100 |
| `sensor` Acceso da, RAM interna, PSRAM, Recuperi del collegamento radio | diagnostica |
| `sensor` Motivo dell'ultimo riavvio | dice "crash" quando il pannello e' ripartito da solo dopo un guaio |
| `sensor` Dashboard mostrata | quale dashboard sta facendo vedere |

Se il pannello smette di farsi vivo per più di un minuto e mezzo le entità
diventano *non disponibile*, invece di restare ferme sull'ultimo valore: uno
sguardo alla scheda e si sa che qualcosa non va, senza doverselo chiedere.

**Servizi**

- `crowpanel.mostra_dashboard` — fa saltare il pannello a una dashboard e a
  una vista. Utile nelle automazioni: suona il campanello, il pannello mostra
  la telecamera.
- `crowpanel.avviso` — fa leggere un messaggio ad alta voce al pannello.
  **Per ora solo la voce**: il messaggio scritto sullo schermo non è ancora
  implementato, quindi con `voce: false` non succede niente.
- `crowpanel.invia_slideshow` — rimanda le impostazioni dello slideshow.
  Normalmente non serve.

**Impostazioni dello slideshow**

Sulla scheda dell'integrazione, *Configura*. Da lì si scelgono i valori da
mostrare in sovrimpressione durante lo slideshow (con il selettore di entità
di HA, quindi con la ricerca), il nome corto da scrivere sopra a ognuno, i
tempi e la cartella delle foto.

Finché la casella **"Gestisci lo slideshow da qui"** è spuntata, comanda
Home Assistant e la pagina web del pannello mostra quella parte in sola
lettura: due padroni per la stessa configurazione vorrebbe dire che l'ultimo
che salva cancella l'altro senza dirlo a nessuno. Togliendo la spunta il
pannello torna a gestirsela da sé.

Il pannello tiene comunque una copia in flash dell'ultima configurazione
ricevuta: se Home Assistant è spento o irraggiungibile, lo slideshow parte lo
stesso.

## Se qualcosa non va

**Il pannello non compare.** Guarda il log del pannello (seriale, oppure
Impostazioni → Debug): se dice *"integrazione non installata in Home
Assistant"*, HA non ha ancora l'integrazione o non è stato riavviato dopo
averla copiata. Il comando `info` sulla seriale dice `integrazione=assente`
o `installata`.

**Serve un account amministratore.** Il pannello si mette in ascolto degli
eventi di HA, e HA lascia farlo solo agli amministratori. L'abbinamento del
pannello è già fatto con un account amministratore (è quello che chiede la
configurazione guidata), quindi normalmente non ci si pensa; se hai abbinato
il pannello con un utente normale, i comandi da HA non arriveranno.

**Provare un comando a mano.** I comandi viaggiano come eventi normali sul
bus, quindi si vedono e si possono lanciare da *Strumenti per sviluppatori →
Eventi*. Tipo evento `crowpanel_command`, dati:

```yaml
id: a0f262854468        # l'identificativo del tuo pannello
comando: schermo
acceso: false
```
