#pragma once

#include "_pos3.hpp"
#include "enums/_facing6.hpp"

#include <je2be/nbt.hpp>
#include <je2be/pos2.hpp>

#include <minecraft-file.hpp>

#include <tuple>

namespace je2be::bedrock {

class ChestPair {
  ChestPair() = delete;

public:
  struct Pair {
    Pos3i fPos;
    bool fLead;
  };

  static bool IsChest(mcfile::be::Block const &block) {
    return block.fName == u8"minecraft:chest" ||
           block.fName == u8"minecraft:trapped_chest" ||
           block.fName.ends_with(u8"copper_chest");
  }

  template <class BlockAccessor>
  static CompoundTagPtr Normalize(Pos3i const &pos, mcfile::be::Block const &block, CompoundTag const &tag, BlockAccessor &cache) {
    auto normalized = tag.copy();
    normalized->erase(u8"pairlead");
    normalized->erase(u8"pairx");
    normalized->erase(u8"pairz");

    auto pair = Resolve(pos, block, tag, cache);
    if (pair) {
      normalized->set(u8"pairlead", Bool(pair->fLead));
      normalized->set(u8"pairx", Int(pair->fPos.fX));
      normalized->set(u8"pairz", Int(pair->fPos.fZ));
    }
    return normalized;
  }

  static std::u8string JavaType(Pos3i const &pos, mcfile::be::Block const &block, CompoundTag const &tag) {
    auto pair = DeclaredPair(pos, tag);
    if (!pair) {
      return u8"single";
    }

    Pos2i const facing = Facing(block);
    Pos2i const current(pos.fX, pos.fZ);
    Pos2i const paired(pair->fX, pair->fZ);
    if (paired + Right90(facing) == current) {
      return u8"right";
    }
    if (paired + Left90(facing) == current) {
      return u8"left";
    }
    return u8"single";
  }

  static std::optional<Pos3i> TakeItemsFrom(Pos3i const &pos, mcfile::be::Block const &block, CompoundTag const &tag) {
    auto pair = DeclaredPair(pos, tag);
    if (!pair) {
      return std::nullopt;
    }

    auto type = JavaType(pos, block, tag);
    bool const pairLead = tag.boolean(u8"pairlead", false);
    if ((type == u8"right" && !pairLead) || (type == u8"left" && pairLead)) {
      return pair;
    }
    return std::nullopt;
  }

private:
  template <class BlockAccessor>
  static std::optional<Pair> Resolve(Pos3i const &pos, mcfile::be::Block const &block, CompoundTag const &tag, BlockAccessor &cache) {
    if (tag.boolean(u8"forceunpair", false)) {
      return std::nullopt;
    }

    if (auto declared = DeclaredPair(pos, tag); declared) {
      if (auto pairTag = ValidPartner(pos, block, *declared, cache); pairTag) {
        return Pair{*declared, PairLead(pos, tag, *declared, *pairTag)};
      }
    }

    Pos2i const facing = Facing(block);
    std::optional<Pair> found;
    // Either half may omit pair coordinates, so accept a unique adjacent
    // chest that explicitly points back to this position.
    for (Pos2i const offset : {Left90(facing), Right90(facing)}) {
      Pos3i const candidate(pos.fX + offset.fX, pos.fY, pos.fZ + offset.fZ);
      auto pairTag = ValidPartner(pos, block, candidate, cache);
      if (!pairTag) {
        continue;
      }
      auto reverse = DeclaredPair(candidate, *pairTag);
      if (!reverse || *reverse != pos) {
        continue;
      }
      if (found) {
        return std::nullopt;
      }
      found = Pair{candidate, PairLead(pos, tag, candidate, *pairTag)};
    }
    return found;
  }

  template <class BlockAccessor>
  static std::shared_ptr<CompoundTag const> ValidPartner(Pos3i const &pos, mcfile::be::Block const &block, Pos3i const &candidate, BlockAccessor &cache) {
    if (!IsSideNeighbor(pos, block, candidate)) {
      return nullptr;
    }
    auto pairBlock = cache.blockAt(candidate.fX, candidate.fY, candidate.fZ);
    if (!pairBlock || pairBlock->fName != block.fName || Facing(*pairBlock) != Facing(block)) {
      return nullptr;
    }
    auto pairTag = cache.blockEntityAt(candidate);
    if (!pairTag) {
      // Bedrock may store the pair coordinates on only one half. The block
      // itself still proves the partner exists; the caller can synthesize the
      // missing block entity when writing the Java chest.
      return Compound();
    }
    if (pairTag->boolean(u8"forceunpair", false)) {
      return nullptr;
    }
    if (auto reverse = DeclaredPair(candidate, *pairTag); reverse && *reverse != pos) {
      return nullptr;
    }
    return pairTag;
  }

  static std::optional<Pos3i> DeclaredPair(Pos3i const &pos, CompoundTag const &tag) {
    auto pairX = tag.int32(u8"pairx");
    auto pairZ = tag.int32(u8"pairz");
    if (!pairX || !pairZ) {
      return std::nullopt;
    }
    return Pos3i(*pairX, pos.fY, *pairZ);
  }

  static bool PairLead(Pos3i const &pos, CompoundTag const &tag, Pos3i const &pair, CompoundTag const &pairTag) {
    auto current = tag.boolean(u8"pairlead");
    auto other = pairTag.boolean(u8"pairlead");
    if (current && other && *current != *other) {
      return *current;
    }
    if (current && !other) {
      return *current;
    }
    if (!current && other) {
      return !*other;
    }
    // Corrupt or incomplete lead flags still need complementary values.
    return std::tie(pos.fX, pos.fZ) < std::tie(pair.fX, pair.fZ);
  }

  static bool IsSideNeighbor(Pos3i const &pos, mcfile::be::Block const &block, Pos3i const &candidate) {
    if (candidate.fY != pos.fY) {
      return false;
    }
    Pos2i const facing = Facing(block);
    Pos2i const delta(candidate.fX - pos.fX, candidate.fZ - pos.fZ);
    if (facing == Pos2i(0, 0) || delta == Pos2i(0, 0)) {
      return false;
    }
    return delta == Left90(facing) || delta == Right90(facing);
  }

  static Pos2i Facing(mcfile::be::Block const &block) {
    if (!block.fStates) {
      return Pos2i(0, 0);
    }
    Facing6 facing;
    if (auto cardinal = block.fStates->string(u8"minecraft:cardinal_direction"); cardinal) {
      facing = Facing6FromBedrockCardinalDirection(*cardinal);
    } else {
      facing = Facing6FromBedrockFacingDirectionA(block.fStates->int32(u8"facing_direction", 0));
    }
    Pos3i const direction = Pos3iFromFacing6(facing);
    return Pos2i(direction.fX, direction.fZ);
  }
};

} // namespace je2be::bedrock
