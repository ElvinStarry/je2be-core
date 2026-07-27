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
