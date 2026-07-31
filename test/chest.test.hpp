#include "bedrock/_chest-pair.hpp"

#include <array>

namespace {

class ChestPairTestAccessor {
public:
  std::shared_ptr<mcfile::be::Block const> blockAt(int x, int y, int z) {
    auto found = fBlocks.find(Pos3i(x, y, z));
    return found == fBlocks.end() ? nullptr : found->second;
  }

  std::shared_ptr<CompoundTag const> blockEntityAt(Pos3i const &pos) {
    auto found = fBlockEntities.find(pos);
    return found == fBlockEntities.end() ? nullptr : found->second;
  }

  std::unordered_map<Pos3i, std::shared_ptr<mcfile::be::Block const>, Pos3iHasher> fBlocks;
  std::unordered_map<Pos3i, CompoundTagPtr, Pos3iHasher> fBlockEntities;
};

static CompoundTagPtr ChestTag(Pos3i const &pos, std::optional<Pos3i> const &pair, std::optional<bool> pairLead, bool forceUnpair = false) {
  auto tag = Compound();
  tag->set(u8"id", u8"Chest");
  tag->set(u8"x", Int(pos.fX));
  tag->set(u8"y", Int(pos.fY));
  tag->set(u8"z", Int(pos.fZ));
  if (pair) {
    tag->set(u8"pairx", Int(pair->fX));
    tag->set(u8"pairz", Int(pair->fZ));
  }
  if (pairLead) {
    tag->set(u8"pairlead", Bool(*pairLead));
  }
  if (forceUnpair) {
    tag->set(u8"forceunpair", Bool(true));
  }
  return tag;
}

} // namespace

TEST_CASE("bedrock adjacent double chest pairing") {
  auto states = Compound();
  states->set(u8"minecraft:cardinal_direction", u8"north");
  auto chest = std::make_shared<mcfile::be::Block>(u8"minecraft:chest", states, java::kBlockDataVersion);

  ChestPairTestAccessor cache;
  std::array<Pos3i, 4> const positions = {
      Pos3i(0, 64, 0),
      Pos3i(1, 64, 0),
      Pos3i(2, 64, 0),
      Pos3i(3, 64, 0),
  };
  for (Pos3i const &pos : positions) {
    cache.fBlocks[pos] = chest;
  }

  // Bedrock saves can describe a pair on only one half. The reverse pointer
  // must complete that pair without joining the two middle adjacent chests.
  cache.fBlockEntities[positions[0]] = ChestTag(positions[0], positions[1], true);
  cache.fBlockEntities[positions[1]] = ChestTag(positions[1], std::nullopt, false);
  cache.fBlockEntities[positions[2]] = ChestTag(positions[2], positions[3], true);
  cache.fBlockEntities[positions[3]] = ChestTag(positions[3], std::nullopt, false);

  std::array<std::u8string, 4> const expectedTypes = {u8"left", u8"right", u8"left", u8"right"};
  for (size_t i = 0; i < positions.size(); i++) {
    auto normalized = je2be::bedrock::ChestPair::Normalize(positions[i], *chest, *cache.fBlockEntities[positions[i]], cache);
    REQUIRE(normalized);
    CHECK(normalized->int32(u8"pairx") == positions[i ^ 1].fX);
    CHECK(normalized->int32(u8"pairz") == positions[i ^ 1].fZ);
    CHECK(normalized->boolean(u8"pairlead") == (i % 2 == 0));
    CHECK(je2be::bedrock::ChestPair::JavaType(positions[i], *chest, *normalized) == expectedTypes[i]);
    CHECK(je2be::bedrock::ChestPair::TakeItemsFrom(positions[i], *chest, *normalized) == positions[i ^ 1]);
  }
}

TEST_CASE("bedrock forced unpaired chest remains single") {
  auto states = Compound();
  states->set(u8"minecraft:cardinal_direction", u8"north");
  auto chest = std::make_shared<mcfile::be::Block>(u8"minecraft:chest", states, java::kBlockDataVersion);

  Pos3i const left(0, 64, 0);
  Pos3i const right(1, 64, 0);
  ChestPairTestAccessor cache;
  cache.fBlocks[left] = chest;
  cache.fBlocks[right] = chest;
  cache.fBlockEntities[left] = ChestTag(left, right, true);
  cache.fBlockEntities[right] = ChestTag(right, left, false, true);

  for (Pos3i const &pos : {left, right}) {
    auto normalized = je2be::bedrock::ChestPair::Normalize(pos, *chest, *cache.fBlockEntities[pos], cache);
    REQUIRE(normalized);
    CHECK_FALSE(normalized->int32(u8"pairx"));
    CHECK_FALSE(normalized->int32(u8"pairz"));
    CHECK(je2be::bedrock::ChestPair::JavaType(pos, *chest, *normalized) == u8"single");
  }
}
