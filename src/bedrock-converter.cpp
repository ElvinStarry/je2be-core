#include <je2be/bedrock/converter.hpp>

#include <je2be/bedrock/options.hpp>
#include <je2be/bedrock/progress.hpp>
#include <je2be/nbt.hpp>
#include <je2be/strings.hpp>

#include "_data-version.hpp"
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
#include <fstream>
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

    if constexpr (kJavaDataVersion >= 4903) {
      if (auto data = levelDat->compoundTag(u8"Data"); data) {
        data->erase(u8"Player");

        auto dataDir = output / u8"data" / u8"minecraft";
        error_code ec;
        fs::create_directories(dataDir, ec);

        // Move WorldGenSettings to data/minecraft/world_gen_settings.dat
        if (auto wgs = data->compoundTag(u8"WorldGenSettings"); wgs) {
          data->erase(u8"WorldGenSettings");
          auto wgsTag = Compound();
          wgsTag->set(u8"data", wgs);
          wgsTag->set(u8"DataVersion", Int(kJavaDataVersion));
          auto s = make_shared<mcfile::stream::GzFileOutputStream>(dataDir / u8"world_gen_settings.dat");
          if (!CompoundTag::Write(*wgsTag, s, mcfile::Encoding::Java)) {
            return JE2BE_ERROR;
          }
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
          auto rulesTag = Compound();
          rulesTag->set(u8"data", rulesJ);
          rulesTag->set(u8"DataVersion", Int(kJavaDataVersion));
          auto s = make_shared<mcfile::stream::GzFileOutputStream>(dataDir / u8"game_rules.dat");
          if (!CompoundTag::Write(*rulesTag, s, mcfile::Encoding::Java)) {
            return JE2BE_ERROR;
          }
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
          auto weatherTag = Compound();
          weatherTag->set(u8"data", weather);
          weatherTag->set(u8"DataVersion", Int(kJavaDataVersion));
          auto s = make_shared<mcfile::stream::GzFileOutputStream>(dataDir / u8"weather.dat");
          if (!CompoundTag::Write(*weatherTag, s, mcfile::Encoding::Java)) {
            return JE2BE_ERROR;
          }
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
          auto clocksTag = Compound();
          clocksTag->set(u8"data", clocks);
          clocksTag->set(u8"DataVersion", Int(kJavaDataVersion));
          auto s = make_shared<mcfile::stream::GzFileOutputStream>(dataDir / u8"world_clocks.dat");
          if (!CompoundTag::Write(*clocksTag, s, mcfile::Encoding::Java)) {
            return JE2BE_ERROR;
          }
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

          auto enderDragonFightDir = output / u8"dimensions" / u8"minecraft" / u8"the_end" / u8"data" / u8"minecraft";
          Fs::CreateDirectories(enderDragonFightDir);
          auto edfTag = Compound();
          edfTag->set(u8"data", fightJ);
          edfTag->set(u8"DataVersion", Int(kJavaDataVersion));
          auto edfPath = enderDragonFightDir / u8"ender_dragon_fight.dat";
          auto edfStream = make_shared<mcfile::stream::GzFileOutputStream>(edfPath);
          if (!CompoundTag::Write(*edfTag, edfStream, mcfile::Encoding::Java)) {
            return JE2BE_ERROR;
          }
        }

        // Add singleplayer_uuid from local player
        if (resolvedLocalPlayerUuid) {
          data->set(u8"singleplayer_uuid", resolvedLocalPlayerUuid->toIntArrayTag());
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
        if constexpr (kJavaDataVersion >= 4903) {
          auto base = output / u8"dimensions" / u8"minecraft";
          switch (dim) {
          case mcfile::Dimension::Nether:
            directory = base / u8"the_nether" / u8"region";
            break;
          case mcfile::Dimension::End:
            directory = base / u8"the_end" / u8"region";
            break;
          case mcfile::Dimension::Overworld:
          default:
            directory = base / u8"overworld" / u8"region";
            break;
          }
        } else {
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
