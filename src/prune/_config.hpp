#pragma once

#include <je2be/pos2.hpp>
#include <je2be/status.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace je2be::prune {

enum class Action {
  Keep,
  Delete,
};

// Canonical dimension id used by the selection file:
//   -1 = nether, 0 = overworld, 1 = end.
// Bedrock stores these as 1, 0 and 2 respectively.
enum class CanonicalDimension : int {
  Nether = -1,
  Overworld = 0,
  End = 1,
};

struct ChunkRegion {
  Action fAction = Action::Keep;
  int32_t fMinCx = 0;
  int32_t fMaxCx = 0;
  int32_t fMinCz = 0;
  int32_t fMaxCz = 0;

  bool contains(int32_t cx, int32_t cz) const {
    return fMinCx <= cx && cx <= fMaxCx && fMinCz <= cz && cz <= fMaxCz;
  }
};

class Config {
public:
  int fSchemaVersion = 0;
  std::string fFormat;
  int32_t fDimension = 0;
  int32_t fChunkSize = 0;
  // Optional metadata. It documents the export UI default and never drives
  // execution; --default is the only fallback source.
  Action fDefaultAction = Action::Keep;
  std::string fOverlapPolicy;
  std::vector<ChunkRegion> fRegions;

  // Returns the action of the last matching region, or nullopt when no region
  // matches. Regions are inclusive and evaluated in file order.
  std::optional<Action> lastMatch(int32_t cx, int32_t cz) const {
    for (auto it = fRegions.rbegin(); it != fRegions.rend(); it++) {
      if (it->contains(cx, cz)) {
        return it->fAction;
      }
    }
    return std::nullopt;
  }

  static Status ParseFile(std::filesystem::path const &path, Config &out);
  static Status ParseJson(std::string const &text, Config &out);
  static char const *ActionName(Action a) {
    return a == Action::Delete ? "delete" : "keep";
  }
};

inline std::optional<CanonicalDimension> CanonicalDimensionFromInt(int32_t d) {
  switch (d) {
  case -1:
    return CanonicalDimension::Nether;
  case 0:
    return CanonicalDimension::Overworld;
  case 1:
    return CanonicalDimension::End;
  default:
    return std::nullopt;
  }
}

inline char const *CanonicalDimensionName(CanonicalDimension d) {
  switch (d) {
  case CanonicalDimension::Nether:
    return "nether";
  case CanonicalDimension::End:
    return "end";
  case CanonicalDimension::Overworld:
  default:
    return "overworld";
  }
}

} // namespace je2be::prune
