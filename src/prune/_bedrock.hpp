#pragma once

#include "prune/_config.hpp"

#include <je2be/status.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace je2be::prune {

struct BedrockChunkKey {
  int32_t fCx = 0;
  int32_t fCz = 0;
  uint8_t fTag = 0;
  CanonicalDimension fDimension = CanonicalDimension::Overworld;
  bool fSubChunk = false;
  bool fStructure = false;
};

// True for keys that belong to the global Bedrock namespace and must never be
// treated as chunk data, even when their byte length happens to match a chunk
// key shape (for example "BiomeData", "scoreboard" or "PosTrackDB-0x00000001").
bool IsBedrockGlobalKey(std::string const &key);

// Parses a known chunk-scoped Bedrock key shape. Only tags known to belong to
// chunk data are accepted; arbitrary binary keys are rejected.
bool ParseBedrockChunkKey(std::string const &key, BedrockChunkKey &out);

// Parses a "digp" key (per-chunk entity id list).
bool ParseBedrockDigpKey(std::string const &key, int32_t &cx, int32_t &cz, CanonicalDimension &dimension);

struct BedrockPruneResult {
  uint64_t fTotalKeys = 0;
  uint64_t fExistingChunks = 0;
  uint64_t fDeletedChunks = 0;
  uint64_t fDeletedChunkKeys = 0;
  uint64_t fDeletedDigpKeys = 0;
  uint64_t fDeletedActorPrefixes = 0;
  uint64_t fMapRecords = 0;
  uint64_t fProtectedChunks = 0;
  uint64_t fUnknownChunkShapedKeys = 0;
  std::vector<std::string> fWarnings;
};

// Opens <input>/db as a live LevelDB database, validates stored map records,
// then deletes every chunk-scoped record of the selected chunks in the target
// dimension. Global records are only removed when ownership is unambiguous
// (digp actor references and fully removed villages).
Status PruneBedrockWorld(std::filesystem::path const &input, Config const &config, Action fallback, BedrockPruneResult &result);

} // namespace je2be::prune
