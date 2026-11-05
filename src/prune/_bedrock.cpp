#include "prune/_bedrock.hpp"

#include "prune/_selection.hpp"

#include <je2be/nbt.hpp>

#include <minecraft-file.hpp>

#include <leveldb/db.h>
#include <leveldb/env.h>
#include <leveldb/iterator.h>
#include <leveldb/options.h>
#include <leveldb/write_batch.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <charconv>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace je2be::prune {

namespace {

namespace fs = std::filesystem;

Status Err(std::string const &what) {
  return JE2BE_ERROR_WHAT(what);
}

int32_t ReadI32LE(std::string const &s, size_t offset) {
  unsigned char const *p = reinterpret_cast<unsigned char const *>(s.data()) + offset;
  uint32_t v = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
  return static_cast<int32_t>(v);
}

bool IsPrintableAscii(std::string const &s) {
  for (unsigned char c : s) {
    if (c < 0x20 || 0x7e < c) {
      return false;
    }
  }
  return !s.empty();
}

bool IsChunkRecordTag(uint8_t tag) {
  if (0x2b <= tag && tag <= 0x41) {
    return true;
  }
  return tag == 0x76 || tag == 0x77;
}

bool IsStableChunkTag(uint8_t tag) {
  switch (tag) {
  case 0x2b: // Data3D
  case 0x2c: // Version
  case 0x2d: // Data2D
  case 0x2f: // SubChunk
  case 0x76: // VersionLegacy
  case 0x77:
    return true;
  default:
    return false;
  }
}

std::optional<CanonicalDimension> FromBedrockDimension(int32_t d) {
  switch (d) {
  case 0:
    return CanonicalDimension::Overworld;
  case 1:
    return CanonicalDimension::Nether;
  case 2:
    return CanonicalDimension::End;
  default:
    return std::nullopt;
  }
}

bool BedrockDimensionMatches(int32_t bedrockDimension, CanonicalDimension target) {
  switch (target) {
  case CanonicalDimension::Nether:
    return bedrockDimension == 1;
  case CanonicalDimension::End:
    return bedrockDimension == 2;
  case CanonicalDimension::Overworld:
  default:
    return bedrockDimension == 0;
  }
}


// Matches only the chunk-key byte shapes documented by the Bedrock level
// format, and only tags that belong to chunk-scoped records. All global ASCII
// keys are filtered by IsBedrockGlobalKey before this is called.
bool LooksLikeChunkKeyShape(std::string const &key, int32_t &cx, int32_t &cz, CanonicalDimension &dimension, uint8_t &tag) {
  size_t const n = key.size();
  if (n < 9 || IsPrintableAscii(key)) {
    return false;
  }
  switch (n) {
  case 9: {
    cx = ReadI32LE(key, 0);
    cz = ReadI32LE(key, 4);
    tag = static_cast<uint8_t>(key[8]);
    dimension = CanonicalDimension::Overworld;
    return true;
  }
  case 10: {
    if (static_cast<uint8_t>(key[8]) != 0x2f) {
      return false;
    }
    cx = ReadI32LE(key, 0);
    cz = ReadI32LE(key, 4);
    tag = 0x2f;
    dimension = CanonicalDimension::Overworld;
    return true;
  }
  case 13: {
    int32_t const d = ReadI32LE(key, 8);
    if (d != 1 && d != 2) {
      return false;
    }
    cx = ReadI32LE(key, 0);
    cz = ReadI32LE(key, 4);
    tag = static_cast<uint8_t>(key[12]);
    dimension = d == 1 ? CanonicalDimension::Nether : CanonicalDimension::End;
    return true;
  }
  case 14: {
    int32_t const d = ReadI32LE(key, 8);
    if ((d != 1 && d != 2) || static_cast<uint8_t>(key[12]) != 0x2f) {
      return false;
    }
    cx = ReadI32LE(key, 0);
    cz = ReadI32LE(key, 4);
    tag = 0x2f;
    dimension = d == 1 ? CanonicalDimension::Nether : CanonicalDimension::End;
    return true;
  }
  case 17: {
    if (static_cast<uint8_t>(key[8]) != 0x78) {
      return false;
    }
    cx = ReadI32LE(key, 0);
    cz = ReadI32LE(key, 4);
    tag = 0x78;
    dimension = CanonicalDimension::Overworld;
    return true;
  }
  case 21: {
    int32_t const d = ReadI32LE(key, 8);
    if ((d != 1 && d != 2) || static_cast<uint8_t>(key[12]) != 0x78) {
      return false;
    }
    cx = ReadI32LE(key, 0);
    cz = ReadI32LE(key, 4);
    tag = 0x78;
    dimension = d == 1 ? CanonicalDimension::Nether : CanonicalDimension::End;
    return true;
  }
  default:
    return false;
  }
}

std::string TrimAsciiWhitespace(std::string s) {
  auto isSpace = [](unsigned char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
  };
  size_t begin = 0;
  while (begin < s.size() && isSpace(static_cast<unsigned char>(s[begin]))) {
    begin++;
  }
  size_t end = s.size();
  while (begin < end && isSpace(static_cast<unsigned char>(s[end - 1]))) {
    end--;
  }
  return s.substr(begin, end - begin);
}

// Bedrock clients do not lock db/LOCK; they keep db/<MANIFEST> locked while
// the world is open. Lock the manifest before opening LevelDB so a running
// game cannot be modified just because db/LOCK happened to be free.
class BedrockManifestLock {
public:
  ~BedrockManifestLock() {
    if (fLock) {
      leveldb::Env::Default()->UnlockFile(fLock);
      fLock = nullptr;
    }
  }

  Status lock(fs::path const &dbDir) {
    std::ifstream ifs(dbDir / "CURRENT", std::ios::binary);
    if (!ifs) {
      return Err("cannot read " + (dbDir / "CURRENT").string());
    }
    std::string name;
    std::getline(ifs, name);
    name = TrimAsciiWhitespace(name);
    if (name.empty() || name.find('/') != std::string::npos || name.find('\\') != std::string::npos || name.find("..") != std::string::npos) {
      return Err("invalid Bedrock CURRENT manifest name in " + dbDir.string());
    }
    fs::path manifest = dbDir / name;
    std::error_code ec;
    bool const isFile = fs::is_regular_file(manifest, ec);
    if (ec) {
      return Err("cannot inspect Bedrock manifest " + manifest.string() + ": " + ec.message());
    }
    if (!isFile) {
      return Err("Bedrock manifest " + manifest.string() + " does not exist");
    }
    leveldb::Status st = leveldb::Env::Default()->LockFile(manifest, &fLock);
    if (!st.ok()) {
      return Err("Bedrock database " + dbDir.string() + " is in use (manifest lock): " + st.ToString());
    }
    return Status::Ok();
  }

private:
  leveldb::FileLock *fLock = nullptr;
};

Status ReadMapProtection(leveldb::DB *db, CanonicalDimension target, MapProtection &protection, uint64_t &mapRecords) {
  leveldb::ReadOptions ro;
  ro.fill_cache = false;
  std::unique_ptr<leveldb::Iterator> it(db->NewIterator(ro));
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    std::string key = it->key().ToString();
    if (!key.starts_with("map_")) {
      continue;
    }
    // Only map_<decimal id> is a map record. Keys such as "map_bad" belong to
    // the global namespace and must be preserved instead of interpreted.
    int64_t mapId = 0;
    {
      std::string const suffix = key.substr(4);
      if (suffix.empty()) {
        continue;
      }
      auto parsed = std::from_chars(suffix.data(), suffix.data() + suffix.size(), mapId);
      if (parsed.ec != std::errc() || parsed.ptr != suffix.data() + suffix.size()) {
        continue;
      }
      (void)mapId;
    }
    mapRecords++;
    leveldb::Slice value = it->value();
    auto tag = CompoundTag::Read(value.ToString(), mcfile::Encoding::LittleEndian);
    if (!tag) {
      return Err("cannot read Bedrock map record " + key);
    }
    auto dimension = tag->byte(u8"dimension");
    if (!dimension) {
      return Err("Bedrock map record " + key + " is missing the dimension byte");
    }
    auto canonical = FromBedrockDimension(*dimension);
    if (!canonical) {
      return Err("Bedrock map record " + key + " has an unsupported dimension " + std::to_string(static_cast<int>(*dimension)));
    }
    if (*canonical != target) {
      continue;
    }
    auto scale = tag->byte(u8"scale");
    auto xCenter = tag->int32(u8"xCenter");
    auto zCenter = tag->int32(u8"zCenter");
    if (!scale) {
      return Err("Bedrock map record " + key + " is missing the scale byte");
    }
    if (!xCenter || !zCenter) {
      return Err("Bedrock map record " + key + " is missing xCenter or zCenter");
    }
    ChunkRect rect;
    Status st = MapRectFromCenter(*xCenter, *zCenter, *scale, rect);
    if (!st.ok()) {
      return JE2BE_ERROR_PUSH(st);
    }
    protection.add(rect);
  }
  if (!it->status().ok()) {
    return Err("LevelDB iteration failed while reading map records: " + it->status().ToString());
  }
  return Status::Ok();
}

} // namespace

bool IsBedrockGlobalKey(std::string const &key) {
  static std::unordered_set<std::string> const sExact = {
      "BiomeData",
      "Overworld",
      "Nether",
      "TheEnd",
      "mobevents",
      "scoreboard",
      "~local_player",
      "portals",
      "AutonomousEntities",
      "LevelChunkMetaDataDictionary",
      "WorldClocks",
      "PositionTrackDB-LastId",
  };
  if (sExact.find(key) != sExact.end()) {
    return true;
  }
  static char const *const sPrefixes[] = {
      "map_",
      "player_",
      "VILLAGE_",
      "PosTrackDB-",
      "PositionTrackDB-",
      "structuretemplate_",
      "RealmsStoriesData_",
      "actorprefix",
      "digp",
  };
  for (char const *prefix : sPrefixes) {
    if (key.starts_with(prefix)) {
      return true;
    }
  }
  return false;
}

bool ParseBedrockChunkKey(std::string const &key, BedrockChunkKey &out) {
  if (IsBedrockGlobalKey(key)) {
    return false;
  }
  int32_t cx = 0;
  int32_t cz = 0;
  CanonicalDimension dimension;
  uint8_t tag = 0;
  if (!LooksLikeChunkKeyShape(key, cx, cz, dimension, tag)) {
    return false;
  }
  bool structure = tag == 0x78;
  if (!structure && !IsChunkRecordTag(tag)) {
    return false;
  }
  out.fCx = cx;
  out.fCz = cz;
  out.fTag = tag;
  out.fDimension = dimension;
  out.fSubChunk = tag == 0x2f;
  out.fStructure = structure;
  return true;
}

bool ParseBedrockDigpKey(std::string const &key, int32_t &cx, int32_t &cz, int32_t &bedrockDimension) {
  if (key.size() == 12 && key.starts_with("digp")) {
    cx = ReadI32LE(key, 4);
    cz = ReadI32LE(key, 8);
    bedrockDimension = 0;
    return true;
  }
  if (key.size() == 16 && key.starts_with("digp")) {
    cx = ReadI32LE(key, 4);
    cz = ReadI32LE(key, 8);
    bedrockDimension = ReadI32LE(key, 12);
    return true;
  }
  return false;
}

Status PruneBedrockWorld(std::filesystem::path const &input, Config const &config, Action fallback, BedrockPruneResult &result) {
  namespace fs = std::filesystem;

  std::optional<CanonicalDimension> target = CanonicalDimensionFromInt(config.fDimension);
  if (!target) {
    return Err("unsupported dimension in config");
  }
  fs::path worldDir = input;
  fs::path dbDir = worldDir / "db";
  std::error_code ec;
  if (!fs::is_directory(dbDir, ec)) {
    return Err(worldDir.string() + " is not a Bedrock world directory (db/ not found)");
  }
  if (!fs::is_regular_file(worldDir / "level.dat", ec) || ec) {
    return Err(worldDir.string() + " is not a Bedrock world directory (level.dat not found)");
  }

  BedrockManifestLock manifestLock;
  Status lockStatus = manifestLock.lock(dbDir);
  if (!lockStatus.ok()) {
    return JE2BE_ERROR_PUSH(lockStatus);
  }

  leveldb::Options options;
  options.create_if_missing = false;
  options.compression = leveldb::kZlibRawCompression;
  leveldb::DB *raw = nullptr;
  leveldb::Status openStatus = leveldb::DB::Open(options, dbDir, &raw);
  if (!openStatus.ok()) {
    return Err("cannot open Bedrock database " + dbDir.string() + " (stop the world before pruning): " + openStatus.ToString());
  }
  std::unique_ptr<leveldb::DB> db(raw);

  MapProtection protection;
  Status st = ReadMapProtection(db.get(), *target, protection, result.fMapRecords);
  if (!st.ok()) {
    return JE2BE_ERROR_PUSH(st);
  }

  Selection selection;
  selection.fConfig = &config;
  selection.fFallback = fallback;
  selection.fMaps = &protection;

  // Pass 1: validate and index. Nothing is written here.
  std::unordered_set<Pos2i, Pos2iHasher> existing;
  std::unordered_map<std::string, int> actorRefs;
  leveldb::ReadOptions ro;
  ro.fill_cache = false;
  {
    std::unique_ptr<leveldb::Iterator> it(db->NewIterator(ro));
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      result.fTotalKeys++;
      std::string key = it->key().ToString();

      int32_t dx = 0;
      int32_t dz = 0;
      int32_t digpDimension = 0;
      if (ParseBedrockDigpKey(key, dx, dz, digpDimension)) {
        leveldb::Slice value = it->value();
        if (value.size() % 8 != 0) {
          return Err("Bedrock digp record " + key + " has an invalid actor id list length");
        }
        size_t const count = value.size() / 8;
        for (size_t i = 0; i < count; i++) {
          actorRefs[std::string(value.data() + i * 8, 8)]++;
        }
        if (BedrockDimensionMatches(digpDimension, *target)) {
          existing.insert(Pos2i(dx, dz));
        }
        continue;
      }
      if (key.starts_with("actorprefix")) {
        continue;
      }
      if (IsBedrockGlobalKey(key)) {
        continue;
      }
      BedrockChunkKey chunkKey;
      if (ParseBedrockChunkKey(key, chunkKey)) {
        if (chunkKey.fDimension == *target && IsStableChunkTag(chunkKey.fTag)) {
          existing.insert(Pos2i(chunkKey.fCx, chunkKey.fCz));
        }
        continue;
      }
      int32_t cx = 0;
      int32_t cz = 0;
      CanonicalDimension dimension;
      uint8_t tag = 0;
      if (LooksLikeChunkKeyShape(key, cx, cz, dimension, tag)) {
        result.fUnknownChunkShapedKeys++;
      }
    }
    if (!it->status().ok()) {
      return Err("LevelDB iteration failed while indexing: " + it->status().ToString());
    }
  }
  result.fExistingChunks = existing.size();

  std::unordered_set<Pos2i, Pos2iHasher> deleted;
  for (Pos2i const &p : existing) {
    if (selection.resolve(p.fX, p.fZ) == Action::Delete) {
      deleted.insert(p);
    } else if (!config.lastMatch(p.fX, p.fZ) && protection.protects(p.fX, p.fZ)) {
      result.fProtectedChunks++;
    }
  }
  result.fDeletedChunks = deleted.size();
  if (deleted.empty()) {
    return Status::Ok();
  }

  // Pass 2: delete. Writes are batched; each batch is atomic but a failure
  // partway through leaves the world partially pruned.
  leveldb::WriteBatch batch;
  size_t pending = 0;
  auto flush = [&]() -> Status {
    if (pending == 0) {
      return Status::Ok();
    }
    leveldb::Status ws = db->Write(leveldb::WriteOptions(), &batch);
    if (!ws.ok()) {
      return Err("LevelDB write failed: " + ws.ToString());
    }
    batch.Clear();
    pending = 0;
    return Status::Ok();
  };
  auto enqueueDelete = [&](std::string const &key) -> Status {
    batch.Delete(key);
    pending++;
    if (8192 <= pending) {
      return flush();
    }
    return Status::Ok();
  };

  {
    std::unique_ptr<leveldb::Iterator> it(db->NewIterator(ro));
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      std::string key = it->key().ToString();

      int32_t dx = 0;
      int32_t dz = 0;
      int32_t digpDimension = 0;
      if (ParseBedrockDigpKey(key, dx, dz, digpDimension)) {
        if (!BedrockDimensionMatches(digpDimension, *target) || deleted.find(Pos2i(dx, dz)) == deleted.end()) {
          continue;
        }
        leveldb::Slice value = it->value();
        for (size_t i = 0; i + 8 <= value.size(); i += 8) {
          std::string actor(value.data() + i, 8);
          auto found = actorRefs.find(actor);
          if (found == actorRefs.end()) {
            continue;
          }
          found->second--;
          if (found->second <= 0) {
            st = enqueueDelete("actorprefix" + actor);
            if (!st.ok()) {
              return JE2BE_ERROR_PUSH(st);
            }
            result.fDeletedActorPrefixes++;
          }
        }
        st = enqueueDelete(key);
        if (!st.ok()) {
          return JE2BE_ERROR_PUSH(st);
        }
        result.fDeletedDigpKeys++;
        continue;
      }
      if (key.starts_with("actorprefix")) {
        // Deleted only through the digp reference count above.
        continue;
      }
      if (IsBedrockGlobalKey(key)) {
        continue;
      }
      // Once a coordinate is anchored by a stable chunk record, every key
      // that has a chunk-key shape at that coordinate is chunk-scoped data,
      // including tags this build does not know about.
      int32_t cx = 0;
      int32_t cz = 0;
      CanonicalDimension dimension;
      uint8_t tag = 0;
      if (!LooksLikeChunkKeyShape(key, cx, cz, dimension, tag)) {
        continue;
      }
      if (dimension != *target) {
        continue;
      }
      if (deleted.find(Pos2i(cx, cz)) == deleted.end()) {
        continue;
      }
      st = enqueueDelete(key);
      if (!st.ok()) {
        return JE2BE_ERROR_PUSH(st);
      }
      result.fDeletedChunkKeys++;
    }
    if (!it->status().ok()) {
      return Err("LevelDB iteration failed while deleting: " + it->status().ToString());
    }
  }
  st = flush();
  if (!st.ok()) {
    return JE2BE_ERROR_PUSH(st);
  }
  return Status::Ok();
}

} // namespace je2be::prune
