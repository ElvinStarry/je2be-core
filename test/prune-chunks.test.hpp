#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <random>
#include <sstream>

#include <leveldb/db.h>
#include <leveldb/options.h>
#include <leveldb/write_batch.h>

#include "prune/_bedrock.hpp"
#include "prune/_config.hpp"
#include "prune/_java.hpp"
#include "prune/_selection.hpp"

namespace {

std::filesystem::path MakePruneTempDir(char const *tag) {
  std::random_device rd;
  std::mt19937 engine(rd());
  std::uniform_int_distribution<uint64_t> dist;
  std::filesystem::path dir = std::filesystem::temp_directory_path() / ("je2be-prune-" + std::string(tag) + "-" + std::to_string(dist(engine)));
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  return dir;
}

std::string PruneConfigJson(std::string const &regions, char const *defaultAction = "keep") {
  std::string json = R"({
    "schemaVersion": 1,
    "format": "mbr-chunk-selection",
    "dimension": 0,
    "chunkSize": 16,
    "defaultAction": "__DEFAULT__",
    "overlapPolicy": "last-wins",
    "createdAt": "2026-11-05T15:23:30.065Z",
    "source": {
      "archiveSha256": "5777a930073a4e142e10ceb029deaa7296688faf439aecae46885a57ca2e5ffd",
      "rulesVersion": "test",
      "datasetCreatedAt": "2026-11-05T13:30:38+00:00"
    },
    "regions": [__REGIONS__]
  })";
  json.replace(json.find("__DEFAULT__"), 11, defaultAction);
  json.replace(json.find("__REGIONS__"), 11, regions);
  return json;
}

je2be::prune::Config ParseTestConfig(std::string const &json) {
  je2be::prune::Config config;
  auto status = je2be::prune::Config::ParseJson(json, config);
  REQUIRE(status.ok());
  return config;
}

std::string SerializeLittleEndian(CompoundTag const &tag) {
  auto stream = std::make_shared<mcfile::stream::ByteStream>();
  REQUIRE(CompoundTag::Write(tag, stream, mcfile::Encoding::LittleEndian));
  std::string out;
  stream->drain(out);
  return out;
}

void WriteFile(std::filesystem::path const &path, std::string const &bytes) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
  REQUIRE(ofs.good());
  ofs.write(bytes.data(), bytes.size());
  REQUIRE(ofs.good());
}

std::string ReadFile(std::filesystem::path const &path) {
  std::ifstream ifs(path, std::ios::binary);
  REQUIRE(ifs.good());
  std::ostringstream ss;
  ss << ifs.rdbuf();
  return ss.str();
}

void PutU32BE(std::string &s, size_t offset, uint32_t value) {
  s[offset + 0] = static_cast<char>((value >> 24) & 0xff);
  s[offset + 1] = static_cast<char>((value >> 16) & 0xff);
  s[offset + 2] = static_cast<char>((value >> 8) & 0xff);
  s[offset + 3] = static_cast<char>(value & 0xff);
}

void PutI32LE(std::string &s, size_t offset, int32_t value) {
  uint32_t v = static_cast<uint32_t>(value);
  s[offset + 0] = static_cast<char>(v & 0xff);
  s[offset + 1] = static_cast<char>((v >> 8) & 0xff);
  s[offset + 2] = static_cast<char>((v >> 16) & 0xff);
  s[offset + 3] = static_cast<char>((v >> 24) & 0xff);
}

std::string StructureKey(int32_t cx, int32_t cz, uint64_t id, std::optional<int32_t> dimension) {
  std::string key;
  key.resize(dimension ? 21 : 17);
  PutI32LE(key, 0, cx);
  PutI32LE(key, 4, cz);
  if (dimension) {
    PutI32LE(key, 8, *dimension);
    key[12] = static_cast<char>(0x78);
    for (int i = 0; i < 8; i++) {
      key[13 + i] = static_cast<char>((id >> (i * 8)) & 0xff);
    }
  } else {
    key[8] = static_cast<char>(0x78);
    for (int i = 0; i < 8; i++) {
      key[9 + i] = static_cast<char>((id >> (i * 8)) & 0xff);
    }
  }
  return key;
}

} // namespace

TEST_CASE("prune-chunks-config") {
  SUBCASE("valid") {
    auto config = ParseTestConfig(PruneConfigJson(R"(
      {"action":"keep","minCx":-13,"maxCx":-11,"minCz":-11,"maxCz":-10},
      {"action":"delete","minCx":-12,"maxCx":-11,"minCz":-11,"maxCz":-9},
      {"action":"keep","minCx":-13,"maxCx":-12,"minCz":-11,"maxCz":-10}
    )"));
    CHECK(config.lastMatch(-12, -10) == je2be::prune::Action::Keep);
    CHECK(config.lastMatch(-11, -11) == je2be::prune::Action::Delete);
    CHECK(config.lastMatch(0, 0) == std::nullopt);
  }
  SUBCASE("rejects-invalid") {
    using je2be::prune::Config;
    Config config;
    CHECK(!Config::ParseJson("{}", config).ok());
    CHECK(!Config::ParseJson(R"({"schemaVersion":2})", config).ok());
    CHECK(!Config::ParseJson(PruneConfigJson("", "bogus"), config).ok());
    CHECK(!Config::ParseJson(PruneConfigJson(R"({"action":"bogus","minCx":0,"maxCx":0,"minCz":0,"maxCz":0})"), config).ok());
    CHECK(!Config::ParseJson(PruneConfigJson(R"({"action":"keep","minCx":1,"maxCx":0,"minCz":0,"maxCz":0})"), config).ok());
    CHECK(!Config::ParseJson(PruneConfigJson(R"({"action":"keep","minCx":0.5,"maxCx":1,"minCz":0,"maxCz":0})"), config).ok());
    CHECK(!Config::ParseJson(PruneConfigJson(R"({"action":"keep","minCx":0,"maxCx":1,"minCz":0,"maxCz":0})", "keep").substr(0, 10), config).ok());
    // Values that nlohmann types as uint64 must not wrap into the int32 range.
    CHECK(!Config::ParseJson(R"({"schemaVersion":1,"format":"mbr-chunk-selection","dimension":0,"chunkSize":16,"overlapPolicy":"last-wins","regions":[{"action":"keep","minCx":18446744073709551615,"maxCx":0,"minCz":0,"maxCz":0}]})", config).ok());
    CHECK(!Config::ParseJson(R"({"schemaVersion":18446744073709551615,"format":"mbr-chunk-selection","dimension":0,"chunkSize":16,"overlapPolicy":"last-wins","regions":[]})", config).ok());
    CHECK(!Config::ParseJson(R"({"schemaVersion":1,"format":"mbr-chunk-selection","dimension":18446744073709551615,"chunkSize":16,"overlapPolicy":"last-wins","regions":[]})", config).ok());
    CHECK(!Config::ParseJson(R"({"schemaVersion":1,"format":"mbr-chunk-selection","dimension":0,"chunkSize":16,"overlapPolicy":"last-wins","regions":[{"action":"keep","minCx":-9223372036854775808,"maxCx":0,"minCz":0,"maxCz":0}]})", config).ok());
  }
  SUBCASE("metadata-is-optional") {
    using je2be::prune::Config;
    Config config;
    CHECK(Config::ParseJson(R"({"schemaVersion":1,"format":"mbr-chunk-selection","dimension":0,"chunkSize":16,"overlapPolicy":"last-wins","regions":[{"action":"delete","minCx":0,"maxCx":0,"minCz":0,"maxCz":0}],"createdAt":null,"source":null})", config).ok());
    CHECK(config.lastMatch(0, 0) == je2be::prune::Action::Delete);
    CHECK(config.lastMatch(1, 1) == std::nullopt);
  }
}

TEST_CASE("prune-chunks-selection") {
  using namespace je2be::prune;
  SUBCASE("map-rect") {
    ChunkRect rect;
    REQUIRE(MapRectFromCenter(0, 0, 0, rect).ok());
    CHECK(rect.fMinCx == -4);
    CHECK(rect.fMaxCx == 3);
    CHECK(rect.fMinCz == -4);
    CHECK(rect.fMaxCz == 3);
    REQUIRE(MapRectFromCenter(-100, -100, 0, rect).ok());
    CHECK(rect.fMinCx == -11);
    CHECK(rect.fMaxCx == -3);
    REQUIRE(MapRectFromCenter(960, 960, 4, rect).ok());
    CHECK(rect.fMinCx == -4);
    CHECK(rect.fMaxCx == 123);
    CHECK(!MapRectFromCenter(0, 0, 5, rect).ok());
  }
  SUBCASE("precedence") {
    auto config = ParseTestConfig(PruneConfigJson(R"(
      {"action":"delete","minCx":0,"maxCx":10,"minCz":0,"maxCz":10},
      {"action":"keep","minCx":5,"maxCx":20,"minCz":5,"maxCz":20}
    )"));
    MapProtection protection;
    ChunkRect rect;
    REQUIRE(MapRectFromCenter(16, 16, 1, rect).ok());
    protection.add(rect);
    Selection selection;
    selection.fConfig = &config;
    selection.fMaps = &protection;
    selection.fFallback = Action::Delete;
    CHECK(selection.resolve(0, 0) == Action::Delete);   // explicit delete
    CHECK(selection.resolve(10, 10) == Action::Keep);   // last matching keep
    CHECK(selection.resolve(-1, 0) == Action::Keep);     // map protection beats --default delete
    CHECK(selection.resolve(-100, -100) == Action::Delete); // fallback
    CHECK(protection.protects(-1, 0));
    CHECK(!protection.protects(-100, -100));
  }
}

TEST_CASE("prune-chunks-java-world") {
  using namespace je2be::prune;
  auto dir = MakePruneTempDir("java");
  WriteFile(dir / "level.dat", "");
  fs::create_directories(dir / "region");
  fs::create_directories(dir / "entities");
  fs::create_directories(dir / "poi");

  auto makeTag = [](int x, int z) {
    auto tag = std::make_shared<CompoundTag>();
    tag->set(u8"xPos", je2be::Int(x));
    tag->set(u8"zPos", je2be::Int(z));
    return tag;
  };
  {
    auto editor = mcfile::je::McaEditor::Open(dir / "region" / "r.0.0.mca");
    REQUIRE(editor);
    REQUIRE(editor->insert(0, 0, *makeTag(0, 0)));
    REQUIRE(editor->insert(1, 0, *makeTag(1, 0)));
    REQUIRE(editor->insert(0, 1, *makeTag(0, 1)));
    REQUIRE(editor->write(dir / "region" / "r.0.0.mca"));
  }
  {
    auto editor = mcfile::je::McaEditor::Open(dir / "entities" / "r.0.0.mca");
    REQUIRE(editor);
    REQUIRE(editor->insert(0, 0, *makeTag(0, 0)));
    REQUIRE(editor->write(dir / "entities" / "r.0.0.mca"));
  }
  {
    auto editor = mcfile::je::McaEditor::Open(dir / "poi" / "r.0.0.mca");
    REQUIRE(editor);
    REQUIRE(editor->insert(0, 0, *makeTag(0, 0)));
    REQUIRE(editor->write(dir / "poi" / "r.0.0.mca"));
  }
  {
    // A stored overworld map centered at block (16, 0) with scale 0 covers
    // chunk (1, 0), so that chunk must survive --default delete.
    auto data = std::make_shared<CompoundTag>();
    data->set(u8"dimension", je2be::String(u8"minecraft:overworld"));
    data->set(u8"xCenter", je2be::Int(16));
    data->set(u8"zCenter", je2be::Int(0));
    data->set(u8"scale", je2be::Byte(0));
    auto root = std::make_shared<CompoundTag>();
    root->set(u8"data", data);
    root->set(u8"DataVersion", je2be::Int(4903));
    fs::create_directories(dir / "data");
    auto stream = std::make_shared<mcfile::stream::GzFileOutputStream>(dir / "data" / "map_0.dat");
    REQUIRE(CompoundTag::Write(*root, stream, mcfile::Encoding::Java));
    stream.reset();
  }

  auto config = ParseTestConfig(PruneConfigJson(R"(
    {"action":"delete","minCx":0,"maxCx":0,"minCz":0,"maxCz":1}
  )", "delete"));
  JavaWorldLocation location;
  REQUIRE(LocateJavaWorld(dir, CanonicalDimension::Overworld, location).ok());
  JavaPruneResult result;
  je2be::Status javaStatus = PruneJavaWorld(location, config, Action::Delete, result);
  if (!javaStatus.ok() && javaStatus.error()) {
    INFO("PruneJavaWorld: " + javaStatus.error()->fWhat);
  }
  REQUIRE(javaStatus.ok());
  CHECK(result.fMapFiles == 1);
  CHECK(result.fExistingChunks == 3);
  CHECK(result.fDeletedChunks == 2);
  CHECK(result.fKeptByMapProtection == 1);

  auto terrain = mcfile::je::Region::MakeRegion(dir / "region" / "r.0.0.mca", 0, 0);
  REQUIRE(terrain);
  CHECK(!terrain->chunkExists(0, 0));
  CHECK(!terrain->chunkExists(0, 1));
  CHECK(terrain->chunkExists(1, 0));
  {
    auto editor = mcfile::je::McaEditor::Open(dir / "region" / "r.0.0.mca");
    REQUIRE(editor);
    auto kept = editor->get(1, 0);
    REQUIRE(kept);
    CHECK(kept->int32(u8"xPos") == 1);
    CHECK(!editor->get(0, 0));
  }
  auto entities = mcfile::je::Region::MakeRegion(dir / "entities" / "r.0.0.mca", 0, 0);
  REQUIRE(entities);
  CHECK(!entities->chunkExists(0, 0));
  auto poi = mcfile::je::Region::MakeRegion(dir / "poi" / "r.0.0.mca", 0, 0);
  REQUIRE(poi);
  CHECK(!poi->chunkExists(0, 0));
  CHECK(fs::is_regular_file(dir / "session.lock"));

  fs::remove_all(dir);
}

TEST_CASE("prune-chunks-java-external-chunk") {
  using namespace je2be::prune;
  auto dir = MakePruneTempDir("java-mcc");
  WriteFile(dir / "level.dat", "");
  fs::create_directories(dir / "region");
  fs::path regionPath = dir / "region" / "r.0.0.mca";

  auto tag = std::make_shared<CompoundTag>();
  tag->set(u8"xPos", je2be::Int(1));
  {
    auto editor = mcfile::je::McaEditor::Open(regionPath);
    REQUIRE(editor);
    REQUIRE(editor->insert(1, 0, *tag));
    REQUIRE(editor->write(regionPath));
  }
  std::string bytes = ReadFile(regionPath);
  REQUIRE(bytes.size() == 8192 + 4096);
  // Add an external chunk at local (0, 0), sector 3.
  PutU32BE(bytes, 0, (3u << 8) | 1u);
  bytes.resize(8192 + 4096 * 2, '\0');
  PutU32BE(bytes, 8192 + 4096, 1u); // payload length == 1
  bytes[8192 + 4096 + 4] = static_cast<char>(0x82); // zlib | external flag
  WriteFile(regionPath, bytes);
  WriteFile(dir / "region" / "c.0.0.mcc", std::string(1024, '\x5a'));

  auto config = ParseTestConfig(PruneConfigJson(R"(
    {"action":"delete","minCx":0,"maxCx":0,"minCz":0,"maxCz":0}
  )"));
  JavaWorldLocation location;
  REQUIRE(LocateJavaWorld(dir, CanonicalDimension::Overworld, location).ok());
  JavaPruneResult result;
  REQUIRE(PruneJavaWorld(location, config, Action::Keep, result).ok());
  CHECK(result.fDeletedChunks == 1);
  CHECK(result.fDeletedExternalFiles == 1);
  CHECK(!fs::exists(dir / "region" / "c.0.0.mcc"));

  auto region = mcfile::je::Region::MakeRegion(regionPath, 0, 0);
  REQUIRE(region);
  CHECK(!region->chunkExists(0, 0));
  CHECK(region->chunkExists(1, 0));
  {
    auto editor = mcfile::je::McaEditor::Open(regionPath);
    REQUIRE(editor);
    auto kept = editor->get(1, 0);
    REQUIRE(kept);
    CHECK(kept->int32(u8"xPos") == 1);
    CHECK(!editor->get(0, 0));
  }

  fs::remove_all(dir);
}

TEST_CASE("prune-chunks-bedrock-world") {
  using namespace je2be::prune;
  auto dir = MakePruneTempDir("bedrock");
  WriteFile(dir / "level.dat", "");
  fs::create_directories(dir / "db");
  fs::path dbDir = dir / "db";

  auto mapTag = std::make_shared<CompoundTag>();
  mapTag->set(u8"mapId", je2be::Long(1));
  mapTag->set(u8"dimension", je2be::Byte(0));
  mapTag->set(u8"scale", je2be::Byte(0));
  mapTag->set(u8"xCenter", je2be::Int(16));
  mapTag->set(u8"zCenter", je2be::Int(0));
  std::string const mapValue = SerializeLittleEndian(*mapTag);

  auto deletedVillageInfo = std::make_shared<CompoundTag>();
  deletedVillageInfo->set(u8"X0", je2be::Int(1600));
  deletedVillageInfo->set(u8"X1", je2be::Int(1615));
  deletedVillageInfo->set(u8"Z0", je2be::Int(1600));
  deletedVillageInfo->set(u8"Z1", je2be::Int(1615));
  std::string const deletedVillageValue = SerializeLittleEndian(*deletedVillageInfo);
  auto keptVillageInfo = std::make_shared<CompoundTag>();
  keptVillageInfo->set(u8"X0", je2be::Int(16));
  keptVillageInfo->set(u8"X1", je2be::Int(31));
  keptVillageInfo->set(u8"Z0", je2be::Int(0));
  keptVillageInfo->set(u8"Z1", je2be::Int(15));
  std::string const keptVillageValue = SerializeLittleEndian(*keptVillageInfo);

  auto actorValue = SerializeLittleEndian(*std::make_shared<CompoundTag>());
  std::string const actorA("AAAAAAA1", 8);
  std::string const actorB("BBBBBBB2", 8);
  std::string const actorC("CCCCCCC3", 8);
  std::string const actorD("DDDDDDD4", 8);

  std::string digp00;
  digp00.append(actorA);
  digp00.append(actorB);
  digp00.append(actorD);
  std::string digp10 = actorB;
  std::string digp100 = actorC;
  // An actor referenced by a digp in an unknown dimension must keep its
  // actorprefix even when its dimension-0 digp is deleted.
  std::string unknownDimensionDigp = "digp";
  unknownDimensionDigp.resize(16);
  PutI32LE(unknownDimensionDigp, 4, 0);
  PutI32LE(unknownDimensionDigp, 8, 0);
  PutI32LE(unknownDimensionDigp, 12, 3);
  std::string unknownDimensionValue = actorD;

  std::string unknownTagKey(9, '\0');
  PutI32LE(unknownTagKey, 0, 0);
  PutI32LE(unknownTagKey, 4, 0);
  unknownTagKey[8] = static_cast<char>(0x50);

  {
    leveldb::Options options;
    options.create_if_missing = true;
    leveldb::DB *raw = nullptr;
    REQUIRE(leveldb::DB::Open(options, dbDir, &raw).ok());
    std::unique_ptr<leveldb::DB> db(raw);
    auto put = [&](std::string const &key, std::string const &value) {
      REQUIRE(db->Put(leveldb::WriteOptions(), key, value).ok());
    };
    put("map_1", mapValue);
    for (int i = 0; i < 3; i++) {
      int32_t cx = i == 0 ? 0 : (i == 1 ? 1 : 100);
      int32_t cz = i == 0 ? 0 : (i == 1 ? 0 : 100);
      put(mcfile::be::DbKey::SubChunk(cx, 0, cz, mcfile::Dimension::Overworld), "sub");
      put(mcfile::be::DbKey::Version(cx, cz, mcfile::Dimension::Overworld), "ver");
      put(mcfile::be::DbKey::Data3D(cx, cz, mcfile::Dimension::Overworld), "d3d");
      put(mcfile::be::DbKey::Data2D(cx, cz, mcfile::Dimension::Overworld), "d2d");
      put(StructureKey(cx, cz, 7, std::nullopt), "structure");
    }
    put(mcfile::be::DbKey::Digp(0, 0, mcfile::Dimension::Overworld), digp00);
    put(mcfile::be::DbKey::Digp(1, 0, mcfile::Dimension::Overworld), digp10);
    put(mcfile::be::DbKey::Digp(100, 100, mcfile::Dimension::Overworld), digp100);
    put(mcfile::be::DbKey::Actorprefix(actorA), actorValue);
    put(mcfile::be::DbKey::Actorprefix(actorB), actorValue);
    put(mcfile::be::DbKey::Actorprefix(actorC), actorValue);
    put(mcfile::be::DbKey::Actorprefix(actorD), actorValue);
    put(unknownDimensionDigp, unknownDimensionValue);
    put("mobevents", "mob");
    put("scoreboard", "sc");
    put("BiomeData", "bd");
    put("PosTrackDB-0x00000001", "pt");
    put(unknownTagKey, "unknown");
    put("VILLAGE_Overworld_00000000-0000-0000-0000-000000000001_INFO", deletedVillageValue);
    put("VILLAGE_Overworld_00000000-0000-0000-0000-000000000001_DWELLERS", "dwellers");
    put("VILLAGE_Overworld_00000000-0000-0000-0000-000000000002_INFO", keptVillageValue);
    put("VILLAGE_Overworld_00000000-0000-0000-0000-000000000002_DWELLERS", "dwellers");
  }
  auto config = ParseTestConfig(PruneConfigJson(R"(
    {"action":"delete","minCx":0,"maxCx":0,"minCz":0,"maxCz":1}
  )", "delete"));
  BedrockPruneResult result;
  je2be::Status bedrockStatus = PruneBedrockWorld(dir, config, Action::Delete, result);
  if (!bedrockStatus.ok() && bedrockStatus.error()) {
    INFO("PruneBedrockWorld: " + bedrockStatus.error()->fWhat);
  }
  REQUIRE(bedrockStatus.ok());
  CHECK(result.fMapRecords == 1);
  CHECK(result.fExistingChunks == 3);
  CHECK(result.fDeletedChunks == 2);
  CHECK(result.fProtectedChunks == 1);

  {
    leveldb::Options options;
    options.create_if_missing = false;
    leveldb::DB *raw = nullptr;
    REQUIRE(leveldb::DB::Open(options, dbDir, &raw).ok());
    std::unique_ptr<leveldb::DB> db(raw);
    auto exists = [&](std::string const &key) {
      std::string value;
      return db->Get(leveldb::ReadOptions(), key, &value).ok();
    };
    CHECK(!exists(mcfile::be::DbKey::Version(0, 0, mcfile::Dimension::Overworld)));
    CHECK(!exists(mcfile::be::DbKey::SubChunk(0, 0, 0, mcfile::Dimension::Overworld)));
    CHECK(!exists(mcfile::be::DbKey::Data3D(0, 1, mcfile::Dimension::Overworld)));
    CHECK(!exists(mcfile::be::DbKey::Version(100, 100, mcfile::Dimension::Overworld)));
    CHECK(!exists(StructureKey(0, 0, 7, std::nullopt)));
    CHECK(!exists(StructureKey(100, 100, 7, std::nullopt)));
    CHECK(!exists(unknownTagKey));
    CHECK(exists(mcfile::be::DbKey::Version(1, 0, mcfile::Dimension::Overworld)));
    CHECK(exists(StructureKey(1, 0, 7, std::nullopt)));
    CHECK(exists(mcfile::be::DbKey::Digp(1, 0, mcfile::Dimension::Overworld)));
    CHECK(!exists(mcfile::be::DbKey::Digp(0, 0, mcfile::Dimension::Overworld)));
    CHECK(!exists(mcfile::be::DbKey::Digp(100, 100, mcfile::Dimension::Overworld)));
    CHECK(!exists(mcfile::be::DbKey::Actorprefix(actorA)));
    CHECK(exists(mcfile::be::DbKey::Actorprefix(actorB)));
    CHECK(!exists(mcfile::be::DbKey::Actorprefix(actorC)));
    CHECK(exists(mcfile::be::DbKey::Actorprefix(actorD)));
    CHECK(exists(unknownDimensionDigp));
    CHECK(exists("mobevents"));
    CHECK(exists("scoreboard"));
    CHECK(exists("BiomeData"));
    CHECK(exists("PosTrackDB-0x00000001"));
    CHECK(exists("map_1"));
    // Bedrock villages are global, not chunk-keyed: they are preserved even
    // when their area is pruned.
    CHECK(exists("VILLAGE_Overworld_00000000-0000-0000-0000-000000000001_INFO"));
    CHECK(exists("VILLAGE_Overworld_00000000-0000-0000-0000-000000000001_DWELLERS"));
    CHECK(exists("VILLAGE_Overworld_00000000-0000-0000-0000-000000000002_INFO"));
    CHECK(exists("VILLAGE_Overworld_00000000-0000-0000-0000-000000000002_DWELLERS"));
  }

  fs::remove_all(dir);
}
