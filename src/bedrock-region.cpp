#include "bedrock/_region.hpp"

#include "_parallel.hpp"
#include "_pos2i-set.hpp"
#include "bedrock/_chunk.hpp"
#include "bedrock/_context.hpp"
#include "bedrock/_java-chunk.hpp"
#include "terraform/_leaves.hpp"
#include "terraform/java/_block-accessor-java-mca.hpp"

using namespace std;
namespace fs = std::filesystem;

namespace je2be::bedrock {

class Region::Impl {
public:
  static Status Convert(mcfile::Dimension d,
                        Pos2iSet const &chunks,
                        Pos2i region,
                        unsigned int concurrency,
                        mcfile::be::DbInterface *db,
                        std::filesystem::path destination,
                        Context const &parentContext,
                        std::function<bool(void)> progress,
                        std::atomic_uint64_t &numConvertedChunks,
                        std::filesystem::path terrainTempDir,
                        std::shared_ptr<Context> &out) {
    using namespace mcfile;
    using namespace mcfile::stream;

    auto ctx = parentContext.make();
    int rx = region.fX;
    int rz = region.fZ;

    auto name = mcfile::je::Region::GetDefaultRegionFileName(rx, rz);
    auto terrainMcaPath = terrainTempDir / name;
    auto entitiesMcaPath = destination / "entities" / name;

    auto terrain = mcfile::je::McaEditor::Open(terrainMcaPath);
    if (!terrain) {
      return JE2BE_ERROR;
    }

    auto entities = mcfile::je::McaEditor::Open(entitiesMcaPath);
    if (!entities) {
      return JE2BE_ERROR;
    }

    vector<Pos2i> sortedChunks;
    sortedChunks.reserve(chunks.size());
    for (Pos2i const &chunk : chunks) {
      sortedChunks.push_back(chunk);
    }
    sort(sortedChunks.begin(), sortedChunks.end(), [](Pos2i const &a, Pos2i const &b) {
      return a.fZ == b.fZ ? a.fX < b.fX : a.fZ < b.fZ;
    });

    unique_ptr<terraform::bedrock::BlockAccessorBedrock<3, 3>> cache;
    for (Pos2i const &chunk : sortedChunks) {
      int const cx = chunk.fX;
      int const cz = chunk.fZ;
      if (!cache) {
        cache = make_unique<terraform::bedrock::BlockAccessorBedrock<3, 3>>(d, cx - 1, cz - 1, db, ctx->fEncoding);
      } else if (cache->fChunkX != cx - 1 || cache->fChunkZ != cz - 1) {
        cache.reset(cache->makeRelocated(cx - 1, cz - 1));
      }

      auto b = mcfile::be::Chunk::Load(cx, cz, d, *db, ctx->fEncoding);
      if (!b) {
        return JE2BE_ERROR_WHAT("Failed to load Bedrock chunk [" + std::to_string(cx) + ", " + std::to_string(cz) + "] in dimension " + std::to_string(static_cast<int>(d)));
      }
      cache->set(cx, cz, b);

      shared_ptr<mcfile::je::WritableChunk> j;
      if (auto st = Chunk::Convert(d, cx, cz, *b, *cache, *ctx, j); !st.ok()) {
        return JE2BE_ERROR_PUSH(st);
      }

      int localX = cx - rx * 32;
      int localZ = cz - rz * 32;
      auto terrainTag = j->toCompoundTag(d);
      if (!terrainTag || !EnsureJavaChunkSections(*terrainTag) || !terrain->insert(localX, localZ, *terrainTag)) {
        return JE2BE_ERROR;
      }
      auto entitiesTag = j->toEntitiesCompoundTag();
      if (!entitiesTag || !entities->insert(localX, localZ, *entitiesTag)) {
        return JE2BE_ERROR;
      }
      numConvertedChunks.fetch_add(1);
      if (!progress()) {
        return JE2BE_ERROR;
      }
    }

    string writeError;
    if (!terrain->write(terrainMcaPath, &writeError)) {
      return JE2BE_ERROR_WHAT(writeError);
    }
    terrain.reset();

    if (!entities->write(entitiesMcaPath, &writeError)) {
      return JE2BE_ERROR_WHAT(writeError);
    }
    entities.reset();

    out.swap(ctx);
    return Status::Ok();
  }
};

Status Region::Convert(mcfile::Dimension d,
                       Pos2iSet const &chunks,
                       Pos2i region,
                       unsigned int concurrency,
                       mcfile::be::DbInterface *db,
                       std::filesystem::path destination,
                       Context const &parentContext,
                       std::function<bool(void)> progress,
                       std::atomic_uint64_t &numConvertedChunks,
                       std::filesystem::path terrainTempDir,
                       std::shared_ptr<Context> &out) {
  return Impl::Convert(d, chunks, region, concurrency, db, destination, parentContext, progress, numConvertedChunks, terrainTempDir, out);
}

} // namespace je2be::bedrock
