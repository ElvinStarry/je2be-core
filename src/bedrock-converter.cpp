#include <je2be/bedrock/converter.hpp>

#include <je2be/bedrock/options.hpp>
#include <je2be/bedrock/progress.hpp>
#include <je2be/nbt.hpp>
#include <je2be/strings.hpp>

#include <defer.hpp>

#include "_data-version.hpp"
#include "_props.hpp"
#include "_walk.hpp"
#include "_world-data-override.hpp"
#include "bedrock/_context.hpp"
#include "bedrock/_java-chunk.hpp"
#include "bedrock/_java-player.hpp"
#include "bedrock/_level-data.hpp"
#include "bedrock/_terraform-region-scheduler.hpp"
#include "bedrock/_world.hpp"
#include "db/_readonly-db.hpp"
#include "enums/_game-mode.hpp"
#include "terraform/_leaves.hpp"
#include "terraform/java/_block-accessor-java-directory.hpp"
#include "terraform/lighting/_lighting.hpp"

#include <atomic>
#include <exception>
#include <fstream>

using namespace std;
namespace fs = std::filesystem;

namespace je2be::bedrock {

class Converter::Impl {
  struct ResolvedPlayer {
    Context::PlayerData fSource;
    i64 fBedrockId;
    Uuid fJavaId;
  };

  struct PlayerListEntry {
    std::string fBedrockUuid;
    Uuid fJavaId;
    bool fIsReal;
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
    vector<PlayerListEntry> playerList;
    if (auto st = ResolvePlayers(players, options, *bin, effectiveOptions, resolvedPlayers, playerList); !st.ok()) {
      return JE2BE_ERROR_PUSH(st);
    }

    if (!PrepareOutputDirectory(output)) {
      return JE2BE_ERROR;
    }
    if (auto st = WritePlayerList(output / "player_list.csv", playerList); !st.ok()) {
      return JE2BE_ERROR_PUSH(st);
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

    atomic_uint64_t numTerraformedChunks(0);
    if (total > 0 && progress && !progress->reportTerraform({0, total}, 0)) {
      return JE2BE_ERROR;
    }

    for (Dimension d : {Dimension::Overworld, Dimension::Nether, Dimension::End}) {
      if (!effectiveOptions.fDimensionFilter.empty()) {
        if (effectiveOptions.fDimensionFilter.find(d) == effectiveOptions.fDimensionFilter.end()) {
          continue;
        }
      }
      auto foundRegions = regions.find(d);
      if (foundRegions == regions.end() || foundRegions->second.empty()) {
        continue;
      }
      auto createdTempDir = File::CreateTempDir(effectiveOptions.getTempDirectory());
      if (!createdTempDir) {
        return JE2BE_ERROR;
      }
      fs::path terrainTempDir = *createdTempDir;
      defer {
        Fs::DeleteAll(terrainTempDir);
      };

      auto &regionsInDimension = foundRegions->second;
      vector<Pos2i> regionPositions;
      regionPositions.reserve(regionsInDimension.size());
      unordered_map<Pos2i, Context::ChunksInRegion const *, Pos2iHasher> chunksByRegion;
      chunksByRegion.reserve(regionsInDimension.size());
      for (auto const &[region, chunks] : regionsInDimension) {
        regionPositions.push_back(region);
        chunksByRegion.emplace(region, &chunks);
      }
      TerraformRegionScheduler scheduler(regionPositions);
      fs::path terrainOutputDir = TerrainOutputDirectory(output, d);

      auto regionConverted = [&scheduler, &chunksByRegion, terrainTempDir, terrainOutputDir, d, &numTerraformedChunks, total, progress](Pos2i const &converted) -> Status {
        auto ready = scheduler.markConverted(converted);
        if (!ready) {
          return JE2BE_ERROR_WHAT("Invalid Terraform conversion state for region [" + std::to_string(converted.fX) + ", " + std::to_string(converted.fZ) + "]");
        }
        for (Pos2i const &region : *ready) {
          auto chunks = chunksByRegion.find(region);
          if (chunks == chunksByRegion.end() || !chunks->second) {
            return JE2BE_ERROR_WHAT("Missing chunks for Terraform region [" + std::to_string(region.fX) + ", " + std::to_string(region.fZ) + "]");
          }
          if (auto st = TerraformRegion(region, *chunks->second, terrainOutputDir, terrainTempDir, d, numTerraformedChunks, total, progress); !st.ok()) {
            return JE2BE_ERROR_PUSH(st);
          }

          auto releasable = scheduler.markCompleted(region);
          if (!releasable) {
            return JE2BE_ERROR_WHAT("Invalid Terraform completion state for region [" + std::to_string(region.fX) + ", " + std::to_string(region.fZ) + "]");
          }
          for (Pos2i const &source : *releasable) {
            auto name = mcfile::je::Region::GetDefaultRegionFileName(source.fX, source.fZ);
            auto file = terrainTempDir / name;
            error_code ec;
            bool removed = fs::remove(file, ec);
            if (ec || !removed) {
              string why = ec ? ec.message() : "file does not exist";
              return JE2BE_ERROR_WHAT("Failed to release temporary region " + file.string() + ": " + why);
            }
          }
        }
        return Status::Ok();
      };

      shared_ptr<Context> result;
      if (auto st = World::Convert(d, regionsInDimension, *db, output, concurrency, *bin, result, reportProgress, numConvertedChunks, terrainTempDir, regionConverted); !st.ok()) {
        return JE2BE_ERROR_PUSH(st);
      }
      if (!scheduler.allCompleted()) {
        return JE2BE_ERROR_WHAT("Terraform did not complete all regions in dimension " + std::to_string(static_cast<int>(d)));
      }
      if (result) {
        result->mergeInto(*bin);
      }
      if (cancelRequested.load()) {
        return JE2BE_ERROR;
      }
    }

    if (numTerraformedChunks.load() != total) {
      return JE2BE_ERROR_WHAT("Terraform processed " + std::to_string(numTerraformedChunks.load()) + " of " + std::to_string(total) + " chunks");
    }
    if (total > 0 && progress && !progress->reportTerraform({total, total}, total)) {
      return JE2BE_ERROR;
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

    WorldDataOverrideEngine::JavaDocuments worldData;
    worldData.fLevel = levelDat->compoundTag(u8"Data");
    if (!worldData.fLevel) {
      worldData.fLevel = Compound();
      levelDat->set(u8"Data", worldData.fLevel);
    }

    if constexpr (kJavaDataVersion >= 4903) {
      if (auto data = levelDat->compoundTag(u8"Data"); data) {
        data->erase(u8"Player");

        // Move WorldGenSettings to data/minecraft/world_gen_settings.dat
        if (auto wgs = data->compoundTag(u8"WorldGenSettings"); wgs) {
          data->erase(u8"WorldGenSettings");
          worldData.fWorldGenSettings = wgs;
        }

        // Remove GameRules from level.dat and write to data/minecraft/game_rules.dat
        if (auto gameRules = data->compoundTag(u8"GameRules"); gameRules) {
          data->erase(u8"GameRules");

          auto rulesJ = Compound();
          for (auto const &kv : *gameRules) {
            auto name = kv.first;
            auto valueTag = kv.second;
            auto str = valueTag->asString();
            if (!str) {
              continue;
            }
            bool boolVal = str->fValue == u8"true";
            i32 intVal = 0;
            bool isInt = false;
            if (!boolVal && str->fValue != u8"false") {
              auto i = strings::ToI32(str->fValue);
              if (i) {
                intVal = *i;
                isInt = true;
              } else {
                continue;
              }
            }

            // Map old Java names to 26.2 names
            std::optional<std::u8string> newName;
            bool invert = false;
            if (name == u8"announceAdvancements") newName = u8"show_advancement_messages";
            else if (name == u8"commandBlockOutput") newName = u8"command_block_output";
            else if (name == u8"commandBlocksEnabled") newName = u8"command_blocks_work";
            else if (name == u8"doDaylightCycle") newName = u8"advance_time";
            else if (name == u8"doEntityDrops") newName = u8"entity_drops";
            else if (name == u8"doFireTick") { if (!isInt) { intVal = boolVal ? 128 : 0; isInt = true; } newName = u8"fire_spread_radius_around_player"; }
            else if (name == u8"doImmediateRespawn") newName = u8"immediate_respawn";
            else if (name == u8"doInsomnia") newName = u8"spawn_phantoms";
            else if (name == u8"doLimitedCrafting") newName = u8"limited_crafting";
            else if (name == u8"doMobLoot") newName = u8"mob_drops";
            else if (name == u8"doMobSpawning") newName = u8"spawn_mobs";
            else if (name == u8"doPatrolSpawning") newName = u8"spawn_patrols";
            else if (name == u8"doTileDrops") newName = u8"block_drops";
            else if (name == u8"doTraderSpawning") newName = u8"spawn_wandering_traders";
            else if (name == u8"doWardenSpawning") newName = u8"spawn_wardens";
            else if (name == u8"doWeatherCycle") newName = u8"advance_weather";
            else if (name == u8"drowningDamage") newName = u8"drowning_damage";
            else if (name == u8"fallDamage") newName = u8"fall_damage";
            else if (name == u8"fireDamage") newName = u8"fire_damage";
            else if (name == u8"freezeDamage") newName = u8"freeze_damage";
            else if (name == u8"keepInventory") newName = u8"keep_inventory";
            else if (name == u8"locatorBar") newName = u8"locator_bar";
            else if (name == u8"maxCommandChainLength") newName = u8"max_command_sequence_length";
            else if (name == u8"mobGriefing") newName = u8"mob_griefing";
            else if (name == u8"naturalRegeneration") newName = u8"natural_health_regeneration";
            else if (name == u8"pvp") newName = u8"pvp";
            else if (name == u8"randomTickSpeed") newName = u8"random_tick_speed";
            else if (name == u8"reducedDebugInfo") newName = u8"reduced_debug_info";
            else if (name == u8"sendCommandFeedback") newName = u8"send_command_feedback";
            else if (name == u8"showDeathMessages") newName = u8"show_death_messages";
            else if (name == u8"tntExplodes") newName = u8"tnt_explodes";
            else if (name == u8"tntExplosionDropDecay") newName = u8"tnt_explosion_drop_decay";
            else if (name == u8"maxCommandChainLength") newName = u8"max_command_sequence_length";
            else if (name == u8"playersSleepingPercentage") newName = u8"players_sleeping_percentage";
            else if (name == u8"projectilesCanBreakBlocks") newName = u8"projectiles_can_break_blocks";
            else if (name == u8"spawnRadius") newName = u8"respawn_radius";
            else if (name == u8"spawnerBlocksEnabled") newName = u8"spawner_blocks_work";
            else if (name == u8"enderPearlsVanishOnDeath") newName = u8"ender_pearls_vanish_on_death";
            else if (name == u8"spawnMonsters") newName = u8"spawn_monsters";
            else if (name == u8"disableElytraMovementCheck") { newName = u8"elytra_movement_check"; invert = true; }
            else if (name == u8"disablePlayerMovementCheck") { newName = u8"player_movement_check"; invert = true; }
            else if (name == u8"disableRaids") { newName = u8"raids"; invert = true; }
            else if (name == u8"allowFireTicksAwayFromPlayer") { if (!isInt) { intVal = boolVal ? -1 : 128; isInt = true; } newName = u8"fire_spread_radius_around_player"; }

            if (newName) {
              if (isInt) {
                rulesJ->set(u8"minecraft:" + *newName, Int(intVal));
              } else {
                rulesJ->set(u8"minecraft:" + *newName, Bool(invert ? !boolVal : boolVal));
              }
            }
          }
          worldData.fGameRules = rulesJ;
        }

        // Move weather data to data/minecraft/weather.dat
        {
          auto weather = Compound();
          weather->set(u8"rain_time", Int(data->int32(u8"rainTime", 0)));
          weather->set(u8"raining", Bool(data->boolean(u8"raining", false)));
          weather->set(u8"thundering", Bool(data->boolean(u8"thundering", false)));
          weather->set(u8"thunder_time", Int(data->int32(u8"thunderTime", 0)));
          weather->set(u8"clear_weather_time", Int(0));
          data->erase(u8"rainTime");
          data->erase(u8"raining");
          data->erase(u8"thunderTime");
          data->erase(u8"thundering");
          worldData.fWeather = weather;
        }

        // Move time data to data/minecraft/world_clocks.dat
        {
          auto time = data->int64(u8"Time", 0);
          // auto dayTime = data->int64(u8"DayTime", 0);   // not exist
          auto clocks = Compound();
          auto overworld = Compound();
          overworld->set(u8"total_ticks", Long(time));
          clocks->set(u8"minecraft:overworld", overworld);
          auto end = Compound();
          end->set(u8"total_ticks", Long(time));
          clocks->set(u8"minecraft:the_end", end);
          auto nether = Compound();
          nether->set(u8"total_ticks", Long(time));
          clocks->set(u8"minecraft:the_nether", nether);
          data->erase(u8"DayTime");
          worldData.fWorldClocks = clocks;
        }

        // Convert Difficulty/hardcore to difficulty_settings
        {
          auto ds = Compound();
          auto difficultyName = [&]() -> std::u8string {
            switch (data->byte(u8"Difficulty", 2)) {
            case 0: return u8"peaceful";
            case 1: return u8"easy";
            case 2: return u8"normal";
            case 3: return u8"hard";
            default: return u8"normal";
            }
          }();
          ds->set(u8"difficulty", difficultyName);
          ds->set(u8"hardcore", Bool(data->boolean(u8"hardcore", false)));
          ds->set(u8"locked", Bool(false));
          data->set(u8"difficulty_settings", ds);
          data->erase(u8"Difficulty");
          data->erase(u8"hardcore");
        }

        // Move DragonFight to dimensions/minecraft/the_end/data/minecraft/ender_dragon_fight.dat
        if (auto dragonFight = data->compoundTag(u8"DragonFight"); dragonFight) {
          data->erase(u8"DragonFight");

          auto fightJ = Compound();

          CopyBoolValues(*dragonFight, *fightJ, {{u8"DragonKilled", u8"dragon_killed"}, {u8"PreviouslyKilled", u8"previously_killed"}});
          fightJ->set(u8"needs_state_scanning", Bool(dragonFight->boolean(u8"NeedsStateScanning", false)));

          if (auto dragonUuid = dragonFight->intArrayTag(u8"Dragon"); dragonUuid) {
            fightJ->set(u8"dragon_uuid", dragonUuid);
          } else {
            fightJ->set(u8"needs_state_scanning", Bool(true));
          }

          if (auto gateways = dragonFight->listTag(u8"Gateways"); gateways) {
            auto gw = List<Tag::Type::Int>();
            for (auto const &it : *gateways) {
              auto v = it->asInt();
              if (v) {
                gw->push_back(Int(v->fValue));
              }
            }
            fightJ->set(u8"gateways", gw);
          }

          if (auto exitPortal = dragonFight->compoundTag(u8"ExitPortalLocation"); exitPortal) {
            Pos3i pos(exitPortal->int32(u8"X", 0), exitPortal->int32(u8"Y", 0), exitPortal->int32(u8"Z", 0));
            fightJ->set(u8"exit_portal_location", IntArrayFromPos3i(pos));
          }

          fightJ->set(u8"respawn_time", Int(0));

          worldData.fEnderDragonFight = fightJ;
        }

        // Add singleplayer_uuid from local player
        if (resolvedLocalPlayerUuid) {
          data->set(u8"singleplayer_uuid", resolvedLocalPlayerUuid->toIntArrayTag());
        }
      }
    }

    if constexpr (kJavaDataVersion < 4903) {
      worldData.fGameRules = worldData.fLevel->compoundTag(u8"GameRules");
      worldData.fWorldGenSettings = worldData.fLevel->compoundTag(u8"WorldGenSettings");
    }

    if (auto st = WorldDataOverrideEngine::ApplyJava(worldData, options.fWorldDataOverrides); !st.ok()) {
      return JE2BE_ERROR_PUSH(st);
    }

    if constexpr (kJavaDataVersion < 4903) {
      if (worldData.fGameRules) {
        worldData.fLevel->set(u8"GameRules", worldData.fGameRules);
      }
      if (worldData.fWorldGenSettings) {
        worldData.fLevel->set(u8"WorldGenSettings", worldData.fWorldGenSettings);
      }
    }

    if constexpr (kJavaDataVersion >= 4903) {
      auto writeDataFile = [](CompoundTagPtr const &data, fs::path const &path) -> bool {
        if (!data) {
          return true;
        }
        auto root = Compound();
        root->set(u8"data", data);
        root->set(u8"DataVersion", Int(kJavaDataVersion));
        auto stream = make_shared<mcfile::stream::GzFileOutputStream>(path);
        return CompoundTag::Write(*root, stream, mcfile::Encoding::Java);
      };

      auto dataDir = output / u8"data" / u8"minecraft";
      error_code ec;
      fs::create_directories(dataDir, ec);
      if (ec) {
        return JE2BE_ERROR_WHAT(ec.message());
      }
      if (!writeDataFile(worldData.fWorldGenSettings, dataDir / u8"world_gen_settings.dat") ||
          !writeDataFile(worldData.fGameRules, dataDir / u8"game_rules.dat") ||
          !writeDataFile(worldData.fWeather, dataDir / u8"weather.dat") ||
          !writeDataFile(worldData.fWorldClocks, dataDir / u8"world_clocks.dat")) {
        return JE2BE_ERROR;
      }

      if (worldData.fEnderDragonFight) {
        auto enderDragonFightDir = output / u8"dimensions" / u8"minecraft" / u8"the_end" / u8"data" / u8"minecraft";
        if (!Fs::CreateDirectories(enderDragonFightDir)) {
          return JE2BE_ERROR;
        }
        if (!writeDataFile(worldData.fEnderDragonFight, enderDragonFightDir / u8"ender_dragon_fight.dat")) {
          return JE2BE_ERROR;
        }
      }
    }

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
      auto playerDataDirectory = [&]() -> fs::path {
        if constexpr (kJavaDataVersion >= 4903) {
          return output / u8"players" / u8"data";
        } else {
          return output / "playerdata";
        }
      }();
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
                               std::vector<ResolvedPlayer> &resolvedPlayers,
                               std::vector<PlayerListEntry> &playerList) {
    struct LookupResult {
      std::optional<Uuid> fUuid;
      std::string fError;
    };
    struct Mapping {
      Uuid fJavaId;
      bool fIsReal;
    };

    std::map<std::u8string, LookupResult> cache;
    std::map<i64, Mapping> mappings;
    std::vector<ResolvedPlayer> candidates;
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
        LookupResult result;
        try {
          result.fUuid = JavaPlayer::ResolveUuid(*name, options);
        } catch (std::exception const &e) {
          result.fError = e.what();
        } catch (...) {
          result.fError = "unknown error";
        }
        found = cache.emplace(key, std::move(result)).first;
      }
      if (!found->second.fUuid) {
        if (!player.isLocal()) {
          continue;
        }
        std::string const javaName(reinterpret_cast<char const *>(name->data()), name->size());
        std::string what = "Failed to resolve Java UUID for JavaTag player '" + javaName + "'";
        if (!found->second.fError.empty()) {
          what += ": " + found->second.fError;
        }
        return JE2BE_ERROR_WHAT(what);
      }
      Uuid uuid = *found->second.fUuid;
      auto mapped = mappings.find(*bedrockId);
      if (mapped != mappings.end() && !UuidPred{}(mapped->second.fJavaId, uuid)) {
        return JE2BE_ERROR_WHAT("Conflicting Java UUIDs for Bedrock player UniqueID " + std::to_string(*bedrockId));
      }
      mappings.insert_or_assign(*bedrockId, Mapping{uuid, true});
      candidates.push_back({player, *bedrockId, uuid});
    }

    std::unordered_set<Uuid, UuidHasher, UuidPred> usedJavaIds;
    for (auto const &[_, mapping] : mappings) {
      usedJavaIds.insert(mapping.fJavaId);
    }

    playerList.clear();
    constexpr std::string_view serverPlayerPrefix = "player_server_";
    for (auto const &player : players) {
      if (!player.fKey.starts_with(serverPlayerPrefix) ||
          player.fKey.size() == serverPlayerPrefix.size()) {
        continue;
      }
      auto bedrockId = player.fEntity->int64(u8"UniqueID");
      if (!bedrockId) {
        continue;
      }
      auto mapped = mappings.find(*bedrockId);
      if (mapped == mappings.end()) {
        Uuid generated;
        do {
          generated = Uuid::Gen();
        } while (usedJavaIds.contains(generated));
        usedJavaIds.insert(generated);
        mapped = mappings.emplace(*bedrockId, Mapping{generated, false}).first;
        candidates.push_back({player, *bedrockId, generated});
      }
      playerList.push_back({player.fKey.substr(serverPlayerPrefix.size()),
                            mapped->second.fJavaId,
                            mapped->second.fIsReal});
    }

    for (auto const &[bedrockId, mapping] : mappings) {
      ctx.addPlayerMapping(bedrockId, mapping.fJavaId);
    }

    std::map<std::u8string, ResolvedPlayer> selectedPlayers;
    for (auto const &player : candidates) {
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
      if (mapped == mappings.end()) {
        if (!options.fLocalPlayer) {
          continue;
        }
        auto key = options.fLocalPlayer->toString();
        auto selected = selectedPlayers.find(key);
        if (selected != selectedPlayers.end() && selected->second.fBedrockId != *bedrockId) {
          std::string const javaId(reinterpret_cast<char const *>(key.data()), key.size());
          return JE2BE_ERROR_WHAT("Java UUID " + javaId + " is assigned to multiple Bedrock player UniqueIDs");
        }
        ctx.addPlayerMapping(*bedrockId, *options.fLocalPlayer);
      } else {
        effectiveOptions.fLocalPlayer = std::make_shared<Uuid const>(mapped->second.fJavaId);
        selectedPlayers[mapped->second.fJavaId.toString()] = {player, *bedrockId, mapped->second.fJavaId};
      }
    }

    resolvedPlayers.clear();
    for (auto const &[_, player] : selectedPlayers) {
      resolvedPlayers.push_back(player);
    }
    return Status::Ok();
  }

  static Status WritePlayerList(std::filesystem::path const &path, std::vector<PlayerListEntry> const &players) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
      return JE2BE_ERROR_WHAT("Failed to create player list: " + path.string());
    }
    stream << "uuid0,uuid1,isReal\n";
    for (auto const &player : players) {
      auto javaId = player.fJavaId.toString();
      std::string const javaIdString(reinterpret_cast<char const *>(javaId.data()), javaId.size());
      stream << player.fBedrockUuid << ',' << javaIdString << ',' << (player.fIsReal ? "true" : "false") << '\n';
    }
    if (!stream) {
      return JE2BE_ERROR_WHAT("Failed to write player list: " + path.string());
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

  static fs::path TerrainOutputDirectory(fs::path const &output, mcfile::Dimension dim) {
    if constexpr (kJavaDataVersion >= 4903) {
      auto base = output / u8"dimensions" / u8"minecraft";
      switch (dim) {
      case mcfile::Dimension::Nether:
        return base / u8"the_nether" / u8"region";
      case mcfile::Dimension::End:
        return base / u8"the_end" / u8"region";
      case mcfile::Dimension::Overworld:
      default:
        return base / u8"overworld" / u8"region";
      }
    } else {
      switch (dim) {
      case mcfile::Dimension::Nether:
        return output / "DIM-1" / "region";
      case mcfile::Dimension::End:
        return output / "DIM1" / "region";
      case mcfile::Dimension::Overworld:
      default:
        return output / "region";
      }
    }
  }

  static Status TerraformRegion(
      Pos2i region,
      Context::ChunksInRegion const &chunksInRegion,
      fs::path const &outputDirectory,
      fs::path const &inputDirectory,
      mcfile::Dimension dim,
      atomic_uint64_t &done,
      u64 numChunks,
      Progress *progress) {
    int const rx = region.fX;
    int const rz = region.fZ;
    auto name = mcfile::je::Region::GetDefaultRegionFileName(rx, rz);
    auto mcaIn = inputDirectory / name;
    auto mcaOut = outputDirectory / name;

    error_code ec;
    if (!fs::is_regular_file(mcaIn, ec) || ec) {
      return JE2BE_ERROR_WHAT("Missing temporary region: " + mcaIn.string());
    }
    auto editor = mcfile::je::McaEditor::Open(mcaIn);
    if (!editor) {
      return JE2BE_ERROR_WHAT("Failed to open temporary region: " + mcaIn.string());
    }

    vector<Pos2i> chunks;
    chunks.reserve(chunksInRegion.fChunks.size());
    for (Pos2i const &chunk : chunksInRegion.fChunks) {
      chunks.push_back(chunk);
    }
    sort(chunks.begin(), chunks.end(), [](Pos2i const &a, Pos2i const &b) {
      return a.fZ == b.fZ ? a.fX < b.fX : a.fZ < b.fZ;
    });

    terraform::lighting::LightCache lightCache(rx, rz);
    shared_ptr<terraform::java::BlockAccessorJavaDirectory<3, 3>> blockAccessor;
    for (Pos2i const &chunk : chunks) {
      if (auto st = TerraformChunk(chunk.fX, chunk.fZ, *editor, inputDirectory, blockAccessor, dim, lightCache); !st.ok()) {
        return JE2BE_ERROR_PUSH(st);
      }
      lightCache.dispose(chunk.fX - 1, chunk.fZ - 1);

      u64 count = done.fetch_add(1) + 1;
      if (progress && !progress->reportTerraform({count, numChunks}, count)) {
        return JE2BE_ERROR_WHAT("Terraform cancelled");
      }
    }

    string writeError;
    if (!editor->write(mcaOut, &writeError)) {
      return JE2BE_ERROR_WHAT(writeError);
    }
    return Status::Ok();
  }

  static Status TerraformChunk(
      int cx,
      int cz,
      mcfile::je::McaEditor &editor,
      fs::path const &inputDirectory,
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

    if (!EnsureJavaChunkSections(*current)) {
      return JE2BE_ERROR_WHAT("Invalid sections tag in Java chunk [" + std::to_string(cx) + ", " + std::to_string(cz) + "]");
    }
    auto ch = mcfile::je::Chunk::MakeChunk(cx, cz, current);
    if (!ch) {
      return JE2BE_ERROR_WHAT("Failed to parse Java chunk [" + std::to_string(cx) + ", " + std::to_string(cz) + "]");
    }
    blockAccessor->set(ch);

    // WritableChunk only reads the source NBT. Sharing it avoids a full deep
    // copy while keeping independent parsed chunks for the neighbor cache.
    auto writable = mcfile::je::WritableChunk::MakeChunk(cx, cz, current);
    if (!writable) {
      return JE2BE_ERROR_WHAT("Failed to create writable Java chunk [" + std::to_string(cx) + ", " + std::to_string(cz) + "]");
    }

    terraform::BlockPropertyAccessorJava propertyAccessor(*ch);
    terraform::Leaves::Do(*writable, *blockAccessor, propertyAccessor);
    terraform::lighting::Lighting::Do(dim, *writable, *blockAccessor, lightCache);

    auto tag = writable->toCompoundTag(dim);
    if (!tag) {
      return JE2BE_ERROR_WHAT("Failed to serialize Java chunk [" + std::to_string(cx) + ", " + std::to_string(cz) + "]");
    }
    if (!EnsureJavaChunkSections(*tag)) {
      return JE2BE_ERROR_WHAT("Invalid sections tag in Java chunk [" + std::to_string(cx) + ", " + std::to_string(cz) + "]");
    }
    if (!editor.insert(x, z, *tag)) {
      return JE2BE_ERROR_WHAT("Failed to update Java chunk [" + std::to_string(cx) + ", " + std::to_string(cz) + "]");
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
