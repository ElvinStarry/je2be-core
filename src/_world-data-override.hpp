#pragma once

#include <je2be/nbt.hpp>
#include <je2be/status.hpp>
#include <je2be/world-data-override.hpp>

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace je2be {

class WorldDataOverrideEngine {
public:
  struct JavaDocuments {
    CompoundTagPtr fLevel;
    CompoundTagPtr fGameRules;
    CompoundTagPtr fWorldGenSettings;
    CompoundTagPtr fWeather;
    CompoundTagPtr fWorldClocks;
    CompoundTagPtr fEnderDragonFight;
  };

  static Status Apply(CompoundTag &root, WorldDataOverride const &entry) {
    std::string reason;
    auto path = ParsePath(entry.fPath, reason);
    if (!path) {
      return Error(entry, reason);
    }
    return Apply(root, *path, 0, entry, reason);
  }

  static Status Apply(CompoundTag &root, std::vector<WorldDataOverride> const &entries) {
    for (auto const &entry : entries) {
      auto st = Apply(root, entry);
      if (!st.ok()) {
        return st;
      }
    }
    return Status::Ok();
  }

  static Status ApplyBedrock(CompoundTag &root, std::vector<WorldDataOverride> const &entries) {
    bool experimentsTouched = false;
    for (auto const &entry : entries) {
      std::string reason;
      auto path = ParsePath(entry.fPath, reason);
      if (!path) {
        return Error(entry, reason);
      }
      size_t offset = 0;
      if (IsOneOf(path->at(0), {u8"level", u8"level_dat"})) {
        offset++;
      }
      if (offset >= path->size()) {
        return Error(entry, "path does not name a field");
      }
      if (path->at(offset) == u8"experiments" && offset + 1 < path->size() &&
          !IsOneOf(path->at(offset + 1), {u8"experiments_ever_used", u8"saved_with_toggled_experiments"})) {
        experimentsTouched = true;
      }
      auto st = Apply(root, *path, offset, entry, reason);
      if (!st.ok()) {
        return st;
      }
    }

    if (experimentsTouched) {
      auto experiments = root.compoundTag(u8"experiments");
      if (!experiments) {
        experiments = Compound();
        root.set(u8"experiments", experiments);
      }
      experiments->set(u8"experiments_ever_used", Bool(true));
      experiments->set(u8"saved_with_toggled_experiments", Bool(true));
    }
    return Status::Ok();
  }

  static Status ApplyJava(JavaDocuments &documents, std::vector<WorldDataOverride> const &entries) {
    for (auto const &entry : entries) {
      std::string reason;
      auto path = ParsePath(entry.fPath, reason);
      if (!path) {
        return Error(entry, reason);
      }

      size_t offset = 0;
      if (IsOneOf(path->at(offset), {u8"level", u8"level_dat"})) {
        offset++;
      }
      if (offset < path->size() && path->at(offset) == u8"Data") {
        offset++;
      }
      if (offset >= path->size()) {
        return Error(entry, "path does not name a field");
      }

      auto const &head = path->at(offset);
      if (head == u8"experiments") {
        if (offset + 2 != path->size()) {
          return Error(entry, "Java experiment paths must be experiments.<feature-id>");
        }
        if (!documents.fLevel) {
          documents.fLevel = Compound();
        }
        bool enabled = false;
        if (!ParseBoolean(entry.fValue, enabled, reason)) {
          return Error(entry, reason);
        }
        if (!SetJavaExperiment(*documents.fLevel, path->at(offset + 1), enabled, reason)) {
          return Error(entry, reason);
        }
        continue;
      }

      CompoundTagPtr *target = &documents.fLevel;
      if (IsOneOf(head, {u8"game_rules", u8"GameRules"})) {
        target = &documents.fGameRules;
        offset++;
      } else if (IsOneOf(head, {u8"world_gen_settings", u8"WorldGenSettings"})) {
        target = &documents.fWorldGenSettings;
        offset++;
      } else if (head == u8"weather") {
        target = &documents.fWeather;
        offset++;
      } else if (head == u8"world_clocks") {
        target = &documents.fWorldClocks;
        offset++;
      } else if (IsOneOf(head, {u8"ender_dragon_fight", u8"DragonFight"})) {
        target = &documents.fEnderDragonFight;
        offset++;
      }

      if (offset >= path->size()) {
        return Error(entry, "path does not name a field inside the selected Java data document");
      }
      if (!*target) {
        *target = Compound();
      }
      auto st = Apply(**target, *path, offset, entry, reason);
      if (!st.ok()) {
        return st;
      }
    }
    return Status::Ok();
  }

private:
  static std::string Narrow(std::u8string const &value) {
    return std::string(reinterpret_cast<char const *>(value.data()), value.size());
  }

  static Status Error(WorldDataOverride const &entry, std::string const &reason) {
    return Status::Error(__FILE__, __LINE__, "invalid world data override '" + Narrow(entry.fPath) + "': " + reason);
  }

  static bool IsOneOf(std::u8string const &value, std::initializer_list<std::u8string_view> choices) {
    return std::find(choices.begin(), choices.end(), value) != choices.end();
  }

  static std::optional<std::vector<std::u8string>> ParsePath(std::u8string const &raw, std::string &reason) {
    if (raw.empty()) {
      reason = "path is empty";
      return std::nullopt;
    }
    std::vector<std::u8string> ret;
    std::u8string part;
    bool escaped = false;
    for (char8_t ch : raw) {
      if (escaped) {
        part.push_back(ch);
        escaped = false;
      } else if (ch == u8'\\') {
        escaped = true;
      } else if (ch == u8'.') {
        if (part.empty()) {
          reason = "path contains an empty component";
          return std::nullopt;
        }
        ret.push_back(part);
        part.clear();
      } else {
        part.push_back(ch);
      }
    }
    if (escaped) {
      reason = "path ends with an incomplete escape";
      return std::nullopt;
    }
    if (part.empty()) {
      reason = "path contains an empty component";
      return std::nullopt;
    }
    ret.push_back(part);
    return ret;
  }

  static std::optional<size_t> ParseIndex(std::u8string const &raw) {
    size_t ret = 0;
    auto first = reinterpret_cast<char const *>(raw.data());
    auto last = first + raw.size();
    auto parsed = std::from_chars(first, last, ret);
    if (parsed.ec != std::errc{} || parsed.ptr != last) {
      return std::nullopt;
    }
    return ret;
  }

  static std::shared_ptr<Tag> ParseSnbt(std::u8string const &raw) {
    std::string compact;
    std::optional<char> quote;
    bool escaped = false;
    for (char ch : Narrow(raw)) {
      if (quote) {
        compact.push_back(ch);
        if (escaped) {
          escaped = false;
        } else if (ch == '\\') {
          escaped = true;
        } else if (ch == *quote) {
          quote.reset();
        }
      } else if (ch == '\'' || ch == '"') {
        quote = ch;
        compact.push_back(ch);
      } else if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
        compact.push_back(ch);
      }
    }
    if (quote || escaped) {
      return nullptr;
    }

    std::istringstream stream("{value:" + compact + "}");
    auto parsed = CompoundTag::FromSnbt(stream);
    if (!parsed || parsed->size() != 1) {
      return nullptr;
    }
    stream >> std::ws;
    if (!stream.eof()) {
      return nullptr;
    }
    return parsed->tag(u8"value");
  }

  static std::optional<i64> IntegralValue(Tag const &tag) {
    switch (tag.type()) {
    case Tag::Type::Byte: {
      u8 raw = tag.asByte()->fValue;
      return std::bit_cast<i8>(raw);
    }
    case Tag::Type::Short:
      return tag.asShort()->fValue;
    case Tag::Type::Int:
      return tag.asInt()->fValue;
    case Tag::Type::Long:
      return tag.asLong()->fValue;
    default:
      return std::nullopt;
    }
  }

  static std::optional<long double> NumericValue(Tag const &tag) {
    if (auto integer = IntegralValue(tag); integer) {
      return static_cast<long double>(*integer);
    }
    if (tag.type() == Tag::Type::Float) {
      return static_cast<long double>(tag.asFloat()->fValue);
    }
    if (tag.type() == Tag::Type::Double) {
      return static_cast<long double>(tag.asDouble()->fValue);
    }
    return std::nullopt;
  }

  static std::shared_ptr<Tag> ParseValue(std::u8string const &raw, Tag const *existing, std::string &reason) {
    if (existing && existing->type() == Tag::Type::String) {
      if (!raw.empty() && (raw.front() == u8'\'' || raw.front() == u8'"')) {
        auto parsed = ParseSnbt(raw);
        if (!parsed || parsed->type() != Tag::Type::String) {
          reason = "value is not a valid quoted String";
          return nullptr;
        }
        return parsed;
      }
      return String(raw);
    }

    if (!existing && std::any_of(raw.begin(), raw.end(), [](char8_t ch) { return ch == u8' ' || ch == u8'\t' || ch == u8'\r' || ch == u8'\n'; }) &&
        !raw.empty() && raw.front() != u8'{' && raw.front() != u8'[' && raw.front() != u8'\'' && raw.front() != u8'"') {
      return String(raw);
    }

    auto parsed = ParseSnbt(raw);
    if (!parsed) {
      if (existing) {
        auto numeric = ParseNumericAs(raw, existing->type());
        if (numeric) {
          return numeric;
        }
      }
      if (!existing && !raw.empty() && raw.front() != u8'{' && raw.front() != u8'[' && raw.front() != u8'\"' && raw.front() != u8'\'') {
        return String(raw);
      }
      reason = "value is not valid SNBT for the target field";
      return nullptr;
    }
    if (!existing) {
      return parsed;
    }

    switch (existing->type()) {
    case Tag::Type::Byte: {
      auto value = IntegralValue(*parsed);
      if (!value || *value < std::numeric_limits<i8>::lowest() || *value > std::numeric_limits<i8>::max()) {
        reason = "value is not a valid Byte";
        return nullptr;
      }
      return Byte(static_cast<i8>(*value));
    }
    case Tag::Type::Short: {
      auto value = IntegralValue(*parsed);
      if (!value || *value < std::numeric_limits<i16>::lowest() || *value > std::numeric_limits<i16>::max()) {
        reason = "value is not a valid Short";
        return nullptr;
      }
      return Short(static_cast<i16>(*value));
    }
    case Tag::Type::Int: {
      auto value = IntegralValue(*parsed);
      if (!value || *value < std::numeric_limits<i32>::lowest() || *value > std::numeric_limits<i32>::max()) {
        reason = "value is not a valid Int";
        return nullptr;
      }
      return Int(static_cast<i32>(*value));
    }
    case Tag::Type::Long: {
      auto value = IntegralValue(*parsed);
      if (!value) {
        reason = "value is not a valid Long";
        return nullptr;
      }
      return Long(*value);
    }
    case Tag::Type::Float: {
      auto value = NumericValue(*parsed);
      if (!value || !std::isfinite(static_cast<double>(*value)) ||
          *value < -std::numeric_limits<float>::max() || *value > std::numeric_limits<float>::max()) {
        reason = "value is not a valid Float";
        return nullptr;
      }
      return Float(static_cast<float>(*value));
    }
    case Tag::Type::Double: {
      auto value = NumericValue(*parsed);
      if (!value || !std::isfinite(static_cast<double>(*value))) {
        reason = "value is not a valid Double";
        return nullptr;
      }
      return Double(static_cast<double>(*value));
    }
    case Tag::Type::List: {
      if (parsed->type() != Tag::Type::List) {
        reason = "value is not a List";
        return nullptr;
      }
      auto const *expected = existing->asList();
      auto actual = std::dynamic_pointer_cast<ListTag>(parsed);
      if (actual->empty() && expected->fType != Tag::Type::End) {
        return std::make_shared<ListTag>(expected->fType);
      }
      if (expected->fType != Tag::Type::End && actual->fType != expected->fType) {
        reason = "list element type does not match the target field";
        return nullptr;
      }
      return actual;
    }
    case Tag::Type::Compound:
    case Tag::Type::ByteArray:
    case Tag::Type::IntArray:
    case Tag::Type::LongArray:
      if (parsed->type() != existing->type()) {
        reason = "value type does not match the target field";
        return nullptr;
      }
      return parsed;
    case Tag::Type::String:
      return String(raw);
    case Tag::Type::End:
    default:
      reason = "target field has an unsupported NBT type";
      return nullptr;
    }
  }

  static Status Apply(CompoundTag &compound,
                      std::vector<std::u8string> const &path,
                      size_t offset,
                      WorldDataOverride const &entry,
                      std::string &reason) {
    auto const &name = path.at(offset);
    if (offset + 1 == path.size()) {
      auto existing = compound.tag(name);
      auto value = ParseValue(entry.fValue, existing.get(), reason);
      if (!value) {
        return Error(entry, reason);
      }
      compound.set(name, value);
      return Status::Ok();
    }

    auto &child = compound[name];
    if (!child) {
      child = Compound();
    }
    return Apply(child, path, offset + 1, entry, reason);
  }

  static Status Apply(std::shared_ptr<Tag> &node,
                      std::vector<std::u8string> const &path,
                      size_t offset,
                      WorldDataOverride const &entry,
                      std::string &reason) {
    if (node->type() == Tag::Type::Compound) {
      return Apply(*std::dynamic_pointer_cast<CompoundTag>(node), path, offset, entry, reason);
    }
    if (node->type() == Tag::Type::List) {
      return Apply(*std::dynamic_pointer_cast<ListTag>(node), path, offset, entry, reason);
    }
    if (node->type() == Tag::Type::ByteArray || node->type() == Tag::Type::IntArray || node->type() == Tag::Type::LongArray) {
      return ApplyArray(node, path, offset, entry, reason);
    }
    reason = "path traverses through a scalar NBT field";
    return Error(entry, reason);
  }

  static Status Apply(ListTag &list,
                      std::vector<std::u8string> const &path,
                      size_t offset,
                      WorldDataOverride const &entry,
                      std::string &reason) {
    auto index = ParseIndex(path.at(offset));
    if (!index) {
      reason = "list path component is not an index";
      return Error(entry, reason);
    }
    if (*index > list.size()) {
      reason = "list index is out of range";
      return Error(entry, reason);
    }

    bool const isLast = offset + 1 == path.size();
    if (*index == list.size()) {
      if (isLast) {
        std::shared_ptr<Tag> expected;
        if (list.fType != Tag::Type::End) {
          expected = EmptyTag(list.fType);
        }
        auto value = ParseValue(entry.fValue, expected.get(), reason);
        if (!value || !list.push_back(value)) {
          if (reason.empty()) {
            reason = "value type does not match the list element type";
          }
          return Error(entry, reason);
        }
        return Status::Ok();
      }

      std::shared_ptr<Tag> value;
      if (list.fType == Tag::Type::End || list.fType == Tag::Type::Compound) {
        value = Compound();
      } else if (list.fType == Tag::Type::List) {
        value = std::make_shared<ListTag>(Tag::Type::End);
      } else {
        reason = "cannot append an intermediate object to this list type";
        return Error(entry, reason);
      }
      if (!list.push_back(value)) {
        reason = "cannot append an intermediate object to this list";
        return Error(entry, reason);
      }
    }

    auto &child = list.at(*index);
    if (isLast) {
      auto value = ParseValue(entry.fValue, child.get(), reason);
      if (!value) {
        return Error(entry, reason);
      }
      child = value;
      return Status::Ok();
    }
    return Apply(child, path, offset + 1, entry, reason);
  }

  static Status ApplyArray(std::shared_ptr<Tag> &node,
                           std::vector<std::u8string> const &path,
                           size_t offset,
                           WorldDataOverride const &entry,
                           std::string &reason) {
    if (offset + 1 != path.size()) {
      reason = "array elements cannot contain child fields";
      return Error(entry, reason);
    }
    auto index = ParseIndex(path.at(offset));
    if (!index) {
      reason = "array path component is not an index";
      return Error(entry, reason);
    }

    if (node->type() == Tag::Type::ByteArray) {
      auto array = std::dynamic_pointer_cast<ByteArrayTag>(node);
      if (*index > array->fValue.size()) {
        reason = "array index is out of range";
        return Error(entry, reason);
      }
      auto existing = Byte(*index < array->fValue.size() ? std::bit_cast<i8>(array->fValue[*index]) : 0);
      auto value = ParseValue(entry.fValue, existing.get(), reason);
      if (!value) {
        return Error(entry, reason);
      }
      if (*index == array->fValue.size()) {
        array->fValue.push_back(value->asByte()->fValue);
      } else {
        array->fValue[*index] = value->asByte()->fValue;
      }
      return Status::Ok();
    }
    if (node->type() == Tag::Type::IntArray) {
      auto array = std::dynamic_pointer_cast<IntArrayTag>(node);
      if (*index > array->fValue.size()) {
        reason = "array index is out of range";
        return Error(entry, reason);
      }
      auto existing = Int(*index < array->fValue.size() ? array->fValue[*index] : 0);
      auto value = ParseValue(entry.fValue, existing.get(), reason);
      if (!value) {
        return Error(entry, reason);
      }
      if (*index == array->fValue.size()) {
        array->fValue.push_back(value->asInt()->fValue);
      } else {
        array->fValue[*index] = value->asInt()->fValue;
      }
      return Status::Ok();
    }
    if (node->type() == Tag::Type::LongArray) {
      auto array = std::dynamic_pointer_cast<LongArrayTag>(node);
      if (*index > array->fValue.size()) {
        reason = "array index is out of range";
        return Error(entry, reason);
      }
      auto existing = Long(*index < array->fValue.size() ? array->fValue[*index] : 0);
      auto value = ParseValue(entry.fValue, existing.get(), reason);
      if (!value) {
        return Error(entry, reason);
      }
      if (*index == array->fValue.size()) {
        array->fValue.push_back(value->asLong()->fValue);
      } else {
        array->fValue[*index] = value->asLong()->fValue;
      }
      return Status::Ok();
    }

    reason = "path traverses through a scalar NBT field";
    return Error(entry, reason);
  }

  static std::shared_ptr<Tag> EmptyTag(Tag::Type type) {
    switch (type) {
    case Tag::Type::Byte: return Byte(0);
    case Tag::Type::Short: return Short(0);
    case Tag::Type::Int: return Int(0);
    case Tag::Type::Long: return Long(0);
    case Tag::Type::Float: return Float(0);
    case Tag::Type::Double: return Double(0);
    case Tag::Type::String: return String(u8"");
    case Tag::Type::List: return std::make_shared<ListTag>(Tag::Type::End);
    case Tag::Type::Compound: return Compound();
    case Tag::Type::ByteArray: return std::make_shared<ByteArrayTag>();
    case Tag::Type::IntArray: return std::make_shared<IntArrayTag>();
    case Tag::Type::LongArray: return std::make_shared<LongArrayTag>();
    case Tag::Type::End:
    default: return nullptr;
    }
  }

  static std::shared_ptr<Tag> ParseNumericAs(std::u8string raw, Tag::Type type) {
    while (!raw.empty() && (raw.front() == u8' ' || raw.front() == u8'\t' || raw.front() == u8'\r' || raw.front() == u8'\n')) {
      raw.erase(raw.begin());
    }
    while (!raw.empty() && (raw.back() == u8' ' || raw.back() == u8'\t' || raw.back() == u8'\r' || raw.back() == u8'\n')) {
      raw.pop_back();
    }
    if (raw.empty()) {
      return nullptr;
    }

    auto stripSuffix = [&raw](char8_t lower, char8_t upper) {
      if (!raw.empty() && (raw.back() == lower || raw.back() == upper)) {
        raw.pop_back();
      }
    };
    switch (type) {
    case Tag::Type::Byte: stripSuffix(u8'b', u8'B'); break;
    case Tag::Type::Short: stripSuffix(u8's', u8'S'); break;
    case Tag::Type::Long: stripSuffix(u8'l', u8'L'); break;
    case Tag::Type::Float: stripSuffix(u8'f', u8'F'); break;
    case Tag::Type::Double: stripSuffix(u8'd', u8'D'); break;
    default: break;
    }

    auto first = reinterpret_cast<char const *>(raw.data());
    auto last = first + raw.size();
    if (type == Tag::Type::Byte && (raw == u8"true" || raw == u8"false")) {
      return Bool(raw == u8"true");
    }
    if (type == Tag::Type::Byte || type == Tag::Type::Short || type == Tag::Type::Int || type == Tag::Type::Long) {
      i64 value = 0;
      auto parsed = std::from_chars(first, last, value);
      if (parsed.ec != std::errc{} || parsed.ptr != last) {
        return nullptr;
      }
      switch (type) {
      case Tag::Type::Byte:
        return value >= std::numeric_limits<i8>::lowest() && value <= std::numeric_limits<i8>::max() ? Byte(static_cast<i8>(value)) : nullptr;
      case Tag::Type::Short:
        return value >= std::numeric_limits<i16>::lowest() && value <= std::numeric_limits<i16>::max() ? Short(static_cast<i16>(value)) : nullptr;
      case Tag::Type::Int:
        return value >= std::numeric_limits<i32>::lowest() && value <= std::numeric_limits<i32>::max() ? Int(static_cast<i32>(value)) : nullptr;
      case Tag::Type::Long:
        return Long(value);
      default:
        return nullptr;
      }
    }
    if (type == Tag::Type::Float || type == Tag::Type::Double) {
      auto text = Narrow(raw);
      char *end = nullptr;
      double value = std::strtod(text.c_str(), &end);
      if (end != text.c_str() + text.size() || !std::isfinite(value)) {
        return nullptr;
      }
      if (type == Tag::Type::Float) {
        if (value < -std::numeric_limits<float>::max() || value > std::numeric_limits<float>::max()) {
          return nullptr;
        }
        return Float(static_cast<float>(value));
      }
      return Double(value);
    }
    return nullptr;
  }

  static bool ParseBoolean(std::u8string const &raw, bool &value, std::string &reason) {
    auto expected = Bool(false);
    auto parsed = ParseValue(raw, expected.get(), reason);
    if (!parsed) {
      return false;
    }
    value = parsed->asByte()->fValue != 0;
    return true;
  }

  static bool SetJavaExperiment(CompoundTag &level, std::u8string const &feature, bool enabled, std::string &reason) {
    auto features = level.listTag(u8"enabled_features");
    if (features && features->fType != Tag::Type::String) {
      reason = "enabled_features is not a String list";
      return false;
    }
    if (!features && enabled) {
      features = List<Tag::Type::String>();
      level.set(u8"enabled_features", features);
    }
    if (features) {
      SetStringMembership(*features, feature, enabled);
      if (enabled) {
        SetStringMembership(*features, u8"minecraft:vanilla", true);
      }
    }

    auto packName = feature;
    auto colon = packName.find(u8':');
    if (colon != std::u8string::npos) {
      packName = packName.substr(colon + 1);
    }
    auto packs = level.compoundTag(u8"DataPacks");
    if (!packs && enabled) {
      packs = Compound();
      level.set(u8"DataPacks", packs);
    }
    if (packs) {
      auto enabledPacks = EnsureStringList(*packs, u8"Enabled");
      auto disabledPacks = EnsureStringList(*packs, u8"Disabled");
      if (!enabledPacks || !disabledPacks) {
        reason = "DataPacks.Enabled and DataPacks.Disabled must be String lists";
        return false;
      }
      SetStringMembership(*enabledPacks, packName, enabled);
      SetStringMembership(*disabledPacks, packName, !enabled);
    }
    return true;
  }

  static std::shared_ptr<ListTag> EnsureStringList(CompoundTag &compound, std::u8string const &name) {
    auto list = compound.listTag(name);
    if (list) {
      return list->fType == Tag::Type::String ? list : nullptr;
    }
    list = List<Tag::Type::String>();
    compound.set(name, list);
    return list;
  }

  static void SetStringMembership(ListTag &list, std::u8string const &value, bool present) {
    auto found = std::find_if(list.fValue.begin(), list.fValue.end(), [&value](std::shared_ptr<Tag> const &tag) {
      auto string = tag ? tag->asString() : nullptr;
      return string && string->fValue == value;
    });
    if (present && found == list.fValue.end()) {
      list.push_back(String(value));
    } else if (!present && found != list.fValue.end()) {
      list.fValue.erase(found);
    }
  }
};

} // namespace je2be
