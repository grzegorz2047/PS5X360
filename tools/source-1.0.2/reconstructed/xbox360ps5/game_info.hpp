// RECONSTRUCTED for the 1.0.2 source rebuild (tools/source-1.0.2): 1.0.x's reader of the game
// information archive is not published. The archive itself ships in the official package as
// /app0/assets/gameinfo.txt ("[TITLEID]" sections of key=value lines: title, genre, developer,
// publisher, released, rating, compat, compat_issue, about); this reads that file.
#pragma once
#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>

namespace xbox360ps5 {
namespace game_info {
inline constexpr const char* kPath = "/app0/assets/gameinfo.txt";
struct Index {
  bool loaded = false;
  std::unordered_map<std::string, std::streamoff> sections;  // title id -> first line's offset
  std::string last_id;
  std::map<std::string, std::string> last;
};
inline Index& Shared() {
  static Index index;
  return index;
}
inline std::string Upper(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::toupper(c)); });
  return text;
}
}  // namespace game_info

// The archive's fields for a title id ("4D5308D6"), or an empty map. The game page asks every
// frame, so the file is indexed once and the last answer is kept.
inline std::map<std::string, std::string> GameInfoFor(const std::string& title_id) {
  static std::mutex lock;
  std::lock_guard<std::mutex> guard(lock);
  auto& index = game_info::Shared();
  const std::string id = game_info::Upper(title_id);
  if (index.loaded && id == index.last_id) return index.last;
  std::ifstream input(game_info::kPath, std::ios::binary);
  if (!input) return {};
  if (!index.loaded) {
    index.loaded = true;
    std::string line;
    while (std::getline(input, line)) {
      if (line.size() > 2 && line.front() == '[') {
        const size_t end = line.find(']');
        if (end != std::string::npos) index.sections[game_info::Upper(line.substr(1, end - 1))] = input.tellg();
      }
    }
    input.clear();
  }
  std::map<std::string, std::string> fields;
  if (const auto found = index.sections.find(id); found != index.sections.end()) {
    input.seekg(found->second);
    for (std::string line; std::getline(input, line);) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.empty()) continue;
      if (line.front() == '[') break;
      if (line.front() == '#') continue;
      const size_t split = line.find('=');
      if (split != std::string::npos) fields[line.substr(0, split)] = line.substr(split + 1);
    }
  }
  index.last_id = id;
  index.last = fields;
  return fields;
}

// The Portuguese text (translated by Tr) of a Xenia Canary compatibility state; the official
// package's assets/lang files translate exactly these.
inline const char* CompatStateText(const std::string& state) {
  if (state == "Playable") return "Jogável do começo ao fim";
  if (state == "Gameplay") return "Dá para jogar, com problemas";
  if (state == "Loads" || state == "Intro" || state == "Menus") return "Só carrega, não chega a jogar";
  if (state == "Unplayable" || state == "Nothing") return "Não funciona";
  return "Sem avaliação";
}
}  // namespace xbox360ps5
