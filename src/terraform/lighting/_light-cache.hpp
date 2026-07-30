#pragma once

#include "terraform/lighting/_chunk-lighting-model.hpp"
#include "terraform/lighting/_lighting-model.hpp"

namespace je2be::terraform::lighting {

class LightCache {
public:
  LightCache(int rx, int rz)
      : fModels({rx * 32 - 1, rz * 32 - 1}, 34, 34, nullptr), fSkyLights({rx * 32 - 1, rz * 32 - 1}, 34, 34, nullptr), fBlockLights({rx * 32 - 1, rz * 32 - 1}, 34, 34, nullptr) {}

  std::shared_ptr<ChunkLightingModel> getModel(int cx, int cz) {
    return fModels[{cx, cz}];
  }

  void setModel(int cx, int cz, std::shared_ptr<ChunkLightingModel> const &data) {
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
    return fSkyLights[{cx, cz}];
  }

  void setSkyLight(int cx, int cz, std::shared_ptr<ChunkLightCache> const &light) {
    fSkyLights[{cx, cz}] = light;
  }

  std::shared_ptr<ChunkLightCache> getBlockLight(int cx, int cz) {
    return fBlockLights[{cx, cz}];
  }

  void setBlockLight(int cx, int cz, std::shared_ptr<ChunkLightCache> const &light) {
    fBlockLights[{cx, cz}] = light;
  }

private:
  Data2d<std::shared_ptr<ChunkLightingModel>> fModels;
  Data2d<std::shared_ptr<ChunkLightCache>> fSkyLights;
  Data2d<std::shared_ptr<ChunkLightCache>> fBlockLights;
  size_t fDisposeIndex = 0;
};

} // namespace je2be::terraform::lighting
