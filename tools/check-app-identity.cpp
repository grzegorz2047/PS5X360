// SPDX-License-Identifier: MIT
// Fix 46 (tools/source-1.0.2/fixes/46-ps5x360reloaded-identity.patch): PS5X360Reloaded's identity
// (include/xbox360ps5/app_identity.hpp), the version label it gives the home screen
// (build_version.hpp) and which key its web settings server lets in (web_key.hpp): its own, and
// the official PS5X360's Kinect tracker key only for motion frames from the console itself.
// tools/check-title-id.py checks the rest of the tree (no official title ID, every path through
// the data root). Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#define XBOX360PS5_VERSION "1.0.3_candidate_53"
#include "../include/xbox360ps5/app_identity.hpp"
#include "../include/xbox360ps5/build_version.hpp"
#include "../include/xbox360ps5/web_key.hpp"

#include <cassert>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;
namespace id = xbox360ps5::app_identity;
namespace wk = xbox360ps5::web_key;

constexpr bool IsTitleId(std::string_view text) {
  if (text.size() != 9 || text.substr(0, 4) != "PPSA") return false;
  for (const char c : text.substr(4))
    if (c < '0' || c > '9') return false;
  return true;
}
static_assert(IsTitleId(id::kTitleId) && IsTitleId(id::kOfficialTitleId));
static_assert(std::string_view(id::kTitleId) == "PPSA83600");
static_assert(std::string_view(id::kTitleId) != std::string_view(id::kOfficialTitleId));
static_assert(std::string_view(id::kOfficialTitleId) == "PPSA50011");
static_assert(std::string_view(id::kDataRoot) == "/data/homebrew/PPSA83600");
static_assert(std::string_view(XBOX360PS5_DATA_ROOT "/saves") == "/data/homebrew/PPSA83600/saves");
static_assert(std::string_view(id::kOfficialDataRoot) == "/data/homebrew/PPSA50011");
static_assert(std::string_view(id::kProductName) == "PS5X360Reloaded");
static_assert(std::string_view(id::kOfficialProductName) == "PS5X360");

void Identity() {
  // The two folders are siblings: neither is inside the other.
  const std::string own = id::kDataRoot, official = id::kOfficialDataRoot;
  assert(own.rfind(official, 0) != 0 && official.rfind(own, 0) != 0);
  assert(fs::path(own).parent_path() == fs::path(official).parent_path());
  // The home screen's label, as the release checks look for it ("PS5X360Reloaded v<VERSION>").
  assert(std::string(xbox360ps5::kBuildVersionLabel) == "PS5X360Reloaded v1.0.3_candidate_53");
  assert(std::string(xbox360ps5::kBuildVersionLabel).rfind("PS5X360 v", 0) != 0);
  std::printf("  identity: %s, %s, %s (official %s, %s, read only); label \"%s\"\n", id::kProductName,
              id::kTitleId, id::kDataRoot, id::kOfficialTitleId, id::kOfficialDataRoot,
              xbox360ps5::kBuildVersionLabel);
}

void Keys() {
  assert(wk::Valid("0a1b2c") && wk::Valid("ffffff") && wk::Valid("000000"));
  assert(!wk::Valid("") && !wk::Valid("0a1b2") && !wk::Valid("0a1b2c3") && !wk::Valid("0A1B2C") &&
         !wk::Valid("0a1b2g") && !wk::Valid("0a 1b2"));
  char pattern[] = "/tmp/check-app-identity-XXXXXX";
  const char* made = mkdtemp(pattern);
  assert(made);
  const fs::path root = made;
  const auto write = [&](const char* name, const char* text) {
    std::ofstream(root / name, std::ios::binary | std::ios::trunc) << text;
    return root / name;
  };
  assert(wk::Read(write("plain", "0a1b2c\n")) == "0a1b2c");
  assert(wk::Read(write("crlf", "0a1b2c\r\n")) == "0a1b2c");
  assert(wk::Read(write("bare", "0a1b2c")) == "0a1b2c");
  assert(wk::Read(write("upper", "0A1B2C\n")).empty());
  assert(wk::Read(write("long", "0a1b2c3\n")).empty());
  assert(wk::Read(write("empty", "")).empty());
  assert(wk::Read(root / "missing").empty());
  // Read never makes the file.
  assert(!fs::exists(root / "missing"));
  fs::remove_all(root);
  std::printf("  web-settings.key: six lower-case hex digits, read only\n");
}

void Access() {
  assert(wk::Loopback(0x7F000001u) && wk::Loopback(0x7F123456u));
  assert(!wk::Loopback(0xC0A80001u) && !wk::Loopback(0x0A000001u) && !wk::Loopback(0x00000000u));
  const std::string own = "a1b2c3", official = "0f0f0f";
  const std::string frame = "/api/motion/frame";
  // The title's own key: everything, from anywhere, as before.
  for (const char* target : {"/api/state", "/api/set", "/camera", "/api/motion/frame", "/logs.zip"})
    for (const bool loopback : {false, true}) {
      assert(wk::Accept(own, own, official, "GET", target, loopback));
      assert(wk::Accept(own, own, official, "POST", target, loopback));
    }
  // While the server starts (no key yet) an empty key goes in, as before; it never matches the
  // official one.
  assert(wk::Accept("", "", official, "GET", "/api/state", false));
  assert(!wk::Accept("", own, "", "POST", frame, true));
  // The official tracker's key: motion frames posted from this console only.
  assert(wk::Accept(official, own, official, "POST", frame, true));
  assert(!wk::Accept(official, own, official, "POST", frame, false));       // from the network
  assert(!wk::Accept(official, own, official, "GET", frame, true));         // not a post
  for (const char* target : {"/api/state", "/api/set", "/camera", "/api/motion/status", "/logs.zip", "/logs/x.txt",
                             "/api/motion/frame/", "/api/motion/frameX"})
    assert(!wk::Accept(official, own, official, "POST", target, true));
  // No official key (no file, or not a key): nothing more goes in.
  assert(!wk::Accept("0f0f0f", own, "", "POST", frame, true));
  assert(!wk::Accept("0F0F0F", own, "0F0F0F", "POST", frame, true));
  // A wrong key is wrong.
  assert(!wk::Accept("123456", own, official, "POST", frame, true));
  std::printf("  access: the title's key everywhere; the official tracker's key only for POST %s from "
              "127.0.0.0/8\n", frame.c_str());
}
}  // namespace

int main() {
  Identity();
  Keys();
  Access();
  std::puts("check-app-identity: ok");
  return 0;
}
