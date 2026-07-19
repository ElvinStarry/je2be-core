#include "bedrock/_java-player.hpp"
#include "bedrock/_context.hpp"
#include "bedrock/_entity.hpp"
#include "_data-version.hpp"

#include <cstdlib>

TEST_CASE("bedrock java player marker") {
  auto playerWithNames = [](std::initializer_list<std::u8string_view> names) {
    auto player = Compound();
    auto inventory = List<Tag::Type::Compound>();
    for (auto name : names) {
      auto display = Compound();
      display->set(u8"Name", std::u8string(name));
      auto tag = Compound();
      tag->set(u8"display", display);
      auto item = Compound();
      item->set(u8"Count", je2be::Byte(1));
      item->set(u8"tag", tag);
      inventory->push_back(item);
    }
    player->set(u8"Inventory", inventory);
    return player;
  };

  SUBCASE("plain marker") {
    auto player = playerWithNames({u8"JavaTag=Otto"});
    CHECK(je2be::bedrock::JavaPlayer::NameFromInventoryMarker(*player) == u8"Otto");
  }

  SUBCASE("parenthesized marker") {
    auto player = playerWithNames({u8"JavaTag=(Player_123)"});
    CHECK(je2be::bedrock::JavaPlayer::NameFromInventoryMarker(*player) == u8"Player_123");
  }

  SUBCASE("unmarked item name") {
    auto player = playerWithNames({u8"Otto"});
    CHECK_FALSE(je2be::bedrock::JavaPlayer::NameFromInventoryMarker(*player));
  }

  SUBCASE("invalid markers are skipped") {
    auto player = playerWithNames({
        u8"JavaTag=ab",
        u8"JavaTag=abcdefghijklmnopq",
        u8"JavaTag=bad-name",
        u8"JavaTag=(missing",
        u8"JavaTag=Valid_Name",
    });
    CHECK(je2be::bedrock::JavaPlayer::NameFromInventoryMarker(*player) == u8"Valid_Name");
  }
}

TEST_CASE("bedrock java player Mojang response") {
  auto uuid = je2be::bedrock::JavaPlayer::UuidFromMojangResponse(
      R"({"id":"8667ba71b85a4004af54457a9734eed7","name":"Steve"})");
  REQUIRE(uuid);
  CHECK(uuid->toString() == u8"8667ba71-b85a-4004-af54-457a9734eed7");

  CHECK_FALSE(je2be::bedrock::JavaPlayer::UuidFromMojangResponse("not json"));
  CHECK_FALSE(je2be::bedrock::JavaPlayer::UuidFromMojangResponse(R"({"name":"Steve"})"));
  CHECK_FALSE(je2be::bedrock::JavaPlayer::UuidFromMojangResponse(R"({"id":42})"));
  CHECK_FALSE(je2be::bedrock::JavaPlayer::UuidFromMojangResponse(
      R"({"id":"8667ba71-b85a-4004-af54-457a9734eed7"})"));
}

TEST_CASE("bedrock java player injected UUID resolver") {
  je2be::bedrock::Options options;
  std::u8string resolvedName;
  options.fJavaPlayerUuidResolver = [&resolvedName](std::u8string const &name) {
    resolvedName = name;
    return Uuid::FromString(u8"8667ba71-b85a-4004-af54-457a9734eed7");
  };

  auto player = Compound();
  auto inventory = List<Tag::Type::Compound>();
  auto display = Compound();
  display->set(u8"Name", u8"JavaTag=(Test_Player)");
  auto tag = Compound();
  tag->set(u8"display", display);
  auto item = Compound();
  item->set(u8"Count", je2be::Byte(1));
  item->set(u8"tag", tag);
  inventory->push_back(item);
  player->set(u8"Inventory", inventory);

  auto uuid = je2be::bedrock::JavaPlayer::ResolveUuid(*player, options);
  REQUIRE(uuid);
  CHECK(resolvedName == u8"Test_Player");
  CHECK(uuid->toString() == u8"8667ba71-b85a-4004-af54-457a9734eed7");
}

TEST_CASE("bedrock java player Mojang live lookup") {
  if (!std::getenv("JE2BE_TEST_MOJANG_API")) {
    return;
  }
  je2be::bedrock::Options options;
  auto uuid = je2be::bedrock::JavaPlayer::ResolveUuid(u8"Otto", options);
  REQUIRE(uuid);
  CHECK(uuid->toString() == u8"bb84e4a8-a756-42ee-8909-2ef9a527064c");
}

TEST_CASE("bedrock java player entity references") {
  auto tmp = mcfile::File::CreateTempDir(fs::temp_directory_path());
  REQUIRE(tmp);
  defer {
    fs::remove_all(*tmp);
  };

  auto dbPath = *tmp / "db";
  leveldb::Options dbOptions;
  dbOptions.create_if_missing = true;
  leveldb::DB *rawDb = nullptr;
  REQUIRE(leveldb::DB::Open(dbOptions, dbPath, &rawDb).ok());
  delete rawDb;

  je2be::bedrock::Options options;
  std::map<mcfile::Dimension, std::vector<std::pair<Pos2i, je2be::bedrock::Context::ChunksInRegion>>> regions;
  u64 totalChunks = 0;
  std::vector<je2be::bedrock::Context::PlayerData> players;
  std::unique_ptr<je2be::bedrock::Context> ctx;
  REQUIRE(je2be::bedrock::Context::Init(dbPath, options, Encoding::LittleEndian, regions, totalChunks, 0, je2be::GameMode::Survival, 1, players, ctx).ok());
  REQUIRE(ctx);

  i64 const playerId = -8589934586;
  Uuid const playerUuid = *Uuid::FromString(u8"8667ba71-b85a-4004-af54-457a9734eed7");
  ctx->addPlayerMapping(playerId, playerUuid);

  auto entity = [](std::u8string const &id, i64 uniqueId) {
    auto value = Compound();
    value->set(u8"identifier", id);
    value->set(u8"UniqueID", Long(uniqueId));
    return value;
  };

  for (int dataVersion : {kJavaDataVersionComponentIntroduced - 1, kJavaDataVersion}) {
    CAPTURE(dataVersion);

    auto arrow = entity(u8"minecraft:arrow", 1001);
    arrow->set(u8"OwnerNew", Long(-1));
    arrow->set(u8"OwnerID", Long(playerId));
    auto arrowResult = je2be::bedrock::Entity::From(*arrow, *ctx, dataVersion);
    REQUIRE(arrowResult);
    auto arrowOwner = arrowResult->fEntity->intArrayTag(u8"Owner");
    REQUIRE(arrowOwner);
    CHECK(arrowOwner->value() == playerUuid.toIntArrayTag()->value());

    auto wolf = entity(u8"minecraft:wolf", 1002);
    wolf->set(u8"TargetID", Long(playerId));
    auto wolfResult = je2be::bedrock::Entity::From(*wolf, *ctx, dataVersion);
    REQUIRE(wolfResult);
    auto angryAt = wolfResult->fEntity->intArrayTag(u8"AngryAt");
    REQUIRE(angryAt);
    CHECK(angryAt->value() == playerUuid.toIntArrayTag()->value());

    auto nuisance = Compound();
    nuisance->set(u8"ActorId", Long(playerId));
    nuisance->set(u8"Anger", Int(100));
    auto nuisances = List<Tag::Type::Compound>();
    nuisances->push_back(nuisance);
    auto warden = entity(u8"minecraft:warden", 1003);
    warden->set(u8"Nuisances", nuisances);
    auto wardenResult = je2be::bedrock::Entity::From(*warden, *ctx, dataVersion);
    REQUIRE(wardenResult);
    auto anger = wardenResult->fEntity->compoundTag(u8"anger");
    REQUIRE(anger);
    auto suspects = anger->listTag(u8"suspects");
    REQUIRE(suspects);
    REQUIRE(suspects->size() == 1);
    auto suspect = suspects->at(0)->asCompound();
    REQUIRE(suspect);
    auto suspectUuid = suspect->intArrayTag(u8"uuid");
    REQUIRE(suspectUuid);
    CHECK(suspectUuid->value() == playerUuid.toIntArrayTag()->value());
  }
}
