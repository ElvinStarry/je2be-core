#pragma once

#include <je2be/nbt.hpp>

namespace je2be::bedrock {

static inline bool EnsureJavaChunkSections(CompoundTag &root) {
  CompoundTag *column = &root;
  std::u8string name = u8"sections";
  if (auto level = root.compoundTag(u8"Level"); level) {
    column = level.get();
    name = u8"Sections";
  }

  if (auto existing = column->tag(name); existing) {
    auto sections = existing->asList();
    return sections && sections->fType == Tag::Type::Compound;
  }
  column->set(name, List<Tag::Type::Compound>());
  return true;
}

} // namespace je2be::bedrock
