#include "../example/cli-world-data-overrides.hpp"

TEST_CASE("world data override") {
  SUBCASE("CLI extracts override groups without consuming output") {
    std::vector<std::string> raw = {
        "b2j",
        "-i",
        "input",
        "-o",
        "output",
        "-O",
        "GameType=1",
        "LevelName=New World",
        "--override=experiments.test_feature=true",
        "-n",
        "4",
    };
    std::vector<char *> argv;
    for (auto &argument : raw) {
      argv.push_back(argument.data());
    }

    std::vector<std::string> arguments;
    std::vector<WorldDataOverride> overrides;
    std::string error;
    REQUIRE(je2be::cli::ExtractWorldDataOverrides(static_cast<int>(argv.size()), argv.data(), arguments, overrides, error));
    CHECK(arguments == std::vector<std::string>{"b2j", "-i", "input", "-o", "output", "-n", "4"});
    REQUIRE(overrides.size() == 3);
    CHECK(overrides[0].fPath == u8"GameType");
    CHECK(overrides[0].fValue == u8"1");
    CHECK(overrides[1].fPath == u8"LevelName");
    CHECK(overrides[1].fValue == u8"New World");
    CHECK(overrides[2].fPath == u8"experiments.test_feature");

    raw = {"j2b", "-O"};
    argv.clear();
    for (auto &argument : raw) {
      argv.push_back(argument.data());
    }
    CHECK_FALSE(je2be::cli::ExtractWorldDataOverrides(static_cast<int>(argv.size()), argv.data(), arguments, overrides, error));
    CHECK(error.find("at least one") != std::string::npos);
  }

  SUBCASE("nested paths preserve existing NBT types") {
    auto root = Compound();
    root->set(u8"Difficulty", Int(2));
    root->set(u8"LevelName", u8"before");
    root->set(u8"RandomSeed", Long(0));
    root->set(u8"FlatWorldLayers", u8"null");

    auto experiments = Compound();
    experiments->set(u8"test_feature", Bool(false));
    root->set(u8"experiments", experiments);

    auto rules = Compound();
    rules->set(u8"keepInventory", u8"false");
    root->set(u8"GameRules", rules);

    auto versions = List<Tag::Type::Int>();
    versions->push_back(Int(1));
    versions->push_back(Int(2));
    versions->push_back(Int(3));
    root->set(u8"versions", versions);

    auto position = std::make_shared<IntArrayTag>();
    position->fValue = {0, 64, 0};
    root->set(u8"position", position);

    std::vector<WorldDataOverride> entries = {
        {u8"Difficulty", u8"3"},
        {u8"LevelName", u8"after conversion"},
        {u8"RandomSeed", u8"9223372036854775807"},
        {u8"FlatWorldLayers", u8R"({"biome_id":1,"block_layers":[]})"},
        {u8"experiments.test_feature", u8"true"},
        {u8"GameRules.keepInventory", u8"true"},
        {u8"versions.1", u8"26"},
        {u8"position.1", u8"80"},
        {u8"custom\\.key.child", u8"42l"},
        {u8"resource", u8"minecraft:overworld"},
    };

    auto st = WorldDataOverrideEngine::Apply(*root, entries);
    REQUIRE(st.ok());
    CHECK(root->int32(u8"Difficulty") == 3);
    CHECK(root->string(u8"LevelName") == u8"after conversion");
    CHECK(root->int64(u8"RandomSeed") == std::numeric_limits<i64>::max());
    CHECK(root->string(u8"FlatWorldLayers") == u8R"({"biome_id":1,"block_layers":[]})");
    CHECK(experiments->boolean(u8"test_feature") == true);
    CHECK(rules->string(u8"keepInventory") == u8"true");
    CHECK(versions->at(1)->asInt()->fValue == 26);
    CHECK(position->fValue[1] == 80);
    REQUIRE(root->compoundTag(u8"custom.key"));
    CHECK(root->compoundTag(u8"custom.key")->int64(u8"child") == 42);
    CHECK(root->string(u8"resource") == u8"minecraft:overworld");
  }

  SUBCASE("structured SNBT values") {
    auto root = Compound();
    std::vector<WorldDataOverride> entries = {
        {u8"settings", u8"{ seed: 123l, enabled: true }"},
        {u8"enabled_features", u8R"([ "minecraft:vanilla", "minecraft:trade_rebalance" ])"},
        {u8"bytes", u8"[B;1b,0b,-1b]"},
    };

    auto st = WorldDataOverrideEngine::Apply(*root, entries);
    REQUIRE(st.ok());
    auto settings = root->compoundTag(u8"settings");
    REQUIRE(settings);
    CHECK(settings->int64(u8"seed") == 123);
    CHECK(settings->boolean(u8"enabled") == true);
    auto features = root->listTag(u8"enabled_features");
    REQUIRE(features);
    REQUIRE(features->size() == 2);
    CHECK(features->at(1)->asString()->fValue == u8"minecraft:trade_rebalance");
    auto bytes = root->byteArrayTag(u8"bytes");
    REQUIRE(bytes);
    REQUIRE(bytes->fValue.size() == 3);
    CHECK(std::bit_cast<i8>(bytes->fValue[2]) == -1);
  }

  SUBCASE("Java document routing and experiment synchronization") {
    WorldDataOverrideEngine::JavaDocuments documents;
    documents.fLevel = Compound();
    documents.fGameRules = Compound();
    documents.fWorldGenSettings = Compound();

    auto difficulty = Compound();
    difficulty->set(u8"difficulty", u8"normal");
    documents.fLevel->set(u8"difficulty_settings", difficulty);

    auto packs = Compound();
    auto enabledPacks = List<Tag::Type::String>();
    enabledPacks->push_back(je2be::String(u8"vanilla"));
    packs->set(u8"Enabled", enabledPacks);
    auto disabledPacks = List<Tag::Type::String>();
    disabledPacks->push_back(je2be::String(u8"trade_rebalance"));
    packs->set(u8"Disabled", disabledPacks);
    documents.fLevel->set(u8"DataPacks", packs);

    documents.fGameRules->set(u8"minecraft:keep_inventory", Bool(false));
    documents.fWorldGenSettings->set(u8"seed", Long(0));

    std::vector<WorldDataOverride> entries = {
        {u8"difficulty_settings.difficulty", u8"hard"},
        {u8"game_rules.minecraft:keep_inventory", u8"true"},
        {u8"WorldGenSettings.seed", u8"987654321"},
        {u8"experiments.minecraft:trade_rebalance", u8"true"},
        {u8"weather.raining", u8"true"},
    };

    auto st = WorldDataOverrideEngine::ApplyJava(documents, entries);
    REQUIRE(st.ok());
    CHECK(difficulty->string(u8"difficulty") == u8"hard");
    CHECK(documents.fGameRules->boolean(u8"minecraft:keep_inventory") == true);
    CHECK(documents.fWorldGenSettings->int64(u8"seed") == 987654321);
    REQUIRE(documents.fWeather);
    CHECK(documents.fWeather->boolean(u8"raining") == true);

    auto features = documents.fLevel->listTag(u8"enabled_features");
    REQUIRE(features);
    CHECK(features->size() == 2);
    CHECK(enabledPacks->size() == 2);
    CHECK(disabledPacks->empty());

    st = WorldDataOverrideEngine::ApplyJava(documents, {{u8"experiments.minecraft:trade_rebalance", u8"false"}});
    REQUIRE(st.ok());
    CHECK(features->size() == 1);
    CHECK(enabledPacks->size() == 1);
    CHECK(disabledPacks->size() == 1);
  }

  SUBCASE("Bedrock experiment overrides mark the world as experimental") {
    auto root = Compound();
    auto experiments = Compound();
    experiments->set(u8"experiments_ever_used", Bool(false));
    experiments->set(u8"saved_with_toggled_experiments", Bool(false));
    experiments->set(u8"test_feature", Bool(false));
    root->set(u8"experiments", experiments);
    root->set(u8"GameType", Int(0));

    auto st = WorldDataOverrideEngine::ApplyBedrock(
        *root,
        {{u8"experiments.test_feature", u8"true"}, {u8"level.GameType", u8"1"}});
    REQUIRE(st.ok());
    CHECK(experiments->boolean(u8"test_feature") == true);
    CHECK(experiments->boolean(u8"experiments_ever_used") == true);
    CHECK(experiments->boolean(u8"saved_with_toggled_experiments") == true);
    CHECK(root->int32(u8"GameType") == 1);
  }

  SUBCASE("invalid paths and values fail with context") {
    auto root = Compound();
    root->set(u8"Difficulty", Int(2));

    auto st = WorldDataOverrideEngine::Apply(*root, WorldDataOverride{u8"Difficulty.child", u8"3"});
    REQUIRE_FALSE(st.ok());
    REQUIRE(st.error());
    CHECK(st.error()->fWhat.find("Difficulty.child") != std::string::npos);

    st = WorldDataOverrideEngine::Apply(*root, WorldDataOverride{u8"Difficulty", u8"hard"});
    REQUIRE_FALSE(st.ok());
    REQUIRE(st.error());
    CHECK(st.error()->fWhat.find("valid Int") != std::string::npos);

    st = WorldDataOverrideEngine::Apply(*root, WorldDataOverride{u8"invalid", u8"{value:1}trailing"});
    REQUIRE_FALSE(st.ok());
    REQUIRE(st.error());
    CHECK(st.error()->fWhat.find("valid SNBT") != std::string::npos);
  }
}
