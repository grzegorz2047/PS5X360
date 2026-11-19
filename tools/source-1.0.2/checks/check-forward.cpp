// SPDX-License-Identifier: MIT
// The forwarder's launch arguments (fix 21, include/xbox360ps5/forward.hpp of
// the assembled tree) on a PC. After assemble.py:
//   clang++ -std=c++20 -I build/src102/include tools/source-1.0.2/checks/check-forward.cpp -o check-forward
//   ./check-forward
// Adapted from tools/check-forward.cpp of upstream BrinooTk/PS5X360#62.
#include "xbox360ps5/forward.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace xbox360ps5::forward;

static int failures = 0;
#define CHECK(condition)                                                   \
  do {                                                                     \
    if (!(condition)) {                                                    \
      std::fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #condition); \
      ++failures;                                                          \
    }                                                                      \
  } while (0)

template <size_t N>
static Args ParseOf(const char* (&argv)[N]) {
  return Parse(static_cast<int>(N), const_cast<char**>(argv));
}

static void Touch(const fs::path& path) {
  fs::create_directories(path.parent_path());
  std::ofstream(path) << "x";
}

// A package header: magic, and the content type at 0x344, big-endian.
static void Package(const fs::path& path, const char* magic, uint32_t type) {
  fs::create_directories(path.parent_path());
  std::string header(0x1000, '\0');
  header.replace(0, 4, magic, 4);
  for (int i = 0; i < 4; ++i) header[0x344 + i] = char(type >> (24 - 8 * i));
  std::ofstream(path, std::ios::binary) << header;
}

int main() {
  // Arguments.
  {
    const char* argv[] = {"/app0/eboot.bin", "--rom", "Halo 3.iso", "--exit-after-game"};
    const Args a = ParseOf(argv);
    CHECK(a.rom == "Halo 3.iso" && a.exit_after_game);
  }
  {
    const char* argv[] = {"/app0/eboot.bin", "--rom=/mnt/usb0/xbox360/Halo 3/default.xex"};
    const Args a = ParseOf(argv);
    CHECK(a.rom == "/mnt/usb0/xbox360/Halo 3/default.xex" && !a.exit_after_game);
  }
  {
    // Forwarder Manager's PS5X360 tiles: the full path as the only argument.
    const char* argv[] = {"/app0/eboot.bin", "/data/xbox360/Fable 2/default.xex"};
    CHECK(ParseOf(argv).rom == "/data/xbox360/Fable 2/default.xex");
    // A launcher that leaves out argv[0].
    const char* no_zero[] = {"/data/xbox360/Fable 2/default.xex"};
    CHECK(ParseOf(no_zero).rom == "/data/xbox360/Fable 2/default.xex");
    const char* own[] = {"/app0/eboot.bin"};
    CHECK(ParseOf(own).rom.empty());
    const char* own_elsewhere[] = {"/mnt/sandbox/PPSA50011_000/app0/eboot.bin"};
    CHECK(ParseOf(own_elsewhere).rom.empty());
    const char* relative_zero[] = {"eboot.bin"};
    CHECK(ParseOf(relative_zero).rom.empty());
    // --rom wins over a bare path; unknown options are skipped.
    const char* both[] = {"/app0/eboot.bin", "--verbose", "/a.iso", "--rom", "4D5307E6"};
    CHECK(ParseOf(both).rom == "4D5307E6");
    const char* dangling[] = {"/app0/eboot.bin", "--rom"};
    CHECK(ParseOf(dangling).rom.empty());
    const char* only_exit[] = {"/app0/eboot.bin", "--exit-after-game"};
    CHECK(ParseOf(only_exit).rom.empty() && ParseOf(only_exit).exit_after_game);
    const char* padded[] = {"/app0/eboot.bin", "--rom", "  Halo 3.iso\r\n"};
    CHECK(ParseOf(padded).rom == "Halo 3.iso");
  }
  CHECK(Parse(0, nullptr).rom.empty());

  CHECK(IsTitleId("4D5307E6") && IsTitleId("4d5307e6"));
  CHECK(!IsTitleId("4D5307E") && !IsTitleId("4D5307EG") && !IsTitleId("Halo 3.iso"));

  const std::vector<std::string> folders = {"/app0/assets/roms", "/data/xbox360"};
  CHECK((Candidates("Halo 3.iso", folders) ==
         std::vector<fs::path>{"/app0/assets/roms/Halo 3.iso", "/data/xbox360/Halo 3.iso"}));
  CHECK((Candidates("RPG/Fable 2/\r\n", folders) ==
         std::vector<fs::path>{"/app0/assets/roms/RPG/Fable 2", "/data/xbox360/RPG/Fable 2"}));
  CHECK((Candidates("/mnt/usb0/Halo.iso", folders) == std::vector<fs::path>{"/mnt/usb0/Halo.iso"}));
  CHECK(Candidates("../Halo.iso", folders).empty());
  CHECK(Candidates("RPG/../../x.iso", folders).empty());
  CHECK(Candidates("RPG/..x.iso", folders).size() == 2);
  CHECK(Candidates("", folders).empty());
  CHECK((Candidates("a.iso", {"/g", "/g", ""}) == std::vector<fs::path>{"/g/a.iso"}));

  CHECK(OnceForwarded(OnceFlags(false)) && !OnceExitAfterGame(OnceFlags(false)));
  CHECK(OnceForwarded(OnceFlags(true)) && OnceExitAfterGame(OnceFlags(true)));
  CHECK(!OnceForwarded("") && !OnceExitAfterGame("") && !OnceExitAfterGame("exit-after-game"));

  // On disk: the second folder holds the games.
  const fs::path root = fs::temp_directory_path() / "ps5x360-check-forward";
  fs::remove_all(root);
  const fs::path games = root / "games";
  const std::vector<std::string> disk = {(root / "missing").generic_string(), games.generic_string()};
  Touch(games / "Halo 3.iso");
  Touch(games / "Fable 2/DEFAULT.XEX");
  Touch(games / "Fable 2/media.xex.bak");
  Touch(games / "Single/game.xex");
  Touch(games / "Two/a.xex");
  Touch(games / "Two/b.xex");
  fs::create_directories(games / "Empty");
  Touch(games / "Mass Effect 2/Disc1/default.xex");
  Touch(games / "Mass Effect 2/Disc2/default.xex");
  const fs::path limbo = games / "Limbo/584109D1/000D0000/66F65F42464F74941FC49C1A494B56FD73D1505E58";
  Package(limbo, "LIVE", 0x000D0000);
  Package(games / "Limbo/584109D1/000B0000/TU1", "LIVE", 0x000B0000);  // a title update: not a game
  const fs::path reach = games / "Halo Reach GOD/4D53085B/00007000/0A0B0C0D";
  Package(reach, "CON ", 0x00007000);
  Package(games / "Halo Reach GOD/4D53085B/00007000/0A0B0C0D.data/Data0000", "CON ", 0x00007000);
  Package(games / "Arcade/A/000D0000/AAAA", "LIVE", 0x000D0000);
  Package(games / "Arcade/B/000D0000/BBBB", "PIRS", 0x000D0000);
  fs::create_directories(root / "elsewhere/Outside");
  Touch(root / "elsewhere/Outside/default.xex");
  Package(root / "elsewhere/Pack/X/000D0000/CCCC", "LIVE", 0x000D0000);

  // The library as Launcher::Scan lists it (recently played first): path, folder, title ID.
  const std::vector<LibraryGame> library = {
      {games / "Fable 2/DEFAULT.XEX", games / "Fable 2", "4D5307E6"},
      {games / "Halo 3.iso", games, "4D5307E6"},  // a second copy of one title
      {games / "Single/game.xex", games / "Single", "41560817"},
      {games / "Mass Effect 2/Disc1/default.xex", games / "Mass Effect 2", "454108B4"},
      {limbo, limbo.parent_path(), "584109D1"},
      {reach, reach.parent_path(), "4D53085B"},
      {games / "Arcade/A/000D0000/AAAA", games / "Arcade/A/000D0000", "AAAAAAAA"},
      {games / "Arcade/B/000D0000/BBBB", games / "Arcade/B/000D0000", "BBBBBBBB"},
  };
  const auto pick = [&](const std::string& rom) { return Pick(rom, disk, library); };

  CHECK(pick("Halo 3.iso").entry == 1);
  CHECK(pick("Fable 2").entry == 0);
  CHECK(pick("Fable 2/").entry == 0);
  CHECK(pick((games / "Fable 2").generic_string()).entry == 0);
  CHECK(pick((games / "Fable 2/DEFAULT.XEX").generic_string()).entry == 0);
  CHECK(pick("Single").entry == 2);
  CHECK(pick("Mass Effect 2").entry == 3);         // a multi-disc set: its folder
  CHECK(pick("Limbo").entry == 4);                 // an XBLA title's folder
  CHECK(pick("Limbo/584109D1").entry == 4);
  CHECK(pick("Halo Reach GOD").entry == 5);        // a Games on Demand title's folder
  CHECK(pick("Arcade/B").entry == 7);
  {
    const Choice c = pick("Arcade");
    CHECK(c.entry < 0 && c.file.empty() && c.error.find("2 games below") != std::string::npos);
  }
  // Title IDs: the library's first (most recent) game with it, any case.
  CHECK(pick("4D5307E6").entry == 0);
  CHECK(pick("584109d1").entry == 4);
  CHECK(pick("DEADBEEF").entry < 0 && pick("DEADBEEF").error.find("no game with this title ID") != std::string::npos);
  // Not in the library: started as the file the folder holds.
  {
    const Choice c = pick((root / "elsewhere/Outside").generic_string());
    CHECK(c.entry < 0 && c.file == root / "elsewhere/Outside/default.xex");
    const Choice p = pick((root / "elsewhere/Pack").generic_string());
    CHECK(p.entry < 0 && p.file == root / "elsewhere/Pack/X/000D0000/CCCC");
  }
  {
    const Choice c = pick("Two");
    CHECK(c.file.empty() && c.error.find("2 .xex files") != std::string::npos);
    const Choice e = pick("Empty");
    CHECK(e.file.empty() && e.error.find("no default.xex") != std::string::npos);
  }
  CHECK(pick("Gears.iso").error == "Gears.iso: not found in the game folders");
  CHECK(pick("/nowhere/Gears.iso").error == "/nowhere/Gears.iso: not found");
  CHECK(pick("../Gears.iso").error.find("not a path inside") != std::string::npos);
  CHECK(pick("").error == "no game given");
  CHECK(GamePackage(limbo));
  CHECK(!GamePackage(games / "Limbo/584109D1/000B0000/TU1"));
  CHECK(!GamePackage(games / "Halo 3.iso"));
  fs::remove_all(root);

  if (failures) {
    std::fprintf(stderr, "forwarder arguments: %d check(s) failed\n", failures);
    return 1;
  }
  std::puts("forwarder arguments: all checks passed");
  return 0;
}
