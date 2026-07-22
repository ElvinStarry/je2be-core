#pragma once

#include <je2be/status.hpp>

#include <cstdint>
#include <filesystem>

namespace je2be::java {

class UuidReplacer {
public:
  struct Options {
    bool fDryRun = false;
  };

  struct Result {
    uint64_t fVisitedFiles = 0;
    uint64_t fChangedFiles = 0;
    uint64_t fChangedChunks = 0;
    uint64_t fReplacements = 0;
    uint64_t fRenamedFiles = 0;
  };

  [[nodiscard]] static Status Run(std::filesystem::path const &worldDirectory,
                                  std::filesystem::path const &mappingCsv,
                                  Options const &options,
                                  Result &result);

private:
  UuidReplacer() = delete;
};

} // namespace je2be::java
