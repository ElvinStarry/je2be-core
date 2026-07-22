using je2be::java::UuidReplacer;

static std::string JavaUuidString(Uuid const &uuid) {
  auto value = uuid.toString();
  return std::string(reinterpret_cast<char const *>(value.data()), value.size());
}

TEST_CASE("java UUID replacer updates NBT, regions, JSON, and player filenames") {
  auto temporary = mcfile::File::CreateTempDir(fs::temp_directory_path());
  REQUIRE(temporary);
  fs::path world = *temporary / "world";
  fs::create_directories(world / "playerdata");
  fs::create_directories(world / "advancements");
  fs::create_directories(world / "stats");
  fs::create_directories(world / "entities");

  auto before = *Uuid::FromString(u8"8667ba71-b85a-4004-af54-457a9734eed7");
  auto after = *Uuid::FromString(u8"bb84e4a8-a756-42ee-8909-2ef9a527064c");

  auto level = Compound();
  auto levelData = Compound();
  levelData->set(u8"Player", before.toIntArrayTag());
  levelData->set(u8"PlayerText", je2be::String(before.toString()));
  level->set(u8"Data", levelData);
  auto writeGzip = [](CompoundTag const &tag, fs::path const &path) {
    auto output = std::make_shared<mcfile::stream::GzFileOutputStream>(path);
    bool ok = CompoundTag::Write(tag, output, mcfile::Encoding::Java);
    output.reset();
    return ok;
  };
  REQUIRE(writeGzip(*level, world / "level.dat"));

  auto player = Compound();
  player->set(u8"UUID", before.toIntArrayTag());
  auto oldPair = Compound();
  auto pair = [&before](bool most) {
    uint64_t value = 0;
    for (size_t i = most ? 0 : 8; i < (most ? 8 : 16); i++) {
      value = (value << 8) | before.fData[i];
    }
    return static_cast<int64_t>(value);
  };
  oldPair->set(u8"UUIDMost", Long(pair(true)));
  oldPair->set(u8"UUIDLeast", Long(pair(false)));
  player->set(u8"Legacy", oldPair);
  REQUIRE(writeGzip(*player, world / "playerdata" / (JavaUuidString(before) + ".dat")));

  std::ofstream(world / "advancements" / (JavaUuidString(before) + ".json"))
      << "{\"uuid\":\"" << JavaUuidString(before) << "\",\"raw\":\"" << JavaUuidString(before).substr(0, 8) << "\"}";
  std::ofstream(world / "stats" / (JavaUuidString(before) + ".json"))
      << "{\"uuid\":\"" << JavaUuidString(before) << "\"}";

  auto regionDirectory = world / "entities";
  auto regionPath = regionDirectory / "r.0.0.mca";
  auto editor = mcfile::je::McaEditor::Open(regionPath);
  REQUIRE(editor);
  auto chunk = Compound();
  auto entities = List<Tag::Type::Compound>();
  auto wolf = Compound();
  wolf->set(u8"Owner", before.toIntArrayTag());
  entities->push_back(wolf);
  chunk->set(u8"Entities", entities);
  REQUIRE(editor->insert(0, 0, *chunk));
  REQUIRE(editor->write(regionPath));

  auto csv = *temporary / "mapping.csv";
  std::ofstream(csv) << JavaUuidString(before) << ',' << JavaUuidString(after) << '\n';

  UuidReplacer::Options dryRunOptions;
  dryRunOptions.fDryRun = true;
  UuidReplacer::Result dryRunResult;
  REQUIRE(UuidReplacer::Run(world, csv, dryRunOptions, dryRunResult).ok());
  CHECK(dryRunResult.fChangedChunks == 1);
  CHECK(dryRunResult.fRenamedFiles == 3);
  CHECK(fs::exists(world / "playerdata" / (JavaUuidString(before) + ".dat")));
  CHECK_FALSE(fs::exists(world / "playerdata" / (JavaUuidString(after) + ".dat")));

  UuidReplacer::Result result;
  auto replacementStatus = UuidReplacer::Run(world, csv, {}, result);
  auto replacementError = replacementStatus.error();
  std::string replacementMessage = replacementError.has_value() ? replacementError->fWhat : "unknown error";
  REQUIRE_MESSAGE(replacementStatus.ok(), replacementMessage);
  CHECK(result.fChangedFiles >= 4);
  CHECK(result.fChangedChunks == 1);
  CHECK(result.fRenamedFiles == 3);

  auto updatedPlayerPath = world / "playerdata" / (JavaUuidString(after) + ".dat");
  CHECK(fs::exists(updatedPlayerPath));
  auto updatedPlayerInput = std::make_shared<mcfile::stream::GzFileInputStream>(updatedPlayerPath);
  auto updatedPlayer = CompoundTag::Read(updatedPlayerInput, mcfile::Encoding::Java);
  REQUIRE(updatedPlayer);
  auto updatedUuid = updatedPlayer->intArrayTag(u8"UUID");
  REQUIRE(updatedUuid);
  CHECK(updatedUuid->value() == after.toIntArrayTag()->value());
  auto updatedPair = updatedPlayer->compoundTag(u8"Legacy");
  REQUIRE(updatedPair);
  REQUIRE(updatedPair->int64(u8"UUIDMost"));
  REQUIRE(updatedPair->int64(u8"UUIDLeast"));
  uint64_t expectedMost = 0;
  uint64_t expectedLeast = 0;
  for (size_t i = 0; i < 8; i++) {
    expectedMost = (expectedMost << 8) | after.fData[i];
    expectedLeast = (expectedLeast << 8) | after.fData[8 + i];
  }
  CHECK(static_cast<uint64_t>(*updatedPair->int64(u8"UUIDMost")) == expectedMost);
  CHECK(static_cast<uint64_t>(*updatedPair->int64(u8"UUIDLeast")) == expectedLeast);

  auto updatedRegion = mcfile::je::McaEditor::Open(regionPath);
  REQUIRE(updatedRegion);
  auto updatedChunk = updatedRegion->get(0, 0);
  REQUIRE(updatedChunk);
  auto updatedEntities = updatedChunk->listTag(u8"Entities");
  REQUIRE(updatedEntities);
  CHECK(updatedEntities->at(0)->asCompound()->intArrayTag(u8"Owner")->value() == after.toIntArrayTag()->value());

  Fs::DeleteAll(*temporary);
}

TEST_CASE("java UUID replacer rejects duplicate CSV destinations") {
  auto temporary = mcfile::File::CreateTempDir(fs::temp_directory_path());
  REQUIRE(temporary);
  fs::path world = *temporary / "world";
  fs::create_directories(world);
  auto level = Compound();
  auto output = std::make_shared<mcfile::stream::GzFileOutputStream>(world / "level.dat");
  REQUIRE(CompoundTag::Write(*level, output, mcfile::Encoding::Java));
  output.reset();
  auto csv = *temporary / "mapping.csv";
  std::ofstream(csv) << "8667ba71-b85a-4004-af54-457a9734eed7,bb84e4a8-a756-42ee-8909-2ef9a527064c\n"
                     << "bb84e4a8-a756-42ee-8909-2ef9a527064d,bb84e4a8-a756-42ee-8909-2ef9a527064c\n";
  UuidReplacer::Result result;
  CHECK_FALSE(UuidReplacer::Run(world, csv, {}, result).ok());
  Fs::DeleteAll(*temporary);
}
