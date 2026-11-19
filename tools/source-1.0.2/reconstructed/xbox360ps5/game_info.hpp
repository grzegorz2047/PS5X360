// RECONSTRUCTED for the 1.0.2 source rebuild (tools/source-1.0.2): the original file reads the
// game information archive (x360db), not published. This version knows no game, so the game
// page's information tab says the game is not in the archive.
#pragma once
#include <map>
#include <string>

namespace xbox360ps5 {
inline std::map<std::string, std::string> GameInfoFor(const std::string&) { return {}; }
}  // namespace xbox360ps5

namespace xbox360ps5 {
// The Portuguese text (translated by Tr) of a Xenia Canary compatibility state, as the
// official package's assets/lang files list them.
inline const char* CompatStateText(const std::string& state) {
  if (state == "Playable") return "Jogável do começo ao fim";
  if (state == "Gameplay") return "Dá para jogar, com problemas";
  if (state == "Loads" || state == "Intro" || state == "Menus") return "Só carrega, não chega a jogar";
  if (state == "Unplayable" || state == "Nothing") return "Não funciona";
  return "Sem avaliação";
}
}  // namespace xbox360ps5
