#include "bedrock/_context.hpp"
#include "bedrock/_java-chunk.hpp"

TEST_CASE("bedrock empty Java chunks keep their sections list") {
  for (auto const &[dataVersion, chunkY, sectionsName] : {
           std::tuple<int, int, std::u8string>(2730, 0, u8"Sections"),
           std::tuple<int, int, std::u8string>(4903, -4, u8"sections"),
       }) {
    auto chunk = mcfile::je::WritableChunk::MakeEmpty(-3, chunkY, 2);
    chunk->setDataVersion(dataVersion);
    auto tag = chunk->toCompoundTag(Dimension::End);
    REQUIRE(tag);

    auto column = tag->compoundTag(u8"Level");
    if (!column) {
      column = tag;
    }
    column->erase(sectionsName);
    CHECK_FALSE(column->tag(sectionsName));

    REQUIRE(bedrock::EnsureJavaChunkSections(*tag));
    auto sections = column->listTag(sectionsName);
    REQUIRE(sections);
    CHECK(sections->fType == Tag::Type::Compound);
    CHECK(sections->empty());
    CHECK(mcfile::je::Chunk::MakeChunk(-3, 2, tag));
  }
}

TEST_CASE("bedrock End chunk discovery") {
  auto tmp = mcfile::File::CreateTempDir(fs::temp_directory_path());
  REQUIRE(tmp);
  defer {
    fs::remove_all(*tmp);
  };

  auto dbPath = *tmp / "db";
  leveldb::Options dbOptions;
  dbOptions.create_if_missing = true;
  dbOptions.compression = leveldb::kZlibRawCompression;
  leveldb::DB *rawDb = nullptr;
  REQUIRE(leveldb::DB::Open(dbOptions, dbPath, &rawDb).ok());
  std::unique_ptr<leveldb::DB> db(rawDb);

  auto put = [&db](std::string const &key) {
    REQUIRE(db->Put({}, key, "value").ok());
  };
  put(mcfile::be::DbKey::SubChunk(1, 4, 2, Dimension::End));
  put(mcfile::be::DbKey::Entity(35, 36, Dimension::End));
  put(mcfile::be::DbKey::BlockEntity(-35, -36, Dimension::End));
  put(mcfile::be::DbKey::Version(96, 0, Dimension::End));
  put(mcfile::be::DbKey::VersionLegacy(97, 0, Dimension::End));
  put(mcfile::be::DbKey::FinalizedState(98, 0, Dimension::End));

  Pos2i duplicate(64, 64);
  put(mcfile::be::DbKey::Data3D(duplicate.fX, duplicate.fZ, Dimension::End));
  put(mcfile::be::DbKey::Data2D(duplicate.fX, duplicate.fZ, Dimension::End));
  put(mcfile::be::DbKey::SubChunk(duplicate.fX, 0, duplicate.fZ, Dimension::End));
  db.reset();

  bedrock::Options options;
  options.fTempDirectory = *tmp;
  std::map<Dimension, std::vector<std::pair<Pos2i, bedrock::Context::ChunksInRegion>>> regions;
  u64 totalChunks = 0;
  std::vector<bedrock::Context::PlayerData> players;
  std::unique_ptr<bedrock::Context> context;
  REQUIRE(bedrock::Context::Init(dbPath, options, Encoding::LittleEndian, regions, totalChunks, 0, GameMode::Survival, 2, players, context).ok());

  auto found = regions.find(Dimension::End);
  REQUIRE(found != regions.end());
  Pos2iSet chunks;
  for (auto const &entry : found->second) {
    for (Pos2i chunk : entry.second.fChunks) {
      chunks.insert(chunk);
    }
  }

  CHECK(totalChunks == 7);
  CHECK(chunks.size() == 7);
  for (Pos2i chunk : {Pos2i(1, 2), Pos2i(35, 36), Pos2i(-35, -36), Pos2i(96, 0), Pos2i(97, 0), Pos2i(98, 0), duplicate}) {
    CHECK(chunks.count(chunk) == 1);
  }
}

TEST_CASE("end-gateway") {
  SUBCASE("bedrock") {
    fs::path thisFile(__FILE__);
    auto mcworld = thisFile.parent_path() / "data" / "end-gateway" / "bedrock" / "end-gateway.mcworld";
    auto tmp = mcfile::File::CreateTempDir(fs::temp_directory_path());
    REQUIRE(tmp);
    defer {
      fs::remove_all(*tmp);
    };
    auto in = *tmp / "in";
    REQUIRE(ZipFile::Unzip(mcworld, in).ok());
    auto out = *tmp / "out";
    bedrock::Options opt;
    opt.fDimensionFilter.insert(Dimension::End);
    opt.fChunkFilter.insert({-5, -4});
    opt.fChunkFilter.insert({-51, -39});
    auto st = bedrock::Converter::Run(in, out, opt, thread::hardware_concurrency());
    std::string errorMessage;
    if (auto error = st.error(); error) {
      errorMessage = error->fWhat;
    }
    REQUIRE_MESSAGE(st.ok(), errorMessage);

    auto endDirectory = out / "dimensions" / "minecraft" / "the_end";
    REQUIRE(fs::is_directory(endDirectory / "region"));
    CHECK_FALSE(fs::exists(out / "DIM1"));
    mcfile::je::World world(endDirectory);
    {
      auto chunk = world.chunkAt(-5, -4);
      REQUIRE(chunk);
      CHECK(chunk->status() == mcfile::je::Chunk::Status::FULL);
      auto block = chunk->blockAt(-77, 75, -56);
      REQUIRE(block);
      CHECK(block->fId == mcfile::blocks::minecraft::end_gateway);
      auto tile = chunk->tileEntityAt(-77, 75, -56);
      REQUIRE(tile);
      CHECK(tile->string(u8"id") == u8"minecraft:end_gateway");
      auto exitPortal = tile->compoundTag(u8"ExitPortal");
      REQUIRE(exitPortal);
      CHECK(exitPortal->int32(u8"X") == -814);
      CHECK(exitPortal->int32(u8"Y") == 59);
      CHECK(exitPortal->int32(u8"Z") == -613);
      CHECK(tile->int64(u8"Age") == 725);
    }
    {
      auto chunk = world.chunkAt(-51, -39);
      REQUIRE(chunk);
      CHECK(chunk->status() == mcfile::je::Chunk::Status::FULL);
      auto block = chunk->blockAt(-814, 69, -613);
      REQUIRE(block);
      CHECK(block->fId == mcfile::blocks::minecraft::end_gateway);
      auto tile = chunk->tileEntityAt(-814, 69, -613);
      REQUIRE(tile);
      CHECK(tile->string(u8"id") == u8"minecraft:end_gateway");
      auto exitPortal = tile->compoundTag(u8"ExitPortal");
      REQUIRE(exitPortal);
      CHECK(exitPortal->int32(u8"X") == -74);
      CHECK(exitPortal->int32(u8"Y") == 58);
      CHECK(exitPortal->int32(u8"Z") == -52);
      CHECK(tile->int64(u8"Age") == 409);
    }
  }
}
