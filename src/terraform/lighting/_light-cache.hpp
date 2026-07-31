#pragma once

#include "terraform/lighting/_chunk-lighting-model.hpp"
#include "terraform/lighting/_lighting-model.hpp"

#include <utility>

namespace je2be::terraform::lighting {

class LightCache {
public:
  LightCache(int rx, int rz)
      : fModels({rx * 32 - 1, rz * 32 - 1}, kWindowSize, kWindowSize, nullptr), fSkyLights({rx * 32 - 1, rz * 32 - 1}, kWindowSize, kWindowSize, nullptr), fBlockLights({rx * 32 - 1, rz * 32 - 1}, kWindowSize, kWindowSize, nullptr), fModelScratch(kWindowArea), fSkyScratch(kWindowArea), fBlockScratch(kWindowArea) {}

  void relocate(int rx, int rz) {
    Pos2i const start(rx * 32 - 1, rz * 32 - 1);
    if (fModels.fStart == start) {
      return;
    }
    Relocate(fModels, fModelScratch, start);
    Relocate(fSkyLights, fSkyScratch, start);
    Relocate(fBlockLights, fBlockScratch, start);
    fDisposeIndex = 0;
  }

  std::shared_ptr<ChunkLightingModel> getModel(int cx, int cz) {
    if (!Contains(fModels, cx, cz)) {
      return nullptr;
    }
    return fModels[{cx, cz}];
  }

  void setModel(int cx, int cz, std::shared_ptr<ChunkLightingModel> const &data) {
    if (!Contains(fModels, cx, cz)) {
      return;
    }
    fModels[{cx, cz}] = data;
  }

  // Disposes entries through [cx, cz] in z-major order. Repeated calls only
  // visit entries beyond the previous disposal point.
  void dispose(int cx, int cz) {
    int const minX = fModels.fStart.fX;
    int const minZ = fModels.fStart.fZ;
    if (cx < minX || cz < minZ) {
      return;
    }
    cx = (std::min)(cx, fModels.fEnd.fX);
    cz = (std::min)(cz, fModels.fEnd.fZ);
    size_t const width = (size_t)(fModels.fEnd.fX - minX + 1);
    size_t const target = (size_t)(cz - minZ) * width + (size_t)(cx - minX);
    while (fDisposeIndex <= target && fDisposeIndex < fModels.fStorage.size()) {
      fModels.fStorage[fDisposeIndex].reset();
      fSkyLights.fStorage[fDisposeIndex].reset();
      fBlockLights.fStorage[fDisposeIndex].reset();
      fDisposeIndex++;
    }
  }

  std::shared_ptr<ChunkLightCache> getSkyLight(int cx, int cz) {
    if (!Contains(fSkyLights, cx, cz)) {
      return nullptr;
    }
    return fSkyLights[{cx, cz}];
  }

  void setSkyLight(int cx, int cz, std::shared_ptr<ChunkLightCache> const &light) {
    if (!Contains(fSkyLights, cx, cz)) {
      return;
    }
    fSkyLights[{cx, cz}] = light;
  }

  std::shared_ptr<ChunkLightCache> getBlockLight(int cx, int cz) {
    if (!Contains(fBlockLights, cx, cz)) {
      return nullptr;
    }
    return fBlockLights[{cx, cz}];
  }

  void setBlockLight(int cx, int cz, std::shared_ptr<ChunkLightCache> const &light) {
    if (!Contains(fBlockLights, cx, cz)) {
      return;
    }
    fBlockLights[{cx, cz}] = light;
  }

private:
  static size_t constexpr kWindowSize = 34;
  static size_t constexpr kWindowArea = kWindowSize * kWindowSize;

  template <class T>
  static bool Contains(Data2d<T> const &data, int x, int z) {
    return data.fStart.fX <= x && x <= data.fEnd.fX && data.fStart.fZ <= z && z <= data.fEnd.fZ;
  }

  template <class T>
  static void Relocate(Data2d<T> &data, std::vector<T> &scratch, Pos2i const &start) {
    Pos2i const oldStart = data.fStart;
    Pos2i const oldEnd = data.fEnd;
    for (int z = oldStart.fZ; z <= oldEnd.fZ; z++) {
      for (int x = oldStart.fX; x <= oldEnd.fX; x++) {
        if (x < start.fX || start.fX + (int)kWindowSize - 1 < x || z < start.fZ || start.fZ + (int)kWindowSize - 1 < z) {
          continue;
        }
        size_t const index = (size_t)(z - start.fZ) * kWindowSize + (size_t)(x - start.fX);
        scratch[index] = data[{x, z}];
      }
    }
    data.relocate(start, nullptr);
    for (size_t i = 0; i < kWindowArea; i++) {
      data.fStorage[i] = std::move(scratch[i]);
      scratch[i] = nullptr;
    }
  }

  Data2d<std::shared_ptr<ChunkLightingModel>> fModels;
  Data2d<std::shared_ptr<ChunkLightCache>> fSkyLights;
  Data2d<std::shared_ptr<ChunkLightCache>> fBlockLights;
  std::vector<std::shared_ptr<ChunkLightingModel>> fModelScratch;
  std::vector<std::shared_ptr<ChunkLightCache>> fSkyScratch;
  std::vector<std::shared_ptr<ChunkLightCache>> fBlockScratch;
  size_t fDisposeIndex = 0;
};

} // namespace je2be::terraform::lighting
