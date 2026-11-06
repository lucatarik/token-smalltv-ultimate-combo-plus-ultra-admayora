// WebGuard.h — request checks for the web UI: CSRF (Origin / Sec-Fetch-Site on
// state-changing requests) and DNS rebinding (Host on every request).
//
// Pure functions on C strings, no Arduino types, so tests/test_webguard.cpp
// builds them on the host. WebPortal.cpp reads the headers and calls these.
//
// Nothing here asks the caller for a custom header: Home Assistant, n8n, the
// clawdmeter daemon and curl send neither Origin nor Sec-Fetch-Site and keep
// working as they did. Only a browser attaches those, and a page on another
// site cannot forge or drop them.
#pragma once
#include <stddef.h>
#include <string.h>

namespace webguard {

// Name suffixes a home router hands out for <hostname>. A list rather than
// "any suffix": a check that accepted smalltv.<anything> would pass
// smalltv.attacker.com, which an attacker can point at the device's LAN
// address, and rebinding would be back.
static const char* const kHostSuffixes[] = {
  ".local", ".lan", ".home", ".home.arpa", ".internal", ".localdomain", ".fritz.box",
};

static inline char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

// Case-insensitive compare of a[0..alen) with the C string b.
static inline bool equalsNoCase(const char* a, size_t alen, const char* b) {
  if (strlen(b) != alen) return false;
  for (size_t i = 0; i < alen; i++) if (lower(a[i]) != lower(b[i])) return false;
  return true;
}

// "a.b.c.d" with each part 0..255, nothing else.
static inline bool isIPv4(const char* s, size_t n) {
  int parts = 0;
  size_t i = 0;
  while (i < n) {
    size_t start = i;
    int v = 0;
    while (i < n && s[i] >= '0' && s[i] <= '9') {
      v = v * 10 + (s[i] - '0');
      if (v > 255) return false;
      i++;
    }
    if (i == start || i - start > 3) return false;
    parts++;
    if (i == n) break;
    if (s[i] != '.') return false;
    i++;
    if (i == n) return false;   // trailing dot after the last part
  }
  return parts == 4;
}

// Host header value: a name or IPv4 literal, optionally ":port". `hostname` is
// the device's configured name (no domain). A missing or empty header is
// accepted: HTTP/1.0 clients need not send one, and browsers always do.
static inline bool hostAllowed(const char* host, const char* hostname) {
  if (!host || !*host) return true;
  size_t n = strlen(host);
  size_t colon = n;
  for (size_t i = 0; i < n; i++) if (host[i] == ':') { colon = i; break; }
  if (colon < n) {                       // port: 1..5 digits
    size_t p = n - colon - 1;
    if (p == 0 || p > 5) return false;
    for (size_t i = colon + 1; i < n; i++) if (host[i] < '0' || host[i] > '9') return false;
  }
  n = colon;
  if (n && host[n - 1] == '.') n--;      // "smalltv.local." is the same name
  if (n == 0) return false;
  if (isIPv4(host, n)) return true;
  if (!hostname || !*hostname) return false;
  size_t hl = strlen(hostname);
  if (n < hl) return false;
  for (size_t i = 0; i < hl; i++) if (lower(host[i]) != lower(hostname[i])) return false;
  if (n == hl) return true;              // bare <hostname>
  for (size_t i = 0; i < sizeof(kHostSuffixes) / sizeof(kHostSuffixes[0]); i++) {
    const char* sfx = kHostSuffixes[i];
    if (equalsNoCase(host + hl, n - hl, sfx)) return true;
  }
  return false;
}

// Origin / Sec-Fetch-Site of a state-changing request (POST, PUT, DELETE, ...).
// Null or empty means the header was absent. With neither present the caller
// is not a browser and is let through. With either present the request must
// have come from this very page: Origin is exactly http://<Host> (the web UI
// is plain HTTP; "null" and other schemes fail), and Sec-Fetch-Site, if sent,
// is same-origin or none.
static inline bool originAllowed(const char* origin, const char* secFetchSite, const char* host) {
  if (secFetchSite && *secFetchSite &&
      !equalsNoCase(secFetchSite, strlen(secFetchSite), "same-origin") &&
      !equalsNoCase(secFetchSite, strlen(secFetchSite), "none")) return false;
  if (!origin || !*origin) return true;
  if (!host || !*host) return false;
  static const char scheme[] = "http://";
  size_t sl = sizeof(scheme) - 1;
  size_t ol = strlen(origin);
  if (ol <= sl || !equalsNoCase(origin, sl, scheme)) return false;
  return equalsNoCase(origin + sl, ol - sl, host);
}

}  // namespace webguard
