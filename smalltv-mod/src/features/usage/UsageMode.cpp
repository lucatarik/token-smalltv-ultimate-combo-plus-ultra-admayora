#include "UsageMode.h"
#include <Arduino_GFX_Library.h>
#include "Gfx.h"
#include "Draw.h"
#include "UsageClient.h"
#include "Mascot.h"
#include "Clock.h"
#include <time.h>

UsageMode g_usageMode;

// Mascot diff state for the flicker-free full-screen idle animation.
static bool            s_mascotPrimed  = false;
static const uint16_t* s_mascotPalette = nullptr;
static uint8_t         s_prevCells[MASCOT_GRID * MASCOT_GRID];

// Copy a mascot palette into a local RAM array using *byte* reads. pgm_read_byte
// is safe from both RAM and flash; a 16-bit load straight from flash (irom) faults
// on the ESP8266, so this never depends on where the palette actually lives.
// Tinted on the way out so the mascot's own palette follows the Display tab's
// colour correction like everything else.
static void loadPalette(const uint16_t* palette, uint16_t* out) {
  const uint8_t* p = (const uint8_t*)palette;
  for (int k = 0; k < MASCOT_PALETTE_SIZE; k++)
    out[k] = gfxTint((uint16_t)(pgm_read_byte(p + 2 * k) | (pgm_read_byte(p + 2 * k + 1) << 8)));
}

// Draw a 20x20 mascot frame at (x0,y0), cellPx per cell. Reads PROGMEM frame data.
static void blitMascot(Arduino_GFX* gfx, const uint8_t* cells, const uint16_t* palette,
                       int x0, int y0, int cellPx) {
  uint16_t pal[MASCOT_PALETTE_SIZE];
  loadPalette(palette, pal);
  for (int i = 0; i < MASCOT_GRID * MASCOT_GRID; i++) {
    uint8_t code = pgm_read_byte(&cells[i]);
    uint16_t color = (code < MASCOT_PALETTE_SIZE) ? pal[code] : 0;
    int gx = i % MASCOT_GRID, gy = i / MASCOT_GRID;
    gfx->fillRect(x0 + gx * cellPx, y0 + gy * cellPx, cellPx, cellPx, color);
  }
}

static void fmtReset(int mins, char* out, size_t n) {
  if (mins <= 0) { strlcpy(out, "now", n); return; }
  int d = mins / 1440, h = (mins % 1440) / 60, m = mins % 60;
  if (d > 0)      snprintf(out, n, "%dd %dh", d, h);
  else if (h > 0) snprintf(out, n, "%dh %02dm", h, m);
  else            snprintf(out, n, "%dm", m);
}

static uint16_t barColor(float pct) {
  if (pct >= 90) return C_RED;
  if (pct >= 75) return C_ACCENT;
  return C_UGREEN;
}

// One usage card: big %, a 5h/7d label, a fill bar coloured by load, and the
// reset countdown. `top` is the card's top y; the card is 82px tall.
static void drawMeter(Arduino_GFX* gfx, int top, const char* label,
                      float pct, int resetMins) {
  const int x = 8, w = 224, h = 82;
  gfx->fillRoundRect(x, top, w, h, 8, C_PANEL);

  char pc[8];
  snprintf(pc, sizeof(pc), "%d%%", (int)lroundf(constrain(pct, 0.0f, 100.0f)));
  uint8_t sz = gfxFitSize(pc, 150, 5);
  gfx->setTextSize(sz);
  gfx->setTextColor(C_WHITE);
  gfx->setCursor(x + 14, top + 10);
  gfx->print(pc);

  int lw = gfxTextW(label, 2);
  gfx->setTextSize(2);
  gfx->setTextColor(C_DIM);
  gfx->setCursor(x + w - lw - 14, top + 12);
  gfx->print(label);

  int bx = x + 14, by = top + 52, bw = w - 28, bh = 12;
  gfx->fillRoundRect(bx, by, bw, bh, bh / 2, C_BARBG);
  int fw = (int)(bw * constrain(pct, 0.0f, 100.0f) / 100.0f);
  if (fw >= bh)     gfx->fillRoundRect(bx, by, fw, bh, bh / 2, barColor(pct));
  else if (fw > 0)  gfx->fillRect(bx, by, fw, bh, barColor(pct));

  char rs[16], line[28];
  fmtReset(resetMins, rs, sizeof(rs));
  snprintf(line, sizeof(line), "Resets in %s", rs);
  gfx->setTextSize(2);
  gfx->setTextColor(C_DIM);
  gfx->setCursor(x + 14, top + 64);
  gfx->print(line);
}

// The 7-day window length in minutes, for the even-pace marker.
#define WEEK_MIN 10080

// Single-card layout for plans without a 5-hour limit: one tall card holding
// the 7-day figure. The percentage gets the extra room, the bar is thicker and
// carries an even-pace marker (where usage would be if spread evenly over the
// week), and a pace line under the countdown says whether you are ahead of it.
// The card spans y=52..232 and repaints its whole area, like drawMeter().
static void drawWeekly(Arduino_GFX* gfx, float pct, int resetMins) {
  const int x = 8, top = 52, w = 224, h = 180;
  gfx->fillRoundRect(x, top, w, h, 10, C_PANEL);
  pct = constrain(pct, 0.0f, 100.0f);

  gfx->setTextSize(2);
  gfx->setTextColor(C_DIM);
  gfx->setCursor(x + 14, top + 14);
  gfx->print("7-DAY USAGE");

  char pc[8];
  snprintf(pc, sizeof(pc), "%d%%", (int)lroundf(pct));
  uint8_t sz = gfxFitSize(pc, w - 28, 7);
  gfxDrawCentered(pc, top + 44, sz, C_WHITE);

  const int bx = x + 14, by = top + 110, bw = w - 28, bh = 18;
  gfx->fillRoundRect(bx, by, bw, bh, bh / 2, C_BARBG);
  int fw = (int)(bw * pct / 100.0f);
  if (fw >= bh)     gfx->fillRoundRect(bx, by, fw, bh, bh / 2, barColor(pct));
  else if (fw > 0)  gfx->fillRect(bx, by, fw, bh, barColor(pct));

  // Even pace: the share of the week already elapsed. Only meaningful while the
  // countdown is inside one week.
  bool havePace = resetMins > 0 && resetMins <= WEEK_MIN;
  float pace = havePace ? 100.0f * (WEEK_MIN - resetMins) / WEEK_MIN : 0;
  if (havePace) {
    int px = bx + (int)(bw * pace / 100.0f);
    px = constrain(px, bx + 1, bx + bw - 3);
    gfx->fillRect(px, by - 5, 2, bh + 10, C_WHITE);
  }

  char rs[16], line[28];
  fmtReset(resetMins, rs, sizeof(rs));
  snprintf(line, sizeof(line), "Resets in %s", rs);
  gfxDrawCentered(line, top + 140, 2, C_DIM);

  if (havePace) {
    snprintf(line, sizeof(line), "Even pace %d%%", (int)lroundf(pace));
    // Amber once usage runs 5 points or more ahead of an even burn.
    gfxDrawCentered(line, top + 160, 2, pct >= pace + 5 ? C_ACCENT : C_DIM);
  }
}

// ---- Monitor layout: Claude limits, Claude service status and the computer ----------
// Flicker-free by construction, in the manner of GiT-BeDa/KleinerAnzeigeComputer (each changed row is
// built off-screen and sent in one piece, unchanged rows are not touched), but without a frame buffer: the
// ESP8266 has about 9 KB of heap left, so
//   - every element remembers what it shows and is redrawn only when that changes;
//   - text is composed pixel by pixel with its background and sent row by row, so the old value is
//     replaced in place and the screen never shows a cleared box;
//   - a bar paints only the part that grew or shrank, and never a track under its fill.
// The static layout (panels, labels, divider) is painted once, on a full repaint.

static int clockMinute() {
  struct tm t;
  return clockNow(t) ? t.tm_hour * 60 + t.tm_min : -1;
}


struct MonitorState {
  TextState time, pct5, reset5, at5, pct7, reset7, cpu, ram, disk, dn, up, status;
  BarState  bar5, bar7, barCpu, barRam, barDisk;
  uint16_t  dot; bool dotDrawn;
};
static MonitorState ms;

// A limit card: 46 px tall, or 58 with `at` (the HH:MM of the reset under the countdown).
static void drawLimit(Arduino_GFX* gfx, bool force, int top, const char* label, float pct, const char* reset,
                      TextState& tPct, TextState& tReset, TextState* tAt, const char* at,
                      BarState& bar) {
  const int x = 8, w = 224, h = tAt ? 58 : 46;
  if (force) {
    gfx->fillRoundRect(x, top, w, h, 8, C_PANEL);
    gfx->setTextSize(2); gfx->setTextColor(C_DIM, C_PANEL);
    gfx->setCursor(x + 8, top + 8);  gfx->print(label);
  }
  char pc[8];
  snprintf(pc, sizeof(pc), "%d%%", (int)lroundf(constrain(pct, 0.0f, 100.0f)));
  textBlock(gfx, tPct, force, x + 40, top + 4, 3, 4, pc, C_WHITE, C_PANEL);
  textBlock(gfx, tReset, force, x + w - 8 - 9 * 12, top + 8, 2, 9, reset, C_DIM, C_PANEL, true);
  if (tAt) textBlock(gfx, *tAt, force, x + w - 8 - 9 * 12, top + 26, 2, 9, at, C_DIM, C_PANEL, true);
  barDelta(gfx, bar, force, x + 8, top + h - 14, w - 16, 8, (int)lroundf(pct), barColor(pct));
}

static void drawResource(Arduino_GFX* gfx, bool force, int y, const char* label, int pct, TextState& tPct,
                         BarState& bar) {
  char pc[8];
  if (pct < 0) strlcpy(pc, "--", sizeof(pc)); else snprintf(pc, sizeof(pc), "%d%%", pct);
  if (force) {
    gfx->setTextSize(2); gfx->setTextColor(C_DIM, C_BLACK);
    gfx->setCursor(8, y);  gfx->print(label);
  }
  barDelta(gfx, bar, force, 58, y + 2, 112, 10, pct, pct < 0 ? C_BARBG : barColor(pct));
  textBlock(gfx, tPct, force, 184, y, 2, 4, pc, C_WHITE, C_BLACK, true);
}

static void drawMonitor(const UsageData& u, bool force) {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;
  if (force) memset(&ms, 0, sizeof(ms));   // the screen was cleared: nothing is drawn any more
  char reset5[16], resetW[16];
  fmtReset(u.sessionResetMin, reset5, sizeof(reset5));
  // The weekly reset as weekday and clock time when the device knows the time, else as a countdown.
  struct tm t;
  bool timeKnown = clockNow(t);
  if (u.weeklyResetMin > 0 && timeKnown) {
    time_t at = time(nullptr) + (time_t)u.weeklyResetMin * 60;
    struct tm r;
    localtime_r(&at, &r);
    static const char* const DAY[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    snprintf(resetW, sizeof(resetW), "%s %02d:%02d", DAY[r.tm_wday], r.tm_hour, r.tm_min);
  } else {
    fmtReset(u.weeklyResetMin, resetW, sizeof(resetW));
  }

  if (timeKnown) {
    char hhmm[8];
    snprintf(hhmm, sizeof(hhmm), "%02d:%02d", t.tm_hour, t.tm_min);
    textBlock(gfx, ms.time, force, 134, 14, 2, 5, hhmm, C_WHITE, C_BLACK);
  }
  // Claude service status: a dot and its word, top right.
  const char* cs = u.claudeStatus;
  uint16_t dot = !cs[0] ? C_GRAY : !strcmp(cs, "ok") ? C_UGREEN : (!strcmp(cs, "minor") || !strcmp(cs, "maint")) ? C_YELLOW : C_RED;
  if (force || !ms.dotDrawn || ms.dot != dot) {
    gfx->fillCircle(222, 12, 5, dot);
    ms.dot = dot; ms.dotDrawn = true;
  }
  textBlock(gfx, ms.status, force, 232 - 30, 22, 1, 5, cs[0] ? cs : "?", dot, C_BLACK, true);

  char at5[8] = "--:--";
  if (u.sessionResetMin > 0 && timeKnown) {
    time_t when = time(nullptr) + (time_t)u.sessionResetMin * 60;
    struct tm r;
    localtime_r(&when, &r);
    snprintf(at5, sizeof(at5), "%02d:%02d", r.tm_hour, r.tm_min);
  }
  drawLimit(gfx, force, 44, "5H", u.sessionPct, reset5, ms.pct5, ms.reset5, &ms.at5, at5, ms.bar5);
  drawLimit(gfx, force, 106, "7D", u.weeklyPct, resetW, ms.pct7, ms.reset7, nullptr, nullptr, ms.bar7);
  if (force) gfx->drawFastHLine(8, 154, 224, C_BARBG);
  drawResource(gfx, force, 158, "CPU", u.cpuPct, ms.cpu, ms.barCpu);
  drawResource(gfx, force, 178, "RAM", u.ramPct, ms.ram, ms.barRam);
  drawResource(gfx, force, 198, "DISK", u.diskPct, ms.disk, ms.barDisk);
  char dn[12], up[12], line[12];
  speedText(u.netDownBps, dn, sizeof(dn));
  speedText(u.netUpBps, up, sizeof(up));
  snprintf(line, sizeof(line), "DN %s", dn);
  textBlock(gfx, ms.dn, force, 8, 218, 2, 9, line, C_WHITE, C_BLACK);
  snprintf(line, sizeof(line), "UP %s", up);
  textBlock(gfx, ms.up, force, 232 - 9 * 12, 218, 2, 9, line, C_WHITE, C_BLACK, true);
}

// Last-drawn state of the accent flag, so a routine update can toggle just the
// dot instead of a full-screen clear.
static bool s_flagShown = false;

// `fullRepaint` clears the static layout only on a real transition — cards and
// the flag dot always repaint their own area, so routine updates skip it.
static void drawUsage(const UsageData& u, bool fullRepaint, bool hide5h) {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;

  if (fullRepaint) {
    s_mascotPrimed = false;   // force a full redraw next time the idle animation shows
    gfx->fillScreen(C_BLACK);

    // Header: a small calm mascot pose + title.
    blitMascot(gfx, mascotIdleCells(), mascotIdlePalette(), 6, 4, 2);
    gfx->setTextSize(u.hw ? 2 : 3);   // the monitor layout shares the header with the time
    gfx->setTextColor(C_WHITE);
    gfx->setCursor(56, u.hw ? 14 : 12);
    gfx->print("CLAUDE");
    s_flagShown = false;
  }

  if (!u.valid) {
    if (fullRepaint) gfxDrawCentered(u.error ? "daemon error" : "waiting...", 120, 2, C_DIM);
    return;
  }

  // A status that is not calm gets a small accent flag; erasing with black is
  // safe here since nothing else ever draws at x=228. The daemon has shipped two
  // status vocabularies: the old rate-limit headers said "allowed" /
  // "allowed_warning" / "rejected", the usage endpoint says "normal" / "warning"
  // / "rejected". Both calm values clear the flag. Compare exactly, because the
  // old 7-char prefix match on "allowed" also swallowed "allowed_warning".
  if (u.hw) { drawMonitor(u, fullRepaint); return; }

  bool showFlag = u.status[0]
               && strcmp(u.status, "allowed") != 0
               && strcmp(u.status, "normal")  != 0;
  if (showFlag != s_flagShown || fullRepaint) {
    gfx->fillCircle(228, 18, 5, showFlag ? C_ACCENT : C_BLACK);
    s_flagShown = showFlag;
  }

  // Plans without a 5-hour limit can hide that card; the 7-day figure then gets
  // its own full-height layout.
  if (hide5h) {
    drawWeekly(gfx, u.weeklyPct, u.weeklyResetMin);
  } else {
    drawMeter(gfx, 50,  "5h", u.sessionPct, u.sessionResetMin);
    drawMeter(gfx, 138, "7d", u.weeklyPct,  u.weeklyResetMin);
  }
}

// Idle animation: full-screen mascot, diffed cell-by-cell for a flicker-free draw.
static void drawMascot(const uint8_t* cells, const uint16_t* palette, bool restart) {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx || !cells || !palette) return;
  uint16_t pal[MASCOT_PALETTE_SIZE];
  loadPalette(palette, pal);
  const int CP = TFT_WIDTH / MASCOT_GRID;                 // 240 / 20 = 12
  const int x0 = (TFT_WIDTH  - MASCOT_GRID * CP) / 2;
  const int y0 = (TFT_HEIGHT - MASCOT_GRID * CP) / 2;

  // Full redraw on (re)entry or whenever the palette changes (animation switch);
  // otherwise only repaint the cells that changed since the last frame.
  bool full = restart || !s_mascotPrimed || palette != s_mascotPalette;
  if (full) gfx->fillScreen(C_BLACK);

  for (int i = 0; i < MASCOT_GRID * MASCOT_GRID; i++) {
    uint8_t code = pgm_read_byte(&cells[i]);
    if (!full && code == s_prevCells[i]) continue;
    s_prevCells[i] = code;
    uint16_t color = (code < MASCOT_PALETTE_SIZE) ? pal[code] : 0;
    int gx = i % MASCOT_GRID, gy = i / MASCOT_GRID;
    gfx->fillRect(x0 + gx * CP, y0 + gy * CP, CP, CP, color);
  }
  s_mascotPrimed  = true;
  s_mascotPalette = palette;
}

// The daemon re-POSTs on a fixed timer even when nothing changed, and
// drawUsage() does a full fillScreen — without this check every push flashes.
bool UsageMode::contentChanged(const UsageData& u) const {
  if (!contentPrimed_) return true;
  return u.valid != lastValid_
      || u.error != lastError_
      || u.hw != lastHw_
      || (u.hw && (u.cpuPct != lastCpu_ || u.ramPct != lastRam_ || u.diskPct != lastDisk_
                   || u.netDownBps != lastDown_ || u.netUpBps != lastUp_
                   || strncmp(u.claudeStatus, lastClaude_, sizeof(lastClaude_)) != 0
                   || clockMinute() != lastMinute_))
      || u.sessionPct != lastSessionPct_
      || u.weeklyPct != lastWeeklyPct_
      || u.sessionResetMin != lastSessionResetMin_
      || u.weeklyResetMin != lastWeeklyResetMin_
      || strncmp(u.status, lastStatus_, sizeof(lastStatus_)) != 0;
}

void UsageMode::rememberContent(const UsageData& u) {
  contentPrimed_ = true;
  lastValid_ = u.valid;
  lastHw_ = u.hw;
  lastCpu_ = u.cpuPct; lastRam_ = u.ramPct; lastDisk_ = u.diskPct;
  lastDown_ = u.netDownBps; lastUp_ = u.netUpBps;
  strlcpy(lastClaude_, u.claudeStatus, sizeof(lastClaude_));
  lastMinute_ = clockMinute();
  lastError_ = u.error;
  lastSessionPct_ = u.sessionPct;
  lastWeeklyPct_ = u.weeklyPct;
  lastSessionResetMin_ = u.sessionResetMin;
  lastWeeklyResetMin_ = u.weeklyResetMin;
  strlcpy(lastStatus_, u.status, sizeof(lastStatus_));
}

// ---- DisplayMode ----------------------------------------------------------
void UsageMode::begin(const Settings& s) {
  usageInit(s);
  mascotInit();
  usageSampled_ = 0;
  usageRenderedOk_ = 0xFFFFFFFF;
  showingMascot_ = false;
  needRender_ = true;
  contentPrimed_ = false;
  layoutPrimed_ = false;
}

void UsageMode::invalidate(const Settings& s) {
  needRender_ = true;
  showingMascot_ = false;
  usageRenderedOk_ = 0xFFFFFFFF;
  contentPrimed_ = false;
  layoutPrimed_ = false;
  usageInit(s);
  usageForceRefresh();
}

void UsageMode::service(const Settings& s) {
  // Pull mode: poll the daemon when a Usage URL is set. Push mode: leave it blank
  // and the daemon POSTs to /api/usage (for networks where the device can't reach
  // the PC). Either way usageGet() drives the render below.
  if (s.usage.usageUrl.length() >= 8) usageService(s);

  const UsageData& u = usageGet();

  // The monitor layout shows the time and the reset times, so it needs SNTP. The clock module arms it
  // only for night mode, WireGuard and theme clocks (it costs heap), so arm it here, once per timezone.
  if (u.hw && (!clockArmed_ || clockArmedTz_ != s.clock.tzPosix)) {
    clockBegin(s);
    clockArmed_ = true;
    clockArmedTz_ = s.clock.tzPosix;
  }

  // Feed the burn-rate tracker once per fresh reading (drives the mascot's mood).
  if (u.valid && u.lastOkMs != usageSampled_) {
    usageSampled_ = u.lastOkMs;
    // With the 5h card hidden the session window may no longer exist, so the
    // mood follows the 7-day figure instead.
    mascotSample(s.usage.hide5h ? u.weeklyPct : u.sessionPct);
  }

  // Considered stale after ~2 missed polls (plus a grace) — then show the animation.
  uint32_t staleMs = (uint32_t)s.usage.pollSec * 1000UL * 2UL + USAGE_STALE_GRACE_MS;

  if (usageFresh(staleMs)) {
    bool fullRepaint = !layoutPrimed_;
    if (showingMascot_) { showingMascot_ = false; needRender_ = true; fullRepaint = true; }
    if (u.lastOkMs != usageRenderedOk_) {
      usageRenderedOk_ = u.lastOkMs;
      if (contentChanged(u)) {
        if (u.valid != lastValid_ || u.hw != lastHw_) fullRepaint = true;   // layout itself changes shape
        rememberContent(u);
        needRender_ = true;
      }
    }
    if (needRender_) {
      drawUsage(u, fullRepaint, s.usage.hide5h);
      layoutPrimed_ = true;
      needRender_ = false;
    }
  } else {
    if (!showingMascot_) {
      showingMascot_ = true;
      usageRenderedOk_ = 0xFFFFFFFF;
      mascotReset();
      drawMascot(mascotCells(), mascotPalette(), /*restart=*/true);
    } else if (mascotTick()) {
      drawMascot(mascotCells(), mascotPalette(), /*restart=*/false);
    }
  }
}
