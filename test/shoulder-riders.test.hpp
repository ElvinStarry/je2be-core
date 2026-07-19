#pragma once

#include <fstream>

TEST_CASE("shoulder-riders") {
  fs::path thisFile(__FILE__);

  // The situation is like this: https://gyazo.com/fe4669d4e655e80282484d961a17b167
  auto original = thisFile.parent_path() / "data" / "shoulder-riders";
  auto tmp = mcfile::File::CreateTempDir(fs::temp_directory_path());

  defer {
    fs::remove_all(*tmp);
  };

  auto be = *tmp / "be";
  fs::create_directories(be);
  je2be::java::Options optToBe;
  optToBe.fDimensionFilter.insert(mcfile::Dimension::Overworld);
  optToBe.fChunkFilter.insert(Pos2i(0, 0));
  CHECK(je2be::java::Converter::Run(original, be, optToBe, 1).ok());

  Uuid const playerUuid = *Uuid::FromString(u8"8667ba71-b85a-4004-af54-457a9734eed7");
  Uuid const remotePlayerUuid = *Uuid::FromString(u8"bb84e4a8-a756-42ee-8909-2ef9a527064c");
  i64 const remoteBedrockId = -8589934500;
  {
    leveldb::DB *rawDb = nullptr;
    leveldb::Options dbOptions;
    dbOptions.compression = leveldb::kZlibRawCompression;
    REQUIRE(leveldb::DB::Open(dbOptions, be / "db", &rawDb).ok());
    std::unique_ptr<leveldb::DB> db(rawDb);
    std::string playerData;
    REQUIRE(db->Get({}, mcfile::be::DbKey::LocalPlayer(), &playerData).ok());
    auto player = CompoundTag::Read(playerData, Encoding::LittleEndian);
    REQUIRE(player);
    auto inventory = player->listTag(u8"Inventory");
    REQUIRE(inventory);
    auto display = Compound();
    display->set(u8"Name", u8"JavaTag=Steve");
    auto tag = Compound();
    tag->set(u8"display", display);
    auto marker = Compound();
    marker->set(u8"Count", je2be::Byte(1));
    marker->set(u8"Damage", Short(0));
    marker->set(u8"Name", u8"minecraft:stone");
    marker->set(u8"Slot", je2be::Byte(35));
    marker->set(u8"tag", tag);
    inventory->push_back(marker);
    auto serialized = CompoundTag::Write(*player, Encoding::LittleEndian);
    REQUIRE(serialized);
    REQUIRE(db->Put({}, mcfile::be::DbKey::LocalPlayer(), *serialized).ok());

    auto remotePlayer = player->copy();
    remotePlayer->set(u8"UniqueID", Long(remoteBedrockId));
    remotePlayer->set(u8"LeftShoulderRiderID", Long(-1));
    remotePlayer->set(u8"RightShoulderPassengerID", Long(-1));
    auto remoteInventory = remotePlayer->listTag(u8"Inventory");
    REQUIRE(remoteInventory);
    auto remoteMarker = remoteInventory->at(remoteInventory->size() - 1)->asCompound();
    REQUIRE(remoteMarker);
    auto remoteTag = remoteMarker->compoundTag(u8"tag");
    REQUIRE(remoteTag);
    auto remoteDisplay = remoteTag->compoundTag(u8"display");
    REQUIRE(remoteDisplay);
    remoteDisplay->set(u8"Name", u8"JavaTag=Otto");
    auto remoteSerialized = CompoundTag::Write(*remotePlayer, Encoding::LittleEndian);
    REQUIRE(remoteSerialized);
    REQUIRE(db->Put({}, "player_server_test", *remoteSerialized).ok());
  }

  auto je = *tmp / "je";
  fs::create_directories(je);
  je2be::bedrock::Options optToJe;
  optToJe.fDimensionFilter.insert(mcfile::Dimension::Overworld);
  optToJe.fChunkFilter.insert(Pos2i(0, 0));
  optToJe.fJavaPlayerUuidResolver = [playerUuid, remotePlayerUuid](std::u8string const &name) -> std::optional<Uuid> {
    if (name == u8"Steve") {
      return playerUuid;
    }
    if (name == u8"Otto") {
      return remotePlayerUuid;
    }
    return std::nullopt;
  };
  CHECK(je2be::bedrock::Converter::Run(be, je, optToJe, 1).ok());

  auto level = je / "level.dat";
  auto stream = make_shared<mcfile::stream::GzFileInputStream>(level);
  auto dat = CompoundTag::Read(stream, Encoding::Java);
  auto data = dat->compoundTag(u8"Data");
  auto player = data->compoundTag(u8"Player");
  CHECK(player);

  auto uuid = player->intArrayTag(u8"UUID");
  REQUIRE(uuid);
  CHECK(uuid->value() == playerUuid.toIntArrayTag()->value());

  auto shoulderLeft = player->compoundTag(u8"ShoulderEntityLeft");
  auto shoulderRight = player->compoundTag(u8"ShoulderEntityRight");
  REQUIRE(shoulderLeft);
  REQUIRE(shoulderRight);
  auto ownerLeft = shoulderLeft->intArrayTag(u8"Owner");
  auto ownerRight = shoulderRight->intArrayTag(u8"Owner");
  REQUIRE(ownerLeft);
  REQUIRE(ownerRight);
  CHECK(ownerLeft->value() == playerUuid.toIntArrayTag()->value());
  CHECK(ownerRight->value() == playerUuid.toIntArrayTag()->value());
  CHECK(player->compoundTag(u8"RootVehicle"));

  auto localPlayerPath = je / "playerdata" / fs::path(playerUuid.toString() + u8".dat");
  auto localStream = make_shared<mcfile::stream::GzFileInputStream>(localPlayerPath);
  auto localPlayer = CompoundTag::Read(localStream, Encoding::Java);
  REQUIRE(localPlayer);
  auto localUuid = localPlayer->intArrayTag(u8"UUID");
  REQUIRE(localUuid);
  CHECK(localUuid->value() == playerUuid.toIntArrayTag()->value());
  CHECK(localPlayer->compoundTag(u8"ShoulderEntityLeft"));
  CHECK(localPlayer->compoundTag(u8"ShoulderEntityRight"));
  CHECK(localPlayer->compoundTag(u8"RootVehicle"));

  auto remotePlayerPath = je / "playerdata" / fs::path(remotePlayerUuid.toString() + u8".dat");
  auto remoteStream = make_shared<mcfile::stream::GzFileInputStream>(remotePlayerPath);
  auto remotePlayer = CompoundTag::Read(remoteStream, Encoding::Java);
  REQUIRE(remotePlayer);
  auto remoteUuid = remotePlayer->intArrayTag(u8"UUID");
  REQUIRE(remoteUuid);
  CHECK(remoteUuid->value() == remotePlayerUuid.toIntArrayTag()->value());

  auto failedOutput = *tmp / "failed";
  fs::create_directories(failedOutput);
  auto sentinel = failedOutput / "keep.txt";
  {
    std::ofstream stream(sentinel);
    stream << "keep";
  }
  auto failingOptions = optToJe;
  failingOptions.fJavaPlayerUuidResolver = [](std::u8string const &) -> std::optional<Uuid> {
    return std::nullopt;
  };
  auto failed = je2be::bedrock::Converter::Run(be, failedOutput, failingOptions, 1);
  REQUIRE_FALSE(failed.ok());
  REQUIRE(failed.error());
  CHECK(failed.error()->fWhat.find("Failed to resolve Java UUID") != std::string::npos);
  CHECK(fs::exists(sentinel));

  {
    leveldb::DB *rawDb = nullptr;
    leveldb::Options dbOptions;
    dbOptions.compression = leveldb::kZlibRawCompression;
    REQUIRE(leveldb::DB::Open(dbOptions, be / "db", &rawDb).ok());
    std::unique_ptr<leveldb::DB> db(rawDb);
    std::string playerData;
    REQUIRE(db->Get({}, mcfile::be::DbKey::LocalPlayer(), &playerData).ok());
    auto unmarkedLocalPlayer = CompoundTag::Read(playerData, Encoding::LittleEndian);
    REQUIRE(unmarkedLocalPlayer);
    auto inventory = unmarkedLocalPlayer->listTag(u8"Inventory");
    REQUIRE(inventory);
    auto marker = inventory->at(inventory->size() - 1)->asCompound();
    REQUIRE(marker);
    auto tag = marker->compoundTag(u8"tag");
    REQUIRE(tag);
    auto display = tag->compoundTag(u8"display");
    REQUIRE(display);
    display->set(u8"Name", u8"Steve");
    auto serialized = CompoundTag::Write(*unmarkedLocalPlayer, Encoding::LittleEndian);
    REQUIRE(serialized);
    REQUIRE(db->Put({}, mcfile::be::DbKey::LocalPlayer(), *serialized).ok());
  }

  auto conflictingOptions = optToJe;
  conflictingOptions.fLocalPlayer = std::make_shared<Uuid const>(remotePlayerUuid);
  auto conflict = je2be::bedrock::Converter::Run(be, failedOutput, conflictingOptions, 1);
  REQUIRE_FALSE(conflict.ok());
  REQUIRE(conflict.error());
  CHECK(conflict.error()->fWhat.find("is assigned to multiple Bedrock player UniqueIDs") != std::string::npos);
  CHECK(fs::exists(sentinel));
}
