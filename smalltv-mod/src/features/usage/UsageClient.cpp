#include "UsageClient.h"
#include "Platform.h"
#include "Screens.h"
#include <ArduinoJson.h>
#include <math.h>

static UsageData g_usage;
static uint32_t  g_nextPollMs = 0;
static bool      g_inited = false;

// ---------------------------------------------------------------------------
void usageInit(const Settings& s) {
  (void)s;
  g_usage.clear();
  g_nextPollMs = millis();
  g_inited = true;
}

void usageForceRefresh() { g_nextPollMs = millis(); }

const UsageData& usageGet() { return g_usage; }

bool usageFresh(uint32_t withinMs) {
  return g_usage.valid && (millis() - g_usage.lastOkMs) <= withinMs;
}

// ---- parse: usage contract -------------------------------------------------
// { "s":29, "sr":142, "w":4, "wr":9876, "st":"allowed", "ok":true }
// optional computer readings: "cpu","ram","dk" = percent, "dn","up" = bytes/s, "cs" = Claude service status
//   s  = 5h utilization %        sr = minutes until 5h reset
//   w  = 7d utilization %        wr = minutes until 7d reset
//   st = rate-limit status       ok = false => explicit "no data"
static void usageFilter(JsonDocument& f) {
  f["s"] = true; f["sr"] = true; f["w"] = true;
  f["wr"] = true; f["st"] = true; f["ok"] = true;
  f["cpu"] = true; f["ram"] = true; f["dk"] = true; f["dn"] = true; f["up"] = true; f["cs"] = true;
}

static bool applyUsageDoc(UsageData& d, JsonDocument& doc) {
  if (doc["ok"].is<bool>() && doc["ok"].as<bool>() == false) return false;
  if (!doc["s"].is<float>() && !doc["s"].is<int>()) return false;   // require at least session %

  d.sessionPct      = constrain(doc["s"].as<float>(), 0.0f, 100.0f);
  d.weeklyPct       = constrain(doc["w"] | 0.0f, 0.0f, 100.0f);
  d.sessionResetMin = doc["sr"] | 0;
  d.weeklyResetMin  = doc["wr"] | 0;
  strlcpy(d.status, doc["st"] | "", sizeof(d.status));

  // Computer readings: a field the daemon could not read is left out, and stays unknown here.
  d.cpuPct  = doc["cpu"].is<int>() ? (int8_t)constrain(doc["cpu"].as<int>(), 0, 100) : -1;
  d.ramPct  = doc["ram"].is<int>() ? (int8_t)constrain(doc["ram"].as<int>(), 0, 100) : -1;
  d.diskPct = doc["dk"].is<int>()  ? (int8_t)constrain(doc["dk"].as<int>(), 0, 100)  : -1;
  d.netDownBps = doc["dn"].is<uint32_t>() ? doc["dn"].as<uint32_t>() : HW_UNKNOWN_BPS;
  d.netUpBps   = doc["up"].is<uint32_t>() ? doc["up"].as<uint32_t>() : HW_UNKNOWN_BPS;
  strlcpy(d.claudeStatus, doc["cs"] | "", sizeof(d.claudeStatus));
  d.hw = d.cpuPct >= 0 || d.ramPct >= 0 || d.diskPct >= 0 || d.netDownBps != HW_UNKNOWN_BPS;

  d.valid = true;
  d.error = false;
  d.lastOkMs = millis();
  screensFeed(d);
  return true;
}

static bool parseUsage(UsageData& d, Stream& stream) {
  JsonDocument filter; usageFilter(filter);
  JsonDocument doc;
  if (deserializeJson(doc, stream, DeserializationOption::Filter(filter))) return false;
  return applyUsageDoc(d, doc);
}

// Pushed payload (POST /api/usage): same contract, parsed from a String body.
bool usageApply(const String& body) {
  JsonDocument filter; usageFilter(filter);
  JsonDocument doc;
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) return false;
  return applyUsageDoc(g_usage, doc);
}

// ---- one HTTP(S) GET + parse (mirrors StockClient::fetchUrl) ----------------
static bool fetchUsage(const Settings& s) {
  const String& url = s.usage.usageUrl;
  if (url.length() < 8) return false;
  bool https = url.startsWith("https://");

  std::unique_ptr<NetClient> client;
  if (https) {
    if (ESP.getFreeHeap() < 20000) return false;   // too little heap for TLS (incl. the 9 KB thunk); skip, don't crash
    client.reset(platformMakeSecureClient(2048));   // LAN / self-hosted endpoint
  } else {
    client.reset(new WiFiClient());
  }

  HTTPClient http;
  http.setTimeout(s.httpTimeout);
  http.setReuse(false);
  // HTTP/1.0 so the server can't reply with chunked framing: parseUsage reads the
  // raw stream via getStream(), which neither core de-chunks (same as StockClient).
  http.useHTTP10(true);
  if (!http.begin(*client, url)) return false;
  http.addHeader("Accept", "application/json");

  int code = http.GET();
  if (code != HTTP_CODE_OK) { http.end(); return false; }

  bool ok = parseUsage(g_usage, http.getStream());
  http.end();
  return ok;
}

// ---------------------------------------------------------------------------
void usageService(const Settings& s) {
  if (!g_inited) usageInit(s);
  if ((int32_t)(millis() - g_nextPollMs) < 0) return;

  if (!fetchUsage(s)) g_usage.error = true;   // keep stale data, flag the error

  g_nextPollMs = millis() + (uint32_t)s.usage.pollSec * 1000UL;
}
