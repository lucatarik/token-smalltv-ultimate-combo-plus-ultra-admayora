# CLAUDE.md

Progetto: una schermata di monitoraggio su un orologio GeekMagic SmallTV-Ultra (ESP-12F / ESP8266, schermo ST7789 240×240) che mostra i limiti di Claude (5H e 7D con orari di reset), lo stato del servizio Claude, l'ora e il carico del PC (CPU, RAM, disco, rete). Il PC manda i dati all'orologio; l'orologio disegna.

Lingua con l'utente: italiano. Il codice, i nomi e i commenti nel codice restano in inglese, come nel resto dei repository.

## Cartelle

| Cartella | Che cos'è | Linguaggio |
| --- | --- | --- |
| `smalltv-mod/` | Fork locale di giovi321/smalltv-mod (firmware che sostituisce quello GeekMagic). Le modifiche sono tutte in `src/features/usage/` (`UsageData.h`, `UsageClient.cpp`, `UsageMode.h`, `UsageMode.cpp`). | C++ (Arduino, PlatformIO) |
| `smalltv-daemon/` | `smalltv-monitord`: daemon che ogni secondo fa `POST /api/usage` all'orologio. Legge CPU/RAM/rete da `/proc`, il disco con `statvfs`, i limiti Claude dall'API OAuth, lo stato da status.claude.com. | Rust |
| `smalltv-video/` | `smalltv_video.py`: client Python (test: `cd smalltv-video && python3 -B -m unittest`; serve `ffmpeg`, e `yt-dlp` per YouTube): prende un URL YouTube, una gif o un file video e lo manda in loop all'orologio. | Python |
| `ROADMAP.md` | Decisioni, fasi e stato. | |
| `handoff.md` | Circuito dei bottoni (scala di resistenze su `A0`, non ancora saldato) e punti del codice. | |

## Stato dell'orologio

- Modello: SmallTV-Ultra, chip ESP-12F (ESP8266, 4 MB). La USB-C è solo alimentazione: niente seriale, si flasha via Wi-Fi.
- Indirizzo: `192.168.0.29` (rete `24g`). Firmware: `smalltv-mod 3.3.0` con le mie schermate, variante `esp8266`, flashato il 2026-11-06. Modalità `carousel`. L'aggiornamento firmware si fa solo con `curl` (la nuova pagina web non ha il modulo di upload).
- Fuso: `Europe/Rome`, `tzPosix` `CET-1CEST,M3.5.0,M10.5.0/3`, nel gruppo `clock` della configurazione.
- Non esiste un backup del firmware originale GeekMagic (servirebbe la seriale). Per tornare indietro si può riscrivere un `.bin` ufficiale (repo GeekMagicClock/smalltv-ultra), ma non è stato provato.
- Heap libero a regime (dopo il flash del 2026-11-06, senza ticker/radar/HA): circa 18–19 KB, blocco più grande circa 17–18 KB; prima era 9–10 KB. Resta il vincolo che decide quasi tutto (niente buffer di schermo). `contstk` (stack) scende fino a ~130 B durante snake/video: non aggiungere buffer grossi sullo stack.

## Comandi

Compilare il firmware (PlatformIO è in `~/.local/bin/pio`, non nel PATH):

```
cd smalltv-mod && ~/.local/bin/pio run -e smalltv
```

Caricare sull'orologio (aggiornamento via Wi-Fi). **Prima ferma il daemon**, altrimenti i POST durante l'upload mandano in crash l'orologio (`Exception epc 0x4000df64`):

```
systemctl --user stop smalltv-monitord.service
curl -s -m 90 -F "firmware=@.pio/build/smalltv/firmware.bin" http://192.168.0.29/update   # risposta: OK
systemctl --user start smalltv-monitord.service
```

Il daemon Rust (installato in `~/.local/bin/smalltv-monitord`, unità `systemd` utente `smalltv-monitord.service`):

```
cd smalltv-daemon && cargo test && cargo install --path . --root ~/.local
smalltv-monitord install --device-url http://192.168.0.29   # acceso, e a ogni login
smalltv-monitord uninstall                                   # spento e rimosso
journalctl --user -u smalltv-monitord.service -n 20 --no-pager
```


Configurazione dell'orologio: `GET/POST http://192.168.0.29/api/config`. I campi del fuso stanno annidati: `{"clock":{"tz":"...","tzPosix":"..."}}`; `{"mode":"usage"}` è a livello radice. Stato: `GET /api/status` (`uptime`, `heap`, `maxblk`, `reset`, `synced`).

## Come funziona la schermata

- Il firmware accetta `POST /api/usage` con `{s, sr, w, wr, st, ok}` (limiti) e, nelle mie modifiche, `{cpu, ram, dk, dn, up, cs}` (PC e stato Claude). Con almeno un campo del PC presente si disegna il layout monitor (`drawMonitor` in `UsageMode.cpp`); senza, la schermata Usage originale. Un campo mancante resta sconosciuto e si vede come trattino, mai 0.
- Disegno senza sfarfallio e senza buffer di schermo: ogni elemento ricorda cosa mostra e si ridisegna solo se cambia; il testo è composto pixel per pixel con lo sfondo e inviato una riga alla volta; le barre disegnano solo la parte che cambia. Il pannello fisso si disegna una volta sola (`force`).
- L'ora e gli orari di reset richiedono SNTP. Il modulo `Clock` lo attiva solo per modalità notte, WireGuard e temi; `UsageMode::service` lo attiva da solo alla prima lettura con dati del PC, una volta per fuso.

## Bottoni (tre, su `A0`)

- Sinistra / destra: direzionali, cambiano schermata (modalità del carosello: `carouselPrev` / `carouselNext` in `main.cpp`). Richiedono `mode = carousel`.
- Centro, pressione breve: OK, conferma l'azione della schermata (`DisplayMode::onOk`). Centro tenuto 1 s: annulla/indietro (`DisplayMode::onBack`; se la schermata non ha nulla da annullare, torna alla prima).
- Decodifica in `smalltv-mod/src/features/usage/ButtonLadder.h` (pura, testata su host: `tests/test_buttons.cpp`). `HAS_LDR` è 0: `A0` legge i bottoni.
- Senza circuito saldato: `POST /api/button {"b":"left|right|ok|back"}` simula la pressione; `GET /api/button` dà il valore grezzo `adc` per tarare le soglie. (`adc` non sta in `/api/status`: i suoi byte sono fissati dai test.)

## Schermate, dati e pagina web

- Registro in `main.cpp`: monitor (`UsageMode`), CPU/RAM, rete, meteo, orologio a temi, snake, video. Le nuove stanno in `smalltv-mod/src/features/usage/Screens.cpp` (classe base `Screen`, helper di disegno in `Draw.h`). `mode` deve essere `carousel`; con i bottoni nessuna rotazione automatica, ogni schermata è raggiungibile.
- OK per schermata: CPU/RAM e rete cambiano finestra (2 min / 20 min / 2 h); snake inizia (e in partita mette in pausa); BACK in snake resetta la partita, nelle altre torna alla prima schermata. Snake: sinistra/destra curvano durante la partita.
- Il daemon manda: `POST /api/usage` ogni secondo (anche lo storico CPU/RAM/rete si costruisce da qui), `POST /api/net` ogni 3 s (ultime 5 connessioni non web, da `/proc/net/{tcp,tcp6,udp,udp6}`: `>` TCP aperta da noi, `<` TCP verso una porta in ascolto, `u` UDP con peer noto; contano anche le connessioni appena chiuse; scartati loopback, porte 80/443/8080/8443 e DNS/QUIC), `POST /api/wx` ogni minuto (il meteo lo scarica ogni 10 minuti da Open-Meteo). Luogo: `SMALLTV_LAT`, `SMALLTV_LON`, `SMALLTV_PLACE` (default Cecchina 41.75, 12.64).
- La schermata video compare nel carosello solo mentre c'è uno stream (`carouselHas`).
- Video: `python3 smalltv-video/smalltv_video.py <url|file|gif> [--device IP] [--fps 8]`; YouTube va in passthrough (`yt-dlp -g` dà l'indirizzo, `ffmpeg` lo legge, niente download) e riparte da capo quando finisce, file e gif girano in loop; video e gif vengono ritagliati al centro in un quadrato (`--crop-x/--crop-y` per spostare il ritaglio). `--desktop` cattura un riquadro quadrato dello schermo (X11, `ffmpeg x11grab`): cornice rossa da trascinare, rotella o cursore per la dimensione, Start/Stop; `--box X,Y,SIZE` per il riquadro iniziale. Arma `POST /api/video {"on":true}`, poi TCP sulla porta 8888: record `0xA5, y, n` + `n` righe da 240 pixel RGB565 little-endian. A fine stream, o con BACK, l'orologio torna alla prima schermata.
- Opzioni nella pagina web (campi di `/api/config`): `autoRotate` (rotazione automatica) con `carouselSec` (secondi per schermata, 5–3600), `snakeAuto` (snake parte da solo dopo 3 s senza OK, gioca con una ricerca del percorso più breve verso il cibo e ricomincia quando muore), `screenMask` (bit `MODE_*`: monitor 1, orologio 6, CPU/RAM 7, rete 8, meteo 9, snake 10, video 11). Una partita manuale o un video in corso (`DisplayMode::busy`) non vengono ruotati via.
- Meteo: se nessuno lo spinge da 15 minuti (PC spento), l'orologio lo scarica da solo da Open-Meteo in HTTP semplice (`wxFetch` in `Screens.cpp`, solo quando la schermata è visibile). Luogo nelle impostazioni `wxPlace`, `wxLat`, `wxLon` (default Cecchina), modificabile nella pagina web.
- Temi: `themes/` raccoglie i `.stheme` (esempi di smalltv-mod e `pokemon.stheme`, 44 KB, sorgenti in `themes/pokemon/`: sprite PokeAPI convertiti sul PC e incorporati nel pacchetto; il motore dei temi non carica immagini da internet). Build: `smalltv-mod/tools/smalltv_theme.py build themes/pokemon themes/pokemon.stheme` (serve Pillow; `SMALLTV_ARDUINOJSON_INCLUDE` e `SMALLTV_GFX_INCLUDE` puntano a `.pio/libdeps/smalltv/...`).
- Rete: il daemon campiona i socket 4 volte al secondo e manda a `/api/net` le ultime 8 connessioni (le più nuove in cima, FIFO); la schermata ne mostra 5. Prefissi: `>` aperta da noi, `<` in ingresso, `u` UDP, `w` web (80/443/8080/8443, tenue; si nasconde con `netWeb` nella pagina web). Loopback e DNS esclusi. Su questo PC l'unico traffico non web è spesso la connessione dell'orologio stesso al proxy degli sprite; vedere altro (UDP non connesso, DNS) richiederebbe una cattura di pacchetti (CAP_NET_RAW), non fatta.
- Snake: sul bordo il serpente riappare dal lato opposto (niente muri, perde solo mordendosi); ogni tanto compare un bonus giallo (7 s, lampeggia alla fine) che vale 5 punti e non allunga il verme; OK durante una partita automatica ne prende il controllo senza ricominciare.
- Walker (motore dei temi, `LayerType::Walker`, spec 2): `{"type":"walker","width","height","frames","fps","count":1..10,"speed":[min,max],"sway":0..8,"url":"http://{host}:8899/sprite?s=80x80&f=2&a={A}&m={M}&u=<immagine>{N}...","ids":[...]}`. Ogni walker percorre una linea retta da fuori un bordo a fuori un altro (almeno 120 px), compare e sparisce fuori schermo, poi ne parte un altro con uno sprite a caso tra `ids`; partono scaglionati per un viavai continuo. Lo sprite è girato nel verso di marcia (`{A}` = inclinazione in gradi, passi da 15, -90..90; `{M}` = 1 se va a ovest, specchiato, mai a testa in giù) e ondeggia (`sway`). Le immagini non sono nel pacchetto: `{host}` è l'indirizzo del PC che ha mandato per ultimo `/api/usage`, la porta 8899 è il proxy degli sprite dentro `smalltv-monitord` (`smalltv-daemon/src/sprites.rs`: scarica PNG/GIF di PokeAPI, li ritaglia, scala, gira e manda un blob RGB565 `STR1`; solo l'orologio può chiedere, solo indirizzi permessi, `SMALLTV_IMG_ALLOW` ne aggiunge). L'orologio salva il blob in `/rimgN` su LittleFS (`RemoteAssets.cpp`, un file per walker). Con il PC spento i walker non si vedono.
- Tema `themes/pokemon-walk.stheme`: sei walker da 80×80, 77 Pokémon tra i più famosi (id nel manifest), sfondo con prato e barra rossa con l'ora. Test: `tests/theme/test_walker.cpp`; il pacchetto lo costruisce `smalltv_theme.py build themes/pokemon-walk themes/pokemon-walk.stheme`.
- Pagina web: `GET /` (`src/webui.html`, rigenerare `src/webui.h` con `python3 tools/gzip_webui.py`). Modalità, luminosità, rotazione, fuso, Wi-Fi, prova bottoni, elenco/carica/usa/elimina temi `.stheme`.
- Il firmware è compilato senza ticker, radar e Home Assistant (`env:smalltv`).

## Piano e decisioni

Il piano completo, le decisioni dell'utente e lo stato di ogni fase stanno in `ROADMAP.md`: leggilo prima di lavorare. In breve: schermate come modalità del carosello (monitor esistente per prima; CPU/RAM; rete; meteo; orologio a temi; snake; video), dati pesanti dal PC (daemon Rust: connessioni, meteo di Cecchina; client Python in `smalltv-video/`), una sola pagina web di configurazione minuscola con caricamento temi. Firmware senza ticker, radar e Home Assistant (flag in `env:smalltv`). Non flashare prima della fine.

## Regole

- Non leggere, stampare o copiare file di credenziali (`~/.claude/.credentials.json`, `auth.json`, `config.json` reali di TokenTV). Il daemon Rust legge il token a runtime per l'utente; tu no. I test usano dati finti (`token_tv/sample.py`, JSON inline).
- Non flashare e non cancellare nulla sull'orologio senza che l'utente lo abbia chiesto: il flash sostituisce il firmware GeekMagic, senza backup.
- Ferma `smalltv-monitord` prima di ogni upload del firmware (vedi sopra).
- Un valore sconosciuto non diventa mai 0: si omette dal JSON e si disegna un trattino.
- Non pubblicare, non fare push, non aprire issue per conto dell'utente.
- Quando lanci `pkill -f` o `kill $(pgrep -f ...)` da Bash, il pattern può trovare la riga di comando della tua stessa shell e uccidere il comando (uscita 144). Usa un pattern tra parentesi (`[s]malltv`) e tieni il comando separato dagli altri.
- Il tool Bash non permette `sleep` lunghi concatenati: per aspettare un'attesa usa `timeout N bash -c 'until ...; do sleep 2; done'`.

## Da sapere per modificare il firmware

- Fork di giovi321/smalltv-mod: i file fuori da `src/features/usage/` sono invariati. Non toccare quelli senza motivo, per poter riprendere gli aggiornamenti a monte.
- Il firmware dichiara RAM al 65,5% e flash al 72,9% (build `smalltv`, senza ticker/radar/HA).
- `A0` (ADC) non è usato per la luminosità automatica sul tuo orologio: nessun LDR montato, e `autoBrightness` è spento di default. Vedi `handoff.md`.
