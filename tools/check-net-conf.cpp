// SPDX-License-Identifier: MIT
// Fix 29 (tools/source-1.0.2/fixes/29-network-conf-folder.patch): network.conf's mode, written by
// the home's Online / System Link channel and the guide's network row through KinectSetNetMode.
// 1.0.2 wrote /data/homebrew/PPSA50011/network.conf with a bare std::ofstream, which fails when
// that folder does not exist ("Could not write network.conf.", BrinooTk/PS5X360 issue 111).
// Drives xbox360ps5::net_conf::SetMode (port/net_conf.hpp) in a temporary folder, shows the 1.0.2
// write failing where the fix succeeds, and checks the assembled launcher calls it.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "../port/net_conf.hpp"  // the assembled tree's port/ (run from <tree>/tools)
#include "../include/xbox360ps5/app_identity.hpp"  // fix 46: the title's folder

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
namespace nc = xbox360ps5::net_conf;

std::string Read(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}

void Write(const fs::path& path, const std::string& text) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file << text;
  assert(file);
}

// KinectSetNetMode as 1.0.2 had it.
bool OldSetNetMode(const fs::path& file, const std::string& mode) {
  std::vector<std::string> kept;
  {
    std::ifstream conf(file);
    for (std::string line; std::getline(conf, line);)
      if (line.rfind("mode=", 0) != 0) kept.push_back(line);
  }
  std::ofstream conf(file, std::ios::trunc);
  conf << "mode=" << mode << '\n';
  for (const auto& line : kept) conf << line << '\n';
  return bool(conf);
}

fs::path TempRoot() {
  char pattern[] = "/tmp/check-net-conf-XXXXXX";
  const char* made = mkdtemp(pattern);
  assert(made);
  return made;
}

void MissingFolder(const fs::path& root) {
  // The console without the title's /data/homebrew folder (title installed as a package).
  const fs::path file = root / std::string(XBOX360PS5_DATA_ROOT "/network.conf").substr(1);
  assert(!fs::exists(file.parent_path()));
  assert(!OldSetNetMode(file, "online"));  // 1.0.2: "Could not write network.conf."
  assert(!fs::exists(file.parent_path()));
  std::string error = "untouched";
  assert(nc::SetMode(file, "online", &error));
  assert(error == "untouched");
  assert(fs::is_directory(file.parent_path()));
  assert(Read(file) == "mode=online\n");
  std::printf("  missing folder: 1.0.2 fails, the fix creates it and writes mode=online\n");
}

void KeepsOtherLines(const fs::path& root) {
  const fs::path file = root / "keep/network.conf";
  fs::create_directories(file.parent_path());
  Write(file, "server=http://192.168.1.10:36000\nmode=offline\nupnp=1\n");
  assert(nc::SetMode(file, "systemlink"));
  assert(Read(file) == "mode=systemlink\nserver=http://192.168.1.10:36000\nupnp=1\n");
  assert(nc::SetMode(file, "offline"));  // the guide's row turning it off again
  assert(Read(file) == "mode=offline\nserver=http://192.168.1.10:36000\nupnp=1\n");
  // Several mode= lines (hand-edited): one is left.
  Write(file, "mode=online\nmode=systemlink\nserver=x\n");
  assert(nc::SetMode(file, "online"));
  assert(Read(file) == "mode=online\nserver=x\n");
  // Same result as 1.0.2 where 1.0.2 could write.
  const fs::path old_file = root / "keep/old.conf";
  Write(old_file, "server=http://192.168.1.10:36000\nmode=offline\nupnp=1\n");
  Write(file, "server=http://192.168.1.10:36000\nmode=offline\nupnp=1\n");
  assert(OldSetNetMode(old_file, "online") && nc::SetMode(file, "online"));
  assert(Read(old_file) == Read(file));
  std::printf("  existing file: mode= replaced, the other lines kept, as 1.0.2 wrote it\n");
}

void FailuresSaySo(const fs::path& root) {
  // A file where the folder should be: nothing can be created, and the reason comes back.
  const fs::path blocker = root / "blocked";
  Write(blocker, "not a folder");
  std::string error;
  assert(!nc::SetMode(blocker / XBOX360PS5_TITLE_ID "/network.conf", "online", &error));
  assert(error.rfind("cannot create ", 0) == 0);
  assert(error.find("blocked") != std::string::npos);
  // The path is a folder: it cannot be opened as a file.
  const fs::path folder = root / "folder/network.conf";
  fs::create_directories(folder);
  error.clear();
  assert(!nc::SetMode(folder, "online", &error));
  assert(error.rfind("cannot open ", 0) == 0);
  assert(!nc::SetMode(folder, "online"));  // no error text asked for
  assert(fs::is_directory(folder));
  std::printf("  failures: false, with what could not be created or opened\n");
}

std::string ReadTree(const char* path) {
  std::ifstream file(path, std::ios::binary);
  assert(file && "run from the assembled tree's root");
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}

void LauncherUsesIt() {
  const std::string state = ReadTree("port/kinect_state.inc");
  const auto begin = state.find("bool KinectSetNetMode(const std::string& mode) {");
  assert(begin != std::string::npos);
  const auto end = state.find("\n}\n", begin);
  const std::string body = state.substr(begin, end - begin);
  assert(body.find("xbox360ps5::net_conf::SetMode(kKinectNetConf, mode, &error)") != std::string::npos);
  assert(body.find("std::ofstream") == std::string::npos);
  // Fix 46: the title's own folder (app_identity.hpp), not the official PS5X360's.
  assert(state.find("kKinectNetConf = XBOX360PS5_DATA_ROOT \"/network.conf\";") != std::string::npos);
  assert(state.find("PPSA50011") == std::string::npos);
  assert(std::string(XBOX360PS5_DATA_ROOT "/network.conf") == "/data/homebrew/PPSA83600/network.conf");
  const std::string launcher = ReadTree("src/launcher.cpp");
  const auto include = launcher.find("#include \"net_conf.hpp\"");
  const auto state_include = launcher.find("#include \"kinect_state.inc\"");
  assert(include != std::string::npos && include < state_include);
  assert(launcher.find("KinectSetNetMode(mode)") != std::string::npos);  // fix 22's guide row
  std::printf("  KinectSetNetMode (home channel and the guide's row) writes through it\n");
}
}  // namespace

int main() {
  const fs::path root = TempRoot();
  MissingFolder(root);
  KeepsOtherLines(root);
  FailuresSaySo(root);
  LauncherUsesIt();
  fs::remove_all(root);
  std::puts("PASS: network.conf written with its folder (fix 29)");
}
