#include <je2be/java/uuid-replacer.hpp>

#include <je2be/nbt.hpp>
#include <je2be/uuid.hpp>

#include <minecraft-file.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <set>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace je2be::java {
namespace {

namespace fs = std::filesystem;
using Mapping = std::unordered_map<Uuid, Uuid, UuidHasher, UuidPred>;

struct RenameOperation {
  fs::path fFrom;
  fs::path fTo;
  fs::path fTemporary;
};

std::string ToString(std::u8string const &value) {
  return std::string(reinterpret_cast<char const *>(value.data()), value.size());
}

std::u8string ToU8String(std::string const &value) {
  return std::u8string(reinterpret_cast<char8_t const *>(value.data()), value.size());
}

std::string Trim(std::string_view value) {
  size_t first = 0;
  while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first]))) {
    first++;
  }
  size_t last = value.size();
  while (first < last && std::isspace(static_cast<unsigned char>(value[last - 1]))) {
    last--;
  }
  return std::string(value.substr(first, last - first));
}

bool IsHex(char ch) {
  return ('0' <= ch && ch <= '9') || ('a' <= ch && ch <= 'f') || ('A' <= ch && ch <= 'F');
}

bool IsDashedUuidAt(std::string_view value, size_t offset) {
  if (offset + 36 > value.size()) {
    return false;
  }
  for (size_t i = 0; i < 36; i++) {
    bool const dash = i == 8 || i == 13 || i == 18 || i == 23;
    if (dash ? value[offset + i] != '-' : !IsHex(value[offset + i])) {
      return false;
    }
  }
  return true;
}

bool IsUndashedUuidAt(std::string_view value, size_t offset) {
  if (offset + 32 > value.size()) {
    return false;
  }
  if (offset > 0 && IsHex(value[offset - 1])) {
    return false;
  }
  if (offset + 32 < value.size() && IsHex(value[offset + 32])) {
    return false;
  }
  for (size_t i = 0; i < 32; i++) {
    if (!IsHex(value[offset + i])) {
      return false;
    }
  }
  return true;
}

std::optional<Uuid> FindMapped(Uuid const &before, Mapping const &mapping) {
  auto found = mapping.find(before);
  if (found == mapping.end()) {
    return std::nullopt;
  }
  return found->second;
}

bool ReplaceText(std::string &value, Mapping const &mapping, uint64_t &replacements) {
  bool changed = false;
  for (size_t i = 0; i < value.size();) {
    size_t length = 0;
    bool dashed = false;
    if (IsDashedUuidAt(value, i)) {
      length = 36;
      dashed = true;
    } else if (IsUndashedUuidAt(value, i)) {
      length = 32;
    } else {
      i++;
      continue;
    }

    auto before = Uuid::FromString(ToU8String(value.substr(i, length)));
    auto after = before ? FindMapped(*before, mapping) : std::nullopt;
    if (!after) {
      i += length;
      continue;
    }

    std::string replacement = ToString(after->toString());
    if (!dashed) {
      replacement.erase(std::remove(replacement.begin(), replacement.end(), '-'), replacement.end());
    }
    value.replace(i, length, replacement);
    replacements++;
    changed = true;
    i += replacement.size();
  }
  return changed;
}

Uuid FromLongPair(int64_t most, int64_t least) {
  Uuid result;
  uint64_t values[2] = {static_cast<uint64_t>(most), static_cast<uint64_t>(least)};
  for (size_t part = 0; part < 2; part++) {
    for (size_t i = 0; i < 8; i++) {
      result.fData[part * 8 + i] = static_cast<uint8_t>(values[part] >> ((7 - i) * 8));
    }
  }
  return result;
}

std::pair<int64_t, int64_t> ToLongPair(Uuid const &uuid) {
  uint64_t values[2] = {0, 0};
  for (size_t part = 0; part < 2; part++) {
    for (size_t i = 0; i < 8; i++) {
      values[part] = (values[part] << 8) | uuid.fData[part * 8 + i];
    }
  }
  return {static_cast<int64_t>(values[0]), static_cast<int64_t>(values[1])};
}

bool ReplaceLongPair(std::shared_ptr<LongTag> const &most,
                     std::shared_ptr<LongTag> const &least,
                     Mapping const &mapping,
                     uint64_t &replacements) {
  auto after = FindMapped(FromLongPair(most->fValue, least->fValue), mapping);
  if (!after) {
    return false;
  }
  auto values = ToLongPair(*after);
  most->fValue = values.first;
  least->fValue = values.second;
  replacements++;
  return true;
}

bool ReplaceTag(TagPtr &tag, Mapping const &mapping, uint64_t &replacements);

bool ReplaceCompound(CompoundTag &compound, Mapping const &mapping, uint64_t &replacements) {
  bool changed = false;
  std::vector<std::u8string> keys;
  keys.reserve(compound.size());
  for (auto const &[key, _] : compound) {
    keys.push_back(key);
  }

  for (auto const &key : keys) {
    for (auto const &[firstSuffix, secondSuffix] : {std::pair<std::u8string_view, std::u8string_view>(u8"Most", u8"Least"),
                                                   std::pair<std::u8string_view, std::u8string_view>(u8"MSB", u8"LSB")}) {
      if (!key.ends_with(firstSuffix)) {
        continue;
      }
      std::u8string other = key.substr(0, key.size() - firstSuffix.size());
      other.append(secondSuffix);
      auto first = std::dynamic_pointer_cast<LongTag>(compound.tag(key));
      auto second = std::dynamic_pointer_cast<LongTag>(compound.tag(other));
      if (first && second) {
        changed |= ReplaceLongPair(first, second, mapping, replacements);
      }
    }
  }

  for (auto const &key : keys) {
    TagPtr child = compound.tag(key);
    if (child && ReplaceTag(child, mapping, replacements)) {
      compound[key] = child;
      changed = true;
    }
  }
  return changed;
}

bool ReplaceTag(TagPtr &tag, Mapping const &mapping, uint64_t &replacements) {
  switch (tag->type()) {
  case Tag::Type::Compound:
    return ReplaceCompound(*std::dynamic_pointer_cast<CompoundTag>(tag), mapping, replacements);
  case Tag::Type::List: {
    auto list = std::dynamic_pointer_cast<ListTag>(tag);
    if (list->fType == Tag::Type::Int && list->size() == 4) {
      std::vector<int32_t> values;
      for (auto const &item : *list) {
        values.push_back(item->asInt()->fValue);
      }
      IntArrayTag array(values);
      auto before = Uuid::FromIntArray(array);
      auto after = before ? FindMapped(*before, mapping) : std::nullopt;
      if (after) {
        auto replacement = after->toIntArrayTag()->value();
        for (size_t i = 0; i < 4; i++) {
          list->at(i) = std::make_shared<IntTag>(replacement[i]);
        }
        replacements++;
        return true;
      }
    } else if (list->fType == Tag::Type::Long && list->size() == 2) {
      auto most = std::dynamic_pointer_cast<LongTag>(list->at(0));
      auto least = std::dynamic_pointer_cast<LongTag>(list->at(1));
      if (ReplaceLongPair(most, least, mapping, replacements)) {
        return true;
      }
    }
    bool changed = false;
    for (size_t i = 0; i < list->size(); i++) {
      TagPtr child = list->at(i);
      if (ReplaceTag(child, mapping, replacements)) {
        list->at(i) = child;
        changed = true;
      }
    }
    return changed;
  }
  case Tag::Type::IntArray: {
    auto array = std::dynamic_pointer_cast<IntArrayTag>(tag);
    auto before = Uuid::FromIntArray(*array);
    auto after = before ? FindMapped(*before, mapping) : std::nullopt;
    if (!after) {
      return false;
    }
    tag = after->toIntArrayTag();
    replacements++;
    return true;
  }
  case Tag::Type::LongArray: {
    auto array = std::dynamic_pointer_cast<LongArrayTag>(tag);
    if (array->fValue.size() != 2) {
      return false;
    }
    auto after = FindMapped(FromLongPair(array->fValue[0], array->fValue[1]), mapping);
    if (!after) {
      return false;
    }
    auto values = ToLongPair(*after);
    array->fValue = {values.first, values.second};
    replacements++;
    return true;
  }
  case Tag::Type::ByteArray: {
    auto array = std::dynamic_pointer_cast<ByteArrayTag>(tag);
    if (array->fValue.size() != 16) {
      return false;
    }
    Uuid before;
    std::copy(array->fValue.begin(), array->fValue.end(), before.fData);
    auto after = FindMapped(before, mapping);
    if (!after) {
      return false;
    }
    std::copy(std::begin(after->fData), std::end(after->fData), array->fValue.begin());
    replacements++;
    return true;
  }
  case Tag::Type::String: {
    auto string = std::dynamic_pointer_cast<StringTag>(tag);
    std::string value = ToString(string->fValue);
    if (!ReplaceText(value, mapping, replacements)) {
      return false;
    }
    string->fValue = ToU8String(value);
    return true;
  }
  default:
    return false;
  }
}

Status ReadMapping(fs::path const &csv, Mapping &mapping) {
  std::ifstream stream(csv, std::ios::binary);
  if (!stream) {
    return JE2BE_ERROR_WHAT("Cannot open mapping CSV: " + csv.string());
  }

  std::unordered_map<Uuid, size_t, UuidHasher, UuidPred> destinations;
  std::unordered_map<Uuid, size_t, UuidHasher, UuidPred> sources;
  std::string line;
  size_t lineNumber = 0;
  bool hasRows = false;
  while (std::getline(stream, line)) {
    lineNumber++;
    if (lineNumber == 1 && line.starts_with("\xEF\xBB\xBF")) {
      line.erase(0, 3);
    }
    if (Trim(line).empty()) {
      continue;
    }
    size_t comma = line.find(',');
    if (comma == std::string::npos || line.find(',', comma + 1) != std::string::npos) {
      return JE2BE_ERROR_WHAT("Invalid CSV row " + std::to_string(lineNumber) + ": expected beforeUUID,afterUUID");
    }
    std::string beforeText = Trim(std::string_view(line).substr(0, comma));
    std::string afterText = Trim(std::string_view(line).substr(comma + 1));
    auto before = Uuid::FromString(ToU8String(beforeText));
    auto after = Uuid::FromString(ToU8String(afterText));
    if (!before || !after) {
      return JE2BE_ERROR_WHAT("Invalid UUID in CSV row " + std::to_string(lineNumber));
    }
    hasRows = true;
    auto source = sources.find(*before);
    if (source != sources.end()) {
      return JE2BE_ERROR_WHAT("Duplicate source UUID in CSV rows " + std::to_string(source->second) + " and " + std::to_string(lineNumber));
    }
    sources.emplace(*before, lineNumber);
    auto destination = destinations.find(*after);
    if (destination != destinations.end()) {
      return JE2BE_ERROR_WHAT("Duplicate destination UUID in CSV rows " + std::to_string(destination->second) + " and " + std::to_string(lineNumber));
    }
    destinations.emplace(*after, lineNumber);
    if (!UuidPred{}(*before, *after)) {
      mapping.emplace(*before, *after);
    }
  }
  if (!stream.eof()) {
    return JE2BE_ERROR_WHAT("Failed to read mapping CSV: " + csv.string());
  }
  if (!hasRows) {
    return JE2BE_ERROR_WHAT("Mapping CSV contains no UUID mappings");
  }
  return Status::Ok();
}

std::optional<fs::path> UniqueSibling(fs::path const &path, std::string_view suffix) {
  std::error_code ec;
  for (uint64_t i = 0; i < 10000; i++) {
    fs::path candidate = path;
    candidate += suffix;
    candidate += std::to_string(i);
    if (!fs::exists(candidate, ec) && !ec) {
      return candidate;
    }
    if (ec) {
      return std::nullopt;
    }
  }
  return std::nullopt;
}

template <class Writer>
Status WriteAtomically(fs::path const &path, Writer writer) {
  auto temporary = UniqueSibling(path, ".je2be-uuid-write-");
  auto backup = UniqueSibling(path, ".je2be-uuid-backup-");
  if (!temporary || !backup) {
    return JE2BE_ERROR_WHAT("Cannot allocate a temporary path beside: " + path.string());
  }
  if (!writer(*temporary)) {
    std::error_code ignored;
    fs::remove(*temporary, ignored);
    return JE2BE_ERROR_WHAT("Failed to write replacement file: " + path.string());
  }

  std::error_code ec;
  auto permissions = fs::status(path, ec).permissions();
  if (!ec) {
    fs::permissions(*temporary, permissions, ec);
  }
  ec.clear();
  fs::rename(path, *backup, ec);
  if (ec) {
    fs::remove(*temporary, ec);
    return JE2BE_ERROR_WHAT("Failed to stage original file: " + path.string());
  }
  fs::rename(*temporary, path, ec);
  if (ec) {
    std::error_code rollback;
    fs::rename(*backup, path, rollback);
    fs::remove(*temporary, rollback);
    return JE2BE_ERROR_WHAT("Failed to install replacement file: " + path.string());
  }
  fs::remove(*backup, ec);
  if (ec) {
    return JE2BE_ERROR_WHAT("Replacement succeeded but the temporary backup could not be removed: " + backup->string());
  }
  return Status::Ok();
}

bool IsNbtFile(fs::path const &path) {
  std::string extension = path.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return extension == ".dat" || extension == ".dat_old" || extension == ".nbt";
}

Status ProcessNbt(fs::path const &path,
                  Mapping const &mapping,
                  UuidReplacer::Options const &options,
                  UuidReplacer::Result &result) {
  bool gzip = false;
  {
    std::ifstream header(path, std::ios::binary);
    std::array<unsigned char, 2> magic{};
    header.read(reinterpret_cast<char *>(magic.data()), magic.size());
    gzip = header.gcount() == 2 && magic[0] == 0x1f && magic[1] == 0x8b;
  }

  CompoundTagPtr root;
  if (gzip) {
    auto input = std::make_shared<mcfile::stream::GzFileInputStream>(path);
    root = CompoundTag::Read(input, mcfile::Encoding::Java);
  } else {
    root = CompoundTag::ReadFromFile(path, mcfile::Encoding::Java);
  }
  if (!root) {
    return JE2BE_ERROR_WHAT("Failed to parse Java NBT file: " + path.string());
  }

  TagPtr tag = root;
  if (!ReplaceTag(tag, mapping, result.fReplacements)) {
    return Status::Ok();
  }
  result.fChangedFiles++;
  if (options.fDryRun) {
    return Status::Ok();
  }

  return WriteAtomically(path, [root, gzip](fs::path const &temporary) {
    if (gzip) {
      auto output = std::make_shared<mcfile::stream::GzFileOutputStream>(temporary);
      bool ok = CompoundTag::Write(*root, output, mcfile::Encoding::Java);
      output.reset();
      return ok;
    }
    return CompoundTag::Write(*root, temporary, mcfile::Encoding::Java);
  });
}

Status ProcessJson(fs::path const &path,
                   Mapping const &mapping,
                   UuidReplacer::Options const &options,
                   UuidReplacer::Result &result) {
  std::string value;
  {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
      return JE2BE_ERROR_WHAT("Cannot open JSON file: " + path.string());
    }
    value.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
  }
  auto parsed = nlohmann::json::parse(value, nullptr, false);
  if (parsed.is_discarded()) {
    return JE2BE_ERROR_WHAT("Failed to parse JSON file: " + path.string());
  }
  if (!ReplaceText(value, mapping, result.fReplacements)) {
    return Status::Ok();
  }
  result.fChangedFiles++;
  if (options.fDryRun) {
    return Status::Ok();
  }
  return WriteAtomically(path, [&value](fs::path const &temporary) {
    std::ofstream output(temporary, std::ios::binary);
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
    return output.good();
  });
}

Status McaLocations(fs::path const &path, std::array<bool, 1024> &locations) {
  std::ifstream stream(path, std::ios::binary);
  std::array<unsigned char, 4096> header{};
  if (!stream.read(reinterpret_cast<char *>(header.data()), header.size())) {
    return JE2BE_ERROR_WHAT("Failed to read region header: " + path.string());
  }
  for (size_t i = 0; i < locations.size(); i++) {
    size_t p = i * 4;
    locations[i] = header[p] != 0 || header[p + 1] != 0 || header[p + 2] != 0 || header[p + 3] != 0;
  }
  return Status::Ok();
}

bool CopyMcaTimestamps(fs::path const &from, fs::path const &to) {
  std::ifstream input(from, std::ios::binary);
  std::array<char, 4096> timestamps{};
  input.seekg(4096);
  if (!input.read(timestamps.data(), timestamps.size())) {
    return false;
  }
  std::fstream output(to, std::ios::binary | std::ios::in | std::ios::out);
  output.seekp(4096);
  output.write(timestamps.data(), timestamps.size());
  return output.good();
}

Status ProcessMca(fs::path const &path,
                  Mapping const &mapping,
                  UuidReplacer::Options const &options,
                  UuidReplacer::Result &result) {
  std::array<bool, 1024> locations{};
  if (auto status = McaLocations(path, locations); !status.ok()) {
    return status;
  }
  auto editor = mcfile::je::McaEditor::Open(path);
  if (!editor) {
    return JE2BE_ERROR_WHAT("Failed to open region file: " + path.string());
  }

  bool changed = false;
  for (int z = 0; z < 32; z++) {
    for (int x = 0; x < 32; x++) {
      size_t index = static_cast<size_t>(z * 32 + x);
      if (!locations[index]) {
        continue;
      }
      auto root = editor->get(x, z);
      if (!root) {
        return JE2BE_ERROR_WHAT("Failed to decompress chunk " + std::to_string(x) + "," + std::to_string(z) + " in " + path.string());
      }
      TagPtr tag = root;
      if (!ReplaceTag(tag, mapping, result.fReplacements)) {
        continue;
      }
      changed = true;
      result.fChangedChunks++;
      if (!options.fDryRun && !editor->insert(x, z, *root)) {
        return JE2BE_ERROR_WHAT("Failed to update chunk " + std::to_string(x) + "," + std::to_string(z) + " in " + path.string());
      }
    }
  }
  if (!changed) {
    return Status::Ok();
  }
  result.fChangedFiles++;
  if (options.fDryRun) {
    return Status::Ok();
  }
  return WriteAtomically(path, [&path, &editor](fs::path const &temporary) {
    return editor->write(temporary) && CopyMcaTimestamps(path, temporary);
  });
}

Status CollectFiles(fs::path const &world, std::vector<fs::path> &files) {
  std::error_code ec;
  fs::recursive_directory_iterator iterator(world, fs::directory_options::skip_permission_denied, ec);
  fs::recursive_directory_iterator end;
  if (ec) {
    return JE2BE_ERROR_WHAT("Cannot enumerate world directory: " + ec.message());
  }
  while (iterator != end) {
    auto const &entry = *iterator;
    if (entry.is_symlink(ec)) {
      if (entry.is_directory(ec)) {
        iterator.disable_recursion_pending();
      }
    } else if (entry.is_regular_file(ec)) {
      files.push_back(entry.path());
    }
    if (ec) {
      return JE2BE_ERROR_WHAT("Cannot inspect world entry: " + entry.path().string() + ": " + ec.message());
    }
    iterator.increment(ec);
    if (ec) {
      return JE2BE_ERROR_WHAT("Cannot enumerate world directory: " + ec.message());
    }
  }
  std::sort(files.begin(), files.end());
  return Status::Ok();
}

Status PlanRenames(std::vector<fs::path> const &files, Mapping const &mapping, std::vector<RenameOperation> &operations) {
  for (auto const &path : files) {
    std::string parent = path.parent_path().filename().string();
    std::string extension = path.extension().string();
    bool const playerFile = parent == "playerdata" && (extension == ".dat" || extension == ".dat_old");
    bool const jsonFile = (parent == "advancements" || parent == "stats") && extension == ".json";
    if (!playerFile && !jsonFile) {
      continue;
    }
    std::string stem = path.stem().string();
    auto before = Uuid::FromString(ToU8String(stem));
    auto after = before ? FindMapped(*before, mapping) : std::nullopt;
    if (!after) {
      continue;
    }
    fs::path target = path.parent_path() / (ToString(after->toString()) + extension);
    if (target != path) {
      operations.push_back({path, target, {}});
    }
  }

  std::set<fs::path> sources;
  std::set<fs::path> targets;
  for (auto const &operation : operations) {
    sources.insert(operation.fFrom);
    if (!targets.insert(operation.fTo).second) {
      return JE2BE_ERROR_WHAT("Multiple player files would be renamed to: " + operation.fTo.string());
    }
  }
  std::error_code ec;
  for (auto const &operation : operations) {
    if (fs::exists(operation.fTo, ec) && !sources.contains(operation.fTo)) {
      return JE2BE_ERROR_WHAT("Player file rename target already exists: " + operation.fTo.string());
    }
    if (ec) {
      return JE2BE_ERROR_WHAT("Cannot check player file rename target: " + operation.fTo.string());
    }
  }
  return Status::Ok();
}

Status ApplyRenames(std::vector<RenameOperation> &operations) {
  std::error_code ec;
  size_t staged = 0;
  for (; staged < operations.size(); staged++) {
    auto temporary = UniqueSibling(operations[staged].fFrom, ".je2be-uuid-rename-");
    if (!temporary) {
      break;
    }
    operations[staged].fTemporary = *temporary;
    fs::rename(operations[staged].fFrom, *temporary, ec);
    if (ec) {
      break;
    }
  }
  if (staged != operations.size()) {
    for (size_t i = staged; i > 0; i--) {
      std::error_code ignored;
      fs::rename(operations[i - 1].fTemporary, operations[i - 1].fFrom, ignored);
    }
    return JE2BE_ERROR_WHAT("Failed to stage player file rename: " + operations[staged].fFrom.string());
  }

  size_t installed = 0;
  for (; installed < operations.size(); installed++) {
    fs::rename(operations[installed].fTemporary, operations[installed].fTo, ec);
    if (ec) {
      break;
    }
  }
  if (installed != operations.size()) {
    for (size_t i = installed; i > 0; i--) {
      std::error_code ignored;
      fs::rename(operations[i - 1].fTo, operations[i - 1].fFrom, ignored);
    }
    for (size_t i = installed; i < operations.size(); i++) {
      std::error_code ignored;
      fs::rename(operations[i].fTemporary, operations[i].fFrom, ignored);
    }
    return JE2BE_ERROR_WHAT("Failed to install player file rename: " + operations[installed].fTo.string());
  }
  return Status::Ok();
}

} // namespace

Status UuidReplacer::Run(fs::path const &worldDirectory,
                         fs::path const &mappingCsv,
                         Options const &options,
                         Result &result) {
  result = {};
  std::error_code ec;
  if (!fs::is_directory(worldDirectory, ec) || ec) {
    return JE2BE_ERROR_WHAT("Java world directory does not exist: " + worldDirectory.string());
  }
  if (!fs::is_regular_file(worldDirectory / "level.dat", ec) || ec) {
    return JE2BE_ERROR_WHAT("Input is not a Java world (level.dat is missing): " + worldDirectory.string());
  }

  Mapping mapping;
  if (auto status = ReadMapping(mappingCsv, mapping); !status.ok()) {
    return JE2BE_ERROR_PUSH(status);
  }

  std::vector<fs::path> files;
  if (auto status = CollectFiles(worldDirectory, files); !status.ok()) {
    return JE2BE_ERROR_PUSH(status);
  }
  std::vector<RenameOperation> renames;
  if (auto status = PlanRenames(files, mapping, renames); !status.ok()) {
    return JE2BE_ERROR_PUSH(status);
  }

  for (auto const &path : files) {
    result.fVisitedFiles++;
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    Status status;
    if (extension == ".mca") {
      status = ProcessMca(path, mapping, options, result);
    } else if (IsNbtFile(path)) {
      status = ProcessNbt(path, mapping, options, result);
    } else if (extension == ".json") {
      status = ProcessJson(path, mapping, options, result);
    }
    if (!status.ok()) {
      return JE2BE_ERROR_PUSH(status);
    }
  }

  result.fRenamedFiles = renames.size();
  if (!options.fDryRun) {
    if (auto status = ApplyRenames(renames); !status.ok()) {
      return JE2BE_ERROR_PUSH(status);
    }
  }
  return Status::Ok();
}

} // namespace je2be::java
