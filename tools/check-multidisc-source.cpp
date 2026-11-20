// SPDX-License-Identifier: MIT
// Fix 51 (tools/source-1.0.2/fixes/51-multidisc-source.patch): a game that asks for another disc
// (XamSwapDisc; Battlefield 3's campaign is on disc 2) gets it from beside the launched image.
// 1.0.2's canary runtime never told the disc swap which image it launched (SetLaunchedGame), so
// every swap failed with "InvalidPath: Empty, oversized or non-UTF8 path".
//  (a) port/disc_catalog.cpp's CandidatesForDisc on a temporary folder: from "Game (USA) (Disc 1).iso"
//      the set is that game's disc 1 and disc 2, in disc order (another game's disc 2 and the
//      single-disc image beside it are not in it; SwapMultiDisc then reads each one's default.xex
//      to pick the requested disc), the same from disc 2, and an empty launched path gives the
//      error the console logged and no candidate;
//  (b) the assembled tree: canary_game_main.cpp includes title_switch.hpp and calls
//      SetLaunchedGame(game) after the loader data and before emulator.LaunchPath(game);
//      SetLaunchedGame still sets the multi-disc source; a successful swap updates the loader's
//      host path (a title switch then mounts the disc in the drive); XamSwapCancel is answered;
//      disc_catalog.cpp falls back to stat when realpath fails (the console's sandbox).
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "../port/disc_catalog.cpp"

#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;
void Expect(bool condition, const std::string& what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what.c_str());
    ++failures;
  }
}

std::string Read(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}

void CheckCandidates() {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / ("check-multidisc-" + std::to_string(::getpid()));
  fs::remove_all(dir);
  fs::create_directories(dir);
  for (const char* name : {"Game (USA) (Disc 1).iso", "Game (USA) (Disc 2).iso", "Other (Disc 2).iso", "Game (USA).iso"})
    std::ofstream(dir / name) << "image";
  const fs::path disc1 = fs::canonical(dir / "Game (USA) (Disc 1).iso");
  const fs::path disc2 = fs::canonical(dir / "Game (USA) (Disc 2).iso");
  std::string error;
  const auto found = xbox360ps5::multidisc::CandidatesForDisc(disc1, 2, error);
  std::printf("   disc 1 -> disc 2: %zu candidate(s)%s%s\n", found.size(), error.empty() ? "" : ", error ", error.c_str());
  Expect(error.empty(), "no error from disc 1 for disc 2: " + error);
  Expect(found.size() == 2 && fs::path(found[0]) == disc1 && fs::path(found[1]) == disc2,
         "the same game's discs 1 and 2, in order, nothing else");
  const auto back = xbox360ps5::multidisc::CandidatesForDisc(disc2, 1, error);
  Expect(error.empty() && back.size() == 2 && fs::path(back[0]) == disc1 && fs::path(back[1]) == disc2,
         "the same set from disc 2");
  const auto none = xbox360ps5::multidisc::CandidatesForDisc(fs::path(), 2, error);
  std::printf("   empty source: %s\n", error.c_str());
  Expect(none.empty() && error.find("Empty") != std::string::npos,
         "an empty launched path (1.0.2's canary runtime): no candidate, the console's error");
  fs::remove_all(dir);
}

void CheckTree() {
  const std::string main_text = Read("src/canary_game_main.cpp");
  Expect(main_text.find("#include \"xbox360ps5/title_switch.hpp\"") != std::string::npos, "the main includes title_switch.hpp");
  const size_t host = main_text.find("xam->loader_data().host_path = xe::path_to_utf8(game);");
  const size_t set = main_text.find("xbox360ps5::SetLaunchedGame(game);", host);
  const size_t launch = main_text.find("status = emulator.LaunchPath(game);", host);
  Expect(host != std::string::npos && set != std::string::npos && launch != std::string::npos && set < launch,
         "SetLaunchedGame(game) after the loader data, before LaunchPath");
  const std::string switch_text = Read("src/title_switch.cpp");
  Expect(switch_text.find("void SetLaunchedGame(const fs::path& game) { launched_game = game; SetMultiDiscSource(game); }") !=
             std::string::npos,
         "SetLaunchedGame sets the multi-disc source");
  const std::string swap = Read("port/multi_disc.cpp");
  const size_t mounted = swap.find("SetLaunchedGame(accepted_path);");
  const size_t loader = swap.find("xam->loader_data().host_path=xe::path_to_utf8(accepted_path);");
  Expect(mounted != std::string::npos && loader != std::string::npos && loader > mounted,
         "a swap updates the loader's host path");
  const std::string content = Read(".deps/xenia-canary/src/xenia/kernel/xam/xam_content.cc");
  Expect(content.find("dword_result_t XamSwapCancel_entry() { return X_ERROR_SUCCESS; }") != std::string::npos &&
             content.find("DECLARE_XAM_EXPORT1(XamSwapCancel, kContent, kStub);") != std::string::npos,
         "XamSwapCancel is answered");
  const std::string catalog = Read("port/disc_catalog.cpp");
  Expect(catalog.find("auto actual=Physical(p,ec);") != std::string::npos &&
             catalog.find("auto physical=Physical(family_dir,ec);") != std::string::npos &&
             catalog.find("fs::exists(NativePath(p),stat_error)") != std::string::npos,
         "the catalog falls back to stat when realpath fails");
  size_t canonical_calls = 0;
  for (size_t at = catalog.find("fs::canonical("); at != std::string::npos; at = catalog.find("fs::canonical(", at + 1))
    ++canonical_calls;
  Expect(canonical_calls == 1, "fs::canonical only inside Physical");
}

}  // namespace

int main() {
  CheckCandidates();
  CheckTree();
  if (failures) {
    std::printf("check-multidisc-source: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-multidisc-source: ok\n");
  return 0;
}
