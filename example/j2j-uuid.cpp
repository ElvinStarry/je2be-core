#include <cxxopts.hpp>
#include <je2be.hpp>

#include <iostream>

namespace fs = std::filesystem;

int main(int argc, char *argv[]) {
  cxxopts::Options parser("j2j-uuid", "Replace player UUIDs in a Java world save");
  parser.add_options()
      ("i,input", "Java world directory", cxxopts::value<std::string>())
      ("m,mapping", "Headerless CSV containing beforeUUID,afterUUID rows", cxxopts::value<std::string>())
      ("dry-run", "Scan and report changes without writing anything", cxxopts::value<bool>()->default_value("false"))
      ("h,help", "Show help");

  cxxopts::ParseResult options;
  try {
    options = parser.parse(argc, argv);
  } catch (cxxopts::exceptions::exception const &error) {
    std::cerr << error.what() << '\n' << parser.help() << '\n';
    return 2;
  }
  if (options.count("help") || !options.count("input") || !options.count("mapping")) {
    std::cout << parser.help() << '\n';
    return options.count("help") ? 0 : 2;
  }

  je2be::java::UuidReplacer::Options replacerOptions;
  replacerOptions.fDryRun = options["dry-run"].as<bool>();
  je2be::java::UuidReplacer::Result result;
  auto status = je2be::java::UuidReplacer::Run(fs::path(options["input"].as<std::string>()),
                                               fs::path(options["mapping"].as<std::string>()),
                                               replacerOptions,
                                               result);
  if (!status.ok()) {
    auto error = status.error();
    std::cerr << "error: " << (error ? error->fWhat : "UUID replacement failed") << '\n';
    return 1;
  }

  std::cout << (replacerOptions.fDryRun ? "dry-run: " : "")
            << result.fReplacements << " UUID occurrence(s), "
            << result.fChangedFiles << " file(s), "
            << result.fChangedChunks << " chunk(s), "
            << result.fRenamedFiles << " player file(s) to rename";
  if (!replacerOptions.fDryRun) {
    std::cout << " (renamed)";
  }
  std::cout << '\n';
  return 0;
}
