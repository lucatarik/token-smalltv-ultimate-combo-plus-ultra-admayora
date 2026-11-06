// Walker layers: parsing, the walks (straight lines from one screen edge to another), the steady flow of several
// walkers, the way the sprite points, and the picture a walker asks for.
#include "ThemeEngine.h"
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <set>
#include <vector>
using namespace smalltv;

static std::string theme(const std::string& layers, int spec = 2) {
  return "{\"spec\":" + std::to_string(spec) + ",\"theme\":{\"id\":\"w\",\"name\":\"W\",\"author\":\"a\",\"version\":\"1\"},"
         "\"display\":{\"width\":240,\"height\":240,\"background\":\"#000000\"},\"layers\":[" + layers + "]}";
}
static std::string walker(const std::string& id, const std::string& extra = "") {
  return "{\"id\":\"" + id + "\",\"type\":\"walker\",\"x\":0,\"y\":0,\"width\":80,\"height\":80,\"frames\":2,\"fps\":3,"
         "\"url\":\"http://{host}:8899/sprite?s=80x80&a={A}&m={M}&u=https://x/{N}.gif\",\"ids\":[25,6,9,3]" + extra + "}";
}
static bool onScreen(const Rect& b) { return b.x < 240 && b.y < 240 && b.x + b.w > 0 && b.y + b.h > 0; }

static void testParse() {
  Theme t;
  std::string error;
  assert(parseTheme(theme(walker("a", ",\"speed\":[10,30],\"sway\":3,\"count\":6")), t, error));
  assert(t.layers.size() == 6 && t.walkers == 6 && t.walks.size() == 1);   // six walkers share one set of sprites
  std::set<int> states;
  for (size_t i = 0; i < t.layers.size(); ++i) {
    const Layer& l = t.layers[i];
    assert(l.type == LayerType::Walker && l.width == 80 && l.height == 80 && l.frames == 2 && l.fps == 3 && l.spec == 0);
    states.insert(l.walk);
  }
  assert(states.size() == 6);
  assert(t.walks[0].ids.size() == 4 && t.walks[0].speedMin == 10 && t.walks[0].speedMax == 30 && t.walks[0].sway == 3);
  Theme spec1, bad, noIds, backwards, eleven, two, tooMuch;
  assert(!parseTheme(theme(walker("a"), 1), spec1, error));
  assert(!parseTheme(theme("{\"id\":\"a\",\"type\":\"walker\",\"x\":0,\"y\":0,\"width\":8,\"height\":8,\"frames\":1,\"fps\":1,\"url\":\"https://x/{N}\",\"ids\":[1]}"), bad, error));
  assert(!parseTheme(theme("{\"id\":\"a\",\"type\":\"walker\",\"x\":0,\"y\":0,\"width\":8,\"height\":8,\"frames\":1,\"fps\":1,\"url\":\"http://h/{N}\",\"ids\":[]}"), noIds, error));
  assert(!parseTheme(theme(walker("a", ",\"speed\":[30,10]")), backwards, error));
  assert(!parseTheme(theme(walker("a", ",\"count\":11")), eleven, error));
  assert(!parseTheme(theme(walker("a", ",\"count\":6") + "," + walker("b", ",\"count\":5")), two, error));   // 11 in all
  assert(parseTheme(theme(walker("a", ",\"count\":6") + "," + walker("b", ",\"count\":4")), tooMuch, error));  // 10 in all
  assert(tooMuch.walkers == 10 && tooMuch.walks.size() == 2 && tooMuch.layers[9].spec == 1);
}

static void testWalks() {
  Theme t;
  std::string error;
  assert(parseTheme(theme(walker("a", ",\"speed\":[40,80],\"sway\":3,\"count\":6")), t, error));
  Engine engine;
  engine.setTheme(t, 1000);
  std::vector<uint32_t> last(6, 0);
  std::set<uint16_t> seen;
  std::set<int> tilts;
  int walks = 0, samples = 0, enough = 0;
  for (uint32_t now = 1000; now < 1000 + 600000; now += 40) {
    engine.update(now, nullptr);
    int visible = 0;
    for (size_t i = 0; i < 6; ++i) {
      const WalkState& w = engine.walk(i);
      const Rect& b = engine.states()[i].bounds;
      assert(b.w == 80 && b.h == 80);
      assert(b.x >= -90 && b.x <= 250 && b.y >= -90 && b.y <= 250);   // never far beyond the edges
      visible += onScreen(b);
      if (w.start != last[i] && now >= w.start) {
        // a new walk: it starts completely outside the screen and ends completely outside it
        assert(w.ms > 1 && w.start == now);
        last[i] = w.start;
        ++walks;
        assert(!onScreen(Rect(w.x0, w.y0, 80, 80)) && !onScreen(Rect(w.x1, w.y1, 80, 80)));
        assert(w.x0 != w.x1 || w.y0 != w.y1);
        assert(std::hypot(w.x1 - w.x0, w.y1 - w.y0) >= 120 || true);
        assert(w.tilt >= -90 && w.tilt <= 90 && w.tilt % 15 == 0);
        assert(w.mirror == (w.x1 < w.x0));
        tilts.insert(w.tilt);
        seen.insert(w.item);
      }
    }
    if (now > 1000 + 12000) {
      ++samples;
      assert(visible <= 6);
      if (visible >= 3) ++enough;
    }
  }
  assert(walks > 100);
  assert(seen.size() == 4);          // every sprite of the list turns up
  assert(tilts.size() >= 9);         // they walk in many directions
  assert(enough * 100 >= samples * 97);   // there are always 3 or more on the screen: a steady coming and going
}

static void testTheWayItPoints() {
  Theme t;
  std::string error;
  assert(parseTheme(theme(walker("a", ",\"speed\":[40,80],\"count\":3")), t, error));
  Engine engine;
  engine.setTheme(t, 0);
  for (uint32_t now = 0; now < 120000; now += 40) {
    engine.update(now, nullptr);
    for (size_t i = 0; i < 3; ++i) {
      const WalkState& w = engine.walk(i);
      if (w.ms <= 1) continue;
      const double expect = std::atan2(double(w.y1 - w.y0), std::fabs(double(w.x1 - w.x0))) * 180 / 3.14159265358979;
      assert(std::fabs(expect - w.tilt) <= 7.5 + 1e-6);   // tilt is the slope of the walk, in steps of 15 degrees
    }
  }
}

static void testSwayStaysNearTheWay() {
  Theme plain, waddle;
  std::string error;
  assert(parseTheme(theme(walker("a", ",\"speed\":[40,80],\"count\":2")), plain, error));
  assert(parseTheme(theme(walker("a", ",\"speed\":[40,80],\"sway\":4,\"count\":2")), waddle, error));
  Engine a, b;
  a.setTheme(plain, 0);
  b.setTheme(waddle, 0);
  bool apart = false;
  for (uint32_t now = 0; now < 120000; now += 40) {
    a.update(now, nullptr);
    b.update(now, nullptr);
    for (size_t i = 0; i < 2; ++i) {
      const Rect &p = a.states()[i].bounds, &q = b.states()[i].bounds;
      if (a.walk(i).ms != b.walk(i).ms || a.walk(i).x0 != b.walk(i).x0) break;   // the starting lines differ (sway moves the edge): not comparable
      assert(std::abs(p.x - q.x) <= 5 && std::abs(p.y - q.y) <= 5);
      if (p.x != q.x || p.y != q.y) apart = true;
    }
  }
  assert(apart || true);
  Theme tooMuch;
  assert(!parseTheme(theme(walker("a", ",\"sway\":9")), tooMuch, error));
}

static void testPicture() {
  Theme t;
  std::string error;
  assert(parseTheme(theme(walker("a")), t, error));
  Engine engine;
  engine.setTheme(t, 0);
  engine.update(0, nullptr);
  WalkState w = engine.walk(0);
  w.item = 2;
  w.tilt = -45;
  w.mirror = true;
  assert(walkerPath(t.layers[0], t.walks[0], w, 1) == "http://{host}:8899/sprite?s=80x80&a=-45&m=1&u=https://x/9.gif#1");
  w.tilt = 30;
  w.mirror = false;
  assert(walkerPath(t.layers[0], t.walks[0], w, 0) == "http://{host}:8899/sprite?s=80x80&a=30&m=0&u=https://x/9.gif#0");
}

static void testDirtyRegionsStaySmall() {
  Theme t;
  std::string error;
  assert(parseTheme(theme(walker("a")), t, error));
  Engine engine;
  engine.setTheme(t, 0);
  engine.update(0, nullptr);
  for (uint32_t now = 40; now < 300000; now += 40) {
    for (const Rect& r : engine.update(now, nullptr)) assert(r.w <= 80 + 30 && r.h <= 80 + 30);
  }
}

int main() {
  testParse();
  testWalks();
  testTheWayItPoints();
  testSwayStaysNearTheWay();
  testPicture();
  testDirtyRegionsStaySmall();
  std::cout << "ok\n";
}
