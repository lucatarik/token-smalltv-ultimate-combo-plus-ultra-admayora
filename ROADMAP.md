# ROADMAP

Obiettivo: l'orologio diventa un set di schermate da scorrere con tre bottoni. Il PC (daemon Rust e client Python) fornisce i dati pesanti; l'ESP8266 disegna. Vincolo che decide tutto: heap libero ~9–10 KB, niente buffer di schermo, niente TLS sul dispositivo.

Stato: `[x]` fatto, compila e (daemon, client Python) testato su host; mai provato sull'orologio. `[x]` = fatto e compila, `[~]` in corso, `[ ]` da fare. Nessuna fase è stata flashata: si flasha **alla fine**, su richiesta dell'utente (fermare prima `smalltv-monitord`).

## Decisioni dell'utente

- Bottoni: sinistra/destra sono direzionali; centro breve = OK (conferma), centro ≥ 1 s = BACK (annulla).
- La prima schermata resta quella monitor esistente (`UsageMode`); sinistra/destra scorrono le altre.
- Snake: OK inizia la partita, BACK la resetta; durante la partita sinistra/destra curvano.
- Meteo: Cecchina (Albano Laziale, circa 41.75 N, 12.64 E).
- Via ticker, radar e Home Assistant (`-D WITH_TICKER=0 -D WITH_RADAR=0 -D WITH_HA=0` in `env:smalltv`): RAM da 76,9% a 60,7%, flash da 78,6% a 73,5%. Ottimizzare il codice per restare nello spazio.
- Video: nessun test di throughput; il frame rate non importa. Il client manda un video o una gif in loop.
- Interfaccia web: una sola pagina, minuscola, con caricamento temi; librerie da CDN pubblici, temi compressi al massimo.
- Non flashare prima della fine.

## Fasi

1. `[x]` **Bottoni** su `A0` (scala di resistenze): `ButtonLadder.h`, test host `tests/test_buttons.cpp`, `POST/GET /api/button`, `HAS_LDR` a 0, `DisplayMode::onOk/onBack`.
2. `[x]` **Base schermate**: `Draw.h` (helper senza sfarfallio estratti da `UsageMode.cpp`, fatto), `ScreenMode` (classe base), costanti `MODE_*`, registro in `main.cpp`, `DisplayMode::onNav` (la schermata può prendersi sinistra/destra), carosello senza rotazione automatica.
3. `[x]` **CPU/RAM**: storico nel firmware (anelli `u8`, tre finestre 2 min / 20 min / 2 h, 120 campioni), grafico disegnato a colonne senza sfarfallio, OK cambia finestra.
4. `[x]` **Rete**: grafico download/upload (anelli `u16` in KB/s, scala automatica) + ultime connessioni TCP non HTTP. Il daemon legge `/proc/net/tcp` e `/proc/net/tcp6`, scarta le porte 80/443/8080/8443 e il loopback, tiene le ultime 5 nuove; le manda a `POST /api/net` ogni 3 s.
5. `[x]` **Meteo**: il daemon scarica Open-Meteo (nessuna chiave) ogni 10 minuti e manda `POST /api/wx` ogni minuto; il firmware disegna temperatura, icona (primitive grafiche), umidità, vento e i 3 giorni seguenti.
6. `[x]` **Orologio a temi**: `MODE_THEME` (già in smalltv-mod) dentro il carosello; verificare che coesista con le altre schermate.
7. `[x]` **Snake**: griglia 15×14, celle da 16 px, solo le celle cambiate si ridisegnano.
8. `[x]` **Video**: schermata `VideoMode` che apre una porta TCP (8888) quando armata da `POST /api/video`; protocollo a record `0xA5, y, n` + `n` righe RGB565 (480 byte), solo righe cambiate. Client Python in `smalltv-video/` (`yt-dlp` + `ffmpeg`, ritaglio quadrato, `-stream_loop -1`, scarta i frame se il dispositivo è lento).
9. `[x]` **Pagina web di configurazione**: una pagina HTML sola, minuscola, servita dal dispositivo (gzip), con librerie da CDN pubblici; carica i temi di smalltv-mod; temi compressi al massimo.
10. `[x]` **Documentazione e flash finale** (flashato il 2026-11-06; sei schermate scorse con `POST /api/button`, video da gif per 12 s senza crash; heap 18–19 KB): aggiornare `CLAUDE.md`, `handoff.md`, annotare RAM/flash di ogni fase, poi flashare su richiesta.

## Rischi aperti

- Il circuito dei bottoni non è saldato: le soglie ADC (910/590/300) sono calcolate, non misurate.
- Heap: ogni schermata nuova pesa; controllare `heap` e `maxblk` in `/api/status` dopo il flash.
- Il video su TCP con lwIP a bassa memoria sarà lento; non misurato per scelta.
- I temi: serve verificare quanta RAM tiene un tema attivo accanto alle altre schermate.
- La pagina web nuova sostituisce quella di smalltv-mod: i file fuori da `src/features/usage/` cambiano, quindi gli aggiornamenti a monte si riprendono con più fatica.

## Esito (RAM/flash di `env:smalltv`)

| Passo | RAM | Flash |
| --- | --- | --- |
| Partenza, con ticker/radar/HA | 76,9% | 78,4% |
| Bottoni | 76,9% | 78,6% |
| Senza ticker, radar, HA | 60,7% | 73,5% |
| Schermate + pagina web minuscola | 65,5% | 72,9% |

- Pagina web: `src/webui.html` (3 KB, 1,5 KB gzip, Pico CSS da jsDelivr); la vecchia, completa, è `src/webui.stock.html` (non compilata). I test JS/theme che leggono `webui.html` non valgono più per la nuova.
- Temi: il pacchetto `.stheme` lo produce `tools/theme_pack.py`, che sceglie già la codifica più piccola (STI1, STI2 diretto, STI2 indicizzato a 16 colori); la pagina carica i `.stheme` così come sono.
- Il bottone non saldato lascia `A0` flottante: la decodifica parte solo dopo 1,5 s di lettura a riposo.

- Non verificato a occhio: nessuno ha visto lo schermo. Controllare impaginazione di CPU/RAM, rete, meteo, snake e video, e provare i bottoni veri quando saldati.
- Backup dei sorgenti prima del flash: `backup-smalltv-mod-src-20261106.tar.gz`. Il `.bin` precedente non esiste.

## Seconda tornata (2026-11-06)

- Fatto e flashato: rotazione automatica configurabile (`autoRotate`, `carouselSec`), snake autoplay (`snakeAuto`), schermate attivabili (`screenMask`), cattura desktop in `smalltv_video.py --desktop`, sfondo di snake ripulito all'avvio (bug: `full_` azzerato dopo `tick`). RAM 66,1%, flash 73,1%.

## Terza tornata (2026-11-06)

- Rete: FIFO delle ultime connessioni (campionamento 4 volte al secondo, web tenue opzionale). Snake: niente muri, bonus, OK riprende la partita automatica.
- Motore dei temi: layer `walker` (immagini remote tramite il proxy del daemon, linee rette bordo-bordo, sprite girato e che ondeggia, fino a 10 insieme, partenze scaglionate). Test host in `tests/theme/test_walker.cpp`; la suite `tests/theme/run.sh` passa (con `.pio/libdeps/smalltv` al posto di `smalltv_esp32_8mb`).
- `themes/pokemon-walk.stheme` flashato e installato; sprite scaricati dal PC (proxy 8899).
- Non verificato a occhio: fluidità reale del movimento sull'ESP8266 (nessuna misura dei frame al secondo), impaginazione, bonus e FIFO di rete. Heap libero circa 8 KB con il tema attivo: poco margine.
