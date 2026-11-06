#include "Screens.h"
#include <ArduinoJson.h>
#include <ESP8266WiFi.h>
#include "Draw.h"
#include "Platform.h"
#include "Clock.h"
#include <time.h>

extern void appShow(DisplayMode* m);   // main.cpp
extern void appHome();

// Base: clears on a full repaint, then the screen draws only what changed.
class Screen : public DisplayMode {
 public:
  Screen(const char* i, uint8_t m) : i_(i), m_(m) {}
  const char* id() const override { return i_; }
  uint8_t modeConst() const override { return m_; }
  void invalidate(const Settings&) override { full_ = true; }
  void service(const Settings& cfg) override {
    S_ = &cfg;
    Arduino_GFX* g = gfxDev();
    if (!g) return;
    bool full = full_;
    full_ = false;   // before tick(): a screen that asks for a repaint from inside tick() keeps its request
    if (full) g->fillScreen(C_BLACK);
    tick(g, full);
  }
 protected:
  virtual void tick(Arduino_GFX* g, bool full) = 0;
  bool full_ = true;
  const Settings* S_ = nullptr;
 private:
  const char* i_;
  uint8_t m_;
};

// ---- history: 120 samples in three windows (1 s, 10 s, 60 s per sample) ----------------------------
#define HN 120
static const uint8_t PER[3] = {1, 10, 60};
static const char* const WIN[3] = {"2m", "20m", "2h"};
template <class T> struct Hist {
  T v[3][HN];
  uint8_t head[3], cnt[3], n[3];
  uint32_t sum[3];
  void push(T x) {
    for (int w = 0; w < 3; w++) {
      sum[w] += x;
      if (++n[w] < PER[w]) continue;
      v[w][head[w]] = sum[w] / n[w];
      head[w] = (head[w] + 1) % HN;
      if (cnt[w] < HN) cnt[w]++;
      sum[w] = n[w] = 0;
    }
  }
  T at(int w, int i) const { return v[w][(head[w] + HN - cnt[w] + i) % HN]; }
  uint32_t peak(int w) const {
    uint32_t m = 8;
    for (int i = 0; i < cnt[w]; i++) m = max<uint32_t>(m, at(w, i));
    return m;
  }
};
static Hist<uint8_t> hCpu, hRam;
static Hist<uint16_t> hDn, hUp;
static uint32_t g_n;   // bumped on every change a screen shows
static uint16_t s_dn = 0xFFFF, s_up = 0xFFFF;   // current rate, KB/s (0xFFFF unknown)

void screensFeed(const UsageData& u) {
  if (u.cpuPct >= 0) hCpu.push(u.cpuPct);
  if (u.ramPct >= 0) hRam.push(u.ramPct);
  if (u.netDownBps != HW_UNKNOWN_BPS) { s_dn = min<uint32_t>(u.netDownBps >> 10, 0xFFFE); hDn.push(s_dn); }
  if (u.netUpBps != HW_UNKNOWN_BPS) { s_up = min<uint32_t>(u.netUpBps >> 10, 0xFFFE); hUp.push(s_up); }
  g_n++;
}

// Graph x 0..239, newest on the right, each column built whole (no flicker). `top` = value at full height.
template <class T> static void graph(Arduino_GFX* g, int y, int h, const Hist<T>& H, int w, uint32_t top,
                                     uint16_t fill, uint16_t line) {
  uint16_t buf[2 * 80], bg = C_PANEL, grid = C_BARBG;
  for (int i = 0; i < HN; i++) {
    int k = i - (HN - H.cnt[w]), bh = 0;
    if (k >= 0) {
      uint32_t v = H.at(w, k);
      bh = v >= top ? h : v * h / top;
      if (v && !bh) bh = 1;
    }
    for (int r = 0; r < h; r++) {
      int inv = h - 1 - r;
      buf[2 * r] = buf[2 * r + 1] = inv < bh ? (inv == bh - 1 ? line : fill) : r == h / 2 ? grid : bg;
    }
    g->draw16bitRGBBitmap(2 * i, y, buf, 2, h);
  }
}

static void pct(char* o, int v) { if (v < 0) strcpy(o, "--"); else sprintf(o, "%d%%", v); }

// ---- CPU / RAM -----------------------------------------------------------------------------------------
class CpuScreen : public Screen {
 public:
  CpuScreen() : Screen("cpu", MODE_CPU) {}
  bool onOk(const Settings&) override { win_ = (win_ + 1) % 3; seen_ = ~0u; return true; }
 protected:
  void tick(Arduino_GFX* g, bool full) override {
    if (!full && seen_ == g_n) return;
    seen_ = g_n;
    if (full) {
      g->setTextSize(2); g->setTextColor(C_DIM);
      g->setCursor(6, 10); g->print("CPU");
      g->setCursor(6, 124); g->print("RAM");
    }
    char t[8], f[24];
    pct(t, hCpu.cnt[0] ? hCpu.at(0, hCpu.cnt[0] - 1) : -1);
    textBlock(g, a_, full, 240 - 6 - 4 * 18, 4, 3, 4, t, C_WHITE, C_BLACK, true);
    pct(t, hRam.cnt[0] ? hRam.at(0, hRam.cnt[0] - 1) : -1);
    textBlock(g, b_, full, 240 - 6 - 4 * 18, 118, 3, 4, t, C_WHITE, C_BLACK, true);
    graph(g, 34, 76, hCpu, win_, 100, C_ACCENT, C_WHITE);
    graph(g, 148, 76, hRam, win_, 100, C_UGREEN, C_WHITE);
    sprintf(f, "%s window  OK=next", WIN[win_]);
    textBlock(g, c_, full, 6, 230, 1, 24, f, C_DIM, C_BLACK);
  }
 private:
  TextState a_, b_, c_;
  uint8_t win_ = 0;
  uint32_t seen_ = ~0u;
};

// ---- network ----------------------------------------------------------------------------------------------
#define NCX 8   // what the PC sends: its newest connections, newest first
#define NROWS 5 // what fits on the screen
static char g_cx[NCX][22];

bool screensNet(const String& body) {
  JsonDocument d;
  if (deserializeJson(d, body)) return false;
  uint8_t i = 0;
  for (JsonVariantConst c : d["c"].as<JsonArrayConst>())
    if (i < NCX) strlcpy(g_cx[i++], c | "", sizeof(g_cx[0]));
  while (i < NCX) g_cx[i++][0] = 0;
  g_n++;
  return true;
}

class NetScreen : public Screen {
 public:
  NetScreen() : Screen("net", MODE_NET) {}
  bool onOk(const Settings&) override { win_ = (win_ + 1) % 3; seen_ = ~0u; return true; }
 protected:
  void tick(Arduino_GFX* g, bool full) override {
    if (!full && seen_ == g_n) return;
    seen_ = g_n;
    if (full) {
      g->setTextSize(1); g->setTextColor(C_DIM);
      g->setCursor(6, 122); g->print("SUSPICIOUS  > out  < in  u udp");
    }
    char t[12], l[24];
    speedText(s_dn == 0xFFFF ? 0xFFFFFFFFUL : (uint32_t)s_dn << 10, t, sizeof(t));
    sprintf(l, "DN %s", t);
    textBlock(g, a_, full, 4, 4, 2, 9, l, C_UGREEN, C_BLACK);
    speedText(s_up == 0xFFFF ? 0xFFFFFFFFUL : (uint32_t)s_up << 10, t, sizeof(t));
    sprintf(l, "UP %s", t);
    textBlock(g, b_, full, 236 - 9 * 12, 4, 2, 9, l, C_ACCENT, C_BLACK, true);
    uint32_t pd = hDn.peak(win_), pu = hUp.peak(win_);
    graph(g, 24, 44, hDn, win_, pd, C_UGREEN, C_WHITE);
    graph(g, 72, 44, hUp, win_, pu, C_ACCENT, C_WHITE);
    // A FIFO: the newest connection on top, the older ones shifting down as new ones come. Only the kinds the web page
    // ticks are listed (netShow: 1 out, 2 in, 4 UDP, 8 web, web dimmed). Colours: > we opened it, < it came to us, u UDP.
    int row = 0;
    for (int i = 0; i < NCX && row < NROWS; i++) {
      const char c = g_cx[i][0];
      if (!c || (S_ && !(S_->netShow >> (c == '>' ? 0 : c == '<' ? 1 : c == 'u' ? 2 : 3) & 1))) continue;
      textBlock(g, r_[row], full, 6, 134 + 16 * row, 2, 20, g_cx[i], c == '>' ? C_WHITE : c == '<' ? C_YELLOW : c == 'u' ? C_ACCENT : C_DIM, C_BLACK);
      row++;
    }
    if (!row) { textBlock(g, r_[0], full, 6, 134, 2, 20, "nothing suspicious", C_UGREEN, C_BLACK); row = 1; }
    for (; row < NROWS; row++) textBlock(g, r_[row], full, 6, 134 + 16 * row, 2, 20, "", C_WHITE, C_BLACK);
    speedText(pd << 10, t, sizeof(t));
    char u[12];
    speedText(pu << 10, u, sizeof(u));
    sprintf(l, "%s max %s/%s OK=next", WIN[win_], t, u);
    textBlock(g, c_, full, 6, 228, 1, 36, l, C_DIM, C_BLACK);
  }
 private:
  TextState a_, b_, c_, r_[NROWS];
  uint8_t win_ = 0;
  uint32_t seen_ = ~0u;
};

// ---- weather ----------------------------------------------------------------------------------------------
static struct { bool ok; char place[16]; int8_t t, dx[3], dn[3]; uint8_t c, h, w, wd, dc[3]; } wx;
static uint32_t g_wx = 1;
static uint32_t wxAt, wxTried;   // millis() of the last reading (pushed or fetched) and of the last fetch try

bool screensWx(const String& body) {
  JsonDocument d;
  if (deserializeJson(d, body) || !d["t"].is<int>()) return false;
  auto old = wx;
  strlcpy(wx.place, d["p"] | "", sizeof(wx.place));
  wx.t = d["t"]; wx.c = d["c"]; wx.h = d["h"]; wx.w = d["w"]; wx.wd = d["wd"];
  for (int i = 0; i < 3; i++) {
    wx.dx[i] = d["d"][i][0]; wx.dn[i] = d["d"][i][1]; wx.dc[i] = d["d"][i][2];
  }
  wx.ok = true;
  wxAt = millis() | 1;
  if (memcmp(&old, &wx, sizeof(wx))) g_wx++;   // the daemon re-sends the same body: no repaint
  return true;
}

// ---- sky background: weather kind x season x time of day ------------------------------------------------
// Sunrise/sunset in minutes after midnight, per month (central Italy, local time with DST).
static const uint16_t SUNUP[12] = {460, 435, 390, 400, 360, 335, 345, 375, 415, 450, 415, 450};
static const uint16_t SUNDN[12] = {1040, 1070, 1105, 1160, 1190, 1215, 1215, 1185, 1140, 1090, 1010, 1010};
struct Sky { uint8_t phase, season, kind; };   // phase 0 day, 1 dawn/dusk, 2 night; season 0 winter..3 autumn
static uint8_t g_skyKey = 0;
static Sky skyNow(uint8_t k) {
  Sky s{0, 2, k};
  struct tm t;
  if (!clockNow(t)) return s;
  s.season = t.tm_mon == 11 || t.tm_mon < 2 ? 0 : t.tm_mon < 5 ? 1 : t.tm_mon < 8 ? 2 : 3;
  int m = t.tm_hour * 60 + t.tm_min, up = SUNUP[t.tm_mon], dn = SUNDN[t.tm_mon];
  s.phase = m < up - 40 || m > dn + 40 ? 2 : (m < up + 30 || m > dn - 30) ? 1 : 0;
  return s;
}
static uint8_t skyKey(const Sky& s) { return s.phase | s.season << 2 | s.kind << 4; }
static uint8_t mix8(int a, int b, int f) { return (a * (256 - f) + b * f) >> 8; }
// Colour of the sky at row y (0..239): clear-sky gradient of the phase/season, pulled toward grey by the weather.
static uint16_t skyAt(const Sky& s, int y) {
  static const uint8_t CLEAR[3][4][2][3] = {
    {{{70, 120, 180}, {110, 150, 190}}, {{40, 110, 200}, {90, 160, 225}},      // day: winter, spring
     {{20, 90, 200}, {50, 125, 215}},   {{70, 100, 160}, {190, 140, 100}}},    //      summer, autumn
    {{{50, 60, 120}, {200, 110, 80}},   {{60, 70, 130}, {220, 130, 100}},      // dawn/dusk
     {{70, 60, 120}, {230, 110, 60}},   {{70, 50, 100}, {200, 90, 60}}},
    {{{5, 8, 25}, {20, 30, 60}},        {{5, 8, 25}, {20, 30, 60}},            // night
     {{8, 8, 30}, {25, 30, 70}},        {{5, 8, 25}, {20, 25, 50}}}};
  static const uint8_t GREY[3] = {120, 100, 30};
  static const uint8_t PULL[7] = {0, 60, 150, 180, 190, 140, 220};      // toward grey, per kind
  static const uint8_t DARK[7] = {100, 100, 100, 100, 70, 95, 50};       // grey brightness %, per kind
  const uint8_t(*c)[3] = CLEAR[s.phase][s.season];
  int f = y * 256 / 239, rgb[3];
  for (int i = 0; i < 3; i++) {
    int base = mix8(c[0][i], c[1][i], f);
    int grey = GREY[s.phase] * DARK[s.kind] / 100 + (i == 2 ? 10 : 0);
    rgb[i] = mix8(base, grey, PULL[s.kind]);
  }
  return gfxTint(((rgb[0] & 0xF8) << 8) | ((rgb[1] & 0xFC) << 3) | (rgb[2] >> 3));
}
static void skyFill(Arduino_GFX* g, const Sky& s) {
  for (int y = 0; y < 240; y += 2) g->fillRect(0, y, 240, 2, skyAt(s, y));
}

// WMO weather code to icon: 0 sun, 1 partly, 2 cloud, 3 fog, 4 rain, 5 snow, 6 storm.
static uint8_t kind(uint8_t c) {
  return c < 2 ? 0 : c == 2 ? 1 : c == 3 ? 2 : c < 51 ? 3 : (c < 68 || (c > 79 && c < 83)) ? 4 : c >= 95 ? 6 : 5;
}
static const char* const KIND[7] = {"Clear", "Partly cloudy", "Cloudy", "Fog", "Rain", "Snow", "Storm"};

static void drawIcon(Arduino_GFX* g, int x, int y, int r, uint8_t k, const Sky* night = nullptr) {
  uint16_t sun = C_YELLOW, cl = gfxTint(0xC618), rain = C_BLUE, wh = C_WHITE;
  if (k < 2) {
    int sx = k ? x + r / 3 : x, sy = k ? y - r / 3 : y, sr = k ? r / 2 : r * 2 / 3;
    g->fillCircle(sx, sy, sr, night ? gfxTint(0xEF7D) : sun);
    if (night) g->fillCircle(sx + sr / 2, sy - sr / 3, sr * 4 /5, skyAt(*night, sy));   // crescent
  }
  if (k == 0) return;
  g->fillCircle(x - r / 3, y + r / 6, r / 3, cl);
  g->fillCircle(x + r / 10, y - r / 8, r / 2, cl);
  g->fillCircle(x + r / 2, y + r / 5, r / 3, cl);
  g->fillRect(x - r / 3, y + r / 6, r * 5 / 6, r / 3, cl);
  for (int i = 0; i < 3; i++) {
    int px = x - r / 2 + i * r / 2, py = y + r * 2 / 3;
    if (k == 3) g->drawFastHLine(x - r * 2 / 3, y + r / 2 + i * r / 5, r * 4 / 3, wh);
    if (k == 4) g->drawLine(px, py, px - r / 6, py + r / 3, rain);
    if (k == 5) g->fillCircle(px, py + (i & 1) * r / 6, 1 + r / 14, wh);
  }
  if (k == 6) g->fillTriangle(x + r / 6, y + r / 2, x - r / 4, y + r * 5 / 4, x + r / 5, y + r, sun);
}

// The clock reads the weather itself (Open-Meteo over plain HTTP) when nothing has pushed it for 15 minutes,
// so the screen still works with the PC off. Day of the week of "YYYY-MM-DD", 0 = Sunday.
static uint8_t weekday(int y, int m, int d) {
  static const uint8_t T[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3) y--;
  return (y + y / 4 - y / 100 + y / 400 + T[m - 1] + d) % 7;
}

static void wxFetch(const Settings& s) {
  if (ESP.getFreeHeap() < 8000) return;
  char url[240];
  snprintf(url, sizeof(url),
           "http://api.open-meteo.com/v1/forecast?latitude=%.2f&longitude=%.2f&current=temperature_2m,"
           "relative_humidity_2m,weather_code,wind_speed_10m&daily=weather_code,temperature_2m_max,"
           "temperature_2m_min&forecast_days=4&timezone=auto", s.wxLat, s.wxLon);
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(3000);
  http.setReuse(false);
  http.useHTTP10(true);   // no chunked framing: the stream is read raw
  if (!http.begin(client, url)) return;
  JsonDocument doc;
  bool ok = http.GET() == HTTP_CODE_OK && !deserializeJson(doc, http.getStream());
  http.end();
  int y, m, d;
  if (!ok || sscanf(doc["daily"]["time"][0] | "", "%d-%d-%d", &y, &m, &d) != 3 || m < 1 || m > 12 || !doc["current"]["temperature_2m"].is<float>()) return;
  strlcpy(wx.place, s.wxPlace.c_str(), sizeof(wx.place));
  JsonObjectConst c = doc["current"], dl = doc["daily"];
  wx.t = lroundf(c["temperature_2m"]);
  wx.c = c["weather_code"];
  wx.h = lroundf(c["relative_humidity_2m"] | 0.0f);
  wx.w = lroundf(c["wind_speed_10m"] | 0.0f);
  wx.wd = weekday(y, m, d);
  for (int i = 0; i < 3; i++) {
    wx.dx[i] = lroundf(dl["temperature_2m_max"][i + 1] | 0.0f);
    wx.dn[i] = lroundf(dl["temperature_2m_min"][i + 1] | 0.0f);
    wx.dc[i] = dl["weather_code"][i + 1];
  }
  wx.ok = true;
  wxAt = millis() | 1;
  g_wx++;
}

class WxScreen : public Screen {
 public:
  WxScreen() : Screen("weather", MODE_WEATHER) {}
 protected:
  void tick(Arduino_GFX* g, bool full) override {
    uint32_t now = millis();
    if (S_ && now - wxTried > 60000 && (!wx.ok || now - wxAt > 900000)) {   // nothing recent: ask Open-Meteo ourselves
      wxTried = now;
      wxFetch(*S_);
    }
    Sky sky = skyNow(wx.ok ? kind(wx.c) : 0);
    uint8_t key = skyKey(sky);
    if (!full && seen_ == g_wx && key == g_skyKey) return;
    seen_ = g_wx;
    g_skyKey = key;
    skyFill(g, sky);
    const uint16_t sub = gfxTint(0xDEFB);   // light grey: readable on every sky
    if (!wx.ok) { gfxDrawCentered("no weather yet", 110, 2, sub); return; }
    gfxDrawCentered(wx.place, 6, 2, sub);
    char s[24];
    g->setTextSize(7); g->setTextColor(C_WHITE);
    g->setCursor(8, 34); g->printf("%d", wx.t);
    g->setTextSize(3); g->print("C");
    drawIcon(g, 190, 62, 40, kind(wx.c), sky.phase == 2 ? &sky : nullptr);
    g->setTextSize(2); g->setTextColor(C_WHITE);
    g->setCursor(8, 96); g->print(KIND[kind(wx.c)]);
    g->setTextColor(sub);
    g->setCursor(8, 118); g->printf("H %d%%  W %dkm/h", wx.h, wx.w);
    g->drawFastHLine(8, 142, 224, sub);
    static const char* const DAY[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    for (int i = 0; i < 3; i++) {
      int cx = 40 + 80 * i;
      g->setTextSize(2); g->setTextColor(C_WHITE);
      g->setCursor(cx - 18, 150); g->print(DAY[(wx.wd + 1 + i) % 7]);
      drawIcon(g, cx, 184, 16, kind(wx.dc[i]));
      sprintf(s, "%d/%d", wx.dx[i], wx.dn[i]);
      g->setTextColor(sub);
      g->setCursor(cx - strlen(s) * 6, 212); g->print(s);
    }
  }
 private:
  uint32_t seen_ = 0;
};

// ---- snake -------------------------------------------------------------------------------------------------
#define SW 15
#define SH 14
#define SC (SW * SH)
class SnakeScreen : public Screen {
 public:
  SnakeScreen() : Screen("snake", MODE_SNAKE) {}
  bool onOk(const Settings&) override {
    if (st_ == PLAY && auto_) { auto_ = false; pend_ = cur_; }          // OK in an autoplay game: you take over that very game
    else if (st_ == PLAY) { paused_ = !paused_; full_ = true; }         // OK in your own game: pause / go on
    else start(false);                                                  // OK on the title or after a game: a new game
    return true;
  }
  bool busy() const override { return st_ == PLAY && !auto_ && !paused_; }
  bool onBack(const Settings&) override {
    if (st_ == IDLE) return false;
    st_ = IDLE; auto_ = false; full_ = true;
    return true;
  }
  bool onNav(const Settings&, int8_t dir) override {
    if (st_ != PLAY || paused_ || auto_) return false;
    uint8_t nd = (pend_ + (dir > 0 ? 1 : 3)) & 3;
    if (nd != ((cur_ + 2) & 3)) pend_ = nd;
    return true;
  }
 protected:
  void tick(Arduino_GFX* g, bool full) override {
    if (full) {
      if (st_ == IDLE) idleAt_ = millis();
      paint(g);
    }
    uint32_t now = millis();
    if (S_ && S_->snakeAuto) {   // not pressed OK within 3 s: it plays itself, and starts over when it dies
      if (st_ == IDLE && now - idleAt_ > 3000) start(true);
      else if (st_ == OVER && auto_ && now - idleAt_ > 1500) start(true);
    }
    if (st_ != PLAY || paused_ || now - last_ < (auto_ ? 90 : max(70, 170 - 4 * (int)len_))) return;
    last_ = now;
    bonuses(g, now);
    if (auto_) pend_ = ai();
    step(g);
  }
 private:
  enum { IDLE, PLAY, OVER } st_ = IDLE;
  uint8_t body_[SC], len_, tail_, food_, cur_, pend_, bonus_ = 0xFF;
  uint32_t bonusUntil_, bonusNext_;
  bool bonusShown_;
  uint16_t score_, best_;
  bool paused_, auto_;
  uint32_t last_, idleAt_;
  uint8_t par_[SC], q_[SC];
  TextState t_;

  static void cell(Arduino_GFX* g, uint8_t c, uint16_t col) {
    g->fillRect(c % SW * 16 + 1, 16 + c / SW * 16 + 1, 14, 14, col);
  }
  uint8_t headc() const { return body_[(tail_ + len_ - 1) % SC]; }
  bool on(uint8_t c) const {
    for (int i = 0; i < len_; i++) if (body_[(tail_ + i) % SC] == c) return true;
    return false;
  }
  void pick() { do food_ = random(SC); while (on(food_) || food_ == bonus_); }
  // A special bonus now and then: worth 5 points, does not make the snake longer, gone after 10 to 20 s (it blinks at the end).
  void drawBonus(Arduino_GFX* g, bool on) {
    const int x = bonus_ % SW * 16 + 8, y = 24 + bonus_ / SW * 16;
    cell(g, bonus_, C_BLACK);
    if (on) { g->fillCircle(x, y, 7, C_YELLOW); g->fillCircle(x, y, 3, C_WHITE); }
    bonusShown_ = on;
  }
  void bonuses(Arduino_GFX* g, uint32_t now) {
    if (bonus_ == 0xFF) {
      if ((int32_t)(now - bonusNext_) < 0) return;
      do bonus_ = random(SC); while (on(bonus_) || bonus_ == food_);
      bonusUntil_ = now + 10000 + random(10001);   // stays 10 to 20 s
      drawBonus(g, true);
    } else if ((int32_t)(now - bonusUntil_) >= 0) {
      drawBonus(g, false);
      bonus_ = 0xFF;
      bonusNext_ = now + 8000 + random(12000);
    } else if (bonusUntil_ - now < 2000) {
      const bool lit = (now / 200) & 1;
      if (lit != bonusShown_) drawBonus(g, lit);
    }
  }
  void spawn(Arduino_GFX* g) {
    pick();
    g->fillCircle(food_ % SW * 16 + 8, 24 + food_ / SW * 16, 6, C_RED);
  }
  void head(Arduino_GFX* g) {
    char s[24];
    snprintf(s, sizeof(s), paused_ ? "PAUSED  %u" : "SCORE %u", score_);
    textBlock(g, t_, true, 4, 0, 2, 14, s, C_WHITE, C_BLACK);
  }
  void paint(Arduino_GFX* g) {
    if (st_ == IDLE) {
      gfxDrawCentered("SNAKE", 60, 5, C_UGREEN);
      gfxDrawCentered("OK = start", 130, 2, C_WHITE);
      gfxDrawCentered("L/R = turn", 154, 2, C_DIM);
      gfxDrawCentered("BACK = reset", 178, 2, C_DIM);
      return;
    }
    head(g);
    for (int i = 0; i < len_; i++) cell(g, body_[(tail_ + i) % SC], i == len_ - 1 ? C_WHITE : C_UGREEN);
    g->fillCircle(food_ % SW * 16 + 8, 24 + food_ / SW * 16, 6, C_RED);
    if (bonus_ != 0xFF) drawBonus(g, true);
    if (st_ == OVER) {
      gfxDrawCentered("GAME OVER", 96, 3, C_RED);
      gfxDrawCentered("OK = again", 132, 2, C_WHITE);
    }
  }
  void start(bool a) {
    auto_ = a;
    randomSeed(micros());
    len_ = 3; tail_ = 0; cur_ = pend_ = 1; score_ = 0; paused_ = false;
    bonus_ = 0xFF;
    bonusNext_ = millis() + 6000 + random(8000);
    for (int i = 0; i < 3; i++) body_[i] = 7 * SW + 4 + i;
    st_ = PLAY; full_ = true; last_ = millis();
    pick();
  }
  // Autoplay: the first step of the shortest free path to the nearest of food and bonus, else any free neighbour.
  uint8_t ai() {
    static const int8_t DX[4] = {0, 1, 0, -1}, DY[4] = {-1, 0, 1, 0};
    memset(par_, 0xFF, sizeof(par_));
    for (int i = 0; i < len_; i++) par_[body_[(tail_ + i) % SC]] = 0xFE;
    uint8_t h = headc(), qh = 0, qt = 0;
    q_[qt++] = h;
    uint8_t fallback = cur_, target = food_;
    bool found = false;
    while (qh < qt && !found) {
      uint8_t c = q_[qh++];
      for (int d = 0; d < 4; d++) {
        int nx = (c % SW + DX[d] + SW) % SW, ny = (c / SW + DY[d] + SH) % SH;   // no walls: the edges wrap
        uint8_t n = ny * SW + nx;
        if (par_[n] != 0xFF) continue;
        par_[n] = c;
        if (c == h) fallback = d;
        q_[qt++] = n;
        if (n == food_ || n == bonus_) { target = n; found = true; break; }
      }
    }
    if (!found) return fallback;
    uint8_t c = target;
    while (par_[c] != h) c = par_[c];
    for (int d = 0; d < 4; d++) if ((h % SW + DX[d] + SW) % SW == c % SW && (h / SW + DY[d] + SH) % SH == c / SW) return d;
    return cur_;
  }
  void step(Arduino_GFX* g) {
    static const int8_t DX[4] = {0, 1, 0, -1}, DY[4] = {-1, 0, 1, 0};
    cur_ = pend_;
    uint8_t h = headc();
    int nx = (h % SW + DX[cur_] + SW) % SW, ny = (h / SW + DY[cur_] + SH) % SH;   // no walls: the edges wrap
    uint8_t nc = ny * SW + nx;
    bool eat = nc == food_, bonus = nc == bonus_;
    if ((on(nc) && !(!eat && nc == body_[tail_])) || len_ >= SC - 1) {   // only biting itself ends the game
      st_ = OVER; best_ = max(best_, score_); full_ = true; idleAt_ = millis();
      return;
    }
    cell(g, h, C_UGREEN);
    if (eat) len_++;
    else { cell(g, body_[tail_], C_BLACK); tail_ = (tail_ + 1) % SC; }
    body_[(tail_ + len_ - 1) % SC] = nc;
    cell(g, nc, C_WHITE);
    if (eat) { score_++; head(g); spawn(g); }
    if (bonus) {   // more points, no growth
      score_ += 5;
      bonus_ = 0xFF;
      bonusNext_ = millis() + 8000 + random(12000);
      head(g);
    }
  }
};

// ---- video: TCP stream of RGB565 rows from smalltv-video/smalltv_video.py ----------------------------
// Record: 0xA5, y, n, then n rows of 240 little-endian RGB565 pixels. Only changed rows are sent.
class VideoScreen : public Screen {
 public:
  VideoScreen() : Screen("video", MODE_VIDEO) {}
  void arm(bool on) {
    if (on) { armed_ = true; got_ = false; since_ = millis(); appShow(this); return; }
    disarm();
    appHome();
  }
  bool onBack(const Settings&) override { disarm(); return false; }
  bool busy() const override { return armed_; }
 protected:
  void tick(Arduino_GFX* g, bool full) override {
    if (!armed_) {
      if (full) { gfxDrawCentered("VIDEO", 80, 4, C_WHITE); gfxDrawCentered("smalltv_video.py <url>", 140, 1, C_DIM); }
      return;
    }
    if (!srv_) { srv_ = new WiFiServer(8888); srv_->begin(); }
    if (!cl_ || !cl_.connected()) {
      if (cl_) cl_.stop();
      WiFiClient c = srv_->available();
      if (c) { cl_ = c; cl_.setNoDelay(true); g->fillScreen(C_BLACK); got_ = true; left_ = hg_ = rg_ = 0; }
      else if (got_ || millis() - since_ > 30000) { arm(false); }
      return;
    }
    for (int budget = 0; budget < 4096 && cl_.available(); ) {
      if (!left_) {                                   // header: 0xA5, y, n
        int b = cl_.read();
        budget++;
        if (!hg_ && b != 0xA5) continue;
        hdr_[hg_++] = b;
        if (hg_ == 3) { y_ = hdr_[1]; left_ = hdr_[2]; hg_ = rg_ = 0; }
      } else {
        int r = cl_.read((uint8_t*)row_ + rg_, 480 - rg_);
        if (r <= 0) break;
        budget += r; rg_ += r;
        if (rg_ == 480) {
          if (y_ < 240) g->draw16bitRGBBitmap(0, y_, row_, 240, 1);
          y_++; left_--; rg_ = 0;
        }
      }
    }
  }
 private:
  void disarm() {
    armed_ = false; full_ = true;
    if (cl_) cl_.stop();
    if (srv_) { srv_->stop(); delete srv_; srv_ = nullptr; }
  }
  WiFiServer* srv_ = nullptr;
  WiFiClient cl_;
  bool armed_ = false, got_ = false;
  uint32_t since_ = 0;
  uint8_t hdr_[3], hg_ = 0, left_ = 0, y_ = 0;
  uint16_t rg_ = 0;
  uint16_t row_[240];
};

static CpuScreen s_cpu;
static NetScreen s_net;
static WxScreen s_wx;
static SnakeScreen s_snake;
static VideoScreen s_video;
DisplayMode &g_cpuMode = s_cpu, &g_netMode = s_net, &g_wxMode = s_wx, &g_snakeMode = s_snake, &g_videoMode = s_video;
void videoArm(bool on) { s_video.arm(on); }
