#pragma once

#include <je2be/pos2.hpp>

#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace je2be::bedrock {

class TerraformRegionScheduler {
  struct State {
    uint8_t fDependencies = 0;
    uint8_t fConsumers = 0;
    bool fConverted = false;
    bool fScheduled = false;
    bool fCompleted = false;
  };

public:
  explicit TerraformRegionScheduler(std::vector<Pos2i> const &regions) {
    fStates.reserve(regions.size());
    for (Pos2i const &region : regions) {
      fStates.try_emplace(region);
    }
    for (auto &[target, state] : fStates) {
      for (int dz = -1; dz <= 1; dz++) {
        for (int dx = -1; dx <= 1; dx++) {
          auto source = fStates.find({target.fX + dx, target.fZ + dz});
          if (source == fStates.end()) {
            continue;
          }
          state.fDependencies++;
          source->second.fConsumers++;
        }
      }
    }
  }

  std::optional<std::vector<Pos2i>> markConverted(Pos2i const &region) {
    std::lock_guard<std::mutex> lock(fMutex);
    auto converted = fStates.find(region);
    if (converted == fStates.end() || converted->second.fConverted) {
      return std::nullopt;
    }
    converted->second.fConverted = true;

    std::vector<Pos2i> ready;
    ready.reserve(9);
    for (int dz = -1; dz <= 1; dz++) {
      for (int dx = -1; dx <= 1; dx++) {
        Pos2i target(region.fX + dx, region.fZ + dz);
        auto found = fStates.find(target);
        if (found == fStates.end()) {
          continue;
        }
        State &state = found->second;
        if (state.fDependencies == 0) {
          return std::nullopt;
        }
        state.fDependencies--;
        if (state.fDependencies == 0) {
          if (state.fScheduled) {
            return std::nullopt;
          }
          state.fScheduled = true;
          ready.push_back(target);
        }
      }
    }
    return ready;
  }

  std::optional<std::vector<Pos2i>> markCompleted(Pos2i const &region) {
    std::lock_guard<std::mutex> lock(fMutex);
    auto completed = fStates.find(region);
    if (completed == fStates.end() || !completed->second.fScheduled || completed->second.fCompleted) {
      return std::nullopt;
    }
    completed->second.fCompleted = true;
    fCompleted++;

    std::vector<Pos2i> releasable;
    releasable.reserve(9);
    for (int dz = -1; dz <= 1; dz++) {
      for (int dx = -1; dx <= 1; dx++) {
        Pos2i source(region.fX + dx, region.fZ + dz);
        auto found = fStates.find(source);
        if (found == fStates.end()) {
          continue;
        }
        State &state = found->second;
        if (state.fConsumers == 0) {
          return std::nullopt;
        }
        state.fConsumers--;
        if (state.fConsumers == 0) {
          releasable.push_back(source);
        }
      }
    }
    return releasable;
  }

  size_t size() const {
    return fStates.size();
  }

  bool allCompleted() const {
    std::lock_guard<std::mutex> lock(fMutex);
    return fCompleted == fStates.size();
  }

private:
  std::unordered_map<Pos2i, State, Pos2iHasher> fStates;
  mutable std::mutex fMutex;
  size_t fCompleted = 0;
};

} // namespace je2be::bedrock
