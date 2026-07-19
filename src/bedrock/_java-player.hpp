#pragma once

#include <string_view>

#include <je2be/bedrock/options.hpp>
#include <je2be/nbt.hpp>

namespace je2be::bedrock {

class JavaPlayer {
private:
  JavaPlayer() = delete;

public:
  static std::optional<std::u8string> NameFromInventoryMarker(CompoundTag const &player);
  static std::optional<Uuid> UuidFromMojangResponse(std::string_view response);
  static std::optional<Uuid> ResolveUuid(std::u8string const &name, Options const &options);
  static std::optional<Uuid> ResolveUuid(CompoundTag const &player, Options const &options);
};

} // namespace je2be::bedrock
