#include <je2be/bedrock/converter.hpp>

#include <je2be/bedrock/options.hpp>
#include <je2be/bedrock/progress.hpp>
#include <je2be/nbt.hpp>

#include "_props.hpp"
#include "_queue2d.hpp"
#include "_walk.hpp"
#include "bedrock/_context.hpp"
#include "bedrock/_java-player.hpp"
#include "bedrock/_level-data.hpp"
#include "bedrock/_world.hpp"
#include "db/_readonly-db.hpp"
#include "enums/_game-mode.hpp"
#include "terraform/_leaves.hpp"
#include "terraform/java/_block-accessor-java-directory.hpp"
#include "terraform/lighting/_lighting.hpp"

#include <sparse.hpp>

#include <atomic>
#include <exception>
#include <latch>
#include <thread>

using namespace std;
namespace fs = std::filesystem;

namespace je2be::bedrock {

class Converter::Impl {
  struct ResolvedPlayer {
    Context::PlayerData fSource;
    i64 fBedrockId;
    Uuid fJavaId;
  };

  struct ConvertedPlayer {
    i64 fBedrockId;
    Uuid fJavaId;
    CompoundTagPtr fEntity;
  };

public:
  static Status Run(std::filesystem::path const &input, std::filesystem::path const &output, Options const &options, unsigned concurrency, Progress *progress = nullptr) {
    using namespace std;
    using namespace leveldb;
    using namespace mcfile;
    namespace fs = std::filesystem;

    CompoundTagPtr dat;
    if (!LevelData::Read(input / "level.dat", dat)) {
      return JE2BE_ERROR;
    }
    if (!dat) {
      return JE2BE_ERROR;
    }

    u64 total = 0;
    map<Dimension, vector<pair<Pos2i, Context::ChunksInRegion>>> regions;
    vector<Context::PlayerData> players;
    i64 gameTick = dat->int64(u8"currentTick", 0);
    i32 gameTypeB = dat->int32(u8"GameType", 0);
    GameMode gameMode = GameMode::Survival;
    if (auto t = GameModeFromBedrock(gameTypeB); t) {
      gameMode = *t;
    }
    unique_ptr<Context> bin;
    if (auto st = Context::Init(input / "db", options, mcfile::Encoding::LittleEndian, regions, total, gameTick, gameMode, concurrency, players, bin); !st.ok()) {
      return JE2BE_ERROR_PUSH(st);
    }

    Options effectiveOptions = options;
    vector<ResolvedPlayer> resolvedPlayers;
    if (auto st = ResolvePlayers(players, options, *bin, effectiveOptions, resolvedPlayers); !st.ok()) {
      return JE2BE_ERROR_PUSH(st);
    }

    if (!PrepareOutputDirectory(output)) {
      return JE2BE_ERROR;
    }

    unique_ptr<ReadonlyDb> db;
    if (auto st = ReadonlyDb::Open(input / "db", options.getTempDirectory(), db); !st.ok()) {
      return JE2BE_ERROR_PUSH(st);
    }
    if (!db) {
      return JE2BE_ERROR;
    }
    auto levelDat = LevelData::Import(*dat, *db, effectiveOptions, *bin);
    if (!levelDat) {
      return JE2BE_ERROR;
    }

    vector<ConvertedPlayer> convertedPlayers;
    optional<Uuid> resolvedLocalPlayerUuid;
    for (auto const &resolved : resolvedPlayers) {
      if (resolved.fSource.isLocal()) {
        resolvedLocalPlayerUuid = resolved.fJavaId;
        continue;
      }
      auto player = Entity::LocalPlayer(*resolved.fSource.fEntity, *bin, &resolved.fJavaId, kJavaDataVersion);
      if (!player) {
        continue;
      }
      if (player->fShoulderEntityLeft) {
        bin->setShoulderEntityLeft(resolved.fBedrockId, *player->fShoulderEntityLeft);
      }
      if (player->fShoulderEntityRight) {
        bin->setShoulderEntityRight(resolved.fBedrockId, *player->fShoulderEntityRight);
      }
      convertedPlayers.push_back({resolved.fBedrockId, resolved.fJavaId, player->fEntity});
    }

    atomic<int> done = 0;
    atomic<bool> cancelRequested = false;
    atomic_uint64_t numConvertedChunks(0);
    auto reportProgress = [progress, &done, total, &cancelRequested, &numConvertedChunks]() -> bool {
      u64 p = done.fetch_add(1) + 1;
      if (progress) {
        bool ok = progress->reportConvert({p, total}, numConvertedChunks.load());
        if (!ok) {
          cancelRequested = true;
        }
        return ok;
      } else {
        return true;
      }
    };

    map<Dimension, fs::path> terrainTempDirs;
    for (Dimension d : {Dimension::Overworld, Dimension::Nether, Dimension::End}) {
      if (!effectiveOptions.fDimensionFilter.empty()) {
        if (effectiveOptions.fDimensionFilter.find(d) == effectiveOptions.fDimensionFilter.end()) {
          continue;
        }
      }
      auto terrainTempDir = File::CreateTempDir(effectiveOptions.getTempDirectory());
      if (!terrainTempDir) {
        return JE2BE_ERROR;
      }
      terrainTempDirs[d] = *terrainTempDir;
      shared_ptr<Context> result;
      if (auto st = World::Convert(d, regions[d], *db, output, concurrency, *bin, result, reportProgress, numConvertedChunks, *terrainTempDir); !st.ok()) {
        return JE2BE_ERROR_PUSH(st);
      }
      if (result) {
        result->mergeInto(*bin);
      }
      if (cancelRequested.load()) {
        return JE2BE_ERROR;
      }
    }

    if (auto st = Terraform(regions, output, terrainTempDirs, concurrency, progress); !st.ok()) {
      return JE2BE_ERROR_PUSH(st);
    }
    for (auto const &[_, dir] : terrainTempDirs) {
      Fs::DeleteAll(dir);
    }

    CompoundTagPtr localPlayer;
    if (auto data = levelDat->compoundTag(u8"Data"); data) {
      if (auto player = data->compoundTag(u8"Player"); player) {
        if (auto localPlayerId = bin->localPlayerId(); localPlayerId) {
          AttachPlayerState(*player, *localPlayerId, *bin);
        }
        localPlayer = player;
      }
    }

    for (auto &converted : convertedPlayers) {
      AttachPlayerState(*converted.fEntity, converted.fBedrockId, *bin);
    }

    LevelData::UpdateDataPacksAndEnabledFeatures(*levelDat, *bin);

    if (!LevelData::Write(*levelDat, output / "level.dat")) {
      return JE2BE_ERROR;
    }

    map<u8string, CompoundTagPtr> playerData;
    for (auto const &player : convertedPlayers) {
      playerData[player.fJavaId.toString()] = player.fEntity;
    }
    if (resolvedLocalPlayerUuid && localPlayer) {
      playerData[resolvedLocalPlayerUuid->toString()] = localPlayer;
    }

    if (!playerData.empty()) {
      auto playerDataDirectory = output / "playerdata";
      error_code ec;
      fs::create_directories(playerDataDirectory, ec);
      if (ec) {
        return JE2BE_ERROR_WHAT(ec.message());
      }
      for (auto const &[uuid, player] : playerData) {
        auto path = playerDataDirectory / fs::path(uuid + u8".dat");
        if (!LevelData::Write(*player, path)) {
          return JE2BE_ERROR;
        }
      }
    }

    return bin->postProcess(output, *db);
  }

private:
  static std::u8string PlayerNameCacheKey(std::u8string name) {
    for (auto &ch : name) {
      if (ch >= u8'A' && ch <= u8'Z') {
        ch += u8'a' - u8'A';
      }
    }
    return name;
  }

  static Status ResolvePlayers(std::vector<Context::PlayerData> const &players,
                               Options const &options,
                               Context &ctx,
                               Options &effectiveOptions,
                               std::vector<ResolvedPlayer> &resolvedPlayers) {
    std::map<std::u8string, std::optional<Uuid>> cache;
    std::map<i64, Uuid> mappings;
    std::vector<ResolvedPlayer> markedPlayers;
    for (auto const &player : players) {
      auto bedrockId = player.fEntity->int64(u8"UniqueID");
      if (!bedrockId) {
        continue;
      }
      auto name = JavaPlayer::NameFromInventoryMarker(*player.fEntity);
      if (!name) {
        continue;
      }
      auto key = PlayerNameCacheKey(*name);
      auto found = cache.find(key);
      if (found == cache.end()) {
        try {
          found = cache.emplace(key, JavaPlayer::ResolveUuid(*name, options)).first;
        } catch (std::exception const &e) {
          std::string const javaName(reinterpret_cast<char const *>(name->data()), name->size());
          return JE2BE_ERROR_WHAT("Failed to resolve Java UUID for JavaTag player '" + javaName + "': " + e.what());
        } catch (...) {
          std::string const javaName(reinterpret_cast<char const *>(name->data()), name->size());
          return JE2BE_ERROR_WHAT("Failed to resolve Java UUID for JavaTag player '" + javaName + "'");
        }
      }
      if (!found->second) {
        std::string const javaName(reinterpret_cast<char const *>(name->data()), name->size());
        return JE2BE_ERROR_WHAT("Failed to resolve Java UUID for JavaTag player '" + javaName + "'");
      }
      Uuid uuid = *found->second;
      auto mapped = mappings.find(*bedrockId);
      if (mapped != mappings.end() && !UuidPred{}(mapped->second, uuid)) {
        return JE2BE_ERROR_WHAT("Conflicting Java UUIDs for Bedrock player UniqueID " + std::to_string(*bedrockId));
      }
      mappings[*bedrockId] = uuid;
      markedPlayers.push_back({player, *bedrockId, uuid});
    }

    for (auto const &[bedrockId, javaId] : mappings) {
      ctx.addPlayerMapping(bedrockId, javaId);
    }

    std::map<std::u8string, ResolvedPlayer> selectedPlayers;
    for (auto const &player : markedPlayers) {
      auto key = player.fJavaId.toString();
      auto selected = selectedPlayers.find(key);
      if (selected == selectedPlayers.end()) {
        selectedPlayers.emplace(key, player);
      } else if (selected->second.fBedrockId != player.fBedrockId) {
        std::string const javaId(reinterpret_cast<char const *>(key.data()), key.size());
        return JE2BE_ERROR_WHAT("Java UUID " + javaId + " is assigned to multiple Bedrock player UniqueIDs");
      }
    }

    for (auto const &player : players) {
      if (!player.isLocal()) {
        continue;
      }
      auto bedrockId = player.fEntity->int64(u8"UniqueID");
      if (!bedrockId) {
        continue;
      }
      auto mapped = mappings.find(*bedrockId);
      if (mapped != mappings.end()) {
        effectiveOptions.fLocalPlayer = std::make_shared<Uuid const>(mapped->second);
        selectedPlayers[mapped->second.toString()] = {player, *bedrockId, mapped->second};
      } else if (options.fLocalPlayer) {
        auto key = options.fLocalPlayer->toString();
        auto selected = selectedPlayers.find(key);
        if (selected != selectedPlayers.end() && selected->second.fBedrockId != *bedrockId) {
          std::string const javaId(reinterpret_cast<char const *>(key.data()), key.size());
          return JE2BE_ERROR_WHAT("Java UUID " + javaId + " is assigned to multiple Bedrock player UniqueIDs");
        }
        ctx.addPlayerMapping(*bedrockId, *options.fLocalPlayer);
      }
    }

    resolvedPlayers.clear();
    for (auto const &[_, player] : selectedPlayers) {
      resolvedPlayers.push_back(player);
    }
    return Status::Ok();
  }

  static void AttachPlayerState(CompoundTag &player, i64 bedrockId, Context &ctx) {
    if (auto rootVehicle = ctx.drainRootVehicle(bedrockId); rootVehicle) {
      Uuid vehicleUuid = rootVehicle->first;
      auto entity = rootVehicle->second;
      if (auto vehiclePos = props::GetPos3d(*entity, u8"Pos"); vehiclePos) {
        if (auto playerPos = props::GetPos3d(player, u8"Pos"); playerPos) {
          auto vehicleId = entity->string(u8"id", u8"");
          if (vehicleId == u8"minecraft:boat") {
            playerPos->fY = vehiclePos->fY - 0.45;
          } else if (vehicleId == u8"minecraft:minecart") {
            if (entity->boolean(u8"OnGround", false)) {
              playerPos->fY = vehiclePos->fY - 0.35;
            } else {
              playerPos->fY = vehiclePos->fY - 0.2875;
            }
          }
          player.set(u8"Pos", playerPos->toListTag());
        }
      }
      auto rootVehicleTag = Compound();
      rootVehicleTag->set(u8"Entity", entity);
      rootVehicleTag->set(u8"Attach", vehicleUuid.toIntArrayTag());
      player.set(u8"RootVehicle", rootVehicleTag);
    }

    CompoundTagPtr shoulderEntityLeft;
    CompoundTagPtr shoulderEntityRight;
    ctx.drainShoulderEntities(bedrockId, shoulderEntityLeft, shoulderEntityRight);
    if (shoulderEntityLeft) {
      player.set(u8"ShoulderEntityLeft", shoulderEntityLeft);
    }
    if (shoulderEntityRight) {
      player.set(u8"ShoulderEntityRight", shoulderEntityRight);
    }
  }

  static Status Terraform(
      map<mcfile::Dimension, vector<pair<Pos2i, Context::ChunksInRegion>>> const &regions,
      fs::path const &output,
      map<mcfile::Dimension, fs::path> const &terrainTempDirs,
      unsigned int concurrency,
      Progress *progress) {
    if (regions.empty()) {
      return Status::Ok();
    }

    using Queue = Queue2d<0, true, Sparse>;
    map<mcfile::Dimension, shared_ptr<Queue>> queues;
    u64 numChunks = 0;

    for (auto const &i : regions) {
      if (i.second.empty()) {
        continue;
      }
      Pos2i minR = i.second[0].first;
      Pos2i maxR = minR;
      for (auto const &j : i.second) {
        Pos2i const &region = j.first;
        int minRx = (std::min)(minR.fX, region.fX);
        int maxRx = (std::max)(maxR.fX, region.fX);
        int minRz = (std::min)(minR.fZ, region.fZ);
        int maxRz = (std::max)(maxR.fZ, region.fZ);
        minR = Pos2i(minRx, minRz);
        maxR = Pos2i(maxRx, maxRz);
        numChunks += j.second.fChunks.size();
      }

      int width = maxR.fX - minR.fX + 1;
      int height = maxR.fZ - minR.fZ + 1;
      auto queue = make_shared<Queue>(minR, width, height);
      for (auto const &j : i.second) {
        Pos2i const &region = j.first;
        queue->markTask(region, j.second.fChunks.size());
      }
      queues[i.first] = queue;
    }

    if (progress) {
      if (!progress->reportTerraform({0, numChunks}, numChunks)) {
        return JE2BE_ERROR;
      }
    }

    int numThreads = (int)concurrency - 1;
    unique_ptr<std::latch> latch;
    if (concurrency > 0) {
      latch.reset(new std::latch(concurrency));
    }
    std::latch *latchPtr = latch.get();
    atomic_bool ok(true);
    mutex mut;
    atomic_uint64_t done(0);

    auto action = [latchPtr, &queues, &mut, output, &ok, terrainTempDirs, regions, &done, progress, numChunks]() {
      shared_ptr<terraform::java::BlockAccessorJavaDirectory<3, 3>> blockAccessor;
      optional<mcfile::Dimension> prevDimension;

      while (ok) {
        optional<pair<mcfile::Dimension, Pos2i>> next;
        shared_ptr<Queue> queue;
        bool remaining = false;
        {
          lock_guard<mutex> lock(mut);
          for (auto const &it : queues) {
            if (auto n = it.second->next(); n) {
              remaining = true;
              if (holds_alternative<Queue::Dequeue>(*n)) {
                next = make_pair(it.first, get<Queue::Dequeue>(*n).fRegion);
                queue = it.second;
                break;
              }
            }
          }
        }
        if (!next || !queue) {
          if (remaining) {
            this_thread::sleep_for(chrono::milliseconds(10));
            continue;
          } else {
            break;
          }
        }

        mcfile::Dimension dim = next->first;
        if (dim != prevDimension) {
          blockAccessor.reset();
          prevDimension = dim;
        }

        Pos2i region = next->second;
        int rx = region.fX;
        int rz = region.fZ;

        fs::path directory;
        switch (dim) {
        case mcfile::Dimension::Nether:
          directory = output / "DIM-1" / "region";
          break;
        case mcfile::Dimension::End:
          directory = output / "DIM1" / "region";
          break;
        case mcfile::Dimension::Overworld:
        default:
          directory = output / "region";
          break;
        }

        auto found = terrainTempDirs.find(dim);
        if (found == terrainTempDirs.end()) {
          ok = false;
          break;
        }

        auto name = mcfile::je::Region::GetDefaultRegionFileName(rx, rz);
        auto mcaIn = found->second / name;
        auto mcaOut = directory / name;
        auto editor = mcfile::je::McaEditor::Open(mcaIn);
        if (!editor) {
          ok = false;
          break;
        }

        auto regionsInDim = regions.find(dim);
        if (regionsInDim == regions.end()) {
          ok = false;
          break;
        }

        Context::ChunksInRegion chunksInRegion;
        for (auto const &r : regionsInDim->second) {
          if (r.first == region) {
            chunksInRegion = r.second;
            break;
          }
        }

        terraform::lighting::LightCache lightCache(rx, rz);

        for (int z = 0; z < 32; z++) {
          for (int x = 0; x < 32; x++) {
            int cx = x + rx * 32;
            int cz = z + rz * 32;
            if (chunksInRegion.fChunks.find(Pos2i(cx, cz)) != chunksInRegion.fChunks.end()) {
              if (!TerraformChunk(cx, cz, *editor, found->second, blockAccessor, dim, lightCache).ok()) {
                ok = false;
                break;
              }
              u64 d = done.fetch_add(1) + 1;
              if (progress && !progress->reportTerraform({d, numChunks}, d)) {
                ok = false;
                break;
              }
            }
            lightCache.dispose(cx - 1, cz - 1);
          }
          if (!ok) {
            break;
          }
        }

        if (!editor->write(mcaOut)) {
          ok = false;
        }

        {
          lock_guard<mutex> lock(mut);
          queue->unlock({region});
        }
      }
      if (latchPtr) {
        latchPtr->count_down();
      }
    };

    vector<thread> threads;
    for (int i = 0; i < numThreads; i++) {
      threads.emplace_back(action);
    }
    action();
    if (latch) {
      latch->wait();
    }
    for (auto &th : threads) {
      th.join();
    }
    if (progress && !progress->reportTerraform({1, numChunks}, numChunks)) {
      return JE2BE_ERROR;
    }
    return Status::Ok();
  }

  static Status TerraformChunk(
      int cx,
      int cz,
      mcfile::je::McaEditor &editor,
      fs::path inputDirectory,
      std::shared_ptr<terraform::java::BlockAccessorJavaDirectory<3, 3>> &blockAccessor,
      mcfile::Dimension dim,
      terraform::lighting::LightCache &lightCache) {
    int rx = mcfile::Coordinate::RegionFromChunk(cx);
    int rz = mcfile::Coordinate::RegionFromChunk(cz);

    int x = cx - rx * 32;
    int z = cz - rz * 32;

    auto current = editor.extract(x, z);
    if (!current) {
      return Status::Ok();
    }

    if (!blockAccessor) {
      blockAccessor.reset(new terraform::java::BlockAccessorJavaDirectory<3, 3>(cx - 1, cz - 1, inputDirectory));
    }
    if (blockAccessor->fChunkX != cx - 1 || blockAccessor->fChunkZ != cz - 1) {
      auto next = blockAccessor->makeRelocated(cx - 1, cz - 1);
      blockAccessor.reset(next);
    }
    blockAccessor->loadAllWith(editor, mcfile::Coordinate::RegionFromChunk(cx), mcfile::Coordinate::RegionFromChunk(cz));

    auto copy = current->copy();
    auto ch = mcfile::je::Chunk::MakeChunk(cx, cz, current);
    if (!ch) {
      return JE2BE_ERROR;
    }
    blockAccessor->set(ch);

    auto writable = mcfile::je::WritableChunk::MakeChunk(cx, cz, copy);
    if (!writable) {
      return JE2BE_ERROR;
    }

    terraform::BlockPropertyAccessorJava propertyAccessor(*ch);
    terraform::Leaves::Do(*writable, *blockAccessor, propertyAccessor);
    terraform::lighting::Lighting::Do(dim, *writable, *blockAccessor, lightCache);

    auto tag = writable->toCompoundTag(dim);
    if (!tag) {
      return JE2BE_ERROR;
    }
    if (!editor.insert(x, z, *tag)) {
      return JE2BE_ERROR;
    }

    return Status::Ok();
  }

  static bool PrepareOutputDirectory(std::filesystem::path const &output) {
    using namespace std;
    namespace fs = std::filesystem;
    error_code ec;
    fs::create_directories(output, ec);
    if (ec) {
      return false;
    }
    ec.clear();

    fs::directory_iterator iterator(output, ec);
    if (ec) {
      return false;
    }
    ec.clear();
    for (auto it : iterator) {
      fs::remove_all(it.path(), ec);
      ec.clear();
    }
    return true;
  }
};

Status Converter::Run(std::filesystem::path const &input, std::filesystem::path const &output, Options const &options, unsigned concurrency, Progress *progress) {
  return Impl::Run(input, output, options, concurrency, progress);
}

} // namespace je2be::bedrock
