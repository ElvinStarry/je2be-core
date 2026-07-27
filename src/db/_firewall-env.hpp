#pragma once

#if __has_include(<leveldb/env.h>)
#include <leveldb/env.h>

#include <algorithm>
#include <cwctype>

namespace je2be {

class FirewallEnv : public leveldb::Env {
  using Str = std::filesystem::path::string_type;

public:
  explicit FirewallEnv(std::filesystem::path const &allowedDirectory) : fE(nullptr) {
    namespace fs = std::filesystem;
    auto key = FileKey(allowedDirectory);
    if (!key) {
      return;
    }
    if (!key->ends_with(fs::path::preferred_separator)) {
      key->push_back(fs::path::preferred_separator);
    }
    fAllowed = *key;
    fE = leveldb::Env::Default();
  }

  leveldb::Status NewSequentialFile(std::filesystem::path const &fname, leveldb::SequentialFile **result) override {
    if (!fE) {
      return IOError();
    }
    return fE->NewSequentialFile(fname, result);
  }

  leveldb::Status NewRandomAccessFile(std::filesystem::path const &fname, leveldb::RandomAccessFile **result) override {
    if (!fE) {
      return IOError();
    }
    return fE->NewRandomAccessFile(fname, result);
  }

  leveldb::Status NewWritableFile(std::filesystem::path const &fname, leveldb::WritableFile **result) override {
    if (!fE) {
      return IOError();
    }
    if (isAllowed(fname)) {
      return fE->NewWritableFile(fname, result);
    } else {
      return IOError();
    }
  }

  leveldb::Status NewAppendableFile(std::filesystem::path const &fname, leveldb::WritableFile **result) override {
    if (!fE) {
      return IOError("NewAppendableFile: invalid environment");
    }
    if (isAllowed(fname)) {
      return fE->NewAppendableFile(fname, result);
    } else {
      return IOError("NewAppendableFile: path is outside allowed directory");
    }
  }

  bool FileExists(std::filesystem::path const &fname) override {
    if (!fE) {
      return false;
    }
    return fE->FileExists(fname);
  }

  leveldb::Status GetChildren(std::filesystem::path const &dir, std::vector<std::filesystem::path> *result) override {
    if (!fE) {
      return IOError();
    }
    return fE->GetChildren(dir, result);
  }

  leveldb::Status RemoveFile(std::filesystem::path const &fname) override {
    if (!fE) {
      return IOError();
    }
    if (isAllowed(fname)) {
      return fE->RemoveFile(fname);
    } else {
      return IOError();
    }
  }

  leveldb::Status CreateDir(std::filesystem::path const &dirname) override {
    if (!fE) {
      return IOError();
    }
    if (isAllowed(dirname)) {
      return fE->CreateDir(dirname);
    } else {
      return IOError();
    }
  }

  leveldb::Status RemoveDir(std::filesystem::path const &dirname) override {
    if (!fE) {
      return IOError();
    }
    if (isAllowed(dirname)) {
      return fE->RemoveDir(dirname);
    } else {
      return IOError();
    }
  }

  leveldb::Status GetFileSize(std::filesystem::path const &fname, uint64_t *file_size) override {
    if (!fE) {
      return IOError();
    }
    return fE->GetFileSize(fname, file_size);
  }

  leveldb::Status RenameFile(std::filesystem::path const &src, std::filesystem::path const &target) override {
    if (!fE) {
      return IOError();
    }
    if (isAllowed(src) && isAllowed(target)) {
      return fE->RenameFile(src, target);
    } else {
      return IOError();
    }
  }

  leveldb::Status LockFile(std::filesystem::path const &fname, leveldb::FileLock **lock) override {
    if (!fE) {
      return IOError();
    }
    if (isAllowed(fname)) {
      return fE->LockFile(fname, lock);
    } else {
      return IOError();
    }
  }

  leveldb::Status UnlockFile(leveldb::FileLock *lock) override {
    if (!fE) {
      return IOError();
    }
    return fE->UnlockFile(lock);
  }

  void Schedule(void (*function)(void *arg), void *arg) override {
    if (!fE) {
      return;
    }
    fE->Schedule(function, arg);
  }

  void StartThread(void (*function)(void *arg), void *arg) override {
    if (!fE) {
      return;
    }
    fE->StartThread(function, arg);
  }

  leveldb::Status GetTestDirectory(std::filesystem::path *path) override {
    return IOError();
  }

  leveldb::Status NewLogger(std::filesystem::path const &fname, leveldb::Logger **result) override {
    if (!fE) {
      return IOError();
    }
    if (isAllowed(fname)) {
      return fE->NewLogger(fname, result);
    } else {
      return IOError();
    }
  }

  uint64_t NowMicros() override {
    if (!fE) {
      return 0;
    }
    return fE->NowMicros();
  }

  void SleepForMicroseconds(int micros) override {
    if (!fE) {
      return;
    }
    fE->SleepForMicroseconds(micros);
  }

  bool Valid() const {
    return (bool)fE;
  }

private:
  static leveldb::Status IOError(std::string const &message = "FirewallEnv") {
    return leveldb::Status::IOError("FirewallEnv", message);
  }

  bool isAllowed(std::filesystem::path const &p) {
    namespace fs = std::filesystem;
    auto key = FileKey(p);
    if (!key) {
      return false;
    }
    return key->starts_with(fAllowed);
  }

  static std::optional<Str> FileKey(std::filesystem::path const &p) {
    namespace fs = std::filesystem;
    auto path = p;
    path.make_preferred();

    std::error_code ec;
    auto canonical = fs::weakly_canonical(path, ec);
    if (ec) {
      ec.clear();
      canonical = fs::absolute(path, ec).lexically_normal();
      if (ec) {
        return std::nullopt;
      }
    }
    Str key = canonical.native();
#if defined(_WIN32)
    Str const uncPrefix = LR"(\\?\UNC\)";
    Str const pathPrefix = LR"(\\?\)";
    if (key.starts_with(uncPrefix)) {
      key = LR"(\\)" + key.substr(uncPrefix.size());
    } else if (key.starts_with(pathPrefix)) {
      key = key.substr(pathPrefix.size());
    }
    std::transform(key.begin(), key.end(), key.begin(), [](wchar_t c) {
      return std::towlower(c);
    });
#endif
    return key;
  }

private:
  leveldb::Env *fE;
  Str fAllowed;
};

} // namespace je2be

#endif
