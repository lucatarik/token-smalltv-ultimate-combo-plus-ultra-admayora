# Handoff: bottoni hardware per ciclare le schermate

Stato: firmware e decodifica dei bottoni **implementati e compilati, non flashati**; il circuito non è saldato (vedi `CLAUDE.md`, sezione Bottoni). Le istruzioni sotto sul firmware sono in parte superate: la decodifica è in `ButtonLadder.h`, l'ADC grezzo è su `GET /api/button`. C'è solo la decisione sul circuito e i punti del codice da toccare. Nessun bottone è ancora saldato (non risulta che l'utente abbia aperto la scocca). Leggi prima `CLAUDE.md` per il contesto del progetto, i comandi e le regole.

## Obiettivo

Poter cambiare schermata sull'orologio con tre bottoni fisici. Oggi la schermata monitor (layout in `drawMonitor`, `smalltv-mod/src/features/usage/UsageMode.cpp`) è una sola. Per questo i bottoni hanno senso solo dopo aver aggiunto altre pagine.

## Da chiedere all'utente prima di scrivere codice

1. **Quali pagine ciclare.** Idee proposte, non confermate: solo limiti Claude con numeri grandi; dettaglio PC (CPU, RAM, disco, rete in grande); solo orologio.
2. **Che cosa fa ciascuno dei tre bottoni.** Ipotesi: avanti, indietro, e un terzo per tornare alla prima pagina o spegnere/accendere lo schermo.
3. Se vuole le soglie del circuito tarate subito (serve il circuito montato).

## Decisione sul circuito: tre bottoni su `A0` con scala di resistenze

Il modello è ESP-12F nudo senza LDR montato (l'utente lo ha confermato), quindi `A0` (pin `ADC`, pin 2 del modulo, accanto a `RST` ed `EN`) è libero. Un filo solo, tre bottoni.

```
3V3 ──┬─────────┬──────────┐
      │         │          │
   [BTN1]    [BTN2]     [BTN3]
      │         │          │
    27kΩ      47kΩ      100kΩ
      └─────────┴──────────┘
                │
                ├──────────── A0 (piedino ADC)
                │
              10kΩ (pull-down)
                │
GND ────────────┘
```

Tensioni attese a bottone premuto (calcolate con `3,3 V × 10k / (10k + R)`, **non misurate**): BTN1 ≈ 0,89 V, BTN2 ≈ 0,58 V, BTN3 ≈ 0,30 V; a riposo 0 V. Con ADC a 10 bit e fondo scala ≈ 1 V le letture attese sono circa 910, 590 e 300 su 1023. Due bottoni insieme danno un valore intermedio: va ignorato. Un condensatore da 100 nF tra `A0` e `GND` è facoltativo contro il rumore.

**Vincolo di sicurezza:** l'ingresso ADC del modulo nudo accetta circa 1 V al massimo (non c'è il partitore della NodeMCU). Il circuito è fatto apposta con pull-down a massa, così a riposo l'ingresso è a 0 V. Non collegare mai `3V3` direttamente ad `A0` e non invertire il circuito con una pull-up.

## Alternative scartate o di riserva

Ordine dei pin dell'ESP-12F (tabella di teachmemicro.com/esp12f-pinout-diagram, non verificata su un disegno): `RST`, `ADC (A0)`, `EN`, `GPIO16`, `GPIO14`, `GPIO12`, `GPIO13`, `VCC`, `GND`, `GPIO15`, `GPIO2`, `GPIO0`, `GPIO4`, `GPIO5`, `RXD`, `TXD`. Il sito non dice su quale lato fisico stia ciascun gruppo.

Pin del display (da `board_esp8266.h`, non usabili): `GPIO14` SCLK, `GPIO13` MOSI, `GPIO0` DC, `GPIO2` RST, `GPIO15` CS, `GPIO5` retroilluminazione.

- **`GPIO0`:** escluso. È il pin `DC` del display; un bottone lì corrompe i dati mostrati e premuto all'accensione manda in modalità flash.
- **`GPIO3` / `RXD0`:** la piazzola è esposta (elenco nel forum Home Assistant, post sul modello ESP-12F: `TXD0`, `RXD0`, `3V3`, `GPIO0`, `RST`, `GND`, con `GND` quadrata). Non si sa dove sia sulla scheda. L'utente ha chiesto di non usarla. Il firmware usa la seriale di debug (`Serial.begin(115200)` in `main.cpp`).
- **`GPIO12`, `GPIO4`, `GPIO16`:** liberi sul chip. Richiedono una saldatura diretta sul piedino del modulo, tra pin del display (vicini a 2 mm: rischio cortocircuito). `GPIO12` e `GPIO4` con pull-up interna (bottone verso `GND`); `GPIO16` senza pull-up interna (bottone verso `3V3`, pull-down).

## Cosa non è noto (da verificare con l'utente)

- Se `A0`, `3V3` e `GND` sono raggiungibili: la scocca non è stata aperta, e non ci sono foto della scheda. Chiedere una foto della scheda aperta per indicare i piedini con certezza.
- Le soglie reali dell'ADC: i valori sopra sono calcolati. Il firmware deve poterle tarare (vedi sotto).

## Cosa fare nel firmware (`smalltv-mod/`)

Già presente per le schede ESP32 e da riusare, non da riscrivere:

- `buttonService()` in `src/main.cpp` (circa riga 226): legge un tasto touch con mediana di tre campioni, isteresi e debounce ad integratore (`BTN_CONFIRM`), distingue pressione breve e lunga.
- Azioni `BTN_ACT_*` in `src/config.h` (righe 153–160): `NONE`, `SCREEN_OFF`, `HOME`, `NEXT`, `NETINFO`, `WAKE`, `ACK`, con `DEFAULT_BTN_SHORT = BTN_ACT_NEXT` e `DEFAULT_BTN_LONG = BTN_ACT_NETINFO`.
- `ButtonSettings` in `src/Settings.h` (campi `sensitivity`, `shortAction`, `longAction`) e la scheda "Button" della web UI, che oggi compare solo su schede con bottone.

Da fare:

1. **Lettura ADC:** una funzione che legge `A0` ogni ~20 ms (`analogRead(A0)`), con media su più campioni, e la converte in un'identità di bottone (nessuno, 1, 2, 3) con isteresi e debounce. Soglie in costanti di `board_esp8266.h` (es. `BUTTON_ADC_THRESHOLDS`), con valori di partenza 910/590/300 e una banda di tolleranza (per esempio ±60) attorno a ciascuno.
2. **Conflitto con `HAS_LDR`:** `board_esp8266.h` oggi ha `#define HAS_LDR 1` e `LDR_PIN A0`; `appEffectiveBrightness()` in `main.cpp` (riga ~136) legge `A0` se `autoBrightness` è attivo. Con i bottoni su `A0` va messo `HAS_LDR 0` (o la luminosità automatica va disattivata e nascosta nella web UI), altrimenti la luminosità automatica leggerebbe i bottoni come luce.
3. **Pagine:** dividere `drawMonitor` in più pagine selezionabili (indice di pagina in `UsageMode`, con `wake()`/`invalidate()` che forzano il ridisegno completo). Il cambio di pagina deve mettere `force = true` perché il pannello della pagina nuova sia ridisegnato (i `TextState`/`BarState` in `drawMonitor` si azzerano con `memset` quando `force` è vero).
4. **Azioni:** collegare i tre bottoni (identità da 1 a 3) alle azioni scelte dall'utente. Riusare il dispatcher delle azioni di `buttonService()`; aggiungere eventualmente azioni nuove (pagina precedente) dopo `BTN_ACT_ACK`, senza rompere `BTN_ACT_MAX`.
5. **Taratura:** esporre il valore grezzo di `A0` in `GET /api/status` (campo `adc`) per leggere le soglie vere a circuito montato, e annotare i valori misurati in questo file.
6. **Memoria:** il firmware ha RAM al 76,9% e circa 9–10 KB di heap libero. Niente buffer grandi. Tieni i nuovi stati piccoli (come `MonitorState` in `UsageMode.cpp`).
7. **Test:** `smalltv-mod/tests/` ha test su host in C++ (`test_storage.cpp`, `test_webguard.cpp`). Aggiungere un test per la conversione ADC → bottone (soglie, isteresi, due bottoni insieme ignorati) sulla funzione pura, senza hardware.

## Accettazione

- Premere ciascun bottone cambia pagina come deciso; premere due bottoni insieme non fa nulla.
- Nessun sfarfallio al cambio di pagina; nessun crash, RAM libera simile a prima (guardare `heap` e `maxblk` in `/api/status`).
- La luminosità non cambia premendo i bottoni.
- I valori `adc` misurati con il circuito vero sono annotati qui sotto e le soglie sono tarate.

## Come caricare il firmware e provarlo

Vedi i comandi in `CLAUDE.md` (fermare `smalltv-monitord` prima dell'upload; il caricamento via Wi-Fi sostituisce il firmware in esecuzione). Dopo l'upload riavviare il daemon e controllare che `synced` sia `true` e che la memoria libera non sia calata.

## Rischi

- Cortocircuito saldando `A0` accanto a `RST`/`EN`: `RST` riavvia l'orologio, `EN` lo spegne.
- Tensione sopra circa 1 V su `A0`: può danneggiare il chip. Non cambiare lo schema senza ricalcolare.
- Il flash sostituisce il firmware senza backup completo (nessuna seriale disponibile).
- Le soglie sono calcolate, non misurate: il rischio principale è un bottone che non viene riconosciuto o viene scambiato per un altro. Misurare prima di fidarsi.

## Valori misurati (da compilare)

| Bottone | Resistenza serie | Letto su `A0` (0–1023) |
| --- | --- | --- |
| Nessuno | — | |
| 1 | 27 kΩ | |
| 2 | 47 kΩ | |
| 3 | 100 kΩ | |
