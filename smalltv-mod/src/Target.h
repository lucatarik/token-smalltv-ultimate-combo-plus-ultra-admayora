// Target.h — chip selection, checked once.
//
// Each env in platformio.ini names its chip with exactly one of:
//   -D SMALLTV_ESP8266   original SmallTV, ESP-12F
//   -D SMALLTV_ESP32C2   knockoff SmallTV, ESP32-C2 / ESP8684
//   -D SMALLTV_ESP32     classic ESP32 (NM-TV-154, SmallTV Pro)
// The board and variant macros (SMALLTV_ESP32_PRO, SMALLTV_ESP32_WG,
// SMALLTV_LEAN) are defined alongside a chip macro and never replace it.
//
// The rest of the firmware tests the family macros defined here instead of
// listing chip macros:
//   SMALLTV_FAMILY_ESP32     C2 and classic ESP32 (Arduino core 3.x, mbedTLS)
//   SMALLTV_FAMILY_ESP8266   ESP8266 (Arduino core 2.x, BearSSL)
// Exactly one of the two is defined. A build with no chip macro, or two, stops
// here instead of silently taking the ESP8266 branch of Platform.h.
//
// The loader env (src/loader.cpp, LOADER_BUILD) does not include this header.
#pragma once

#if (defined(SMALLTV_ESP8266) + defined(SMALLTV_ESP32C2) + defined(SMALLTV_ESP32)) != 1
#error "Define exactly one of SMALLTV_ESP8266, SMALLTV_ESP32C2, SMALLTV_ESP32 (see platformio.ini)"
#endif

#if defined(SMALLTV_ESP32C2) || defined(SMALLTV_ESP32)
#define SMALLTV_FAMILY_ESP32 1
#else
#define SMALLTV_FAMILY_ESP8266 1
#endif
