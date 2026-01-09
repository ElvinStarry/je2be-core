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

    // Filter: Only print if key looks like text
    bool isText = true;
    if (keyStr.empty())
      continue;

    // Chunk keys usually start with numeric bytes or are fixed length.
    // Readable keys in Bedrock: "player_...", "~local_player", "autonomous_entity", "scoreboard", etc.
    // Let's print anything that starts with a printable char and contains mostly printable chars.

    if (!isprint((unsigned char)keyStr[0])) {
      // Binary key (chunk data, etc.) -> Skip
      continue;
    }

    // Double check the rest of the key
    for (char c : keyStr) {
      if (!isprint((unsigned char)c)) {
        isText = false;
        break;
      }
    }

    if (isText) {
      std::cout << "KEY: " << keyStr << std::endl;
      printedCount++;

      // Check if it looks like a player key
      if (keyStr.find("player") != std::string::npos) {
        std::cout << "!!! POTENTIAL PLAYER KEY: " << keyStr << std::endl;
      }
    }
  }
  std::cout << "Total readable keys found: " << printedCount << std::endl;

  delete it;
  delete db;

  std::cout << "Scan complete. Found " << foundCount << " player keys." << std::endl;
  return 0;
}
