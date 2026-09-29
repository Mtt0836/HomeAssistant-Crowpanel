# Pannello CrowPanel

Fa comparire il pannello a muro dentro Home Assistant come un dispositivo
vero: schermo che si accende e si spegne, luminosità, diagnostica, e le
impostazioni dello slideshow modificabili da qui — anche da fuori casa.

Il verso è l'opposto di quasi tutte le integrazioni: **non è Home Assistant
ad andare a cercare il pannello sulla rete, è il pannello che si presenta.**
Tiene già aperto un WebSocket verso HA (è così che legge la dashboard) e
appena il collegamento è pronto dice chi è.

Perciò: nessun indirizzo IP da scrivere, nessuna password del pannello da
dare a HA, nessuna porta da aprire, nessun avviso di certificato. E funziona
da fuori casa come da dentro, ovunque arrivi il tuo Home Assistant.

## Dopo l'installazione

1. Riavvia Home Assistant.
2. **Impostazioni → Dispositivi e servizi → Aggiungi integrazione →
   *Pannello CrowPanel***. Non chiede niente: conferma e basta.
3. Aspetta un minuto: il pannello riprova a presentarsi ogni mezzo minuto,
   quindi comparirà da sé.

Serve il firmware del pannello aggiornato, e il pannello deve essere
abbinato a Home Assistant con un account **amministratore** (è quello che
chiede la configurazione guidata del pannello).

## Cosa aggiunge

| | |
|---|---|
| `switch` Schermo | acceso o spento |
| `number` Luminosità | da 1 a 100 |
| `sensor` diagnostici | tempo acceso, memoria libera, recuperi del collegamento radio, dashboard mostrata, motivo dell'ultimo riavvio |
| `crowpanel.mostra_dashboard` | fa saltare il pannello a una dashboard e a una vista |
| `crowpanel.avviso` | fa leggere un messaggio ad alta voce |

Le impostazioni dello slideshow (quali valori mostrare, con che nome, i
tempi) stanno in *Configura* sulla scheda dell'integrazione.

Istruzioni complete: [docs/integrazione-home-assistant.md](docs/integrazione-home-assistant.md)
