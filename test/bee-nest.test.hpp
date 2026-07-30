TEST_CASE("bee-nest") {
  SUBCASE("bedrock") {
    fs::path thisFile(__FILE__);
    auto mcworld = thisFile.parent_path() / "data" / "bee-nest" / "bedrock" / "bee-nest.mcworld";
    auto tmp = mcfile::File::CreateTempDir(fs::temp_directory_path());
    defer {
      fs::remove_all(*tmp);
    };
    auto in = *tmp / "in";
    REQUIRE(ZipFile::Unzip(mcworld, in).ok());
    auto out = *tmp / "out";
    auto temp = *tmp / "temp";
    REQUIRE(fs::create_directories(temp));
    bedrock::Options opt;
    opt.fTempDirectory = temp;
    opt.fDimensionFilter.insert(Dimension::Overworld);
    opt.fChunkFilter.insert({0, 0});

    auto st = bedrock::Converter::Run(in, out, opt, thread::hardware_concurrency());
    CHECK(st.ok());
    CHECK(fs::is_empty(temp));
    auto regionFile = out / u8"dimensions" / u8"minecraft" / u8"overworld" / u8"region" / u8"r.0.0.mca";
    auto region = mcfile::je::Region::MakeRegion(regionFile);
    REQUIRE(region);
    auto chunk = region->chunkAt(0, 0);
    REQUIRE(chunk);
    auto tile = chunk->tileEntityAt(2, -60, 2);
    REQUIRE(tile);
    CHECK(tile->string(u8"id") == u8"minecraft:beehive");
    auto bees = tile->listTag(u8"bees");
    REQUIRE(bees);
    CHECK(bees->size() == 1);
    auto bee = bees->at(0);
    REQUIRE(bee->asCompound());
    auto entityData = bee->asCompound()->compoundTag(u8"entity_data");
    REQUIRE(entityData);
    CHECK(entityData->string(u8"id") == u8"minecraft:bee");

    struct EmptyProgress : bedrock::Progress {
      bool reportConvert(Rational<u64> const &, u64) override {
        return true;
      }
      bool reportTerraform(Rational<u64> const &, u64) override {
        fTerraformReports++;
        return true;
      }
      int fTerraformReports = 0;
    } emptyProgress;
    bedrock::Options emptyOpt;
    emptyOpt.fTempDirectory = temp;
    emptyOpt.fDimensionFilter.insert(Dimension::Overworld);
    emptyOpt.fChunkFilter.insert({10000, 10000});
    auto emptyOut = *tmp / "empty-out";
    st = bedrock::Converter::Run(in, emptyOut, emptyOpt, thread::hardware_concurrency(), &emptyProgress);
    CHECK(st.ok());
    CHECK(emptyProgress.fTerraformReports == 0);
    CHECK(fs::is_empty(temp));
  }
}
