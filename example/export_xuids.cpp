#include <filesystem>
#include <fstream>
#include <iostream>
#include <leveldb/db.h>
#include <minecraft-file.hpp>
#include <string>
#include <vector>

#include "je2be/bedrock/converter.hpp"
#include "je2be/bedrock/options.hpp"

// Utility to print found player data
void PrintPlayer(std::string const &xuid, je2be::Uuid const &uuid, std::string const &name) {
  std::u8string u8uuid = uuid.toString();
  std::string uuidStr(u8uuid.begin(), u8uuid.end());
  std::cout << "FOUND_PLAYER|" << xuid << "|" << uuidStr << "|" << name << std::endl;
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <path_to_bedrock_db_folder>" << std::endl;
    return 1;
  }

  std::filesystem::path dbPath = argv[1];

  leveldb::DB *db = nullptr;
  leveldb::Options options;
  options.create_if_missing = false;

  leveldb::Status status = leveldb::DB::Open(options, dbPath, &db);

  if (!status.ok()) {
    std::cerr << "Error: Failed to open LevelDB at " << dbPath << ": " << status.ToString() << std::endl;
    return 1;
  }

  std::cout << "Scanning LevelDB for player data..." << std::endl;

  // Iterate over all keys
  leveldb::Iterator *it = db->NewIterator(leveldb::ReadOptions());

  int foundCount = 0;

  // Generic SCAN to find any readable keys
  std::cout << "Debug Mode: Scanning for ALL readable keys..." << std::endl;

  int printedCount = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    std::string keyStr = it->key().ToString();

    if (keyStr.rfind("player_", 0) == 0) {
      // It is a player key
      std::string xuid = keyStr.substr(7); // remove "player_"
      std::cout << "FOUND_KEY: " << keyStr << " -> XUID: " << xuid << std::endl;
      foundCount++;
    } else if (keyStr.rfind("player_server_", 0) == 0) {
      // Server-auth player key
      std::string xuid = keyStr.substr(14); // remove "player_server_"
      std::cout << "FOUND_KEY: " << keyStr << " -> XUID: " << xuid << std::endl;
      foundCount++;
    }
  }
  std::cout << "Total readable keys found: " << printedCount << std::endl;

  delete it;
  delete db;

  std::cout << "Scan complete. Found " << foundCount << " player keys." << std::endl;
  return 0;
}
