#pragma once

#include "prune/_config.hpp"
#include "prune/_selection.hpp"

#include <je2be/status.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace je2be::prune {

struct JavaWorldLocation {
  std::filesystem::path fInput;
  std::filesystem::path fLevelRoot;
  std::filesystem::path fDimensionDirectory;
  bool fPaperStructure = false;
};

// Detects the java world layout for the given input directory. Accepts both a
// world directory (level.dat directly inside) and a paper-style server root
// (world/level.dat inside).
Status LocateJavaWorld(std::filesystem::path const &input, CanonicalDimension dimension, JavaWorldLocation &out);

struct JavaPruneResult {
  uint64_t fMapFiles = 0;
  uint64_t fExistingChunks = 0;
  uint64_t fDeletedChunks = 0;
  uint64_t fDeletedRegionEntries = 0;
  uint64_t fDeletedExternalFiles = 0;
  uint64_t fKeptByMapProtection = 0;
  std::vector<std::string> fWarnings;
};

// Acquires <levelRoot>/session.lock, validates every map and region file, then
// removes the selected chunks from region/, entities/ and poi/ in place.
Status PruneJavaWorld(JavaWorldLocation const &location, Config const &config, Action fallback, JavaPruneResult &result);

} // namespace je2be::prune
