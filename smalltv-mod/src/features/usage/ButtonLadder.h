// ButtonLadder.h — three buttons on one ADC pin (resistor ladder), decoded into events.
//
// No Arduino dependency: the host test (tests/test_buttons.cpp) includes this file as is.
//
// Circuit (see handoff.md): each button connects 3V3 to A0 through its own resistor, A0 has a
// 10k pull-down. Expected readings (0..1023): idle ~0, LEFT ~910 (27k), OK ~590 (47k),
// RIGHT ~300 (100k). Two buttons together read outside every band (saturated or in a gap)
// and are ignored.
#pragma once
#include <stdint.h>

namespace ladder {

enum Button : uint8_t { NONE = 0, LEFT = 1, OK = 2, RIGHT = 3, INVALID = 0xFF };

// Events the dispatcher acts on. BACK is OK held for OK_HOLD_MS.
enum Event : uint8_t { EV_NONE = 0, EV_LEFT, EV_RIGHT, EV_OK, EV_BACK };

constexpr uint16_t IDLE_MAX  = 120;   // below this: nothing pressed
constexpr uint16_t BAND      = 60;    // +- around each nominal reading
constexpr uint16_t NOMINAL_LEFT  = 910;
constexpr uint16_t NOMINAL_OK    = 590;
constexpr uint16_t NOMINAL_RIGHT = 300;
constexpr uint8_t  CONFIRM       = 3;     // consecutive samples that must agree
constexpr uint32_t OK_HOLD_MS    = 1000;  // OK held this long is BACK

inline bool inBand(uint16_t v, uint16_t nominal) {
  return v + BAND >= nominal && v <= nominal + BAND;
}

// One raw reading to a button; INVALID for a value in a gap or above the top band.
inline Button classify(uint16_t adc) {
  if (adc <= IDLE_MAX) return NONE;
  if (inBand(adc, NOMINAL_LEFT)) return LEFT;
  if (inBand(adc, NOMINAL_OK)) return OK;
  if (inBand(adc, NOMINAL_RIGHT)) return RIGHT;
  return INVALID;
}

// Debounced press/release tracking. Feed one reading per call (about every 20 ms).
//   LEFT / RIGHT: event on press.
//   OK: EV_OK on release if held < OK_HOLD_MS; EV_BACK the moment the hold reaches
//       OK_HOLD_MS (the release after that gives nothing).
// An INVALID reading is dropped: it neither confirms nor breaks the state, so the
// transitional values of a rising edge, or two buttons together, do nothing.
class Decoder {
 public:
  Event feed(uint16_t adc, uint32_t nowMs) {
    Button c = classify(adc);
    if (c == INVALID) { run_ = 0; return holdCheck(nowMs); }
    if (c == stable_) { run_ = 0; return holdCheck(nowMs); }
    if (c != cand_) { cand_ = c; run_ = 0; }
    if (++run_ < CONFIRM) return holdCheck(nowMs);
    run_ = 0;
    Button was = stable_;
    stable_ = c;
    if (was == OK) {                       // OK released
      bool fired = backFired_;
      backFired_ = false;
      return fired ? EV_NONE : EV_OK;
    }
    if (c == OK) { downAt_ = nowMs; backFired_ = false; return EV_NONE; }
    if (c == LEFT) return EV_LEFT;
    if (c == RIGHT) return EV_RIGHT;
    return EV_NONE;
  }

  Button held() const { return stable_; }

 private:
  Event holdCheck(uint32_t nowMs) {
    if (stable_ == OK && !backFired_ && nowMs - downAt_ >= OK_HOLD_MS) {
      backFired_ = true;
      return EV_BACK;
    }
    return EV_NONE;
  }

  Button   stable_ = NONE;
  Button   cand_ = NONE;
  uint8_t  run_ = 0;
  uint32_t downAt_ = 0;
  bool     backFired_ = false;
};

}  // namespace ladder
