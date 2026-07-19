#pragma once

#include <je2be/bedrock/options.hpp>
#include <je2be/pos2.hpp>
#include <je2be/status.hpp>

#include "_dimension-ext.hpp"
#include "_nbt-ext.hpp"
#include "_poi-blocks.hpp"
#include "_pos2i-set.hpp"
#include "_volume.hpp"
#include "bedrock/_block-entity-convert-result.hpp"
#include "bedrock/_constants.hpp"
#include "bedrock/_map-info.hpp"
#include "bedrock/_structure-info.hpp"
#include "color/_rgba.hpp"
#include "enums/_game-mode.hpp"
#include "item/_map-color.hpp"
#include "structure/_structure-piece.hpp"

namespace je2be::bedrock {

class Context {
  class Impl;

  Context(mcfile::Encoding encoding,
          std::filesystem::path tempDirectory,
          std::shared_ptr<MapInfo const> const &mapInfo,
          std::shared_ptr<StructureInfo const> const &structureInfo,
          i64 gameTick,
          GameMode gameMode,
          std::unordered_map<i32, std::pair<mcfile::Dimension, Pos3i>> const &lodestones)
      : fEncoding(encoding), fTempDirectory(tempDirectory), fGameTick(gameTick), fGameMode(gameMode), fMapInfo(mapInfo), fStructureInfo(structureInfo), fLodestones(lodestones) {}

public:
  struct ChunksInRegion {
    Pos2iSet fChunks;
  };

  struct PlayerData {
    std::string fKey;
    CompoundTagPtr fEntity;

    bool isLocal() const {
      return fKey == "~local_player";
    }
  };

  static Status Init(std::filesystem::path const &dbname,
                     Options opt,
                     mcfile::Encoding encoding,
                     std::map<mcfile::Dimension, std::vector<std::pair<Pos2i, ChunksInRegion>>> &regions,
                     u64 &totalChunks,
                     i64 gameTick,
                     GameMode gameMode,
                     unsigned int concurrency,
                     std::vector<PlayerData> &players,
                     std::unique_ptr<Context> &out);

  void markMapUuidAsUsed(i64 uuid);
  void mergeInto(Context &other) const;
  Status postProcess(std::filesystem::path root, mcfile::be::DbInterface &db) const;
  std::optional<MapInfo::Map> mapFromUuid(i64 mapUuid) const;
  void structures(mcfile::Dimension d, Pos2i chunk, std::vector<StructureInfo::Structure> &buffer);
  std::shared_ptr<Context> make() const;
  void addPlayerMapping(i64 entityIdB, Uuid const &entityIdJ);
  void setLocalPlayerIds(i64 entityIdB, Uuid const &entityIdJ);
  std::optional<Uuid> mapPlayerId(i64 entityIdB) const;
  std::optional<i64> localPlayerId() const;
  Uuid mapEntityId(i64 entityIdB) const;
  bool isPlayerId(Uuid const &uuid) const;
  bool isLocalPlayerId(Uuid const &uuid) const;
  void setRootVehicleForPlayer(i64 playerIdB, Uuid const &vehicleUid);
  void setRootVehicle(Uuid const &vehicleUid);
  void setRootVehicleEntity(Uuid const &vehicleUid, CompoundTagPtr const &vehicleEntity);
  void promoteRootVehicles();
  bool isRootVehicle(Uuid const &uuid) const;
  std::optional<std::pair<Uuid, CompoundTagPtr>> drainRootVehicle(i64 playerIdB);
  void setShoulderEntityLeft(i64 playerIdB, i64 uid);
  void setShoulderEntityRight(i64 playerIdB, i64 uid);
  void setShoulderEntityLeft(i64 uid);
  void setShoulderEntityRight(i64 uid);
  bool setShoulderEntityIfItIs(i64 uid, CompoundTagPtr entityB);
  void drainShoulderEntities(i64 playerIdB, CompoundTagPtr &left, CompoundTagPtr &right);
  void addToPoiIfItIs(mcfile::Dimension dim, Pos3i const &pos, mcfile::je::Block const &block);
  std::optional<std::pair<mcfile::Dimension, Pos3i>> getLodestone(i32 trackingHandle) const;

private:
  Status exportMaps(std::filesystem::path const &root, mcfile::be::DbInterface &db) const;
  Status exportPoi(std::filesystem::path const &root) const;

public:
  mcfile::Encoding const fEncoding;
  std::filesystem::path const fTempDirectory;

  struct VehicleEntity {
    Pos2i fChunk;
    std::map<size_t, Uuid> fPassengers;
  };
  std::unordered_map<Uuid, VehicleEntity, UuidHasher, UuidPred> fVehicleEntities;

  struct LeashedEntity {
    Pos2i fChunk;
    i64 fLeasherId;
  };
  std::unordered_map<Uuid, LeashedEntity, UuidHasher, UuidPred> fLeashedEntities;

  std::unordered_map<i64, Pos3i> fLeashKnots;
  std::unordered_map<Uuid, Pos2i, UuidHasher, UuidPred> fEntities;

  i64 const fGameTick;
  GameMode const fGameMode;

  bool fDataPackTradeRebalance = false;

private:
  std::shared_ptr<MapInfo const> fMapInfo;
  std::shared_ptr<StructureInfo const> fStructureInfo;
  std::unordered_set<i64> fUsedMapUuids;
  std::unordered_map<i64, Uuid> fPlayerIds;

  struct LocalPlayer {
    i64 fBedrockId;
    Uuid fJavaId;
  };
  std::optional<LocalPlayer> fLocalPlayer;

  struct RootVehicle {
    Uuid fAttach;
    Uuid fRoot;
    CompoundTagPtr fVehicle;
  };
  std::unordered_map<i64, RootVehicle> fRootVehicles;

  struct ShoulderEntities {
    std::optional<i64> fLeftId;
    std::optional<i64> fRightId;
    CompoundTagPtr fLeft;
    CompoundTagPtr fRight;
  };
  std::unordered_map<i64, ShoulderEntities> fShoulderEntities;

  std::unordered_map<mcfile::Dimension, PoiBlocks> fPoiBlocks;
  std::unordered_map<i32, std::pair<mcfile::Dimension, Pos3i>> const fLodestones;
};

} // namespace je2be::bedrock
