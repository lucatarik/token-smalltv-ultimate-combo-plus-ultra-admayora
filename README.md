# token-smalltv-ultimate-combo-plus-ultra-admayora — a Claude / PC monitor for the GeekMagic SmallTV-Ultra

Turn a cheap GeekMagic **SmallTV-Ultra** (ESP-12F / ESP8266, 240×240 ST7789 screen) into a desk monitor that shows:

- your **Claude limits** (5-hour and 7-day windows, with reset times) and the **Claude service status**;
- the **time**, the **PC load** (CPU, RAM, disk, network) and the latest network connections;
- **weather**, a **snake** game, themed **clocks** (including walking Pokémon) and **video / GIF / desktop mirroring**.

A small Rust daemon on the PC pushes the data to the clock over Wi-Fi; the clock only draws. Three physical buttons (one wire, a resistor ladder on `A0`) switch between screens.

> Status: firmware flashed and running on the author's clock; the button circuit is **designed and decoded in firmware but not soldered yet** (use `POST /api/button` to simulate presses). Nobody has verified every screen by eye, see [Known limits](#known-limits).

## Repository layout

| Folder | What it is | Language |
| --- | --- | --- |
| `smalltv-mod/` | Local fork of [giovi321/smalltv-mod](https://github.com/giovi321/smalltv-mod): firmware that replaces the GeekMagic one. Our changes live mostly in `src/features/usage/`, plus the theme engine, web page and settings. | C++ (Arduino, PlatformIO) |
| `smalltv-daemon/` | `smalltv-monitord`: pushes data to the clock every second (CPU/RAM/network from `/proc`, disk via `statvfs`, Claude limits from the OAuth API, status from status.claude.com, weather from Open-Meteo) and serves the sprite proxy. | Rust |
| `smalltv-video/` | `smalltv_video.py`: sends a YouTube URL, GIF, video file or a square of your desktop to the clock. | Python |
| `themes/` | `.stheme` packages (clock themes), including `pokemon-walk.stheme` and its sources. | JSON / images |
| `ROADMAP.md`, `handoff.md`, `CLAUDE.md` | Decisions, phases, button circuit notes, working notes (partly in Italian). | |

## How it works

```
 PC (Linux)                                          SmallTV-Ultra (ESP8266)
┌──────────────────────────┐   HTTP over Wi-Fi     ┌──────────────────────────┐
│ smalltv-monitord (Rust)  │ ────────────────────► │ smalltv-mod firmware     │
│  POST /api/usage  1 s    │  limits, CPU, RAM,    │  screens + carousel      │
│  POST /api/net    3 s    │  disk, net, status    │  draws line by line,     │
│  POST /api/wx     1 min  │                       │  no screen buffer        │
│  sprite proxy :8899      │ ◄──────────────────── │  fetches sprites :8899   │
│ smalltv_video.py         │   TCP :8888 (video)   │  web UI on :80           │
└──────────────────────────┘                       └──────────────────────────┘
```

Key constraint: the ESP8266 has about 18 KB of free heap, so there is **no framebuffer**. Every element remembers what it shows and redraws only when it changes; text is composited pixel by pixel against the background and sent one row at a time; bars redraw only the changed part. An unknown value is never shown as 0: it is omitted from the JSON and drawn as a dash.

### Screens (carousel)

Monitor (Claude limits + PC) · CPU/RAM history · Network · Weather · Themed clock · Snake · Video (only while a stream is active).

- Left / right: previous / next screen. OK (short press): screen action (CPU/RAM and network switch window 2 min / 20 min / 2 h; snake starts, then pauses). OK held 1 s = BACK (snake resets, other screens return to the first).
- Web page options (`/api/config`): `autoRotate` + `carouselSec`, `snakeAuto` (snake plays itself with a shortest-path search), `screenMask` (which screens are enabled), `netWeb`, weather place.
- Snake has no walls (wraps around) and a timed 5-point bonus.
- Network screen: last 8 connections, `>` outgoing TCP, `<` incoming TCP, `u` UDP, `w` web (dim, hideable). Loopback and DNS excluded.
- Theme engine supports a `walker` layer: sprites (fetched through the daemon's proxy on port 8899, converted to RGB565) that walk across the screen.

## Button wiring (three buttons, one pin)

The bare ESP-12F has a free ADC pin (`A0`, module pin 2 next to `RST` and `EN`) because the clock ships without a light sensor. Three buttons share that one wire through a resistor ladder.

```
3V3 ──┬─────────┬──────────┐
      │         │          │
   [BTN LEFT] [BTN OK]  [BTN RIGHT]
      │         │          │
     27kΩ      47kΩ      100kΩ
      └─────────┴──────────┘
                │
                ├──────────── A0 (ADC pin)   (optional 100 nF to GND)
                │
              10kΩ (pull-down)
                │
GND ────────────┘
```

Expected ADC readings (0–1023, computed with `3.3 V × 10k / (10k + R)`, **not yet measured**): idle ≈ 0, LEFT ≈ 910, OK ≈ 590, RIGHT ≈ 300. Pressing two buttons gives an in-between value that is ignored. Decoding (`smalltv-mod/src/features/usage/ButtonLadder.h`) uses ±60 bands, 3 consecutive agreeing samples, and a 1 s hold of OK for BACK. It is a pure header, tested on the host (`smalltv-mod/tests/test_buttons.cpp`).

**Safety:** the bare module's ADC accepts about 1 V at most (no divider like on a NodeMCU). The pull-down keeps `A0` at 0 V when idle. Never connect `3V3` straight to `A0` and never turn the ladder into a pull-up. `HAS_LDR` is set to 0, so `A0` reads buttons, not light.

Avoided pins: `GPIO0` is the display DC line (and flash mode), `GPIO3/RXD` was ruled out, `GPIO12/4/16` need direct soldering between display pins. Display pins: `GPIO14` SCLK, `GPIO13` MOSI, `GPIO0` DC, `GPIO2` RST, `GPIO15` CS, `GPIO5` backlight.

Without the circuit: `POST /api/button {"b":"left|right|ok|back"}` simulates a press and `GET /api/button` returns the raw `adc` value so you can calibrate the thresholds once it is wired.

## Build and install

You need [PlatformIO](https://platformio.org/), a Rust toolchain, and (optionally) `ffmpeg`, `yt-dlp`.

### Firmware

The USB-C port is power only (no serial), so flash over Wi-Fi. **Stop the daemon first**, or POSTs during the upload crash the clock.

```bash
cd smalltv-mod && pio run -e smalltv
systemctl --user stop smalltv-monitord.service
curl -s -m 90 -F "firmware=@.pio/build/smalltv/firmware.bin" http://<clock-ip>/update   # answers OK
systemctl --user start smalltv-monitord.service
```

There is no backup of the original GeekMagic firmware (that would need the serial port). Flashing replaces it; an official `.bin` can be rewritten from [GeekMagicClock/smalltv-ultra](https://github.com/GeekMagicClock/smalltv-ultra) (not tested). Build size: RAM 66 %, flash 73 %, built without ticker, radar and Home Assistant.

### Daemon

```bash
cd smalltv-daemon && cargo test && cargo install --path . --root ~/.local
smalltv-monitord install --device-url http://<clock-ip>   # systemd user unit, starts at login
smalltv-monitord uninstall
journalctl --user -u smalltv-monitord.service -n 20 --no-pager
```

The Claude token is read at runtime from your own Claude Code credentials; it is never stored in the repo. Weather location: `SMALLTV_LAT`, `SMALLTV_LON`, `SMALLTV_PLACE` (default Cecchina, Italy). `SMALLTV_IMG_ALLOW` adds hosts the sprite proxy may fetch.

### Video

```bash
python3 smalltv-video/smalltv_video.py <youtube-url|file|gif> [--device IP] [--fps 8]
python3 smalltv-video/smalltv_video.py --desktop     # drag a red square over your screen (X11)
cd smalltv-video && python3 -B -m unittest
```

Protocol: `POST /api/video {"on":true}`, then TCP port 8888, records `0xA5, y, n` followed by `n` rows of 240 little-endian RGB565 pixels; only changed rows are sent.

### Themes

`.stheme` packages can be uploaded from the clock's web page. Build one with `smalltv-mod/tools/smalltv_theme.py build themes/pokemon-walk themes/pokemon-walk.stheme` (needs Pillow).

## Configuration API

`GET/POST http://<clock-ip>/api/config` (timezone is nested: `{"clock":{"tz":"Europe/Rome","tzPosix":"CET-1CEST,M3.5.0,M10.5.0/3"}}`; `{"mode":"carousel"}` at root), `GET /api/status` (`uptime`, `heap`, `maxblk`, `reset`, `synced`), `POST /api/usage`, `/api/net`, `/api/wx`, `/api/video`, `/api/button`. Time and reset times need SNTP, enabled automatically on the first PC data.

## Known limits

- Free heap is about 18–19 KB normally and about 8 KB with a walking-sprite theme; the stack can drop to ~130 B during snake/video.
- Button thresholds are calculated, not measured. Frame rate of walkers and video was never measured. Layouts of the new screens were checked by tests and logic, not by eye.
- Walking sprites need the PC on (they come through the daemon's proxy).
- The daemon targets Linux (`/proc`, `systemd --user`).

## Credits

This project stands on other people's work.

- **[giovi321/smalltv-mod](https://github.com/giovi321/smalltv-mod)** (WTFPL): the firmware we forked. It provides the platform, web portal, settings, theme engine and Claude usage mode. Its own credits include [GeekMagicClock/smalltv-pro](https://github.com/GeekMagicClock/smalltv-pro), [ViToni/esphome-geekmagic-smalltv](https://github.com/ViToni/esphome-geekmagic-smalltv), [MatixYo/ESP32-Plane-Radar](https://github.com/MatixYo/ESP32-Plane-Radar), and [clawdmeter](https://github.com/HermannBjorgvin/Clawdmeter) with mascot frames from [claudepix](https://claudepix.vercel.app).
- **[click6067-ship-it/token-tv](https://github.com/click6067-ship-it/token-tv)** (MIT): the original idea of Claude limits on a SmallTV, and the starting point of this project (we no longer ship its code).
- **GeekMagic** for the SmallTV-Ultra hardware and stock firmware ([GeekMagicClock/smalltv-ultra](https://github.com/GeekMagicClock/smalltv-ultra)).
- Libraries and tools: [Arduino_GFX](https://github.com/moononournation/Arduino_GFX), [ArduinoJson](https://arduinojson.org/), [PlatformIO](https://platformio.org/), [Pico CSS](https://picocss.com/) (web page), Rust crates `gif`, `png`, `ureq`, `serde_json`, `time`, `libc`, plus [FFmpeg](https://ffmpeg.org/) and [yt-dlp](https://github.com/yt-dlp/yt-dlp).
- Data: [Open-Meteo](https://open-meteo.com/) (weather, no key), [status.claude.com](https://status.claude.com) (service status), [PokeAPI](https://pokeapi.co/) (sprites).
- Pokémon and their images belong to Nintendo, Game Freak and The Pokémon Company; used here as fan work. Claude and Anthropic names belong to Anthropic. This project is not affiliated with or endorsed by GeekMagic, Anthropic or any data provider.

## License

**Pasta-ware.** See [LICENSE](LICENSE). `smalltv-mod/` keeps its own license (WTFPL, `smalltv-mod/LICENSE`).
