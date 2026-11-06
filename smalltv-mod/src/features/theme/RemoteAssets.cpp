#include "RemoteAssets.h"
#if WITH_THEME
#include "Platform.h"

extern String g_pcHost;   // WebPortal.cpp: address of the PC that last pushed to the clock

using smalltv::AssetHandle;

// Handle: high bit, slot (4 bits), frame (4 bits).
static constexpr AssetHandle Remote = 0x8000;
static const char* const Store = "/rsprites";

uint32_t RemoteAssets::hashOf(const std::string& path) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < path.size() && path[i] != '#'; i++) h = (h ^ (unsigned char)path[i]) * 16777619u;
  return h | 1;   // never 0: 0 marks a slot nobody has asked for
}

// A fetch in progress: the connection stays open between calls of pump().
struct RemoteAssets::Download {
  WiFiClient client;
  HTTPClient http;
  int slot = 0;
  uint32_t got = 0, total = 0, started = 0;
  uint16_t palette[16];
};

RemoteAssets::RemoteAssets(smalltv::Package& package) : package_(package) {}

RemoteAssets::~RemoteAssets() {
  download_.reset();
  store_.close();
  LittleFS.remove(Store);
}

AssetHandle RemoteAssets::resolve(const std::string& path) {
  if (path.compare(0, 7, "http://")) return package_.resolve(path);
  const size_t hash = path.rfind('#');
  unsigned frame = 0;
  if (hash == std::string::npos || sscanf(path.c_str() + hash + 1, "%u", &frame) != 1 || frame > 15) return smalltv::InvalidAsset;
  const uint32_t id = hashOf(path);
  int pick = 0;
  for (int i = 0; i < Slots; i++) {
    if (slot_[i].hash == id) { pick = i; goto found; }
    if (slot_[i].used < slot_[pick].used) pick = i;
  }
  if (download_ && download_->slot == pick) abort(pick);
  slot_[pick].hash = id;                    // the least recently drawn sprite makes room
  slot_[pick].url = path.substr(0, hash).c_str();
  slot_[pick].ok = false;
  slot_[pick].want = true;
  slot_[pick].failedAt = 0;
found:
  slot_[pick].used = millis();
  return Remote | pick | (frame << 4);
}

bool RemoteAssets::row(AssetHandle handle, int y, int x, int count, uint16_t* colors, uint8_t* alpha) {
  if (!(handle & Remote)) return package_.row(handle, y, x, count, colors, alpha);
  const int index = handle & 15;
  Slot& s = slot_[index];
  const unsigned frame = (handle >> 4) & 15;
  for (int i = 0; i < count; i++) { colors[i] = 0; alpha[i] = 0; }
  if (!s.ok || !store_ || y < s.y0 || y >= s.y1 || x < 0 || x >= s.w || frame >= s.frames) return true;
  // Only the columns that have any colour are read; the rest of the row stays transparent.
  const int from = std::max(x, (int)s.x0), to = std::min({x + count, (int)s.w, (int)s.x1});
  if (from >= to) return true;
  const int stride = (s.w + 1) / 2;
  const uint32_t rowAt = (uint32_t)index * SlotBytes + 44 + ((uint32_t)frame * s.h + y) * stride;
  uint8_t raw[24];                          // read through a small window: little loop stack is left here
  for (int at = from; at < to;) {
    const int first = at % 2, part = std::min(to - at, 40), bytes = (first + part + 1) / 2;
    if (!store_.seek(rowAt + at / 2) || store_.read(raw, bytes) != (size_t)bytes) return true;
    for (int i = 0; i < part; i++) {
      const int p = first + i;              // nibble in raw: even ones are the high half of a byte
      const uint8_t v = (p % 2) ? raw[p / 2] & 15 : raw[p / 2] >> 4;
      colors[at - x + i] = s.palette[v];
      alpha[at - x + i] = v ? 255 : 0;
    }
    at += part;
  }
  return true;
}

void RemoteAssets::abort(int index) {
  download_.reset();
  slot_[index].want = true;
}

uint32_t RemoteAssets::pump() {
  if (!download_) {
    for (int i = 0; i < Slots; i++)
      if (slot_[i].want && !slot_[i].ok && (!slot_[i].failedAt || millis() - slot_[i].failedAt > 8000)) {
        if (ESP.getFreeHeap() < 4000 || !g_pcHost.length() || !begin(i)) slot_[i].failedAt = millis();
        break;
      }
    return 0;
  }
  return advance();
}

// Opens the connection and waits for the answer's header (the PC may be making the sprite: a moment).
bool RemoteAssets::begin(int index) {
  Slot& s = slot_[index];
  String url = s.url;
  url.replace("{host}", g_pcHost);
  download_.reset(new Download);
  download_->slot = index;
  download_->started = millis();
  download_->http.setTimeout(3000);
  download_->http.setReuse(false);
  download_->http.useHTTP10(true);          // no chunked framing: the stream is the file
  if (!download_->http.begin(download_->client, url) || download_->http.GET() != HTTP_CODE_OK) {
    download_.reset();
    return false;
  }
  if (!store_) {
    store_ = LittleFS.open(Store, "r+");
    if (!store_) store_ = LittleFS.open(Store, "w+");
    if (!store_) { download_.reset(); return false; }
  }
  return true;
}

// Takes what has arrived so far and stores it; at the end the slot is ready.
uint32_t RemoteAssets::advance() {
  Download& d = *download_;
  Slot& s = slot_[d.slot];
  uint8_t buf[256];
  for (int round = 0; round < 4; round++) {   // a few small reads, then back to the screen
    const int avail = d.client.available();
    if (avail <= 0) {
      if (!d.client.connected() || millis() - d.started > 10000) { download_.reset(); s.failedAt = millis(); }
      return 0;
    }
    const int n = d.client.read(buf, std::min<int>(avail, d.total && d.total - d.got < sizeof(buf) ? d.total - d.got : sizeof(buf)));
    if (n <= 0) return 0;
    if (d.got == 0) {                         // header and palette first: they say how long the sprite is
      const uint16_t w = buf[4] | (buf[5] << 8), h = buf[6] | (buf[7] << 8);
      if (n < 44 || memcmp(buf, "STR2", 4) || !w || w > 240 || !h || h > 240 || !buf[8] || buf[8] > 16 || buf[9] != 1) {
        download_.reset(); s.failedAt = millis(); return 0;
      }
      d.total = 44 + (uint32_t)((w + 1) / 2) * h * buf[8];
      if (d.total > SlotBytes) { download_.reset(); s.failedAt = millis(); return 0; }
      s.w = w; s.h = h; s.frames = buf[8];
      s.x0 = 255; s.x1 = 0; s.y0 = 255; s.y1 = 0;
      for (int i = 0; i < 16; i++) d.palette[i] = buf[12 + 2 * i] | (buf[13 + 2 * i] << 8);
      while (store_.size() < (uint32_t)d.slot * SlotBytes) {   // slots are filled in any order: pad the file up to this one
        uint8_t zeros[64] = {0};
        store_.seek(store_.size());
        store_.write(zeros, std::min<size_t>(sizeof(zeros), (uint32_t)d.slot * SlotBytes - store_.size()));
      }
    }
    {   // where the sprite has colour, over all its frames (the header and palette hold none)
      const int stride = (s.w + 1) / 2;
      for (int i = d.got == 0 ? 44 : 0; i < n; i++) {
        if (!buf[i]) continue;
        const uint32_t pos = d.got + i - 44;
        const int row = (pos / stride) % s.h, col = (pos % stride) * 2;
        s.y0 = std::min<int>(s.y0, row); s.y1 = std::max<int>(s.y1, row + 1);
        s.x0 = std::min<int>(s.x0, col + ((buf[i] >> 4) ? 0 : 1)); s.x1 = std::max<int>(s.x1, col + ((buf[i] & 15) ? 2 : 1));
      }
    }
    if (d.got + n > d.total || !store_.seek((uint32_t)d.slot * SlotBytes + d.got) || store_.write(buf, n) != (size_t)n) {
      download_.reset(); s.failedAt = millis(); return 0;
    }
    d.got += n;
    if (d.got == d.total) {
      store_.flush();
      memcpy(s.palette, d.palette, sizeof(d.palette));
      s.ok = true;
      s.want = false;
      s.url = String();                       // the address is only needed to fetch
      const uint32_t arrived = s.hash;
      download_.reset();
      return arrived;
    }
  }
  return 0;
}
#endif
