#pragma once

#include <je2be/world-data-override.hpp>

#include <string>
#include <utility>
#include <vector>

namespace je2be::cli {

inline std::u8string ToU8(std::string const &value) {
  return std::u8string(reinterpret_cast<char8_t const *>(value.data()), value.size());
}

inline bool ParseWorldDataOverride(std::string const &argument, WorldDataOverride &result, std::string &error) {
  auto equals = argument.find('=');
  if (equals == std::string::npos || equals == 0) {
    error = "override must use key=value: " + argument;
    return false;
  }
  result.fPath = ToU8(argument.substr(0, equals));
  result.fValue = ToU8(argument.substr(equals + 1));
  return true;
}

inline bool ExtractWorldDataOverrides(int argc,
                                      char *argv[],
                                      std::vector<std::string> &arguments,
                                      std::vector<WorldDataOverride> &overrides,
                                      std::string &error) {
  arguments.clear();
  overrides.clear();
  if (argc > 0) {
    arguments.emplace_back(argv[0]);
  }

  for (int i = 1; i < argc; i++) {
    std::string argument(argv[i]);
    std::string attached;
    bool isOverride = false;
    if (argument == "-O" || argument == "--override") {
      isOverride = true;
    } else if (argument.starts_with("--override=")) {
      isOverride = true;
      attached = argument.substr(std::string("--override=").size());
    } else if (argument.starts_with("-O") && argument.size() > 2) {
      isOverride = true;
      attached = argument.substr(2);
    }

    if (!isOverride) {
      arguments.push_back(argument);
      continue;
    }

    size_t count = 0;
    if (!attached.empty()) {
      WorldDataOverride entry;
      if (!ParseWorldDataOverride(attached, entry, error)) {
        return false;
      }
      overrides.push_back(std::move(entry));
      count++;
    }

    while (i + 1 < argc) {
      std::string next(argv[i + 1]);
      auto equals = next.find('=');
      if (next.starts_with('-') || equals == std::string::npos || equals == 0) {
        break;
      }
      WorldDataOverride entry;
      if (!ParseWorldDataOverride(next, entry, error)) {
        return false;
      }
      overrides.push_back(std::move(entry));
      count++;
      i++;
    }

    if (count == 0) {
      error = argument + " requires at least one key=value argument";
      return false;
    }
  }
  return true;
}

inline std::vector<char *> MutableArgv(std::vector<std::string> &arguments) {
  std::vector<char *> ret;
  ret.reserve(arguments.size());
  for (auto &argument : arguments) {
    ret.push_back(argument.data());
  }
  return ret;
}

} // namespace je2be::cli
