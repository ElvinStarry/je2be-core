#pragma once

#include "_data-version.hpp"
#include "bedrock/_context.hpp"
#include "bedrock/_entity.hpp"
#include "bedrock/_item.hpp"
#include "entity/_sulfur-cube.hpp"
#include "java/_context.hpp"
#include "java/_components.hpp"
#include "java/_entity.hpp"
#include "java/_item.hpp"
#include "java/_java-edition-map.hpp"
#include "java/_lodestone-registrar.hpp"
#include "java/_uuid-registrar.hpp"
#include "java/_world-data.hpp"

static DataVersion NewContentDataVersion() {
  return DataVersion(kJavaDataVersion, kJavaDataVersion);
}

struct NewContentJavaContext {
  je2be::java::JavaEditionMap fMap;
  std::shared_ptr<je2be::java::LodestoneRegistrar> fLodestones;
  std::shared_ptr<je2be::java::UuidRegistrar> fUuids;
  je2be::java::WorldData fWorldData;
  je2be::java::Context fContext;

  NewContentJavaContext()
      : fMap(std::unordered_map<i32, i8>{}),
        fLodestones(std::make_shared<je2be::java::LodestoneRegistrar>()),
        fUuids(std::make_shared<je2be::java::UuidRegistrar>()),
        fWorldData(mcfile::Dimension::Overworld),
        fContext(fMap, fLodestones, fUuids, fWorldData, 0, 1, false, GameMode::Survival) {}
};

static std::unique_ptr<je2be::bedrock::Context> NewContentBedrockContext(std::filesystem::path const &root) {
  auto dbPath = root / "db";
  std::filesystem::create_directories(dbPath);
  leveldb::Options dbOptions;
  dbOptions.create_if_missing = true;
  leveldb::DB *rawDb = nullptr;
  if (!leveldb::DB::Open(dbOptions, dbPath, &rawDb).ok()) {
    return nullptr;
  }
  delete rawDb;

  je2be::bedrock::Options options;
  auto tempDirectory = root / "tmp";
  std::filesystem::create_directories(tempDirectory);
  options.fTempDirectory = tempDirectory;
  std::map<mcfile::Dimension, std::vector<std::pair<Pos2i, je2be::bedrock::Context::ChunksInRegion>>> regions;
  u64 totalChunks = 0;
  std::vector<je2be::bedrock::Context::PlayerData> players;
  std::unique_ptr<je2be::bedrock::Context> context;
  if (!je2be::bedrock::Context::Init(dbPath, options, Encoding::LittleEndian, regions, totalChunks, 0, GameMode::Survival, 1, players, context).ok()) {
    return nullptr;
  }
  return context;
}

static CompoundTagPtr NewContentJavaItem(std::u8string const &id, i32 count = 1) {
  auto item = Compound();
  item->set(u8"id", id);
  item->set(u8"count", Int(count));
  return item;
}

static CompoundTagPtr NewContentJavaEntity(std::u8string const &id, std::u8string const &uuidString) {
  auto entity = Compound();
  entity->set(u8"id", id);
  auto uuid = Uuid::FromString(uuidString);
  if (uuid) {
    entity->set(u8"UUID", uuid->toIntArrayTag());
  }
  auto pos = List<Tag::Type::Double>();
  pos->push_back(Double(0));
  pos->push_back(Double(64));
  pos->push_back(Double(0));
  entity->set(u8"Pos", pos);
  auto motion = List<Tag::Type::Double>();
  motion->push_back(Double(0));
  motion->push_back(Double(0));
  motion->push_back(Double(0));
  entity->set(u8"Motion", motion);
  auto rotation = List<Tag::Type::Float>();
  rotation->push_back(Float(0));
  rotation->push_back(Float(0));
  entity->set(u8"Rotation", rotation);
  entity->set(u8"OnGround", Bool(false));
  return entity;
}

static CompoundTagPtr NewContentBedrockItem(std::u8string const &name, i8 count = 1) {
  auto item = Compound();
  item->set(u8"Name", name);
  item->set(u8"Count", je2be::Byte(count));
  item->set(u8"Damage", Short(0));
  item->set(u8"WasPickedUp", Bool(false));
  return item;
}

static void CheckNewBlockRoundtrip(std::u8string const &blockData) {
  auto dataVersion = NewContentDataVersion();
  auto blockJ = mcfile::je::Block::FromBlockData(blockData, dataVersion.fSource);
  REQUIRE(blockJ);
  auto blockBData = je2be::java::BlockData::From(blockJ, nullptr, dataVersion, {});
  REQUIRE(blockBData);
  auto blockB = mcfile::be::Block::FromCompound(*blockBData);
  REQUIRE(blockB);
  auto blockJ2 = je2be::bedrock::BlockData::From(*blockB, dataVersion.fTarget);
  REQUIRE(blockJ2);
  CHECK(blockJ2->fName == (blockJ->fName == u8"minecraft:potted_golden_dandelion" ? u8"minecraft:flower_pot" : blockJ->fName));
  for (auto property : {u8"type", u8"facing", u8"half", u8"potent_sulfur_state", u8"thickness", u8"vertical_direction"}) {
    CHECK(blockJ2->property(property) == blockJ->property(property));
  }
}

TEST_CASE("new 26.2 block conversion") {
  for (auto const &material : {u8"cinnabar", u8"cinnabar_brick", u8"polished_cinnabar", u8"sulfur", u8"sulfur_brick", u8"polished_sulfur"}) {
    CheckNewBlockRoundtrip(u8"minecraft:" + std::u8string(material) + u8"_slab[type=top,waterlogged=true]");
    CheckNewBlockRoundtrip(u8"minecraft:" + std::u8string(material) + u8"_slab[type=double,waterlogged=false]");
    CheckNewBlockRoundtrip(u8"minecraft:" + std::u8string(material) + u8"_stairs[facing=east,half=top,shape=outer_left,waterlogged=true]");
    CheckNewBlockRoundtrip(u8"minecraft:" + std::u8string(material) + u8"_wall[east=tall,north=low,south=none,up=false,waterlogged=true,west=low]");
  }
  for (auto const &blockData : {
           u8"minecraft:chiseled_cinnabar",
           u8"minecraft:chiseled_sulfur",
           u8"minecraft:cinnabar",
           u8"minecraft:cinnabar_bricks",
           u8"minecraft:golden_dandelion",
           u8"minecraft:polished_cinnabar",
           u8"minecraft:polished_sulfur",
           u8"minecraft:sulfur",
           u8"minecraft:sulfur_bricks",
           u8"minecraft:potted_golden_dandelion",
           u8"minecraft:potent_sulfur[potent_sulfur_state=erupting]",
           u8"minecraft:sulfur_spike[thickness=tip_merge,vertical_direction=down,waterlogged=true]"}) {
    CheckNewBlockRoundtrip(blockData);
  }
}

TEST_CASE("new 26.2 item conversion") {
  NewContentJavaContext javaContext;
  auto dataVersion = NewContentDataVersion();
  for (auto const &id : {
           u8"minecraft:camel_husk_spawn_egg",
           u8"minecraft:copper_nautilus_armor",
           u8"minecraft:copper_spear",
           u8"minecraft:diamond_nautilus_armor",
           u8"minecraft:diamond_spear",
           u8"minecraft:golden_nautilus_armor",
           u8"minecraft:golden_spear",
           u8"minecraft:iron_nautilus_armor",
           u8"minecraft:iron_spear",
           u8"minecraft:music_disc_bounce",
           u8"minecraft:nautilus_spawn_egg",
           u8"minecraft:netherite_horse_armor",
           u8"minecraft:netherite_nautilus_armor",
           u8"minecraft:netherite_spear",
           u8"minecraft:parched_spawn_egg",
           u8"minecraft:stone_spear",
           u8"minecraft:sulfur_cube_spawn_egg",
           u8"minecraft:wooden_spear",
           u8"minecraft:zombie_nautilus_spawn_egg"}) {
    auto item = NewContentJavaItem(id);
    auto converted = je2be::java::Item::From(item, javaContext.fContext, dataVersion);
    REQUIRE(converted);
    CHECK(converted->string(u8"Name") == id);
  }
  CHECK(je2be::java::Item::IsRangedWeapon(u8"minecraft:diamond_spear"));
  CHECK(je2be::java::Item::IsMeleeWeapon(u8"minecraft:diamond_spear"));
  CHECK(Enchantments::BedrockEnchantmentIdFromJava(u8"minecraft:lunge") == 41);
  CHECK(je2be::SulfurCube::ArchetypeFromJavaItem(u8"minecraft:oak_planks") == u8"bouncy");
  CHECK(je2be::SulfurCube::ArchetypeFromJavaItem(u8"minecraft:slime_block") == u8"none");

  SUBCASE("sulfur cube bucket data and content") {
    auto item = NewContentJavaItem(u8"minecraft:sulfur_cube_bucket");
    auto entityData = Compound();
    entityData->set(u8"Health", Float(5.5f));
    entityData->set(u8"PersistenceRequired", Bool(true));
    entityData->set(u8"age", Int(-20));
    entityData->set(u8"age_locked", Bool(true));
    je2be::java::AppendComponent(item, u8"bucket_entity_data", entityData);
    je2be::java::AppendComponent(item, u8"sulfur_cube_content", NewContentJavaItem(u8"minecraft:oak_planks"));

    auto convertedB = je2be::java::Item::From(item, javaContext.fContext, dataVersion);
    REQUIRE(convertedB);
    auto bucketTagB = convertedB->compoundTag(u8"tag");
    REQUIRE(bucketTagB);
    CHECK(bucketTagB->boolean(u8"Persistent", false));
    CHECK(bucketTagB->int32(u8"Age") == -20);
    CHECK(bucketTagB->boolean(u8"GrowthPaused", false));
    CHECK(bucketTagB->listTag(u8"Mainhand")->at(0)->asCompound()->string(u8"Name") == u8"minecraft:oak_planks");

    auto root = mcfile::File::CreateTempDir(fs::temp_directory_path());
    REQUIRE(root);
    defer {
      fs::remove_all(*root);
    };
    auto contextB = NewContentBedrockContext(*root);
    REQUIRE(contextB);
    auto convertedJ = je2be::bedrock::Item::From(*convertedB, *contextB, kJavaDataVersion, {});
    REQUIRE(convertedJ);
    auto entityDataJ = je2be::java::GetComponent<CompoundTag>(*convertedJ, u8"bucket_entity_data");
    REQUIRE(entityDataJ);
    CHECK(entityDataJ->boolean(u8"PersistenceRequired", false));
    CHECK(entityDataJ->int32(u8"age") == -20);
    CHECK(entityDataJ->boolean(u8"age_locked", false));
    auto contentJ = je2be::java::GetComponent<CompoundTag>(*convertedJ, u8"sulfur_cube_content");
    REQUIRE(contentJ);
    CHECK(contentJ->string(u8"id") == u8"minecraft:oak_planks");
  }
}

TEST_CASE("new 26.2 Java entity conversion") {
  NewContentJavaContext context;
  auto dataVersion = NewContentDataVersion();

  SUBCASE("nautilus owner, armor, saddle, and age") {
    auto entity = NewContentJavaEntity(u8"minecraft:nautilus", u8"00000000-0000-0000-0000-000000000001");
    entity->set(u8"Owner", Uuid::FromString(u8"00000000-0000-0000-0000-000000000002")->toIntArrayTag());
    entity->set(u8"Age", Int(-321));
    entity->set(u8"Sitting", Bool(true));
    auto equipment = Compound();
    equipment->set(u8"saddle", NewContentJavaItem(u8"minecraft:saddle"));
    equipment->set(u8"body", NewContentJavaItem(u8"minecraft:diamond_nautilus_armor"));
    entity->set(u8"equipment", equipment);
    auto converted = je2be::java::Entity::From(*entity, context.fContext, dataVersion, {});
    REQUIRE(converted.fEntity);
    CHECK(converted.fEntity->int64(u8"OwnerNew", -1) != -1);
    CHECK(converted.fEntity->boolean(u8"IsBaby", false));
    CHECK(je2be::bedrock::Entity::HasDefinition(*converted.fEntity, u8"+minecraft:nautilus_tame"));
    CHECK(converted.fEntity->listTag(u8"ChestItems"));
    CHECK(converted.fEntity->listTag(u8"Armor")->at(4)->asCompound()->string(u8"Name") == u8"minecraft:diamond_nautilus_armor");
  }

  SUBCASE("camel husk owner and passengers") {
    auto entity = NewContentJavaEntity(u8"minecraft:camel_husk", u8"00000000-0000-0000-0000-000000000003");
    entity->set(u8"Owner", Uuid::FromString(u8"00000000-0000-0000-0000-000000000004")->toIntArrayTag());
    auto passengers = List<Tag::Type::Compound>();
    passengers->push_back(NewContentJavaEntity(u8"minecraft:husk", u8"00000000-0000-0000-0000-000000000005"));
    passengers->push_back(NewContentJavaEntity(u8"minecraft:parched", u8"00000000-0000-0000-0000-000000000006"));
    entity->set(u8"Passengers", passengers);
    auto converted = je2be::java::Entity::From(*entity, context.fContext, dataVersion, {});
    REQUIRE(converted.fEntity);
    CHECK(converted.fEntity->int64(u8"OwnerNew", -1) != -1);
    CHECK(je2be::bedrock::Entity::HasDefinition(*converted.fEntity, u8"+minecraft:camel_husk_with_hostile_rider"));
    REQUIRE(converted.fEntity->listTag(u8"LinksTag"));
    CHECK(converted.fEntity->listTag(u8"LinksTag")->size() == 2);
    CHECK(converted.fPassengers.size() == 2);
  }

  SUBCASE("parched spear uses ranged behavior") {
    auto entity = NewContentJavaEntity(u8"minecraft:parched", u8"00000000-0000-0000-0000-000000000007");
    auto equipment = Compound();
    equipment->set(u8"mainhand", NewContentJavaItem(u8"minecraft:diamond_spear"));
    entity->set(u8"equipment", equipment);
    auto converted = je2be::java::Entity::From(*entity, context.fContext, dataVersion, {});
    REQUIRE(converted.fEntity);
    CHECK(je2be::bedrock::Entity::HasDefinition(*converted.fEntity, u8"+minecraft:ranged_attack"));
    CHECK_FALSE(je2be::bedrock::Entity::HasDefinition(*converted.fEntity, u8"+minecraft:melee_attack"));
  }

  SUBCASE("sulfur cube content and state") {
    auto entity = NewContentJavaEntity(u8"minecraft:sulfur_cube", u8"00000000-0000-0000-0000-000000000008");
    entity->set(u8"Size", Int(1));
    entity->set(u8"Health", Float(8));
    entity->set(u8"from_bucket", Bool(true));
    entity->set(u8"Age", Int(-100));
    entity->set(u8"AgeLocked", Bool(true));
    entity->set(u8"fuse", Int(42));
    entity->set(u8"pickup_timer", Int(17));
    auto equipment = Compound();
    equipment->set(u8"body", NewContentJavaItem(u8"minecraft:oak_planks"));
    entity->set(u8"equipment", equipment);
    auto converted = je2be::java::Entity::From(*entity, context.fContext, dataVersion, {});
    REQUIRE(converted.fEntity);
    CHECK(converted.fEntity->byte(u8"Size") == 2);
    CHECK(converted.fEntity->boolean(u8"GrowthPaused", false));
    CHECK(converted.fEntity->boolean(u8"NaturalSpawn", true) == false);
    CHECK(je2be::bedrock::Entity::HasDefinition(*converted.fEntity, u8"+minecraft:sulfur_cube_bouncy"));
    CHECK(je2be::bedrock::Entity::HasDefinition(*converted.fEntity, u8"+minecraft:sulfur_cube_medium_primed"));
    CHECK(converted.fEntity->listTag(u8"Mainhand")->at(0)->asCompound()->string(u8"Name") == u8"minecraft:oak_planks");
  }
}

TEST_CASE("new 26.2 Bedrock entity conversion") {
  auto root = mcfile::File::CreateTempDir(fs::temp_directory_path());
  REQUIRE(root);
  defer {
    fs::remove_all(*root);
  };
  auto context = NewContentBedrockContext(*root);
  REQUIRE(context);

  SUBCASE("camel husk owner") {
    je2be::java::Entity::Rep rep(123);
    rep.fIdentifier = u8"minecraft:camel_husk";
    rep.fDefinitions = {u8"+minecraft:camel_husk_standing", u8"+minecraft:camel_husk_with_no_hostile_rider", u8"+minecraft:camel_husk_saddled"};
    auto entity = rep.toCompoundTag();
    entity->set(u8"OwnerNew", Long(456));
    auto converted = je2be::bedrock::Entity::From(*entity, *context, kJavaDataVersion);
    REQUIRE(converted);
    REQUIRE(converted->fEntity);
    CHECK(converted->fEntity->boolean(u8"Tame", false));
    CHECK(converted->fEntity->intArrayTag(u8"Owner"));
    CHECK(converted->fEntity->int64(u8"LastPoseTick", 0) == 0);
  }

  SUBCASE("sound variant and growth pause") {
    je2be::java::Entity::Rep rep(124);
    rep.fIdentifier = u8"minecraft:cat";
    rep.fDefinitions = {u8"+minecraft:cat"};
    auto entity = rep.toCompoundTag();
    auto properties = Compound();
    properties->set(u8"minecraft:sound_variant", je2be::String(u8"big"));
    entity->set(u8"properties", properties);
    entity->set(u8"Age", Int(-5));
    entity->set(u8"GrowthPaused", Bool(true));
    auto converted = je2be::bedrock::Entity::From(*entity, *context, kJavaDataVersion);
    REQUIRE(converted);
    REQUIRE(converted->fEntity);
    CHECK(converted->fEntity->string(u8"sound_variant") == u8"minecraft:big");
    CHECK(converted->fEntity->int32(u8"Age") == -5);
    CHECK(converted->fEntity->boolean(u8"AgeLocked", false));
  }

  SUBCASE("sulfur cube content") {
    je2be::java::Entity::Rep rep(125);
    rep.fIdentifier = u8"minecraft:sulfur_cube";
    rep.fDefinitions = {
        u8"+minecraft:sulfur_cube",
        u8"+minecraft:sulfur_cube_ai",
        u8"+minecraft:sulfur_cube_medium",
        u8"+minecraft:sulfur_cube_medium_with_block",
        u8"+minecraft:sulfur_cube_medium_with_block_interactable",
        u8"+minecraft:sulfur_cube_slow_bouncy",
        u8"+minecraft:sulfur_cube_without_target"};
    auto entity = rep.toCompoundTag();
    entity->set(u8"Size", je2be::Byte(2));
    auto content = NewContentBedrockItem(u8"minecraft:stone", 1);
    auto mainhand = List<Tag::Type::Compound>();
    mainhand->push_back(content);
    entity->set(u8"Mainhand", mainhand);
    auto armor = List<Tag::Type::Compound>();
    for (int i = 0; i < 5; i++) {
      armor->push_back(je2be::bedrock::Item::From(*NewContentBedrockItem(u8"", 0), *context, kJavaDataVersion, {}));
    }
    armor->fValue[4] = content;
    entity->set(u8"Armor", armor);
    auto converted = je2be::bedrock::Entity::From(*entity, *context, kJavaDataVersion);
    REQUIRE(converted);
    REQUIRE(converted->fEntity);
    CHECK(converted->fEntity->int32(u8"Size") == 1);
    REQUIRE(converted->fEntity->compoundTag(u8"equipment"));
    CHECK(converted->fEntity->compoundTag(u8"equipment")->compoundTag(u8"body")->string(u8"id") == u8"minecraft:stone");
  }
}
