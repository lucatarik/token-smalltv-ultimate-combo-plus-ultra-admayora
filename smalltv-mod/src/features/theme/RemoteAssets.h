// RemoteAssets.h — the pictures of walker layers: sprites fetched over HTTP from the PC's smalltv-monitord.
//
// The theme engine asks for "http://host/...#frame". The part before '#' is fetched into one slot of a single
// storage file (the PC sends a ready-made blob, already turned the way the sprite walks, see
// smalltv-daemon/src/sprites.rs); the frame picks the picture inside it. One slot per walker, the least recently
// drawn is replaced. The fetch runs a little on every call of pump(), never blocking the screen for long.
//
// Blob "STR2": "STR2", u16 width, u16 height, u8 frames, u8 1, u16 zero; 16 RGB565 colours (little-endian,
// index 0 is transparent); then for every frame the rows of 4-bit pixels, two to a byte (the left one in the
// high half), every row padded to a whole byte. A sprite of 80 x 80 with 2 frames is 6.5 KB.
#pragma once
#include "config.h"
#if WITH_THEME
#include "ThemePackage.h"
#include <LittleFS.h>
#include <memory>

class HTTPClient;
class WiFiClient;

class RemoteAssets : public smalltv::Assets {
 public:
  explicit RemoteAssets(smalltv::Package& package);   // in the .cpp: Download is incomplete here
  ~RemoteAssets();   // gives the storage back: the sprites are fetched again when the theme comes back
  smalltv::AssetHandle resolve(const std::string& path) override;
  // Asks for a sprite ahead of its first drawing (a walker still outside the screen), so it is there when it comes in.
  void want(const std::string& path) { resolve(path); }
  using smalltv::Assets::row;
  // Never fails: a sprite that is not here (yet) is drawn transparent.
  bool row(smalltv::AssetHandle handle, int y, int x, int count, uint16_t* colors, uint8_t* alpha) override;
  // Moves the fetch of a wanted sprite on a little. Returns the hash (see hashOf) of the sprite that has just arrived, else 0.
  uint32_t pump();
  // Identifies the sprite a path asks for (the address without its '#frame').
  static uint32_t hashOf(const std::string& path);
 private:
  static constexpr int Slots = 10;
  static constexpr uint32_t SlotBytes = 8192;
  struct Slot {
    uint32_t hash = 0;   // of the address; the address itself is kept only while the sprite is wanted
    String url;
    bool ok = false, want = false;
    uint16_t w = 0, h = 0, palette[16];
    uint8_t frames = 0;
    uint8_t x0 = 0, x1 = 0, y0 = 0, y1 = 0;   // where the sprite has any colour at all: nothing else is read from the file
    uint32_t used = 0, failedAt = 0;
  };
  struct Download;
  bool begin(int index);
  uint32_t advance();
  void abort(int index);
  smalltv::Package& package_;
  Slot slot_[Slots];
  File store_;                        // all the sprites, SlotBytes each
  std::unique_ptr<Download> download_;
};
#endif
