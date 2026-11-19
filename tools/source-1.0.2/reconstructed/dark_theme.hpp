// RECONSTRUCTED for the 1.0.2 source rebuild (tools/source-1.0.2): 1.0.2's dark home theme. Its
// source is not published; this follows the official executable (b1d1b3cb):
//  * the theme is read once, at start: /data/homebrew/PPSA50011/ui-dark.on exists (any content);
//  * with it on, the home's pictures come from /data/homebrew/PPSA50011/skin, then
//    /app0/assets/skin-dark, then /app0/assets/skin (each picture from the first folder that has
//    it; the official inserts skin-dark at position 1 of kinect_home.inc's folder list);
//  * the toggle removes the marker, or writes "1\n" to it, and the emulator restarts. The home's
//    "Tema" slot (after System Link) says "Tema claro/escuro ligado..." or "Não foi possível
//    trocar o tema."; the Settings sheet's "Tema" row (before "Reiniciar o emulador") saves the
//    settings and restarts without a message.
// The texts are the official ones, translated by the package's assets/lang files.
#pragma once
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace xbox360ps5::dark_theme {
inline constexpr const char* kMarker = "/data/homebrew/PPSA50011/ui-dark.on";
inline constexpr const char* kSkin = "/app0/assets/skin-dark";

inline bool Present() {
  std::error_code error;
  return std::filesystem::exists(kMarker, error);
}
// The theme this run of the emulator draws with (a change takes effect after the restart).
inline bool AtStart() {
  static const bool on = Present();
  return on;
}
// kinect_home.inc's picture folders, with the dark pictures between the user's and the package's.
inline std::vector<std::filesystem::path> SkinFolders(const char* user, const char* own) {
  std::vector<std::filesystem::path> folders{user, own};
  if (AtStart()) folders.insert(folders.begin() + 1, kSkin);
  return folders;
}
// Turns the theme the other way. False when the marker could not be changed.
inline bool Toggle() {
  if (AtStart()) {
    std::error_code error;
    return std::filesystem::remove(kMarker, error) && !error;
  }
  std::ofstream marker(kMarker, std::ios::trunc);
  marker.write("1\n", 2);
  return bool(marker);
}
// The home slot's message after Toggle (Portuguese source text, for Tr).
inline const char* ToggleMessage(bool done) {
  if (!done) return "Não foi possível trocar o tema.";
  return AtStart() ? "Tema claro ligado. O emulador vai reiniciar."
                   : "Tema escuro ligado. O emulador vai reiniciar.";
}
}  // namespace xbox360ps5::dark_theme
