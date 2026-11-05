#include "prune/_java.hpp"

#include "prune/_selection.hpp"

#include <je2be/java/options.hpp>
#include <je2be/nbt.hpp>

#include <minecraft-file.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bitset>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace je2be::prune {

namespace {

namespace fs = std::filesystem;

Status Err(std::string const &what) {
  return JE2BE_ERROR_WHAT(what);
}

std::string ErrnoString(int e) {
  return Errno::StringFromErrno(e);
}

bool SeekFile(FILE *fp, uint64_t offset) {
#if defined(_WIN32)
  return _fseeki64(fp, static_cast<__int64>(offset), SEEK_SET) == 0;
#else
  return fseeko(fp, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}

#if defined(_WIN32)
bool TruncateFile(FILE *fp) {
  return _chsize_s(_fileno(fp), 0) == 0;
}
#else
bool TruncateFile(FILE *fp) {
  return ftruncate(fileno(fp), 0) == 0;
}
#endif

class SessionLockGuard {
public:
  ~SessionLockGuard() {
    unlock();
  }

  Status lock(std::filesystem::path const &levelRoot) {
    namespace fs = std::filesystem;
    fs::path path = levelRoot / "session.lock";
    // Open without truncating: truncating before the lock is held would damage
    // the marker while a running game still owns the file.
    FILE *fp = std::fopen(path.string().c_str(), "r+b");
    if (!fp && errno == ENOENT) {
      fp = std::fopen(path.string().c_str(), "w+b");
    }
    if (!fp) {
      int e = errno;
      return Err("cannot open " + path.string() + " for locking: " + ErrnoString(e));
    }
#if defined(_WIN32)
    int fd = _fileno(fp);
    HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
    if (handle == INVALID_HANDLE_VALUE || !::LockFile(handle, 0, 0, MAXDWORD, MAXDWORD)) {
      std::fclose(fp);
      return Err(path.string() + " is locked; stop the world (server) before pruning");
    }
#else
    struct flock fl;
    std::memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;
    if (fcntl(fileno(fp), F_SETLK, &fl) == -1) {
      int e = errno;
      std::fclose(fp);
      return Err(path.string() + " is locked; stop the world (server) before pruning: " + ErrnoString(e));
    }
#endif
    fFile = fp;
    fPath = path;
    static char const kSnowman[] = "\xe2\x98\x83"; // "☃", same marker as the vanilla session lock
    if (TruncateFile(fp)) {
      std::fseek(fp, 0, SEEK_SET);
      std::fwrite(kSnowman, 1, 3, fp);
      std::fflush(fp);
    }
    return Status::Ok();
  }

private:
  void unlock() {
    if (!fFile) {
      return;
    }
#if defined(_WIN32)
    HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(fFile)));
    if (handle != INVALID_HANDLE_VALUE) {
      ::UnlockFile(handle, 0, 0, MAXDWORD, MAXDWORD);
    }
#else
    struct flock fl;
    std::memset(&fl, 0, sizeof(fl));
    fl.l_type = F_UNLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;
    fcntl(fileno(fFile), F_SETLK, &fl);
#endif
    std::fclose(fFile);
    fFile = nullptr;
  }

  FILE *fFile = nullptr;
  std::filesystem::path fPath;
};

fs::path DimensionDirectory(je2be::java::Options const &options, fs::path const &input, CanonicalDimension dimension) {
  mcfile::Dimension d;
  switch (dimension) {
  case CanonicalDimension::Nether:
    d = mcfile::Dimension::Nether;
    break;
  case CanonicalDimension::End:
    d = mcfile::Dimension::End;
    break;
  case CanonicalDimension::Overworld:
  default:
    d = mcfile::Dimension::Overworld;
    break;
  }
  return options.getWorldDirectory(input, d);
}

bool ParseRegionFileName(std::string const &name, int &rx, int &rz) {
  if (!name.starts_with("r.") || !name.ends_with(".mca")) {
    return false;
  }
  std::string middle = name.substr(2, name.size() - 2 - 4);
  size_t dot = middle.find('.');
  if (dot == std::string::npos || middle.find('.', dot + 1) != std::string::npos) {
    return false;
  }
  std::string xs = middle.substr(0, dot);
  std::string zs = middle.substr(dot + 1);
  if (xs.empty() || zs.empty()) {
    return false;
  }
  auto parse = [](std::string const &s, int &out) {
    int v = 0;
    auto result = std::from_chars(s.data(), s.data() + s.size(), v);
    if (result.ec != std::errc() || result.ptr != s.data() + s.size()) {
      return false;
    }
    out = v;
    return true;
  };
  return parse(xs, rx) && parse(zs, rz);
}

struct RegionPlan {
  fs::path fPath;
  int fRegionX = 0;
  int fRegionZ = 0;
  uint64_t fFileSize = 0;
  std::array<uint32_t, 1024> fLocations {};
  std::bitset<1024> fExternal;
  bool fHasExternal = false;
  bool fEmpty = false;
};

uint32_t GetU32BE(unsigned char const *p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

Status ReadRegionPlan(fs::path const &path, int rx, int rz, RegionPlan &plan) {
  namespace fs = std::filesystem;
  plan.fPath = path;
  plan.fRegionX = rx;
  plan.fRegionZ = rz;

  std::error_code ec;
  uint64_t const size = fs::file_size(path, ec);
  if (ec) {
    return Err("cannot stat region file " + path.string() + ": " + ec.message());
  }
  plan.fFileSize = size;
  if (size == 0) {
    plan.fEmpty = true;
    return Status::Ok();
  }
  if (size < 8192) {
    return Err("region file " + path.string() + " is shorter than its 8 KiB header (" + std::to_string(size) + " bytes)");
  }
  std::ifstream ifs(path, std::ios::binary);
  if (!ifs) {
    return Err("cannot open region file " + path.string());
  }
  std::array<unsigned char, 8192> header;
  if (!ifs.read(reinterpret_cast<char *>(header.data()), header.size())) {
    return Err("cannot read region header " + path.string());
  }
  for (int i = 0; i < 1024; i++) {
    plan.fLocations[i] = GetU32BE(header.data() + i * 4);
  }

  struct Range {
    uint32_t fOffset;
    uint32_t fSectors;
    int fIndex;
  };
  std::vector<Range> ranges;
  for (int i = 0; i < 1024; i++) {
    uint32_t loc = plan.fLocations[i];
    if (loc == 0) {
      continue;
    }
    uint32_t offset = loc >> 8;
    uint32_t sectors = loc & 0xFF;
    if (sectors == 0) {
      return Err("region file " + path.string() + " has chunk index " + std::to_string(i) + " with a zero sector count");
    }
    if (offset < 2) {
      return Err("region file " + path.string() + " has chunk index " + std::to_string(i) + " pointing into the header");
    }
    uint64_t end = (static_cast<uint64_t>(offset) + sectors) * 4096;
    if (size < end) {
      return Err("region file " + path.string() + " has chunk index " + std::to_string(i) + " pointing past the end of the file");
    }
    ranges.push_back({offset, sectors, i});
  }
  std::sort(ranges.begin(), ranges.end(), [](Range const &a, Range const &b) {
    if (a.fOffset != b.fOffset) {
      return a.fOffset < b.fOffset;
    }
    return a.fSectors < b.fSectors;
  });
  for (size_t i = 1; i < ranges.size(); i++) {
    uint64_t prevEnd = static_cast<uint64_t>(ranges[i - 1].fOffset) + ranges[i - 1].fSectors;
    if (ranges[i].fOffset < prevEnd) {
      return Err("region file " + path.string() + " has overlapping chunk sectors (index " + std::to_string(ranges[i - 1].fIndex) + " and " + std::to_string(ranges[i].fIndex) + ")");
    }
  }

  for (Range const &range : ranges) {
    if (!ifs.seekg(static_cast<std::streamoff>(range.fOffset) * 4096, std::ios::beg)) {
      return Err("cannot seek to chunk payload in " + path.string());
    }
    unsigned char head[5];
    if (!ifs.read(reinterpret_cast<char *>(head), sizeof(head))) {
      return Err("cannot read chunk payload header in " + path.string());
    }
    uint32_t length = GetU32BE(head);
    uint8_t compression = head[4];
    uint64_t maxLength = static_cast<uint64_t>(range.fSectors) * 4096 - 4;
    if (length == 0 || maxLength < length) {
      return Err("region file " + path.string() + " has an invalid payload length for chunk index " + std::to_string(range.fIndex));
    }
    if (compression & 0x80) {
      plan.fExternal.set(range.fIndex);
      plan.fHasExternal = true;
    }
  }
  return Status::Ok();
}

std::vector<int> SelectedLocalIndices(RegionPlan const &plan, std::unordered_set<Pos2i, Pos2iHasher> const &toDelete) {
  std::vector<int> indices;
  for (int i = 0; i < 1024; i++) {
    if (plan.fLocations[i] == 0) {
      continue;
    }
    int const cx = plan.fRegionX * 32 + (i % 32);
    int const cz = plan.fRegionZ * 32 + (i / 32);
    if (toDelete.find(Pos2i(cx, cz)) != toDelete.end()) {
      indices.push_back(i);
    }
  }
  return indices;
}

Status ClearInPlace(RegionPlan const &plan, std::vector<int> const &indices) {
  if (indices.empty()) {
    return Status::Ok();
  }
  FILE *fp = std::fopen(plan.fPath.string().c_str(), "r+b");
  if (!fp) {
    int e = errno;
    return Err("cannot open " + plan.fPath.string() + " for writing: " + ErrnoString(e));
  }
  std::vector<char> zeros(4096, 0);
  for (int i : indices) {
    uint32_t loc = plan.fLocations[i];
    uint32_t offset = loc >> 8;
    uint32_t sectors = loc & 0xFF;
    // Clear the location and timestamp entries first. The chunk is already
    // unreachable for the game even if the process dies before the payload is
    // zeroed.
    uint32_t zero = 0;
    if (!SeekFile(fp, static_cast<uint64_t>(i) * 4) || std::fwrite(&zero, 4, 1, fp) != 1) {
      int e = errno;
      std::fclose(fp);
      return Err("cannot clear location entry in " + plan.fPath.string() + ": " + ErrnoString(e));
    }
    if (!SeekFile(fp, 4096 + static_cast<uint64_t>(i) * 4) || std::fwrite(&zero, 4, 1, fp) != 1) {
      int e = errno;
      std::fclose(fp);
      return Err("cannot clear timestamp entry in " + plan.fPath.string() + ": " + ErrnoString(e));
    }
    if (plan.fExternal.test(i)) {
      // The external file holds the payload; only the 5-byte marker lives here.
      if (!SeekFile(fp, static_cast<uint64_t>(offset) * 4096) || std::fwrite(zeros.data(), 5, 1, fp) != 1) {
        int e = errno;
        std::fclose(fp);
        return Err("cannot clear external chunk marker in " + plan.fPath.string() + ": " + ErrnoString(e));
      }
    } else {
      uint64_t remaining = static_cast<uint64_t>(sectors) * 4096;
      if (!SeekFile(fp, static_cast<uint64_t>(offset) * 4096)) {
        int e = errno;
        std::fclose(fp);
        return Err("cannot seek to chunk payload in " + plan.fPath.string() + ": " + ErrnoString(e));
      }
      while (remaining > 0) {
        size_t const amount = static_cast<size_t>(std::min<uint64_t>(remaining, zeros.size()));
        if (std::fwrite(zeros.data(), 1, amount, fp) != amount) {
          int e = errno;
          std::fclose(fp);
          return Err("cannot zero chunk payload in " + plan.fPath.string() + ": " + ErrnoString(e));
        }
        remaining -= amount;
      }
    }
  }
  if (std::fflush(fp) != 0) {
    int e = errno;
    std::fclose(fp);
    return Err("cannot flush " + plan.fPath.string() + ": " + ErrnoString(e));
  }
  std::fclose(fp);
  return Status::Ok();
}

Status RewriteCompact(RegionPlan const &plan, std::vector<int> const &indices) {
  namespace fs = std::filesystem;
  auto editor = mcfile::je::McaEditor::Open(plan.fPath);
  if (!editor) {
    return Err("cannot open region file " + plan.fPath.string() + " for compaction");
  }
  for (int i : indices) {
    int const lx = i % 32;
    int const lz = i / 32;
    if (!editor->extract(lx, lz)) {
      return Err("cannot read chunk data for compaction in " + plan.fPath.string());
    }
  }
  fs::path tmp = plan.fPath.parent_path() / (plan.fPath.filename().string() + ".prune-tmp");
  std::error_code ec;
  fs::remove(tmp, ec);
  ec.clear();
  std::string writeError;
  if (!editor->write(tmp, &writeError)) {
    fs::remove(tmp, ec);
    return Err(writeError.empty() ? ("cannot write " + tmp.string()) : writeError);
  }
  fs::rename(tmp, plan.fPath, ec);
  if (ec) {
    std::error_code removeError;
    fs::remove(tmp, removeError);
    return Err("cannot replace " + plan.fPath.string() + " with compacted data: " + ec.message());
  }
  return Status::Ok();
}

struct FileEntry {
  fs::path fPath;
  int fRegionX = 0;
  int fRegionZ = 0;
};

Status EnumerateRegionFiles(fs::path const &directory, std::vector<FileEntry> &out) {
  namespace fs = std::filesystem;
  std::error_code ec;
  if (!fs::is_directory(directory, ec)) {
    if (ec) {
      return Err("cannot inspect " + directory.string() + ": " + ec.message());
    }
    return Status::Ok();
  }
  fs::directory_iterator itr(directory, ec);
  if (ec) {
    return Err("cannot list " + directory.string() + ": " + ec.message());
  }
  fs::directory_iterator end;
  for (; itr != end; itr.increment(ec)) {
    if (ec) {
      return Err("cannot list " + directory.string() + ": " + ec.message());
    }
    fs::path const &p = itr->path();
    std::error_code typeError;
    if (!itr->is_regular_file(typeError)) {
      continue;
    }
    int rx = 0;
    int rz = 0;
    if (!ParseRegionFileName(p.filename().string(), rx, rz)) {
      continue;
    }
    out.push_back({p, rx, rz});
  }
  return Status::Ok();
}

bool ParseChunkId(std::string const &s, int32_t &out) {
  if (s.empty()) {
    return false;
  }
  int64_t v = 0;
  auto result = std::from_chars(s.data(), s.data() + s.size(), v);
  if (result.ec != std::errc() || result.ptr != s.data() + s.size()) {
    return false;
  }
  if (v < std::numeric_limits<int32_t>::min() || std::numeric_limits<int32_t>::max() < v) {
    return false;
  }
  out = static_cast<int32_t>(v);
  return true;
}

struct JavaMapFile {
  fs::path fPath;
  bool fMapsDirectory = false;
};

Status EnumerateMapFiles(JavaWorldLocation const &location, std::vector<JavaMapFile> &out) {
  namespace fs = std::filesystem;
  std::vector<fs::path> candidates;
  auto add = [&candidates](fs::path const &p) {
    candidates.push_back(p);
  };
  fs::path const &levelRoot = location.fLevelRoot;
  fs::path const &dimDir = location.fDimensionDirectory;
  add(levelRoot / "data" / "minecraft" / "maps");
  add(levelRoot / "data" / "maps");
  add(levelRoot / "data");
  add(dimDir / "data" / "minecraft" / "maps");
  add(dimDir / "data" / "maps");
  add(dimDir / "data");

  std::set<std::string> visited;
  for (fs::path const &dir : candidates) {
    std::error_code ec;
    std::string key = dir.lexically_normal().string();
    if (!visited.insert(key).second) {
      continue;
    }
    if (!fs::is_directory(dir, ec)) {
      continue;
    }
    bool const mapsDirectory = dir.filename() == "maps";
    fs::directory_iterator itr(dir, ec);
    if (ec) {
      return Err("cannot list map directory " + dir.string() + ": " + ec.message());
    }
    fs::directory_iterator end;
    for (; itr != end; itr.increment(ec)) {
      if (ec) {
        return Err("cannot list map directory " + dir.string() + ": " + ec.message());
      }
      std::error_code typeError;
      if (!itr->is_regular_file(typeError)) {
        continue;
      }
      std::string name = itr->path().filename().string();
      if (!name.ends_with(".dat")) {
        continue;
      }
      int32_t id = 0;
      if (mapsDirectory) {
        std::string stem = name.substr(0, name.size() - 4);
        if (!ParseChunkId(stem, id)) {
          continue;
        }
      } else {
        if (!name.starts_with("map_")) {
          continue;
        }
        std::string stem = name.substr(4, name.size() - 4 - 4);
        if (!ParseChunkId(stem, id)) {
          continue;
        }
      }
      out.push_back({itr->path(), mapsDirectory});
    }
  }
  return Status::Ok();
}

Status ReadJavaMapDimension(CompoundTag const &data, std::optional<CanonicalDimension> &out, std::string const &path) {
  Tag const *dimension = data.query(u8"dimension");
  switch (dimension->type()) {
  case Tag::Type::String: {
    std::u8string s = dimension->asString()->fValue;
    if (s == u8"minecraft:overworld" || s == u8"overworld") {
      out = CanonicalDimension::Overworld;
    } else if (s == u8"minecraft:the_nether" || s == u8"the_nether") {
      out = CanonicalDimension::Nether;
    } else if (s == u8"minecraft:the_end" || s == u8"the_end") {
      out = CanonicalDimension::End;
    } else if (s.empty()) {
      return Err("map " + path + " has an empty dimension name");
    } else {
      out = std::nullopt;
    }
    return Status::Ok();
  }
  case Tag::Type::Int: {
    int32_t v = dimension->asInt()->fValue;
    out = CanonicalDimensionFromInt(v);
    return Status::Ok();
  }
  case Tag::Type::Byte: {
    int32_t v = static_cast<int8_t>(dimension->asByte()->fValue);
    out = CanonicalDimensionFromInt(v);
    return Status::Ok();
  }
  case Tag::Type::End:
    // Maps from before the dimension field existed were overworld maps.
    out = CanonicalDimension::Overworld;
    return Status::Ok();
  default:
    return Err("map " + path + " has a dimension field with an unsupported NBT type");
  }
}

Status ScanJavaMaps(JavaWorldLocation const &location, CanonicalDimension target, MapProtection &protection, uint64_t &count) {
  std::vector<JavaMapFile> files;
  Status st = EnumerateMapFiles(location, files);
  if (!st.ok()) {
    return JE2BE_ERROR_PUSH(st);
  }
  for (JavaMapFile const &file : files) {
    auto root = std::make_shared<CompoundTag>();
    auto stream = std::make_shared<mcfile::stream::GzFileInputStream>(file.fPath);
    mcfile::stream::InputStreamReader reader(stream);
    if (!root->read(reader)) {
      return Err("cannot read map data " + file.fPath.string());
    }
    auto dataPtr = root->compoundTag(u8"data");
    if (!dataPtr) {
      return Err("map " + file.fPath.string() + " has no data compound");
    }
    CompoundTag const *data = dataPtr.get();
    std::optional<CanonicalDimension> dimension;
    st = ReadJavaMapDimension(*data, dimension, file.fPath.string());
    if (!st.ok()) {
      return JE2BE_ERROR_PUSH(st);
    }
    auto xCenter = data->int32(u8"xCenter");
    auto zCenter = data->int32(u8"zCenter");
    auto scale = data->byte(u8"scale");
    Tag const *xTag = data->query(u8"xCenter");
    Tag const *zTag = data->query(u8"zCenter");
    Tag const *scaleTag = data->query(u8"scale");
    if (xTag->type() != Tag::Type::End && xTag->type() != Tag::Type::Int) {
      return Err("map " + file.fPath.string() + " has an xCenter field with an unsupported NBT type");
    }
    if (zTag->type() != Tag::Type::End && zTag->type() != Tag::Type::Int) {
      return Err("map " + file.fPath.string() + " has a zCenter field with an unsupported NBT type");
    }
    if (scaleTag->type() != Tag::Type::End && scaleTag->type() != Tag::Type::Byte) {
      return Err("map " + file.fPath.string() + " has a scale field with an unsupported NBT type");
    }
    if (!xCenter || !zCenter) {
      return Err("map " + file.fPath.string() + " is missing xCenter or zCenter");
    }
    if (!scale) {
      scale = 0;
    }
    count++;
    if (!dimension || *dimension != target) {
      continue;
    }
    ChunkRect rect;
    st = MapRectFromCenter(*xCenter, *zCenter, *scale, rect);
    if (!st.ok()) {
      return JE2BE_ERROR_PUSH(st);
    }
    protection.add(rect);
  }
  return Status::Ok();
}

Status RemoveExternalFiles(std::vector<fs::path> const &directories, std::unordered_set<Pos2i, Pos2iHasher> const &deleted, uint64_t &removed) {
  namespace fs = std::filesystem;
  for (fs::path const &dir : directories) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
      continue;
    }
    for (Pos2i const &p : deleted) {
      fs::path path = dir / ("c." + std::to_string(p.fX) + "." + std::to_string(p.fZ) + ".mcc");
      std::error_code existsError;
      if (!fs::is_regular_file(path, existsError)) {
        if (existsError) {
          return Err("cannot inspect external chunk file " + path.string() + ": " + existsError.message());
        }
        continue;
      }
      std::error_code removeError;
      if (!fs::remove(path, removeError)) {
        return Err("cannot remove external chunk file " + path.string() + ": " + removeError.message());
      }
      removed++;
    }
  }
  return Status::Ok();
}

} // namespace

Status LocateJavaWorld(std::filesystem::path const &input, CanonicalDimension dimension, JavaWorldLocation &out) {
  namespace fs = std::filesystem;
  std::error_code ec;
  if (!fs::is_directory(input, ec)) {
    return Err("input directory " + input.string() + " does not exist");
  }
  fs::path levelRoot;
  bool paper = false;
  if (fs::is_regular_file(input / "level.dat", ec) && !ec) {
    levelRoot = input;
    paper = false;
  } else {
    ec.clear();
    if (fs::is_regular_file(input / "world" / "level.dat", ec) && !ec) {
      levelRoot = input / "world";
      paper = true;
    } else {
      return Err(input.string() + " is not a Java world directory (level.dat not found)");
    }
  }
  je2be::java::Options options;
  options.fLevelDirectoryStructure = paper ? LevelDirectoryStructure::Paper : LevelDirectoryStructure::Vanilla;
  fs::path dimDir = DimensionDirectory(options, input, dimension);
  out.fInput = input;
  out.fLevelRoot = levelRoot;
  out.fDimensionDirectory = dimDir;
  out.fPaperStructure = paper;
  return Status::Ok();
}

Status PruneJavaWorld(JavaWorldLocation const &location, Config const &config, Action fallback, JavaPruneResult &result) {
  namespace fs = std::filesystem;

  std::optional<CanonicalDimension> target = CanonicalDimensionFromInt(config.fDimension);
  if (!target) {
    return Err("unsupported dimension in config");
  }
  std::error_code ec;
  if (!fs::is_directory(location.fDimensionDirectory, ec)) {
    if (*target != CanonicalDimension::Overworld) {
      result.fWarnings.push_back("dimension directory " + location.fDimensionDirectory.string() + " does not exist; nothing to prune");
      return Status::Ok();
    }
    return Err("overworld directory " + location.fDimensionDirectory.string() + " does not exist");
  }

  SessionLockGuard lock;
  Status st = lock.lock(location.fLevelRoot);
  if (!st.ok()) {
    return JE2BE_ERROR_PUSH(st);
  }

  MapProtection protection;
  uint64_t mapCount = 0;
  st = ScanJavaMaps(location, *target, protection, mapCount);
  if (!st.ok()) {
    return JE2BE_ERROR_PUSH(st);
  }
  result.fMapFiles = mapCount;

  std::vector<fs::path> directories = {
      location.fDimensionDirectory / "region",
      location.fDimensionDirectory / "entities",
      location.fDimensionDirectory / "poi",
  };
  std::vector<RegionPlan> plans;
  std::unordered_set<Pos2i, Pos2iHasher> existing;
  for (fs::path const &dir : directories) {
    std::vector<FileEntry> files;
    st = EnumerateRegionFiles(dir, files);
    if (!st.ok()) {
      return JE2BE_ERROR_PUSH(st);
    }
    for (FileEntry const &file : files) {
      RegionPlan plan;
      st = ReadRegionPlan(file.fPath, file.fRegionX, file.fRegionZ, plan);
      if (!st.ok()) {
        return JE2BE_ERROR_PUSH(st);
      }
      for (int i = 0; i < 1024; i++) {
        if (plan.fLocations[i] == 0) {
          continue;
        }
        int cx = plan.fRegionX * 32 + (i % 32);
        int cz = plan.fRegionZ * 32 + (i / 32);
        existing.insert(Pos2i(cx, cz));
      }
      plans.push_back(std::move(plan));
    }
  }
  result.fExistingChunks = existing.size();

  Selection selection;
  selection.fConfig = &config;
  selection.fFallback = fallback;
  selection.fMaps = &protection;

  std::unordered_set<Pos2i, Pos2iHasher> deleted;
  for (Pos2i const &p : existing) {
    if (selection.resolve(p.fX, p.fZ) == Action::Delete) {
      deleted.insert(p);
    } else if (!config.lastMatch(p.fX, p.fZ) && protection.protects(p.fX, p.fZ)) {
      result.fKeptByMapProtection++;
    }
  }
  result.fDeletedChunks = deleted.size();
  if (deleted.empty()) {
    return Status::Ok();
  }

  for (RegionPlan const &plan : plans) {
    std::vector<int> indices = SelectedLocalIndices(plan, deleted);
    if (indices.empty()) {
      continue;
    }
    result.fDeletedRegionEntries += indices.size();
    if (plan.fHasExternal) {
      // Never rewrite a region file that contains external chunks: McaEditor
      // cannot decode the 0x80 marker and would drop kept external entries.
      st = ClearInPlace(plan, indices);
      if (!st.ok()) {
        return JE2BE_ERROR_PUSH(st);
      }
      continue;
    }
    st = RewriteCompact(plan, indices);
    if (!st.ok()) {
      // A payload that cannot be decompressed (unknown compression, corruption)
      // must not block deletion of the whole slot. Fall back to clearing only
      // the selected entries in place, which leaves kept chunks untouched.
      st = ClearInPlace(plan, indices);
      if (!st.ok()) {
        return JE2BE_ERROR_PUSH(st);
      }
    }
  }

  uint64_t removedExternal = 0;
  st = RemoveExternalFiles(directories, deleted, removedExternal);
  if (!st.ok()) {
    return JE2BE_ERROR_PUSH(st);
  }
  result.fDeletedExternalFiles = removedExternal;
  return Status::Ok();
}

} // namespace je2be::prune
