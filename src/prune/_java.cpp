#include "prune/_java.hpp"

#include "prune/_selection.hpp"

#include <je2be/java/options.hpp>
#include <je2be/nbt.hpp>

#include <minecraft-file.hpp>

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
#include <vector>

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

enum class PathKind {
  Missing,
  Directory,
  RegularFile,
  Other,
};

// Distinguishes "optional path does not exist" (normal) from permission and
// I/O errors (must abort before any destructive write).
Status ClassifyPath(fs::path const &path, PathKind &out) {
  std::error_code ec;
  fs::file_status const status = fs::status(path, ec);
  if (status.type() == fs::file_type::not_found) {
    out = PathKind::Missing;
    return Status::Ok();
  }
  if (ec) {
    return Err("cannot inspect " + path.string() + ": " + ec.message());
  }
  if (fs::is_directory(status)) {
    out = PathKind::Directory;
  } else if (fs::is_regular_file(status)) {
    out = PathKind::RegularFile;
  } else {
    out = PathKind::Other;
  }
  return Status::Ok();
}

// Takes the vanilla session.lock with a non-blocking write lock. The file is
// opened without truncation and is never rewritten: only the lock matters.
class SessionLockGuard {
public:
  ~SessionLockGuard() {
    unlock();
  }

  Status lock(std::filesystem::path const &levelRoot) {
    fs::path path = levelRoot / "session.lock";
    FILE *fp = std::fopen(path.string().c_str(), "r+b");
    if (!fp && errno == ENOENT) {
      fp = std::fopen(path.string().c_str(), "w+b");
    }
    if (!fp) {
      int e = errno;
      return Err("cannot open " + path.string() + " for locking: " + ErrnoString(e));
    }
#if defined(_WIN32)
    HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(fp)));
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

// A region coordinate maps to chunk coordinates rx*32 .. rx*32+31. Reject
// names whose coordinates cannot be represented as int32 chunk coordinates so
// a crafted name cannot wrap into a different part of the world.
bool ParseRegionFileName(std::string const &name, int32_t &rx, int32_t &rz) {
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
  auto parse = [](std::string const &s, int32_t &out) {
    int64_t v = 0;
    auto result = std::from_chars(s.data(), s.data() + s.size(), v);
    if (result.ec != std::errc() || result.ptr != s.data() + s.size()) {
      return false;
    }
    if (v < std::numeric_limits<int32_t>::min() / 32 || std::numeric_limits<int32_t>::max() / 32 < v) {
      return false;
    }
    out = static_cast<int32_t>(v);
    return true;
  };
  return parse(xs, rx) && parse(zs, rz);
}

struct RegionPlan {
  fs::path fPath;
  int32_t fRegionX = 0;
  int32_t fRegionZ = 0;
  uint64_t fFileSize = 0;
  std::array<uint32_t, 1024> fLocations {};
};

uint32_t GetU32BE(unsigned char const *p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

void PutU32BE(unsigned char *p, uint32_t v) {
  p[0] = static_cast<unsigned char>((v >> 24) & 0xff);
  p[1] = static_cast<unsigned char>((v >> 16) & 0xff);
  p[2] = static_cast<unsigned char>((v >> 8) & 0xff);
  p[3] = static_cast<unsigned char>(v & 0xff);
}

Status ReadRegionPlan(fs::path const &path, int32_t rx, int32_t rz, RegionPlan &plan) {
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
    uint64_t maxLength = static_cast<uint64_t>(range.fSectors) * 4096 - 4;
    if (length == 0 || maxLength < length) {
      return Err("region file " + path.string() + " has an invalid payload length for chunk index " + std::to_string(range.fIndex));
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
    int64_t const cx = static_cast<int64_t>(plan.fRegionX) * 32 + (i % 32);
    int64_t const cz = static_cast<int64_t>(plan.fRegionZ) * 32 + (i / 32);
    if (toDelete.find(Pos2i(static_cast<int32_t>(cx), static_cast<int32_t>(cz))) != toDelete.end()) {
      indices.push_back(i);
    }
  }
  return indices;
}

// Rewrites one region file without decoding any chunk payload:
//   - every kept entry is copied byte-for-byte to a new sector,
//   - selected entries are dropped and their timestamps zeroed,
//   - kept timestamps are preserved.
// This handles zlib, gzip, LZ4, custom compression and external-chunk markers
// uniformly because the compression byte is never interpreted.
Status WritePrunedRegion(RegionPlan const &plan, std::vector<int> const &selected) {
  std::ifstream src(plan.fPath, std::ios::binary);
  if (!src) {
    return Err("cannot open region file " + plan.fPath.string() + " for reading");
  }
  std::array<unsigned char, 8192> header;
  if (!src.read(reinterpret_cast<char *>(header.data()), header.size())) {
    return Err("cannot read region header " + plan.fPath.string());
  }
  std::array<uint32_t, 1024> oldLocations {};
  for (int i = 0; i < 1024; i++) {
    oldLocations[i] = GetU32BE(header.data() + i * 4);
  }
  std::bitset<1024> selectedSet;
  for (int i : selected) {
    selectedSet.set(i);
  }

  std::array<uint32_t, 1024> newLocations {};
  uint64_t nextSector = 2;
  for (int i = 0; i < 1024; i++) {
    uint32_t loc = oldLocations[i];
    if (loc == 0 || selectedSet.test(i)) {
      continue;
    }
    uint32_t sectors = loc & 0xFF;
    if (0xFFFFFF < nextSector + sectors) {
      return Err("region file " + plan.fPath.string() + " cannot be compacted into the 24-bit sector field");
    }
    newLocations[i] = (static_cast<uint32_t>(nextSector) << 8) | sectors;
    nextSector += sectors;
  }

  std::array<unsigned char, 8192> out = header;
  for (int i = 0; i < 1024; i++) {
    if (selectedSet.test(i)) {
      PutU32BE(out.data() + i * 4, 0);
      PutU32BE(out.data() + 4096 + i * 4, 0);
    } else if (oldLocations[i] != 0) {
      PutU32BE(out.data() + i * 4, newLocations[i]);
    }
  }

  fs::path tmp = plan.fPath.parent_path() / (plan.fPath.filename().string() + ".prune-tmp");
  PathKind tmpKind;
  Status tmpStatus = ClassifyPath(tmp, tmpKind);
  if (!tmpStatus.ok()) {
    return JE2BE_ERROR_PUSH(tmpStatus);
  }
  if (tmpKind != PathKind::Missing) {
    if (tmpKind != PathKind::RegularFile) {
      return Err("temporary region path " + tmp.string() + " is not a regular file");
    }
    std::error_code removeError;
    if (!fs::remove(tmp, removeError)) {
      return Err("cannot remove stale temporary file " + tmp.string() + ": " + removeError.message());
    }
  }
  auto fail = [&](std::string const &message) {
    std::error_code removeError;
    fs::remove(tmp, removeError);
    return Err(message);
  };
  std::ofstream dst(tmp, std::ios::binary | std::ios::trunc);
  if (!dst) {
    return Err("cannot create temporary region file " + tmp.string());
  }
  dst.write(reinterpret_cast<char const *>(out.data()), out.size());
  if (!dst) {
    dst.close();
    return fail("cannot write region header " + tmp.string());
  }
  std::vector<char> buffer(1 << 20);
  for (int i = 0; i < 1024; i++) {
    uint32_t loc = oldLocations[i];
    if (loc == 0 || selectedSet.test(i)) {
      continue;
    }
    uint32_t sectors = loc & 0xFF;
    if (!src.seekg(static_cast<std::streamoff>(static_cast<uint64_t>(loc >> 8) * 4096), std::ios::beg)) {
      dst.close();
      return fail("cannot seek to chunk payload in " + plan.fPath.string());
    }
    uint64_t remaining = static_cast<uint64_t>(sectors) * 4096;
    while (remaining > 0) {
      size_t const amount = static_cast<size_t>(std::min<uint64_t>(remaining, buffer.size()));
      src.read(buffer.data(), static_cast<std::streamsize>(amount));
      if (src.gcount() != static_cast<std::streamsize>(amount)) {
        dst.close();
        return fail("cannot read chunk payload in " + plan.fPath.string());
      }
      dst.write(buffer.data(), static_cast<std::streamsize>(amount));
      if (!dst) {
        dst.close();
        return fail("cannot write chunk payload to " + tmp.string());
      }
      remaining -= amount;
    }
  }
  dst.flush();
  if (!dst) {
    dst.close();
    return fail("cannot flush temporary region file " + tmp.string());
  }
  dst.close();
  if (!dst) {
    return fail("cannot close temporary region file " + tmp.string());
  }
  src.close();
  std::error_code renameError;
  fs::rename(tmp, plan.fPath, renameError);
  if (renameError) {
    std::error_code removeError;
    fs::remove(tmp, removeError);
    return Err("cannot replace " + plan.fPath.string() + " with pruned data: " + renameError.message());
  }
  return Status::Ok();
}

struct FileEntry {
  fs::path fPath;
  int32_t fRegionX = 0;
  int32_t fRegionZ = 0;
};

Status EnumerateRegionFiles(fs::path const &directory, std::vector<FileEntry> &out) {
  PathKind kind;
  Status st = ClassifyPath(directory, kind);
  if (!st.ok()) {
    return JE2BE_ERROR_PUSH(st);
  }
  if (kind == PathKind::Missing) {
    return Status::Ok();
  }
  if (kind != PathKind::Directory) {
    return Err("region path " + directory.string() + " is not a directory");
  }
  std::error_code ec;
  fs::directory_iterator itr(directory, ec);
  if (ec) {
    return Err("cannot list region directory " + directory.string() + ": " + ec.message());
  }
  fs::directory_iterator end;
  for (; itr != end; itr.increment(ec)) {
    if (ec) {
      return Err("cannot list region directory " + directory.string() + ": " + ec.message());
    }
    std::error_code typeError;
    bool const isFile = itr->is_regular_file(typeError);
    if (typeError) {
      return Err("cannot inspect region entry " + itr->path().string() + ": " + typeError.message());
    }
    if (!isFile) {
      continue;
    }
    std::string const name = itr->path().filename().string();
    if (!name.ends_with(".mca")) {
      continue;
    }
    int32_t rx = 0;
    int32_t rz = 0;
    if (!ParseRegionFileName(name, rx, rz)) {
      if (name.starts_with("r.")) {
        return Err("region file " + itr->path().string() + " has an unsupported name");
      }
      continue;
    }
    out.push_back({itr->path(), rx, rz});
  }
  if (ec) {
    return Err("cannot list region directory " + directory.string() + ": " + ec.message());
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
};

Status EnumerateMapFiles(JavaWorldLocation const &location, std::vector<JavaMapFile> &out) {
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
    std::string key = dir.lexically_normal().string();
    if (!visited.insert(key).second) {
      continue;
    }
    PathKind kind;
    Status st = ClassifyPath(dir, kind);
    if (!st.ok()) {
      return JE2BE_ERROR_PUSH(st);
    }
    if (kind == PathKind::Missing) {
      continue;
    }
    if (kind != PathKind::Directory) {
      return Err("map path " + dir.string() + " is not a directory");
    }
    bool const mapsDirectory = dir.filename() == "maps";
    std::error_code ec;
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
      bool const isFile = itr->is_regular_file(typeError);
      if (typeError) {
        return Err("cannot inspect map entry " + itr->path().string() + ": " + typeError.message());
      }
      if (!isFile) {
        continue;
      }
      std::string const name = itr->path().filename().string();
      if (!name.ends_with(".dat")) {
        continue;
      }
      int32_t id = 0;
      if (mapsDirectory) {
        if (!ParseChunkId(name.substr(0, name.size() - 4), id)) {
          continue;
        }
      } else {
        if (!name.starts_with("map_")) {
          continue;
        }
        if (!ParseChunkId(name.substr(4, name.size() - 4 - 4), id)) {
          continue;
        }
      }
      out.push_back({itr->path()});
    }
    if (ec) {
      return Err("cannot list map directory " + dir.string() + ": " + ec.message());
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
    out = CanonicalDimensionFromInt(dimension->asInt()->fValue);
    return Status::Ok();
  }
  case Tag::Type::Byte: {
    out = CanonicalDimensionFromInt(static_cast<int8_t>(dimension->asByte()->fValue));
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
    // CompoundTag::read reads the named root wrapper as a single entry, so the
    // map payload lives at /data (the same path je2be's map reader uses).
    Tag const *dataTag = root->query(u8"/data");
    if (dataTag->type() != Tag::Type::Compound) {
      return Err("map " + file.fPath.string() + " has no data compound");
    }
    CompoundTag const *data = dataTag->asCompound();
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
    int const scaleValue = scale ? *scale : 0;
    count++;
    if (!dimension || *dimension != target) {
      continue;
    }
    ChunkRect rect;
    st = MapRectFromCenter(*xCenter, *zCenter, scaleValue, rect);
    if (!st.ok()) {
      return JE2BE_ERROR_PUSH(st);
    }
    protection.add(rect);
  }
  return Status::Ok();
}

Status RemoveExternalFiles(std::vector<fs::path> const &directories, std::unordered_set<Pos2i, Pos2iHasher> const &deleted, uint64_t &removed) {
  for (fs::path const &dir : directories) {
    PathKind kind;
    Status st = ClassifyPath(dir, kind);
    if (!st.ok()) {
      return JE2BE_ERROR_PUSH(st);
    }
    if (kind == PathKind::Missing) {
      continue;
    }
    if (kind != PathKind::Directory) {
      return Err("region path " + dir.string() + " is not a directory");
    }
    for (Pos2i const &p : deleted) {
      fs::path path = dir / ("c." + std::to_string(p.fX) + "." + std::to_string(p.fZ) + ".mcc");
      PathKind sidecarKind;
      Status st = ClassifyPath(path, sidecarKind);
      if (!st.ok()) {
        return JE2BE_ERROR_PUSH(st);
      }
      if (sidecarKind == PathKind::Missing) {
        continue;
      }
      if (sidecarKind != PathKind::RegularFile) {
        return Err("external chunk path " + path.string() + " is not a regular file");
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
  PathKind inputKind;
  Status st = ClassifyPath(input, inputKind);
  if (!st.ok()) {
    return JE2BE_ERROR_PUSH(st);
  }
  if (inputKind == PathKind::Missing) {
    return Err("input directory " + input.string() + " does not exist");
  }
  if (inputKind != PathKind::Directory) {
    return Err("input path " + input.string() + " is not a directory");
  }
  fs::path levelRoot;
  bool paper = false;
  PathKind rootLevelKind;
  st = ClassifyPath(input / "level.dat", rootLevelKind);
  if (!st.ok()) {
    return JE2BE_ERROR_PUSH(st);
  }
  if (rootLevelKind == PathKind::RegularFile) {
    levelRoot = input;
    paper = false;
  } else if (rootLevelKind != PathKind::Missing) {
    return Err((input / "level.dat").string() + " is not a regular file");
  } else {
    PathKind paperLevelKind;
    st = ClassifyPath(input / "world" / "level.dat", paperLevelKind);
    if (!st.ok()) {
      return JE2BE_ERROR_PUSH(st);
    }
    if (paperLevelKind == PathKind::Missing) {
      return Err(input.string() + " is not a Java world directory (level.dat not found)");
    }
    if (paperLevelKind != PathKind::RegularFile) {
      return Err((input / "world" / "level.dat").string() + " is not a regular file");
    }
    levelRoot = input / "world";
    paper = true;
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
  std::optional<CanonicalDimension> target = CanonicalDimensionFromInt(config.fDimension);
  if (!target) {
    return Err("unsupported dimension in config");
  }
  PathKind dimensionKind;
  Status st = ClassifyPath(location.fDimensionDirectory, dimensionKind);
  if (!st.ok()) {
    return JE2BE_ERROR_PUSH(st);
  }
  if (dimensionKind == PathKind::Missing) {
    if (*target != CanonicalDimension::Overworld) {
      result.fWarnings.push_back("dimension directory " + location.fDimensionDirectory.string() + " does not exist; nothing to prune");
      return Status::Ok();
    }
    return Err("overworld directory " + location.fDimensionDirectory.string() + " does not exist");
  }
  if (dimensionKind != PathKind::Directory) {
    return Err("dimension path " + location.fDimensionDirectory.string() + " is not a directory");
  }

  SessionLockGuard lock;
  st = lock.lock(location.fLevelRoot);
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
        int64_t const cx = static_cast<int64_t>(plan.fRegionX) * 32 + (i % 32);
        int64_t const cz = static_cast<int64_t>(plan.fRegionZ) * 32 + (i / 32);
        existing.insert(Pos2i(static_cast<int32_t>(cx), static_cast<int32_t>(cz)));
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
    st = WritePrunedRegion(plan, indices);
    if (!st.ok()) {
      return JE2BE_ERROR_PUSH(st);
    }
    result.fDeletedRegionEntries += indices.size();
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
