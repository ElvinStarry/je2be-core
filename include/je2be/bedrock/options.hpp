#pragma once

#include <functional>
#include <unordered_set>
#include <vector>

#include <je2be/pos2.hpp>
#include <je2be/uuid.hpp>
#include <je2be/world-data-override.hpp>

namespace je2be::bedrock {

class Options {
public:
  std::filesystem::path getTempDirectory() const {
    return fTempDirectory ? *fTempDirectory : std::filesystem::temp_directory_path();
  }

public:
  std::unordered_set<mcfile::Dimension> fDimensionFilter;
  std::unordered_set<Pos2i, Pos2iHasher> fChunkFilter;
  std::shared_ptr<Uuid const> fLocalPlayer;
  std::function<std::optional<Uuid>(std::u8string const &)> fJavaPlayerUuidResolver;
  std::optional<std::filesystem::path> fTempDirectory;
  std::vector<WorldDataOverride> fWorldDataOverrides;
};

} // namespace je2be::bedrock
