// UsageData.h — runtime (volatile) Claude usage snapshot from the daemon.
#pragma once
#include <Arduino.h>

#define HW_UNKNOWN_BPS 0xFFFFFFFFUL

struct UsageData {
  float    sessionPct;       // 5-hour window utilization (0..100)
  int      sessionResetMin;  // minutes until the 5-hour window resets
  float    weeklyPct;        // 7-day window utilization (0..100)
  int      weeklyResetMin;   // minutes until the 7-day window resets
  char     status[16];       // "normal"/"warning"/"rejected" from the usage
                             // endpoint; "allowed"/"allowed_warning"/"rejected"
                             // from older daemons. Both are accepted.

  // Optional computer readings pushed with the usage ({cpu,ram,dk,dn,up,cs}); when present the screen
  // draws the monitor layout. -1 / HW_UNKNOWN_BPS = not reported, drawn as a dash, never as 0.
  bool     hw;               // the daemon sent computer readings
  int8_t   cpuPct, ramPct, diskPct;
  uint32_t netDownBps, netUpBps;  // bytes per second
  char     claudeStatus[8];  // Claude service status: "ok"/"minor"/"major"/"down"/"maint", empty = unknown

  bool     valid;            // populated at least once
  bool     error;            // most recent fetch failed
  uint32_t lastOkMs;         // millis() of last good update

  void clear() {
    sessionPct = weeklyPct = 0;
    sessionResetMin = weeklyResetMin = 0;
    status[0] = 0;
    hw = false;
    cpuPct = ramPct = diskPct = -1;
    netDownBps = netUpBps = HW_UNKNOWN_BPS;
    claudeStatus[0] = 0;
    valid = false;
    error = false;
    lastOkMs = 0;
  }
};
