#include <je2be/java/converter.hpp>

#if defined(_MSC_VER)
#define NOMINMAX
#undef small
#include <windows.h>
#endif

#if defined(__GNUC__)
#include <fcntl.h>
#endif

#include <je2be/java/options.hpp>
#include <je2be/java/progress.hpp>

#include "_directory-iterator.hpp"
#include "_parallel.hpp"
#include "_world-data-override.hpp"
#include "db/_concurrent-db.hpp"
#include "java/_context.hpp"
#include "java/_datapacks.hpp"
#include "java/_entity-store.hpp"
#include "java/_entity.hpp"
#include "java/_level.hpp"
#include "java/_region.hpp"
#include "java/_session-lock.hpp"
#include "java/_world-data.hpp"
#include "java/_world.hpp"

#include <array>

namespace je2be::java {

class Converter::Impl {
  Impl() = delete;

public:
  static Status Run(std::filesystem::path const &input, std::filesystem::path const &output, Options const &o, int concurrency, Progress *progress = nullptr) {
    using namespace std;
    namespace fs = std::filesystem;
    using namespace mcfile;

    SessionLock lock(input);
    if (!lock.lock()) {
      return JE2BE_ERROR;
    }

    double const numTotalChunks = GetTotalNumChunks(input, o);

    auto rootPath = output;
    auto dbPath = rootPath / "db";

    error_code ec;
    fs::create_directories(dbPath, ec);
    if (ec) {
      return JE2BE_ERROR_WHAT(ec.message());
    }

    CompoundTagPtr data;
    if (auto st = ReadInputLevelData(input, o, data); !st.ok()) {
      return JE2BE_ERROR_PUSH(st);
    }
    Level level = Level::ImportFromJava(*data);

    bool ok = Datapacks::Import(input, output);

    auto levelData = std::make_unique<LevelData>(input, o, level.fCurrentTick, level.fDifficulty, level.fCommandsEnabled, level.fGameType, level.fDataVersion);
    ConcurrentDb db(dbPath, concurrency, o.fDbTempDirectory);
    if (!db.valid()) {
      return JE2BE_ERROR;
    }

    auto localPlayerData = LocalPlayerData(*data, *levelData);
    if (localPlayerData) {
      auto k = mcfile::be::DbKey::LocalPlayer();
      if (auto st = db.put(k, *localPlayerData); !st.ok()) {
        return JE2BE_ERROR_PUSH(st);
      }
    }

    struct Work {
      Dimension fDim;
      shared_ptr<mcfile::je::Region> fRegion;

      Work(Dimension dim, shared_ptr<mcfile::je::Region> const &region) : fDim(dim), fRegion(region) {}
    };
    struct Result {
      map<mcfile::Dimension, shared_ptr<WorldData>> fData;

      void mergeInto(Result &out) const {
        for (auto const &it : fData) {
          if (!it.second) {
            continue;
          }
          if (auto found = out.fData.find(it.first); found != out.fData.end()) {
            if (found->second) {
              it.second->drain(*found->second);
            }
          } else {
            out.fData[it.first] = it.second;
          }
        }
      }
    };
    map<mcfile::Dimension, std::shared_ptr<EntityStore>> entityStores;
    vector<Work> works;
    for (auto dim : {Dimension::Overworld, Dimension::Nether, Dimension::End}) {
      if (!o.fDimensionFilter.empty()) [[unlikely]] {
        if (o.fDimensionFilter.find(dim) == o.fDimensionFilter.end()) {
          continue;
        }
      }
      auto entityStoreDir = mcfile::File::CreateTempDir(o.getTempDirectory());
      if (!entityStoreDir) {
        return JE2BE_ERROR;
      }
      auto entityStore = EntityStore::Open(*entityStoreDir);
      if (!entityStore) {
        return JE2BE_ERROR;
      }
      entityStores[dim].reset(entityStore);
      auto dir = o.getWorldDirectory(input, dim);
      mcfile::je::World world(dir);
      world.eachRegions([dim, &works](shared_ptr<mcfile::je::Region> const &region) {
        Work work(dim, region);
        works.push_back(work);
        return true;
      });
    }
    atomic_uint32_t done(0);
    atomic_bool abortSignal(false);
    atomic_uint64_t numConvertedChunks(0);
    LevelData const *ldPtr = levelData.get();
    auto [result, status] = Parallel::Reduce<Work, Result>(
        works,
        concurrency,
        Result(),
        [ldPtr, &db, progress, &done, numTotalChunks, &abortSignal, entityStores, o, &numConvertedChunks](Work const &work) -> pair<Result, Status> {
          auto found = entityStores.find(work.fDim);
          assert(found != entityStores.end());
          shared_ptr<EntityStore> entityStore = found->second;
          auto worldData = Region::Convert(
              work.fDim,
              work.fRegion,
              o,
              entityStore,
              *ldPtr,
              db,
              progress,
              done,
              numTotalChunks,
              abortSignal,
              numConvertedChunks);
          Result ret;
          ret.fData[work.fDim] = worldData;
          return make_pair(ret, Status::Ok());
        },
        [](Result const &from, Result &to) -> void {
          from.mergeInto(to);
        });
    if (!status.ok()) {
      return JE2BE_ERROR_PUSH(status);
    }
    u64 totalEntityChunks = 0;
    for (auto const &it : entityStores) {
      totalEntityChunks += it.second->fChunks.size();
    }
    atomic<u64> doneEntityChunks(0);
    for (auto const &it : result.fData) {
      mcfile::Dimension dim = it.first;

      it.second->drain(*levelData);
      auto entityStore = entityStores[dim];
      assert(entityStore);
      if (!entityStore) {
        continue;
      }

      if (auto st = World::PutWorldEntities(dim, db, entityStore, concurrency, progress, doneEntityChunks, totalEntityChunks); !st.ok()) {
        return JE2BE_ERROR_PUSH(st);
      }
    }
    if (progress) {
      u64 v = std::max<u64>(1, totalEntityChunks);
      if (!progress->reportEntityPostProcess({v, v})) {
        return JE2BE_ERROR;
      }
    }

    if (ok) {
      level.fCurrentTick = max(level.fCurrentTick, levelData->fMaxChunkLastUpdate);
      for (auto const &ex : levelData->fExperiments) {
        level.fExperiments[ex] = true;
      }
      level.fCheatsEnabled = levelData->fAllowCommand;
      auto outputLevelData = level.toBedrockCompoundTag();
      if (auto st = WorldDataOverrideEngine::ApplyBedrock(*outputLevelData, o.fWorldDataOverrides); !st.ok()) {
        return JE2BE_ERROR_PUSH(st);
      }
      ok = Level::Write(*outputLevelData, output / "level.dat");
      if (ok) {
        if (auto st = levelData->put(db, *data, levelData->fUuids); !st.ok()) {
          return JE2BE_ERROR_PUSH(st);
        }
      }
    }

    if (ok) {
      auto st = db.close([progress](Rational<u64> const &p) {
        if (!progress) {
          return true;
        }
        return progress->reportCompaction(p);
      });
      if (!st.ok()) {
        return JE2BE_ERROR_PUSH(st);
      }
    } else {
      db.abandon();
    }
    if (!ok) {
      return JE2BE_ERROR;
    }

    if (levelData->fError) {
      return Status(Status::ErrorData(*levelData->fError));
    } else {
      return Status::Ok();
    }
  }

private:
  enum class GameRuleValue {
    Boolean,
    Integer,
    NonZeroBoolean,
  };

  struct GameRuleMapping {
    std::u8string_view fModern;
    std::u8string_view fLegacy;
    GameRuleValue fValue;
    bool fInvert = false;
  };

  static Status ReadDataDocument(std::filesystem::path const &path, CompoundTagPtr &data) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
      if (ec) {
        return JE2BE_ERROR_WHAT(ec.message());
      }
      data = nullptr;
      return Status::Ok();
    }
    auto root = Level::Read(path);
    if (!root) {
      return JE2BE_ERROR_WHAT("Failed to read Java data file: " + path.string());
    }
    data = root->compoundTag(u8"data");
    if (!data) {
      return JE2BE_ERROR_WHAT("Missing data compound in Java data file: " + path.string());
    }
    return Status::Ok();
  }

  static CompoundTagPtr LegacyGameRules(CompoundTag const &modern) {
    static constexpr std::array<GameRuleMapping, 39> mappings = {{
        {u8"show_advancement_messages", u8"announceAdvancements", GameRuleValue::Boolean},
        {u8"command_block_output", u8"commandBlockOutput", GameRuleValue::Boolean},
        {u8"command_blocks_work", u8"commandBlocksEnabled", GameRuleValue::Boolean},
        {u8"advance_time", u8"doDaylightCycle", GameRuleValue::Boolean},
        {u8"entity_drops", u8"doEntityDrops", GameRuleValue::Boolean},
        {u8"fire_spread_radius_around_player", u8"doFireTick", GameRuleValue::NonZeroBoolean},
        {u8"immediate_respawn", u8"doImmediateRespawn", GameRuleValue::Boolean},
        {u8"spawn_phantoms", u8"doInsomnia", GameRuleValue::Boolean},
        {u8"limited_crafting", u8"doLimitedCrafting", GameRuleValue::Boolean},
        {u8"mob_drops", u8"doMobLoot", GameRuleValue::Boolean},
        {u8"spawn_mobs", u8"doMobSpawning", GameRuleValue::Boolean},
        {u8"spawn_patrols", u8"doPatrolSpawning", GameRuleValue::Boolean},
        {u8"block_drops", u8"doTileDrops", GameRuleValue::Boolean},
        {u8"spawn_wandering_traders", u8"doTraderSpawning", GameRuleValue::Boolean},
        {u8"spawn_wardens", u8"doWardenSpawning", GameRuleValue::Boolean},
        {u8"advance_weather", u8"doWeatherCycle", GameRuleValue::Boolean},
        {u8"drowning_damage", u8"drowningDamage", GameRuleValue::Boolean},
        {u8"fall_damage", u8"fallDamage", GameRuleValue::Boolean},
        {u8"fire_damage", u8"fireDamage", GameRuleValue::Boolean},
        {u8"freeze_damage", u8"freezeDamage", GameRuleValue::Boolean},
        {u8"keep_inventory", u8"keepInventory", GameRuleValue::Boolean},
        {u8"locator_bar", u8"locatorBar", GameRuleValue::Boolean},
        {u8"max_command_sequence_length", u8"maxCommandChainLength", GameRuleValue::Integer},
        {u8"mob_griefing", u8"mobGriefing", GameRuleValue::Boolean},
        {u8"natural_health_regeneration", u8"naturalRegeneration", GameRuleValue::Boolean},
        {u8"pvp", u8"pvp", GameRuleValue::Boolean},
        {u8"random_tick_speed", u8"randomTickSpeed", GameRuleValue::Integer},
        {u8"reduced_debug_info", u8"reducedDebugInfo", GameRuleValue::Boolean},
        {u8"send_command_feedback", u8"sendCommandFeedback", GameRuleValue::Boolean},
        {u8"show_death_messages", u8"showDeathMessages", GameRuleValue::Boolean},
        {u8"tnt_explodes", u8"tntExplodes", GameRuleValue::Boolean},
        {u8"tnt_explosion_drop_decay", u8"tntExplosionDropDecay", GameRuleValue::Boolean},
        {u8"players_sleeping_percentage", u8"playersSleepingPercentage", GameRuleValue::Integer},
        {u8"projectiles_can_break_blocks", u8"projectilesCanBreakBlocks", GameRuleValue::Boolean},
        {u8"respawn_radius", u8"spawnRadius", GameRuleValue::Integer},
        {u8"spawner_blocks_work", u8"spawnerBlocksEnabled", GameRuleValue::Boolean},
        {u8"ender_pearls_vanish_on_death", u8"enderPearlsVanishOnDeath", GameRuleValue::Boolean},
        {u8"spawn_monsters", u8"spawnMonsters", GameRuleValue::Boolean},
        {u8"raids", u8"disableRaids", GameRuleValue::Boolean, true},
    }};

    auto legacy = Compound();
    for (auto const &mapping : mappings) {
      auto found = modern.find(std::u8string(u8"minecraft:") + std::u8string(mapping.fModern));
      if (found == modern.end()) {
        continue;
      }
      auto value = found->second;
      std::optional<i64> number;
      if (auto byte = value->asByte(); byte) {
        number = byte->fValue;
      } else if (auto integer = value->asInt(); integer) {
        number = integer->fValue;
      } else if (auto longValue = value->asLong(); longValue) {
        number = longValue->fValue;
      }
      if (!number) {
        continue;
      }

      std::u8string converted;
      switch (mapping.fValue) {
      case GameRuleValue::Boolean: {
        bool flag = *number != 0;
        if (mapping.fInvert) {
          flag = !flag;
        }
        converted = flag ? u8"true" : u8"false";
        break;
      }
      case GameRuleValue::NonZeroBoolean:
        converted = *number != 0 ? u8"true" : u8"false";
        break;
      case GameRuleValue::Integer:
        converted = mcfile::String::ToString(*number);
        break;
      }
      legacy->set(std::u8string(mapping.fLegacy), String(converted));
    }
    return legacy;
  }

  static CompoundTagPtr LegacyDragonFight(CompoundTag const &modern) {
    auto legacy = Compound();
    if (auto value = modern.byteTag(u8"dragon_killed"); value) {
      legacy->set(u8"DragonKilled", Bool(value->fValue != 0));
    }
    if (auto value = modern.byteTag(u8"previously_killed"); value) {
      legacy->set(u8"PreviouslyKilled", Bool(value->fValue != 0));
    }
    if (auto value = modern.byteTag(u8"needs_state_scanning"); value) {
      legacy->set(u8"NeedsStateScanning", Bool(value->fValue != 0));
    }
    if (auto value = modern.intArrayTag(u8"dragon_uuid"); value) {
      legacy->set(u8"Dragon", value);
    }
    if (auto value = modern.listTag(u8"gateways"); value) {
      legacy->set(u8"Gateways", value);
    }
    if (auto value = modern.intArrayTag(u8"exit_portal_location"); value && value->fValue.size() == 3) {
      auto pos = Compound();
      pos->set(u8"X", Int(value->fValue[0]));
      pos->set(u8"Y", Int(value->fValue[1]));
      pos->set(u8"Z", Int(value->fValue[2]));
      legacy->set(u8"ExitPortalLocation", pos);
    }
    return legacy;
  }

  static Status ReadInputLevelData(std::filesystem::path const &input, Options const &options, CompoundTagPtr &root) {
    namespace fs = std::filesystem;
    root = Level::Read(options.getLevelDatFilePath(input));
    if (!root) {
      return JE2BE_ERROR_WHAT("Failed to read Java level.dat");
    }
    auto level = root->compoundTag(u8"Data");
    if (!level) {
      return Status::Ok();
    }

    auto dataDir = options.getDataDirectory(input) / u8"minecraft";
    CompoundTagPtr document;
    if (auto st = ReadDataDocument(dataDir / u8"world_gen_settings.dat", document); !st.ok()) {
      return JE2BE_ERROR_PUSH(st);
    }
    if (document) {
      level->set(u8"WorldGenSettings", document);
    }

    if (auto st = ReadDataDocument(dataDir / u8"game_rules.dat", document); !st.ok()) {
      return JE2BE_ERROR_PUSH(st);
    }
    if (document) {
      level->set(u8"GameRules", LegacyGameRules(*document));
    }

    if (auto st = ReadDataDocument(dataDir / u8"weather.dat", document); !st.ok()) {
      return JE2BE_ERROR_PUSH(st);
    }
    if (document) {
      level->set(u8"rainTime", Int(document->int32(u8"rain_time", 0)));
      level->set(u8"raining", Bool(document->boolean(u8"raining", false)));
      level->set(u8"thunderTime", Int(document->int32(u8"thunder_time", 0)));
      level->set(u8"thundering", Bool(document->boolean(u8"thundering", false)));
    }

    if (auto st = ReadDataDocument(dataDir / u8"world_clocks.dat", document); !st.ok()) {
      return JE2BE_ERROR_PUSH(st);
    }
    if (document) {
      if (auto overworld = document->compoundTag(u8"minecraft:overworld"); overworld) {
        if (auto ticks = overworld->int64(u8"total_ticks"); ticks) {
          level->set(u8"DayTime", Long(*ticks));
        }
      }
    }

    auto endData = options.getWorldDirectory(input, mcfile::Dimension::End) / u8"data" / u8"minecraft";
    if (auto st = ReadDataDocument(endData / u8"ender_dragon_fight.dat", document); !st.ok()) {
      return JE2BE_ERROR_PUSH(st);
    }
    if (document) {
      level->set(u8"DragonFight", LegacyDragonFight(*document));
    }

    if (auto difficulty = level->compoundTag(u8"difficulty_settings"); difficulty) {
      static std::map<std::u8string, i8> const values = {
          {u8"peaceful", 0},
          {u8"easy", 1},
          {u8"normal", 2},
          {u8"hard", 3},
      };
      if (auto name = difficulty->string(u8"difficulty"); name) {
        if (auto found = values.find(*name); found != values.end()) {
          level->set(u8"Difficulty", Byte(found->second));
        }
      }
      level->set(u8"hardcore", Bool(difficulty->boolean(u8"hardcore", false)));
    }

    if (!level->compoundTag(u8"Player")) {
      if (auto id = level->intArrayTag(u8"singleplayer_uuid"); id) {
        if (auto uuid = Uuid::FromIntArray(*id); uuid) {
          auto playerPath = options.getPlayerDataDirectory(input) / fs::path(uuid->toString() + u8".dat");
          std::error_code ec;
          if (fs::is_regular_file(playerPath, ec) && !ec) {
            if (auto player = Level::Read(playerPath); player) {
              level->set(u8"Player", player);
            } else {
              return JE2BE_ERROR_WHAT("Failed to read Java player data: " + playerPath.string());
            }
          }
        }
      }
    }
    return Status::Ok();
  }

  static std::optional<std::string> LocalPlayerData(CompoundTag const &tag, LevelData &ld) {
    using namespace std;
    using namespace mcfile::stream;

    auto data = tag.compoundTag(u8"Data");
    if (!data) {
      return std::nullopt;
    }
    auto playerJ = data->compoundTag(u8"Player");
    if (!playerJ) {
      return std::nullopt;
    }

    WorldData wd(mcfile::Dimension::Overworld);
    Context ctx(ld.fJavaEditionMap, ld.fLodestones, ld.fUuids, wd, ld.fGameTick, ld.fDifficultyBedrock, ld.fAllowCommand, ld.fGameType);
    DataVersion dataVersion(ld.fDataVersion, ld.fDataVersion);
    auto playerB = Entity::LocalPlayer(*playerJ, ctx, dataVersion, {});
    if (!playerB) {
      return std::nullopt;
    }
    wd.drain(ld);

    LevelData::PlayerAttachedEntities pae;
    pae.fDim = playerB->fDimension;
    pae.fLocalPlayerUid = playerB->fUid;
    if (auto rootVehicle = playerJ->compoundTag(u8"RootVehicle"); rootVehicle) {
      if (auto entityJ = rootVehicle->compoundTag(u8"Entity"); entityJ) {
        if (auto entityB = Entity::From(*entityJ, ctx, dataVersion, {Entity::Flag::RootVehicle}); entityB.fEntity) {
          LevelData::VehicleAndPassengers vap;
          vap.fChunk = playerB->fChunk;
          vap.fVehicle = entityB.fEntity;
          vap.fPassengers.swap(entityB.fPassengers);
          pae.fVehicle = vap;

          auto vehicleUid = entityB.fEntity->int64(u8"UniqueID");
          if (vehicleUid) {
            playerB->fEntity->set(u8"RideID", Long(*vehicleUid));
          }
        }
      }
    }

    struct Rider {
      u8string fBedrockKey;
      int fLinkId;
      float fRotation;

      Rider(u8string const &bedrockKey, int linkId, float rotation) : fBedrockKey(bedrockKey), fLinkId(linkId), fRotation(rotation) {}
    };
    double const kDistanceToPlayer = 0.4123145064147021;        // distance between player and parrot.
    double const kAngleAgainstPlayerFacing = 104.0364421885305; // angle in degrees
    auto linksTag = List<Tag::Type::Compound>();
    static unordered_map<u8string, Rider> const keys = {
        {u8"ShoulderEntityLeft", Rider(u8"LeftShoulderRiderID", 0, -kAngleAgainstPlayerFacing)},
        {u8"ShoulderEntityRight", Rider(u8"RightShoulderPassengerID", 1, kAngleAgainstPlayerFacing)}};
    for (auto const &key : keys) {
      auto keyJ = key.first;
      auto keyB = key.second.fBedrockKey;
      auto linkId = key.second.fLinkId;
      auto angle = key.second.fRotation / 180.0f * std::numbers::pi;

      // player: [9.0087, 72.62, 1.96865] yaw: -174.737
      // riderLeft: [8.60121, 72.42, 2.03154]
      // riderRight: [9.39784, 72.42, 2.10492]
      // https://gyazo.com/e797402bff04e5c3db9bae4bd1730629
      auto shoulderEntity = playerJ->compoundTag(keyJ);
      if (!shoulderEntity) {
        continue;
      }
      auto pos = props::GetPos3f(*playerB->fEntity, u8"Pos");
      if (!pos) {
        continue;
      }
      auto rot = props::GetRotation(*playerJ, u8"Rotation");
      if (!rot) {
        continue;
      }
      float yaw = (rot->fYaw + 90.0) / 180.0 * std::numbers::pi;
      Pos2d facing = Pos2d(kDistanceToPlayer, 0).rotated(yaw);
      Pos2d rider = facing.rotated(angle);
      Pos2d riderPos2d = Pos2d(pos->fX, pos->fZ) + rider;
      Pos3f riderPos3f(riderPos2d.fX, pos->fY - 0.2, riderPos2d.fZ);
      // ground: 71 -> boat: 71.375 -> player: 72.62 -> parrot: 72.42
      // ground: 71 -> player: 72.62 -> parrot: 72.42
      if (auto riderB = Entity::From(*shoulderEntity, ctx, dataVersion, {Entity::Flag::ShoulderRider}); riderB.fEntity) {
        auto id = riderB.fEntity->int64(u8"UniqueID");
        if (id) {
          riderB.fEntity->set(u8"Pos", riderPos3f.toListTag());

          int cx = mcfile::Coordinate::ChunkFromBlock((int)floorf(riderPos3f.fX));
          int cz = mcfile::Coordinate::ChunkFromBlock((int)floorf(riderPos3f.fZ));
          Pos2i chunkPos(cx, cz);
          pae.fShoulderRiders.push_back(make_pair(chunkPos, riderB.fEntity));

          playerB->fEntity->set(keyB, Long(*id));

          auto link = Compound();
          link->set(u8"entityID", Long(*id));
          link->set(u8"linkID", Int(linkId));
          linksTag->push_back(link);
        }
      }
    }
    if (!linksTag->empty()) {
      playerB->fEntity->set(u8"LinksTag", linksTag);
    }
    if (pae.fVehicle || !pae.fShoulderRiders.empty()) {
      ld.fPlayerAttachedEntities = pae;
    }

    return CompoundTag::Write(*playerB->fEntity, mcfile::Encoding::LittleEndian);
  }

  static double GetTotalNumChunks(std::filesystem::path const &input, Options o) {
    namespace fs = std::filesystem;
    u32 num = 0;
    for (auto dim : {mcfile::Dimension::Overworld, mcfile::Dimension::Nether, mcfile::Dimension::End}) {
      auto dir = o.getWorldDirectory(input, dim) / "region";
      if (!fs::exists(dir)) {
        continue;
      }
      for (DirectoryIterator itr(dir); itr.valid(); itr.next()) {
        auto name = itr->path().filename().string();
        if (!name.starts_with("r.") || !name.ends_with(".mca")) {
          continue;
        }
        if (!itr->is_regular_file()) {
          continue;
        }
        num++;
      }
    }
    return num * 32.0 * 32.0;
  }
};

Status Converter::Run(std::filesystem::path const &input, std::filesystem::path const &output, Options const &o, int concurrency, Progress *progress) {
  return Impl::Run(input, output, o, concurrency, progress);
}

} // namespace je2be::java
