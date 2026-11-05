#pragma once

#include "prune/_config.hpp"

#include <je2be/status.hpp>

#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

namespace je2be::prune {

inline int64_t FloorDiv(int64_t a, int64_t b) {
  int64_t q = a / b;
  int64_t r = a % b;
  if (r != 0 && ((r < 0) != (b < 0))) {
    q--;
  }
  return q;
}

struct ChunkRect {
  int32_t fMinCx = 0;
  int32_t fMaxCx = 0;
  int32_t fMinCz = 0;
  int32_t fMaxCz = 0;

  bool contains(int32_t cx, int32_t cz) const {
    return fMinCx <= cx && cx <= fMaxCx && fMinCz <= cz && cz <= fMaxCz;
  }
};

// Computes the chunk rectangle covered by a map whose display area is a
// 128 * 2^scale block square centered on (centerX, centerZ).
inline Status MapRectFromCenter(int64_t centerX, int64_t centerZ, int scale, ChunkRect &out) {
  if (scale < 0 || 4 < scale) {
    return JE2BE_ERROR_WHAT("map scale " + std::to_string(scale) + " is outside the supported range 0..4");
  }
  int64_t const length = static_cast<int64_t>(128) << scale;
  int64_t const minBlockX = centerX - length / 2;
  int64_t const maxBlockX = centerX + length / 2 - 1;
  int64_t const minBlockZ = centerZ - length / 2;
  int64_t const maxBlockZ = centerZ + length / 2 - 1;
  int64_t const minCx = FloorDiv(minBlockX, 16);
  int64_t const maxCx = FloorDiv(maxBlockX, 16);
  int64_t const minCz = FloorDiv(minBlockZ, 16);
  int64_t const maxCz = FloorDiv(maxBlockZ, 16);
  constexpr int64_t kMin = std::numeric_limits<int32_t>::min();
  constexpr int64_t kMax = std::numeric_limits<int32_t>::max();
  if (minCx < kMin || kMax < maxCx || minCz < kMin || kMax < maxCz) {
    return JE2BE_ERROR_WHAT("map display range overflows chunk coordinates");
  }
  out.fMinCx = static_cast<int32_t>(minCx);
  out.fMaxCx = static_cast<int32_t>(maxCx);
  out.fMinCz = static_cast<int32_t>(minCz);
  out.fMaxCz = static_cast<int32_t>(maxCz);
  return Status::Ok();
}

// Spatial index of protected map rectangles. Rectangles are inserted into
// every 32x32-chunk (region sized) cell they overlap so a query only tests
// the few maps that can actually contain the chunk.
class MapProtection {
public:
  void add(ChunkRect const &rect) {
    int64_t const minRx = FloorDiv(rect.fMinCx, 32);
    int64_t const maxRx = FloorDiv(rect.fMaxCx, 32);
    int64_t const minRz = FloorDiv(rect.fMinCz, 32);
    int64_t const maxRz = FloorDiv(rect.fMaxCz, 32);
    for (int64_t rz = minRz; rz <= maxRz; rz++) {
      for (int64_t rx = minRx; rx <= maxRx; rx++) {
        fCells[CellKey(static_cast<int32_t>(rx), static_cast<int32_t>(rz))].push_back(rect);
      }
    }
    fSize++;
  }

  bool protects(int32_t cx, int32_t cz) const {
    int64_t const rx = FloorDiv(cx, 32);
    int64_t const rz = FloorDiv(cz, 32);
    auto found = fCells.find(CellKey(static_cast<int32_t>(rx), static_cast<int32_t>(rz)));
    if (found == fCells.end()) {
      return false;
    }
    for (ChunkRect const &rect : found->second) {
      if (rect.contains(cx, cz)) {
        return true;
      }
    }
    return false;
  }

  size_t size() const { return fSize; }
  bool empty() const { return fSize == 0; }

private:
  static uint64_t CellKey(int32_t rx, int32_t rz) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(rx)) << 32) | static_cast<uint32_t>(rz);
  }

  std::unordered_map<uint64_t, std::vector<ChunkRect>> fCells;
  size_t fSize = 0;
};

struct Selection {
  Config const *fConfig = nullptr;
  Action fFallback = Action::Keep;
  MapProtection const *fMaps = nullptr;

  // Deletion precedence:
  //   1. the last matching config region (explicit keep or delete),
  //   2. stored map display protection (keep),
  //   3. the CLI --default value.
  Action resolve(int32_t cx, int32_t cz) const {
    if (fConfig) {
      if (auto action = fConfig->lastMatch(cx, cz)) {
        return *action;
      }
    }
    if (fMaps && fMaps->protects(cx, cz)) {
      return Action::Keep;
    }
    return fFallback;
  }
};

} // namespace je2be::prune
