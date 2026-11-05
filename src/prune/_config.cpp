#include "prune/_config.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <limits>
#include <sstream>

namespace je2be::prune {

namespace {

Status Fail(std::string const &what) {
  return JE2BE_ERROR_WHAT(what);
}

Status GetString(nlohmann::json const &parent, char const *key, std::string &out, std::string const &context) {
  auto found = parent.find(key);
  if (found == parent.end()) {
    return Fail(context + ": missing \"" + key + "\"");
  }
  if (!found->is_string()) {
    return Fail(context + ": \"" + key + "\" must be a string");
  }
  out = found->get<std::string>();
  return Status::Ok();
}

Status GetInt32(nlohmann::json const &parent, char const *key, int32_t &out, std::string const &context) {
  auto found = parent.find(key);
  if (found == parent.end()) {
    return Fail(context + ": missing \"" + key + "\"");
  }
  // nlohmann reports uint64 values as integers too. Convert them separately
  // so UINT64_MAX does not wrap to -1 through get<int64_t>().
  if (found->is_number_unsigned()) {
    uint64_t v = found->get<uint64_t>();
    if (std::numeric_limits<int32_t>::max() < v) {
      return Fail(context + ": \"" + key + "\" is out of int32 range");
    }
    out = static_cast<int32_t>(v);
    return Status::Ok();
  }
  if (!found->is_number_integer()) {
    return Fail(context + ": \"" + key + "\" must be an integer");
  }
  int64_t v = found->get<int64_t>();
  if (v < std::numeric_limits<int32_t>::min() || std::numeric_limits<int32_t>::max() < v) {
    return Fail(context + ": \"" + key + "\" is out of int32 range");
  }
  out = static_cast<int32_t>(v);
  return Status::Ok();
}

Status ParseAction(std::string const &s, Action &out, std::string const &context) {
  if (s == "keep") {
    out = Action::Keep;
    return Status::Ok();
  }
  if (s == "delete") {
    out = Action::Delete;
    return Status::Ok();
  }
  return Fail(context + ": unknown action \"" + s + "\"; expected \"keep\" or \"delete\"");
}

} // namespace

Status Config::ParseFile(std::filesystem::path const &path, Config &out) {
  std::ifstream ifs(path, std::ios::binary);
  if (!ifs) {
    return Fail("cannot open config file " + path.string());
  }
  std::ostringstream ss;
  ss << ifs.rdbuf();
  if (!ifs.good() && !ifs.eof()) {
    return Fail("cannot read config file " + path.string());
  }
  Status st = ParseJson(ss.str(), out);
  if (!st.ok()) {
    return JE2BE_ERROR_PUSH(st);
  }
  return Status::Ok();
}

Status Config::ParseJson(std::string const &text, Config &out) {
  using nlohmann::json;

  json root;
  try {
    root = json::parse(text);
  } catch (json::parse_error &e) {
    return Fail(std::string("invalid JSON: ") + e.what());
  }
  if (!root.is_object()) {
    return Fail("config root must be an object");
  }

  std::string const context = "config";

  {
    int32_t version = 0;
    Status st = GetInt32(root, "schemaVersion", version, context);
    if (!st.ok()) {
      return JE2BE_ERROR_PUSH(st);
    }
    if (version != 1) {
      return Fail(context + ": unsupported \"schemaVersion\" " + std::to_string(version) + "; expected 1");
    }
    out.fSchemaVersion = 1;
  }

  {
    Status st = GetString(root, "format", out.fFormat, context);
    if (!st.ok()) {
      return JE2BE_ERROR_PUSH(st);
    }
    if (out.fFormat != "mbr-chunk-selection") {
      return Fail(context + ": unsupported \"format\" \"" + out.fFormat + "\"; expected \"mbr-chunk-selection\"");
    }
  }

  {
    Status st = GetInt32(root, "dimension", out.fDimension, context);
    if (!st.ok()) {
      return JE2BE_ERROR_PUSH(st);
    }
    if (!CanonicalDimensionFromInt(out.fDimension)) {
      return Fail(context + ": unsupported \"dimension\" " + std::to_string(out.fDimension) + "; expected -1 (nether), 0 (overworld), or 1 (end)");
    }
  }

  {
    Status st = GetInt32(root, "chunkSize", out.fChunkSize, context);
    if (!st.ok()) {
      return JE2BE_ERROR_PUSH(st);
    }
    if (out.fChunkSize != 16) {
      return Fail(context + ": unsupported \"chunkSize\" " + std::to_string(out.fChunkSize) + "; expected 16");
    }
  }

  out.fDefaultAction = Action::Keep;
  if (auto found = root.find("defaultAction"); found != root.end()) {
    if (!found->is_string()) {
      return Fail(context + ": \"defaultAction\" must be a string");
    }
    Status st = ParseAction(found->get<std::string>(), out.fDefaultAction, context + ".defaultAction");
    if (!st.ok()) {
      return JE2BE_ERROR_PUSH(st);
    }
  }

  {
    Status st = GetString(root, "overlapPolicy", out.fOverlapPolicy, context);
    if (!st.ok()) {
      return JE2BE_ERROR_PUSH(st);
    }
    if (out.fOverlapPolicy != "last-wins") {
      return Fail(context + ": unsupported \"overlapPolicy\" \"" + out.fOverlapPolicy + "\"; expected \"last-wins\"");
    }
  }

  auto regions = root.find("regions");
  if (regions == root.end()) {
    return Fail(context + ": missing \"regions\"");
  }
  if (!regions->is_array()) {
    return Fail(context + ": \"regions\" must be an array");
  }
  out.fRegions.clear();
  out.fRegions.reserve(regions->size());
  for (size_t i = 0; i < regions->size(); i++) {
    json const &r = (*regions)[i];
    std::string const rc = context + ".regions[" + std::to_string(i) + "]";
    if (!r.is_object()) {
      return Fail(rc + ": must be an object");
    }
    ChunkRegion region;
    std::string action;
    Status st = GetString(r, "action", action, rc);
    if (!st.ok()) {
      return JE2BE_ERROR_PUSH(st);
    }
    st = ParseAction(action, region.fAction, rc + ".action");
    if (!st.ok()) {
      return JE2BE_ERROR_PUSH(st);
    }
    st = GetInt32(r, "minCx", region.fMinCx, rc);
    if (!st.ok()) {
      return JE2BE_ERROR_PUSH(st);
    }
    st = GetInt32(r, "maxCx", region.fMaxCx, rc);
    if (!st.ok()) {
      return JE2BE_ERROR_PUSH(st);
    }
    st = GetInt32(r, "minCz", region.fMinCz, rc);
    if (!st.ok()) {
      return JE2BE_ERROR_PUSH(st);
    }
    st = GetInt32(r, "maxCz", region.fMaxCz, rc);
    if (!st.ok()) {
      return JE2BE_ERROR_PUSH(st);
    }
    if (region.fMaxCx < region.fMinCx) {
      return Fail(rc + ": maxCx must be greater than or equal to minCx");
    }
    if (region.fMaxCz < region.fMinCz) {
      return Fail(rc + ": maxCz must be greater than or equal to minCz");
    }
    out.fRegions.push_back(region);
  }

  return Status::Ok();
}

} // namespace je2be::prune
