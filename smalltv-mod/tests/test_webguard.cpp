// c++ -std=c++11 -Wall -Wextra -Werror -g -Isrc tests/test_webguard.cpp -o /tmp/smalltv-webguard-tests
// && /tmp/smalltv-webguard-tests
#include "WebGuard.h"
#include <cstdio>
#include <cstdlib>

using namespace webguard;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void testHost() {
  const char* hn = "smalltv";
  // No Host header (HTTP/1.0)
  CHECK(hostAllowed(nullptr, hn));
  CHECK(hostAllowed("", hn));
  // IPv4 literals, with and without a port
  CHECK(hostAllowed("192.168.1.50", hn));
  CHECK(hostAllowed("192.168.4.1", hn));
  CHECK(hostAllowed("10.0.0.2:80", hn));
  CHECK(hostAllowed("192.168.1.50:8080", hn));
  CHECK(!hostAllowed("192.168.1.256", hn));
  CHECK(!hostAllowed("192.168.1", hn));
  CHECK(!hostAllowed("192.168.1.1.1", hn));
  CHECK(hostAllowed("192.168.1.1.", hn));   // one trailing dot is the same name
  CHECK(!hostAllowed("1.2.3.4:", hn));
  CHECK(!hostAllowed("1.2.3.4:80x", hn));
  CHECK(!hostAllowed("1.2.3.4:123456", hn));
  // The device's own name
  CHECK(hostAllowed("smalltv", hn));
  CHECK(hostAllowed("smalltv:80", hn));
  CHECK(hostAllowed("smalltv.local", hn));
  CHECK(hostAllowed("SmallTV.LOCAL", hn));
  CHECK(hostAllowed("smalltv.local.", hn));
  CHECK(hostAllowed("smalltv.lan", hn));
  CHECK(hostAllowed("smalltv.home", hn));
  CHECK(hostAllowed("smalltv.home.arpa", hn));
  CHECK(hostAllowed("smalltv.internal", hn));
  CHECK(hostAllowed("smalltv.localdomain", hn));
  CHECK(hostAllowed("smalltv.fritz.box", hn));
  CHECK(hostAllowed("smalltv.fritz.box:80", hn));
  CHECK(hostAllowed("smalltv-desk.local", "smalltv-desk"));
  CHECK(hostAllowed("smalltv-a1b2", "smalltv-a1b2"));
  // Rebinding candidates
  CHECK(!hostAllowed("smalltv.attacker.com", hn));
  CHECK(!hostAllowed("smalltv.local.attacker.com", hn));
  CHECK(!hostAllowed("smalltv.lan.evil.example", hn));
  CHECK(!hostAllowed("smalltv.com", hn));
  CHECK(!hostAllowed("attacker.com", hn));
  CHECK(!hostAllowed("evilsmalltv.local", hn));
  CHECK(!hostAllowed("smalltvx.local", hn));
  CHECK(!hostAllowed("smalltv.localx", hn));
  CHECK(hostAllowed("smalltv.", "smalltv"));   // bare name with a trailing dot
  CHECK(!hostAllowed("localhost", hn));
  CHECK(!hostAllowed("smalltv.local:", hn));
  CHECK(!hostAllowed(".", hn));
  CHECK(!hostAllowed(":80", hn));
  CHECK(!hostAllowed("attacker.com", ""));
  CHECK(!hostAllowed("attacker.com", nullptr));
  CHECK(hostAllowed("192.168.1.50", ""));
}

static void testOrigin() {
  const char* h = "smalltv.local";
  // Not a browser: neither header
  CHECK(originAllowed(nullptr, nullptr, h));
  CHECK(originAllowed("", "", h));
  CHECK(originAllowed(nullptr, nullptr, nullptr));
  // The web UI itself
  CHECK(originAllowed("http://smalltv.local", nullptr, h));
  CHECK(originAllowed("http://smalltv.local", "same-origin", h));
  CHECK(originAllowed("http://SmallTV.Local", nullptr, h));
  CHECK(originAllowed("http://192.168.1.50", "same-origin", "192.168.1.50"));
  CHECK(originAllowed("http://192.168.1.50:8080", nullptr, "192.168.1.50:8080"));
  // Address-bar navigation / bookmarks
  CHECK(originAllowed(nullptr, "none", h));
  // Cross-site
  CHECK(!originAllowed("http://attacker.com", nullptr, h));
  CHECK(!originAllowed("http://attacker.com", "cross-site", h));
  CHECK(!originAllowed(nullptr, "cross-site", h));
  CHECK(!originAllowed(nullptr, "same-site", h));
  CHECK(!originAllowed("http://smalltv.local", "cross-site", h));
  CHECK(!originAllowed("null", nullptr, h));
  CHECK(!originAllowed("null", "same-origin", h));
  // Port must match
  CHECK(!originAllowed("http://smalltv.local:8080", nullptr, h));
  CHECK(!originAllowed("http://smalltv.local", nullptr, "smalltv.local:8080"));
  // Scheme must be http, host must match whole
  CHECK(!originAllowed("https://smalltv.local", nullptr, h));
  CHECK(!originAllowed("http://smalltv.local.attacker.com", nullptr, h));
  CHECK(!originAllowed("http://smalltv.localx", nullptr, h));
  CHECK(!originAllowed("http://", nullptr, h));
  CHECK(!originAllowed("http://smalltv.local/", nullptr, h));
  // Origin but no Host to compare against
  CHECK(!originAllowed("http://smalltv.local", nullptr, nullptr));
  CHECK(!originAllowed("http://smalltv.local", nullptr, ""));
}

int main() {
  testHost();
  testOrigin();
  if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
  std::printf("webguard: all tests passed\n");
  return 0;
}
