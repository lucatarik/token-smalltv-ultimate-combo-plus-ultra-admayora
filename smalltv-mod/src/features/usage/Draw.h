// Draw.h — flicker-free text and bar helpers shared by the usage monitor and the extra screens.
//
// No frame buffer (the ESP8266 has about 9 KB of heap): an element remembers what it shows and is
// redrawn only when that changes; text is composed pixel by pixel with its background.
#pragma once
#include <Arduino_GFX_Library.h>
#include "Gfx.h"
#include "font/glcdfont.h"   // the 5x7 font Arduino_GFX itself draws text with

// Claude-usage palette (Anthropic-inspired dark theme, RGB565 of the originals).
// Through gfxTint() like the shared palette, so the Display tab's colour
// correction reaches these too.
#define C_ACCENT  gfxTint(0xDBAA)   // terra-cotta 0xd97757
#define C_UGREEN  gfxTint(0x7C6B)   // green 0x788c5d
#define C_PANEL   gfxTint(0x18E3)   // card fill 0x1f1f1e
#define C_BARBG   gfxTint(0x2945)   // unfilled bar track 0x2a2a28
#define C_DIM     gfxTint(0xB574)   // secondary text 0xb0aea5

// What an element shows now, so an unchanged one is left alone.
struct TextState { char s[16]; uint16_t fg; bool drawn; };
struct BarState  { int16_t fw; uint16_t col; bool drawn; };

// `text` in a box of `cells` characters of size `size` at (x,y), padded so a shorter value covers the longer one,
// in one pass: one pixel row of the box at a time, each row final (text and background together).
static inline void textBlock(Arduino_GFX* gfx, TextState& st, bool force, int x, int y, uint8_t size, int cells,
                      const char* text, uint16_t fg, uint16_t bg, bool right = false) {
  if (!force && st.drawn && st.fg == fg && !strncmp(st.s, text, sizeof(st.s))) return;
  strlcpy(st.s, text, sizeof(st.s));
  st.fg = fg; st.drawn = true;
  char pad[24];
  int len = strlen(text);
  if (cells >= (int)sizeof(pad)) cells = sizeof(pad) - 1;
  if (len > cells) len = cells;
  memset(pad, ' ', cells);
  memcpy(pad + (right ? cells - len : 0), text, len);
  uint16_t cell[6 * 3];                                    // one character cell of one pixel row, size <= 3
  const int cw = 6 * size;
  for (int gy = 0; gy < 8; gy++) {
    for (int rep = 0; rep < size; rep++) {
      int py = y + gy * size + rep;
      for (int c = 0; c < cells; c++) {
        uint8_t ch = (uint8_t)pad[c];
        for (int gx = 0; gx < 6; gx++) {
          bool on = gx < 5 && gy < 7 && ((pgm_read_byte(&font[ch * 5 + gx]) >> gy) & 1);
          for (int k = 0; k < size; k++) cell[gx * size + k] = on ? fg : bg;
        }
        gfx->draw16bitRGBBitmap(x + c * cw, py, cell, cw, 1);
      }
    }
  }
}

// A bar of `pct` %. Same colour: only the part that changed is painted. A new colour or a full repaint paints
// the fill and the track side by side, each once, so the track is never drawn under the fill.
static inline void barDelta(Arduino_GFX* gfx, BarState& st, bool force, int x, int y, int w, int h, int pct,
                     uint16_t fill) {
  int fw = pct < 0 ? 0 : w * constrain(pct, 0, 100) / 100;
  if (force || !st.drawn || st.col != fill) {
    if (fw > 0) gfx->fillRect(x, y, fw, h, fill);
    if (fw < w) gfx->fillRect(x + fw, y, w - fw, h, C_BARBG);
  } else if (fw > st.fw) {
    gfx->fillRect(x + st.fw, y, fw - st.fw, h, fill);
  } else if (fw < st.fw) {
    gfx->fillRect(x + fw, y, st.fw - fw, h, C_BARBG);
  }
  st.fw = fw; st.col = fill; st.drawn = true;
}


static inline void speedText(uint32_t bps, char* out, size_t n) {
  if (bps == 0xFFFFFFFFUL)  strlcpy(out, "--", n);
  else if (bps >= 10u << 20)  snprintf(out, n, "%luM", (unsigned long)(bps >> 20));
  else if (bps >= 1u << 20)   snprintf(out, n, "%lu.%luM", (unsigned long)(bps >> 20), (unsigned long)((bps * 10 >> 20) % 10));
  else if (bps >= 10u << 10)  snprintf(out, n, "%luK", (unsigned long)(bps >> 10));
  else if (bps >= 1u << 10)   snprintf(out, n, "%lu.%luK", (unsigned long)(bps >> 10), (unsigned long)((bps * 10 >> 10) % 10));
  else                        snprintf(out, n, "%luB", (unsigned long)bps);
}
