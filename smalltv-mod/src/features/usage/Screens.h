// Screens.h — extra carousel screens: CPU/RAM graph, network, weather, snake, video.
// Data comes from the PC: history from the 1 s usage push, /api/net, /api/wx; video over TCP.
#pragma once
#include "Mode.h"
#include "UsageData.h"

void screensFeed(const UsageData& u);   // every usage push: extends the CPU/RAM/network history
bool screensNet(const String& body);    // POST /api/net {"c":["ip:port",...]}
bool screensWx(const String& body);     // POST /api/wx
void videoArm(bool on);                 // POST /api/video {"on":bool}

extern DisplayMode &g_cpuMode, &g_netMode, &g_wxMode, &g_snakeMode, &g_videoMode;
