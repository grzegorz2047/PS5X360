// SPDX-License-Identifier: MIT
// Fix 33 (tools/source-1.0.2/fixes/33-switch-title-settings.patch): a title switch keeps the
// game's own options. Drives xbox360ps5::switch_title (port/switch_title.hpp) in a temporary
// folder with launch_data.bin files written as XamModule::SaveLoaderData writes them, and checks:
//  * the next game's host path is read without consuming the file (the core reads it later);
//    short, empty and missing files give no path;
//  * the file is written back byte for byte for a restart;
//  * the title id comes from the reader of the file's kind (.xex, .iso, package): 1.0.2 used the
//    XEX reader for all, which gave The Orange Box's ISO no id and so no preset;
//  * restarts into the game's options: always for a start the user made, once per title for a
//    switched one, never for a switched one without an id; the guard is read once;
//  * the assembled canary_game_main.cpp reads the pending switch at start and uses all of it.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "../port/switch_title.hpp"  // the assembled tree's port/ (run from <tree>/tools)

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
namespace st = xbox360ps5::switch_title;

// XamModule::SaveLoaderData's layout: host path, launch path (each a uint16 length and bytes),
// launch flags (uint32), launch data (a uint16 size and bytes); little-endian (x86).
std::string LoaderFile(const std::string& host, const std::string& launch, uint32_t flags,
                       const std::string& data) {
  std::string out;
  auto u16 = [&out](size_t v) { out.push_back(char(v & 0xFF)); out.push_back(char((v >> 8) & 0xFF)); };
  u16(host.size()); out += host;
  u16(launch.size()); out += launch;
  for (int i = 0; i < 4; ++i) out.push_back(char((flags >> (8 * i)) & 0xFF));
  u16(data.size()); out += data;
  return out;
}

void Write(const fs::path& path, const std::string& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), std::streamsize(bytes.size()));
  assert(out);
}

std::string Read(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

// The readers the title has, as fakes: what kind of file each was asked about.
struct Readers {
  std::vector<std::string> asked;
  std::string Xex(const fs::path& p) { asked.push_back("xex:" + p.filename().string()); return p.stem() == "default" ? "4541080F" : "584108A9"; }
  std::string Iso(const fs::path& p) { asked.push_back("iso:" + p.filename().string()); return "4541080F"; }
  std::string Package(const fs::path& p) { asked.push_back("package:" + p.filename().string()); return "4D5307D3"; }
  std::string IdOf(const fs::path& p) {
    return st::TitleIdOf(p, [this](const fs::path& x) { return Xex(x); },
                         [this](const fs::path& x) { return Iso(x); },
                         [this](const fs::path& x) { return Package(x); });
  }
};

void CheckPending(const fs::path& dir) {
  const fs::path file = dir / "launch_data.bin";
  // The Orange Box: its menu starts Half-Life 2 on the same disc (an ISO), with launch data.
  const std::string data("\x01\x02\x00\x03hl2", 7);
  const std::string bytes = LoaderFile("/data/games/The Orange Box.iso", "hl2.xex", 0x10, data);
  Write(file, bytes);
  const st::Pending pending = st::ReadPending(file);
  assert(pending.host_path == "/data/games/The Orange Box.iso");
  assert(pending.bytes == bytes);
  assert(fs::exists(file));  // read only: the core consumes it later
  // An XEX switch as SaveLoaderData resolves it (the new executable's full path).
  Write(file, LoaderFile("/data/games/Collection/game2/default.xex", "", 0, ""));
  assert(st::ReadPending(file).host_path == "/data/games/Collection/game2/default.xex");
  // A long path (over 255 bytes: the length's high byte).
  const std::string long_path = "/data/" + std::string(300, 'a') + ".xex";
  Write(file, LoaderFile(long_path, "", 0, ""));
  assert(st::ReadPending(file).host_path == long_path);
  // Missing, empty, truncated or empty-path files give no path (and no crash).
  fs::remove(file);
  assert(st::ReadPending(file).host_path.empty() && st::ReadPending(file).bytes.empty());
  Write(file, "");
  assert(st::ReadPending(file).host_path.empty());
  Write(file, std::string("\x05", 1));
  assert(st::ReadPending(file).host_path.empty());
  Write(file, std::string("\x10\x00/short", 8));
  assert(st::ReadPending(file).host_path.empty());
  Write(file, LoaderFile("", "x.xex", 0, ""));
  assert(st::ReadPending(file).host_path.empty());
  // Written back byte for byte, binary data included.
  std::string binary = LoaderFile("/data/games/x.iso", "a\\b.xex", 0xFFFFFFFF, std::string("\0\r\n\x1a\xff", 5));
  Write(file, binary);
  const st::Pending kept = st::ReadPending(file);
  fs::remove(file);
  assert(st::RestorePending(file, kept));
  assert(Read(file) == binary);
  assert(!st::RestorePending(dir / "never.bin", st::Pending{}));
  assert(!fs::exists(dir / "never.bin"));
  std::puts("launch_data.bin read, kept and written back: ok");
}

void CheckTitleIds() {
  assert(st::KindOf("/a/default.xex") == st::Kind::xex);
  assert(st::KindOf("/a/DEFAULT.XEX") == st::Kind::xex);
  assert(st::KindOf("/a/game.iso") == st::Kind::iso);
  assert(st::KindOf("/a/Game.ISO") == st::Kind::iso);
  assert(st::KindOf("/a/584108A9/00007000/0123456789ABCDEF") == st::Kind::package);
  Readers readers;
  assert(readers.IdOf("/g/The Orange Box.iso") == "4541080F");
  assert(readers.IdOf("/g/c/default.xex") == "4541080F");
  assert(readers.IdOf("/g/584108A9/00007000/ABCDEF") == "4D5307D3");
  assert(readers.IdOf("") == "");
  assert((readers.asked == std::vector<std::string>{"iso:The Orange Box.iso", "xex:default.xex", "package:ABCDEF"}));
  // 1.0.2's switch branch: the XEX reader on an ISO finds no XEX header, so no id and no preset.
  auto old_xex_reader_on_iso = [](const fs::path& p) { return st::KindOf(p) == st::Kind::xex ? std::string("4541080F") : std::string(); };
  assert(old_xex_reader_on_iso("/g/The Orange Box.iso").empty());
  std::puts("title id by kind: ok");
}

void CheckRestarts(const fs::path& dir) {
  // A start the user made restarts into its options as before.
  assert(st::MayRestart(false, "", "4541080F"));
  assert(st::MayRestart(false, "4541080F", "4541080F"));
  assert(st::MayRestart(false, "", ""));
  // A switched one: once per title, never without an id.
  assert(st::MayRestart(true, "", "4541080F"));
  assert(!st::MayRestart(true, "4541080F", "4541080F"));
  assert(st::MayRestart(true, "4541080F", "584108A9"));
  assert(!st::MayRestart(true, "", ""));
  // The guard is read once (removed when read), so a later switch may restart again.
  const fs::path guard = dir / "switch-restart.txt";
  assert(st::TakeRestartGuard(guard).empty());
  assert(st::SetRestartGuard(guard, "4541080F"));
  assert(st::TakeRestartGuard(guard) == "4541080F");
  assert(!fs::exists(guard));
  assert(st::TakeRestartGuard(guard).empty());
  // The whole sequence: switch, restart into FSI once, start again, no second restart.
  const fs::path data = dir / "launch_data.bin";
  const std::string bytes = LoaderFile("/g/State of Decay.iso", "", 0, "");
  Write(data, bytes);
  bool fsi_in_effect = false;
  const bool fsi_wanted = true;
  int restarts = 0;
  for (int start = 0; start < 4; ++start) {
    const st::Pending pending = st::ReadPending(data);
    const std::string restarted = st::TakeRestartGuard(guard);
    if (pending.host_path.empty()) break;  // the launcher: the switch is over
    fs::remove(data);                     // the core's LoadLoaderData consumes it
    const std::string title = "584109A5";
    if (st::MayRestart(true, restarted, title) && fsi_wanted != fsi_in_effect) {
      st::SetRestartGuard(guard, title);
      assert(st::RestorePending(data, pending));
      fsi_in_effect = fsi_wanted;  // FsiForNextStart
      ++restarts;
      continue;
    }
    break;  // the game runs
  }
  assert(restarts == 1 && fsi_in_effect);
  assert(!fs::exists(data));
  std::puts("restarts into the game's options: ok");
}

void CheckTree() {
  const std::string main = Read("src/canary_game_main.cpp");
  assert(!main.empty());
  auto has = [&main](const char* text) { return main.find(text) != std::string::npos; };
  assert(has("#include \"switch_title.hpp\""));
  assert(has("switch_title::ReadPending("));
  assert(has("kXamModuleLoaderDataFileName"));
  assert(has("TakeRestartGuard(storage / \"switch-restart.txt\")"));
  assert(has("settings_title = GameTitleId(switch_pending.host_path);"));
  assert(has("if (log_id.empty()) log_id = GameTitleId(game);"));
  assert(has("may_restart && fsi_wanted != xbox360ps5::FsiInEffect()"));
  assert(has("may_restart && xbox360ps5::StartOptionsDiffer(settings, started)"));
  assert(has("RestorePending("));
  assert(!has("!switched && fsi_wanted"));
  assert(!has("!switched && xbox360ps5::StartOptionsDiffer"));
  assert(!has("log_id = xbox360ps5::ReadXexTitleId(game)"));
  // The pending switch is read before the start's options are made from settings_title.
  assert(main.find("switch_title::ReadPending(") < main.find("xbox360ps5::ForGame(global, settings_title, overrides)"));
  std::puts("assembled canary_game_main.cpp: ok");
}
}  // namespace

int main() {
  const fs::path dir = fs::temp_directory_path() / "check-switch-title";
  fs::remove_all(dir);
  fs::create_directories(dir);
  CheckPending(dir);
  CheckTitleIds();
  CheckRestarts(dir);
  CheckTree();
  fs::remove_all(dir);
  std::puts("check-switch-title: all passed");
  return 0;
}
