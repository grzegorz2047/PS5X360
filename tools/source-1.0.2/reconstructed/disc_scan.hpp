// RECONSTRUCTED for the 1.0.2 source rebuild (tools/source-1.0.2): the original file is
// build/disc-20261015/include/disc_scan.hpp (games on a disc in the console's drive), not
// published. This version finds no disc: the library lists no drive games.
#pragma once
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace xbox360ps5::disc_scan {
struct Game {
  std::string name;
  std::string descriptor;
  uint64_t bytes = 0;
  std::string title_id;
};
struct Result {
  std::vector<Game> games;
  std::vector<std::string> notes;  // "original": a pressed Xbox 360 disc the drive cannot read
};
inline Result Scan(const std::filesystem::path&,
                   const std::function<std::string(const std::filesystem::path&)>&) {
  return {};
}
inline std::string SizeText(uint64_t bytes) {
  const double gib = double(bytes) / (1024.0 * 1024.0 * 1024.0);
  char text[32];
  std::snprintf(text, sizeof(text), "%.1f GB", gib);
  return text;
}
}  // namespace xbox360ps5::disc_scan
