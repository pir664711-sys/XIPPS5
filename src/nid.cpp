// nid.cpp - SHA-1 + NID encoding + known-name table.
#include "nid.h"
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {

struct Sha1 {
  uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
  uint8_t  buf[64]; size_t blen = 0; uint64_t total = 0;
  static uint32_t rol(uint32_t v, int s) { return (v << s) | (v >> (32 - s)); }
  void block(const uint8_t* p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i*4] << 24 | (uint32_t)p[i*4+1] << 16 | (uint32_t)p[i*4+2] << 8 | p[i*4+3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
      uint32_t f, k;
      if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999u; }
      else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1u; }
      else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
      else             { f = b ^ c ^ d;                   k = 0xCA62C1D6u; }
      uint32_t t = rol(a, 5) + f + e + k + w[i];
      e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
  }
  void update(const uint8_t* p, size_t n) {
    total += n;
    while (n) {
      size_t k = 64 - blen; if (k > n) k = n;
      memcpy(buf + blen, p, k); blen += k; p += k; n -= k;
      if (blen == 64) { block(buf); blen = 0; }
    }
  }
  void final(uint8_t out[20]) {
    uint64_t bits = total * 8;
    uint8_t pad = 0x80; update(&pad, 1);
    uint8_t z = 0; while (blen != 56) update(&z, 1);
    uint8_t len[8]; for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    update(len, 8);
    for (int i = 0; i < 5; i++) { out[i*4] = h[i] >> 24; out[i*4+1] = h[i] >> 16; out[i*4+2] = h[i] >> 8; out[i*4+3] = (uint8_t)h[i]; }
  }
};

const uint8_t kSalt[16] = {0x51,0x8D,0x64,0xA6,0x35,0xDE,0xD8,0xC1,0xE6,0xB0,0x39,0xB1,0xC3,0xE5,0x52,0x30};

const char* const kNames[] = {
#define X(n) n,
#include "nid_names.inc"
#undef X
};

std::mutex g_mx;
std::unordered_map<std::string, std::string> g_byNid;   // NID -> name
bool g_init = false;

void AddName(const char* n) {
  std::string nid = NidFromName(n);
  g_byNid.emplace(nid, n);
}
void Init() {
  if (g_init) return;
  g_init = true;
  for (const char* n : kNames) AddName(n);
}

} // namespace

std::string NidFromName(const char* name) {
  Sha1 s; s.update((const uint8_t*)name, strlen(name)); s.update(kSalt, 16);
  uint8_t d[20]; s.final(d);
  uint8_t b[8]; for (int i = 0; i < 8; i++) b[i] = d[7 - i];     // first 8 bytes, byte-reversed
  static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
  std::string o;
  for (int i = 0; i < 8; i += 3) {
    uint32_t v = (uint32_t)b[i] << 16 | (i + 1 < 8 ? (uint32_t)b[i+1] << 8 : 0) | (i + 2 < 8 ? b[i+2] : 0);
    int chars = (i + 3 <= 8) ? 4 : 3;                             // last group: 2 bytes -> 3 chars
    for (int k = 0; k < chars; k++) o += A[(v >> (18 - 6 * k)) & 63];
  }
  return o;
}

const char* NidToName(const std::string& nid) {
  std::lock_guard<std::mutex> l(g_mx); Init();
  auto it = g_byNid.find(nid);
  return it == g_byNid.end() ? nullptr : it->second.c_str();
}

void NidLoadExtra(const std::wstring& path) {
  FILE* f = _wfopen(path.c_str(), L"rb");
  if (!f) return;
  std::lock_guard<std::mutex> l(g_mx); Init();
  char line[512];
  while (fgets(line, sizeof line, f)) {
    size_t n = strlen(line);
    while (n && (line[n-1] == '\n' || line[n-1] == '\r' || line[n-1] == ' ' || line[n-1] == '\t')) line[--n] = 0;
    if (n && line[0] != '#' && line[0] != ';') AddName(line);
  }
  fclose(f);
}

size_t NidKnownCount() { std::lock_guard<std::mutex> l(g_mx); Init(); return g_byNid.size(); }
