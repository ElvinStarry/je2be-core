#include "prune/_bedrock.hpp"
#include "prune/_config.hpp"
#include "prune/_java.hpp"

#include <cxxopts.hpp>

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

using namespace je2be::prune;

namespace {

void PrintStatus(je2be::Status const &status) {
  if (auto error = status.error()) {
    if (!error->fWhat.empty()) {
      std::cerr << "error: " << error->fWhat << std::endl;
    } else {
      std::cerr << "error: operation failed" << std::endl;
    }
    for (auto const &where : error->fTrace) {
      std::cerr << "  at " << where.fFile << ":" << where.fLine << std::endl;
    }
  }
}

bool ParseDefault(std::string const &s, Action &out) {
  if (s == "keep") {
    out = Action::Keep;
    return true;
  }
  if (s == "delete") {
    out = Action::Delete;
    return true;
  }
  return false;
}

} // namespace

int main(int argc, char *argv[]) {
  cxxopts::Options parser("prune-chunks", "Delete chunks from a Java or Bedrock world in place, using an mbr-chunk-selection file.");
  parser.add_options()                                                                                  //
      ("i,input", "world directory to modify in place (Java world or Bedrock world)", cxxopts::value<std::string>()) //
      ("c,config", "mbr-chunk-selection JSON file with explicit chunk decisions", cxxopts::value<std::string>())     //
      ("d,default", "action outside the configured regions: keep or delete", cxxopts::value<std::string>()->default_value("keep")) //
      ("h,help", "print usage");

  cxxopts::ParseResult result;
  try {
    result = parser.parse(argc, argv);
  } catch (cxxopts::exceptions::exception &e) {
    std::cerr << e.what() << std::endl;
    std::cerr << parser.help() << std::endl;
    return 1;
  }
  if (result.count("help")) {
    std::cout << parser.help() << std::endl;
    return 0;
  }
  if (!result.count("input")) {
    std::cerr << "error: --input is required" << std::endl;
    std::cerr << parser.help() << std::endl;
    return 1;
  }
  if (!result.count("config")) {
    std::cerr << "error: --config is required" << std::endl;
    std::cerr << parser.help() << std::endl;
    return 1;
  }

  fs::path input(result["input"].as<std::string>());
  fs::path configPath(result["config"].as<std::string>());
  std::string const defaultString = result["default"].as<std::string>();
  Action fallback = Action::Keep;
  if (!ParseDefault(defaultString, fallback)) {
    std::cerr << "error: unknown --default value \"" << defaultString << "\"; expected \"keep\" or \"delete\"" << std::endl;
    return 1;
  }

  std::error_code ec;
  if (!fs::is_directory(input, ec)) {
    std::cerr << "error: input directory " << input.string() << " does not exist" << std::endl;
    return 1;
  }

  Config config;
  je2be::Status status = Config::ParseFile(configPath, config);
  if (!status.ok()) {
    PrintStatus(status);
    return 1;
  }
  std::optional<CanonicalDimension> dimension = CanonicalDimensionFromInt(config.fDimension);
  if (!dimension) {
    std::cerr << "error: unsupported dimension" << std::endl;
    return 1;
  }

  bool const bedrock = fs::is_directory(input / "db", ec);
  std::cout << "prune-chunks: input=" << input.string() << " config=" << configPath.string()
            << " edition=" << (bedrock ? "bedrock" : "java")
            << " dimension=" << CanonicalDimensionName(*dimension)
            << " default=" << Config::ActionName(fallback) << std::endl;

  if (bedrock) {
    BedrockPruneResult pruneResult;
    status = PruneBedrockWorld(input, config, fallback, pruneResult);
    if (!status.ok()) {
      PrintStatus(status);
      return 1;
    }
    for (std::string const &warning : pruneResult.fWarnings) {
      std::cerr << "warning: " << warning << std::endl;
    }
    std::cout << "bedrock: keys=" << pruneResult.fTotalKeys
              << " maps=" << pruneResult.fMapRecords
              << " protected=" << pruneResult.fProtectedChunks
              << " existingChunks=" << pruneResult.fExistingChunks
              << " deletedChunks=" << pruneResult.fDeletedChunks
              << " deletedChunkKeys=" << pruneResult.fDeletedChunkKeys
              << " deletedDigp=" << pruneResult.fDeletedDigpKeys
              << " deletedActorprefixes=" << pruneResult.fDeletedActorPrefixes
              << " unknownChunkShapedKeys=" << pruneResult.fUnknownChunkShapedKeys << std::endl;
  } else {
    JavaWorldLocation location;
    status = LocateJavaWorld(input, *dimension, location);
    if (!status.ok()) {
      PrintStatus(status);
      return 1;
    }
    JavaPruneResult pruneResult;
    status = PruneJavaWorld(location, config, fallback, pruneResult);
    if (!status.ok()) {
      PrintStatus(status);
      return 1;
    }
    for (std::string const &warning : pruneResult.fWarnings) {
      std::cerr << "warning: " << warning << std::endl;
    }
    std::cout << "java: maps=" << pruneResult.fMapFiles
              << " existingChunks=" << pruneResult.fExistingChunks
              << " deletedChunks=" << pruneResult.fDeletedChunks
              << " deletedRegionEntries=" << pruneResult.fDeletedRegionEntries
              << " deletedExternalFiles=" << pruneResult.fDeletedExternalFiles
              << " protectedByMap=" << pruneResult.fKeptByMapProtection << std::endl;
  }
  return 0;
}
