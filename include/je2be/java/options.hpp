#pragma once

#include <vector>

#include <je2be/enums/level-directory-structure.hpp>
#include <je2be/pos2.hpp>
#include <je2be/world-data-override.hpp>

namespace je2be::java {

class Options {
public:
  LevelDirectoryStructure fLevelDirectoryStructure = LevelDirectoryStructure::Vanilla;
  std::unordered_set<mcfile::Dimension> fDimensionFilter;
  std::unordered_set<Pos2i, Pos2iHasher> fChunkFilter;
  std::optional<std::filesystem::path> fTempDirectory;
  std::optional<std::filesystem::path> fDbTempDirectory;
  std::vector<WorldDataOverride> fWorldDataOverrides;

  std::filesystem::path getWorldDirectory(std::filesystem::path const &root, mcfile::Dimension dim) const {
    using namespace mcfile;
    namespace fs = std::filesystem;
    auto levelRoot = getLevelDatFilePath(root).parent_path();
    std::error_code ec;
    auto modernBase = levelRoot / u8"dimensions" / u8"minecraft";
    if (fs::is_directory(modernBase / u8"overworld", ec) && !ec) {
      switch (dim) {
      case Dimension::Nether:
        return modernBase / u8"the_nether";
      case Dimension::End:
        return modernBase / u8"the_end";
      case Dimension::Overworld:
      default:
        return modernBase / u8"overworld";
      }
    }
    switch (fLevelDirectoryStructure) {
    case LevelDirectoryStructure::Paper: {
      switch (dim) {
      case Dimension::Nether:
        return root / "world_nether" / "DIM-1";
      case Dimension::End:
        return root / "world_the_end" / "DIM1";
      case Dimension::Overworld:
      default:
        return root / "world";
      }
      break;
    }
    case LevelDirectoryStructure::Vanilla:
    default: {
      switch (dim) {
      case Dimension::Nether:
        return root / "DIM-1";
      case Dimension::End:
        return root / "DIM1";
      case Dimension::Overworld:
      default:
        return root;
      }
      break;
    }
    }
  }

  std::filesystem::path getDataDirectory(std::filesystem::path const &root) const {
    switch (fLevelDirectoryStructure) {
    case LevelDirectoryStructure::Paper:
      return root / "world" / "data";
    case LevelDirectoryStructure::Vanilla:
    default:
      return root / "data";
    }
  }

  std::filesystem::path getMapDataDirectory(std::filesystem::path const &root) const {
    namespace fs = std::filesystem;
    auto data = getDataDirectory(root);
    auto modern = data / u8"minecraft" / u8"maps";
    std::error_code ec;
    if (fs::is_directory(modern, ec) && !ec) {
      return modern;
    }
    return data;
  }

  std::filesystem::path getPlayerDataDirectory(std::filesystem::path const &root) const {
    namespace fs = std::filesystem;
    auto levelRoot = getLevelDatFilePath(root).parent_path();
    auto modern = levelRoot / u8"players" / u8"data";
    std::error_code ec;
    if (fs::is_directory(modern, ec) && !ec) {
      return modern;
    }
    return levelRoot / u8"playerdata";
  }

  std::filesystem::path getLevelDatFilePath(std::filesystem::path const &root) const {
    switch (fLevelDirectoryStructure) {
    case LevelDirectoryStructure::Paper:
      return root / "world" / "level.dat";
    case LevelDirectoryStructure::Vanilla:
    default:
      return root / "level.dat";
    }
  }

  std::filesystem::path getTempDirectory() const {
    return fTempDirectory ? *fTempDirectory : std::filesystem::temp_directory_path();
  }
};

} // namespace je2be::java
