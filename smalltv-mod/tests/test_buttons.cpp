// c++ -std=c++11 -Wall -Wextra -Werror -g -Isrc/features/usage tests/test_buttons.cpp -o /tmp/smalltv-buttons-tests
// && /tmp/smalltv-buttons-tests
#include "ButtonLadder.h"
#include <cstdio>

using namespace ladder;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// Hold a reading for `ms` at one sample per 20 ms; return the events seen, in order.
struct Run { Event ev[8]; int n = 0; uint32_t t = 0; Decoder d; };
static void hold(Run& r, uint16_t adc, uint32_t ms) {
  for (uint32_t i = 0; i < ms / 20; i++) {
    r.t += 20;
    Event e = r.d.feed(adc, r.t);
    if (e != EV_NONE && r.n < 8) r.ev[r.n++] = e;
  }
}

static void testClassify() {
  CHECK(classify(0) == NONE);
  CHECK(classify(120) == NONE);
  CHECK(classify(300) == RIGHT);
  CHECK(classify(590) == OK);
  CHECK(classify(910) == LEFT);
  CHECK(classify(780) == INVALID);    // 47k and 100k together fall in a gap
  CHECK(classify(1023) == INVALID);   // saturated: 27k with another button
  CHECK(classify(200) == INVALID);    // gap between idle and RIGHT
  CHECK(classify(240) == RIGHT);      // lower edge of the band
  CHECK(classify(970) == LEFT);       // upper edge
  CHECK(classify(971) == INVALID);
}

static void testNavigation() {
  Run r;
  hold(r, 0, 200);
  hold(r, 910, 200);   // LEFT press: event at once
  hold(r, 0, 100);
  hold(r, 300, 200);   // RIGHT press
  hold(r, 0, 100);
  CHECK(r.n == 2);
  CHECK(r.ev[0] == EV_LEFT);
  CHECK(r.ev[1] == EV_RIGHT);
}

static void testOkShortAndLong() {
  Run r;
  hold(r, 590, 400);   // short: event on release
  CHECK(r.n == 0);
  hold(r, 0, 100);
  CHECK(r.n == 1 && r.ev[0] == EV_OK);

  Run l;
  hold(l, 590, 1200);  // long: BACK while still held, nothing on release
  CHECK(l.n == 1 && l.ev[0] == EV_BACK);
  hold(l, 0, 200);
  CHECK(l.n == 1);
}

static void testNoiseAndChords() {
  Run r;
  hold(r, 780, 400);   // two buttons together: nothing
  hold(r, 1023, 400);
  CHECK(r.n == 0);

  // a one-sample glitch is not a press
  Run g;
  g.d.feed(910, 20);
  g.d.feed(0, 40);
  g.d.feed(910, 60);
  hold(g, 0, 100);
  CHECK(g.n == 0);

  // rising edge through gap values, then LEFT: one event
  Run e;
  hold(e, 200, 40);
  hold(e, 780, 40);
  hold(e, 910, 200);
  CHECK(e.n == 1 && e.ev[0] == EV_LEFT);

  // a chord while OK is held does not break the hold or fire anything extra
  Run c;
  hold(c, 590, 300);
  hold(c, 780, 200);
  hold(c, 590, 300);
  hold(c, 0, 100);
  CHECK(c.n == 1 && c.ev[0] == EV_OK);
}

int main() {
  testClassify();
  testNavigation();
  testOkShortAndLong();
  testNoiseAndChords();
  if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
  std::printf("ok\n");
  return 0;
}
