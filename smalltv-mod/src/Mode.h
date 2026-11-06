// Mode.h — the display-mode interface.
//
// Each feature (ticker / usage / radar) is a self-contained DisplayMode: it owns
// its own data fetch, render, dirty-tracking and settings slice. main.cpp keeps a
// registry of the compiled-in modes and dispatches to whichever one matches the
// active settings.mode — it holds no per-feature state of its own.
#pragma once
#include <Arduino.h>
#include "Settings.h"

class DisplayMode {
 public:
  virtual ~DisplayMode() {}

  // Stable string id (also the settings.mode token, e.g. "stocks"/"usage"/"radar").
  virtual const char* id() const = 0;
  // The MODE_* constant this mode answers to (matched against settings.mode).
  virtual uint8_t modeConst() const = 0;

  virtual void begin(const Settings& s) {}          // one-time init at boot
  virtual void service(const Settings& s) {}        // every loop tick: fetch + render
  virtual void invalidate(const Settings& s) {}     // settings changed: re-init + repaint
  // Another mode drew on the screen (carousel switch): repaint from cached data,
  // do NOT refetch. Falls back to invalidate for modes without a light path.
  virtual void wake(const Settings& s) { invalidate(s); }
  // Centre button, short press: confirm the screen's action. Return true when handled.
  virtual bool onOk(const Settings& s) { return false; }
  // Centre button held 1 s: cancel what onOk started. Return false to let main.cpp go home.
  virtual bool onBack(const Settings& s) { return false; }
  // True while the screen must not be rotated away (a game in play, a video streaming).
  virtual bool busy() const { return false; }
  // The carousel moves on to another screen: free what this one holds that wake() can rebuild (the theme clock's
  // package, engine and sprites), so the screen in view has the heap. Nothing runs for a screen out of view anyway.
  virtual void leave() {}
  // Left/right button (-1/+1): return true when the screen uses it, else the carousel moves on.
  virtual bool onNav(const Settings& s, int8_t dir) { return false; }
  // Free what the next service() call can rebuild, so a web request that
  // needs a lot of heap gets it. Modes that hold little keep the default.
  virtual void freeForRequest() {}
};
