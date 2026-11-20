// SPDX-License-Identifier: MIT
// Fix 46 (tools/source-1.0.2/fixes/46-ps5x360reloaded-identity.patch): the first start's copy of
// the official PS5X360's saves and small settings files into PS5X360Reloaded's folder
// (include/xbox360ps5/official_import.hpp).
//   * the decision alone (Decide): run once (the marker), nothing when the folders overlap, the
//     marker only when there is no official folder or not enough room (the free space exactly
//     enough is enough), the saves only when the title has none;
//   * on a temporary folder (Run): the saves copied into place, links and unreadable files left
//     out, nothing the title has overwritten, caches, logs and the web key not copied; the
//     official folder byte for byte, mode for mode and time for time as it was (and read-only
//     during the run); a second start does nothing; without the marker the title's own saves are
//     kept; short of room nothing but the marker is written; a staging folder left by an
//     interrupted start is cleared; a link in the title's folder that leads into the official one
//     is not written through; overlapping folders are refused without a marker.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "../include/xbox360ps5/official_import.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace {
namespace fs = std::filesystem;
namespace oi = xbox360ps5::official_import;
using oi::Decision;

constexpr uint64_t kBig = 1ull << 40;

void Decisions() {
  oi::Facts facts;
  facts.marker = true;
  facts.source = facts.source_saves = true;
  facts.space_known = true;
  facts.free = kBig;
  auto plan = oi::Decide(facts);
  assert(plan.decision == Decision::kAlreadyDone && !plan.copy_saves && !plan.copy_small && !plan.write_marker);
  facts.marker = false;
  facts.unsafe = true;
  plan = oi::Decide(facts);
  assert(plan.decision == Decision::kUnsafe && !plan.copy_saves && !plan.copy_small && !plan.write_marker);
  facts.unsafe = false;
  // No official folder: the marker only, whatever the room.
  oi::Facts none;
  plan = oi::Decide(none);
  assert(plan.decision == Decision::kNothingToImport && plan.write_marker && !plan.copy_saves && !plan.copy_small);
  // Saves and small files, with room.
  facts.saves_bytes = 5000;
  facts.small_bytes = 300;
  plan = oi::Decide(facts);
  assert(plan.decision == Decision::kImport && plan.copy_saves && plan.copy_small && plan.write_marker);
  assert(plan.needed == 5300 + oi::kMargin);
  // Exactly enough room is enough; one byte less is not, and then nothing is copied.
  facts.free = 5300 + oi::kMargin;
  assert(oi::Decide(facts).decision == Decision::kImport);
  facts.free -= 1;
  plan = oi::Decide(facts);
  assert(plan.decision == Decision::kNoSpace && !plan.copy_saves && !plan.copy_small && plan.write_marker);
  // Room not known: not enough.
  facts.free = kBig;
  facts.space_known = false;
  assert(oi::Decide(facts).decision == Decision::kNoSpace);
  facts.space_known = true;
  // The title has saves: never merged; only the small files, and only their bytes count.
  facts.target_saves = true;
  facts.free = 300 + oi::kMargin;
  plan = oi::Decide(facts);
  assert(plan.decision == Decision::kImport && !plan.copy_saves && plan.copy_small && plan.needed == 300 + oi::kMargin);
  // No official saves.
  facts.target_saves = false;
  facts.source_saves = false;
  facts.free = kBig;
  plan = oi::Decide(facts);
  assert(plan.decision == Decision::kImport && !plan.copy_saves && plan.copy_small && plan.needed == 300 + oi::kMargin);
  std::printf("  decisions: once, refused on overlap, room for all of it or nothing, saves only when the title has none\n");
}

// --- a temporary console --------------------------------------------------------------------
fs::path Temp() {
  char pattern[] = "/tmp/check-official-import-XXXXXX";
  const char* made = mkdtemp(pattern);
  assert(made);
  return made;
}
void Write(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file << text;
  assert(file);
}
std::string Read(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}
// Everything about a folder that a write could change: each entry's type, mode, size, times and
// contents (a link's target), and each folder's times (an entry made or removed changes them).
std::map<std::string, std::string> Snapshot(const fs::path& root) {
  std::map<std::string, std::string> entries;
  std::error_code error;
  const auto describe = [&](const fs::path& path) {
    struct stat info {};
    assert(!lstat(path.c_str(), &info));
    std::ostringstream text;
    text << std::oct << info.st_mode << std::dec << ' ' << info.st_size << ' ' << info.st_mtim.tv_sec << '.'
         << info.st_mtim.tv_nsec << ' ' << info.st_ctim.tv_sec << '.' << info.st_ctim.tv_nsec << ' ' << info.st_nlink;
    if (S_ISLNK(info.st_mode)) {
      text << " -> " << fs::read_symlink(path).string();
    } else if (S_ISREG(info.st_mode)) {
      std::ifstream file(path, std::ios::binary);
      if (file) {
        std::stringstream data;
        data << file.rdbuf();
        text << " = " << data.str();
      } else {
        text << " (unreadable)";
      }
    }
    entries[path.lexically_relative(root.parent_path()).string()] = text.str();
  };
  describe(root);
  for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, error), end;
       !error && it != end; it.increment(error))
    describe(it->path());
  assert(!error);
  return entries;
}
void Chmod(const fs::path& root, fs::perms folders) {
  std::error_code error;
  for (fs::recursive_directory_iterator it(root, error), end; !error && it != end; it.increment(error))
    if (it->symlink_status().type() == fs::file_type::directory) fs::permissions(it->path(), folders);
  fs::permissions(root, folders);
}
bool Exists(const fs::path& path) {
  std::error_code error;
  const auto type = fs::symlink_status(path, error).type();
  return type != fs::file_type::not_found && type != fs::file_type::none;
}
oi::FreeSpace Room(uint64_t bytes) {
  return [bytes](const fs::path&, uint64_t* free) {
    *free = bytes;
    return true;
  };
}

constexpr char kSave[] = "saves/E000ABCDEF012345/4D5307E6/00000001/Halo3Save";
constexpr char kProfile[] = "saves/E000ABCDEF012345/FFFE07D1/00010000/E000ABCDEF012345";
constexpr char kDlc[] = "saves/0000000000000000/4D5307E6/00000002/MapPack1";

// The official folder: saves (with a link and, unless running as root, an unreadable file),
// settings, pictures, caches, logs and its web key.
void MakeOfficial(const fs::path& official, bool* unreadable) {
  Write(official / kSave, std::string(4096, 's'));
  Write(official / kProfile, "profile");
  Write(official / kDlc, std::string(10000, 'd'));
  Write(official / "network.conf", "mode=online\nserver=http://192.168.1.10:36000\n");
  Write(official / "vibration.conf", "mode=1\nstrength=70\n");
  Write(official / "ui-dark.on", "1\n");
  Write(official / "skin/home.png", "png");
  Write(official / "skin/sub/tile.png", "tile");
  Write(official / "game-art/4D5307E6.jpg", "jpg");
  Write(official / "dashboard/dash.xex", "XEX2");
  Write(official / "cache/achievements/4D5307E6-1.png", "icon");
  Write(official / "cache/core/shaders.bin", std::string(2048, 'c'));
  Write(official / "logs/session.log", "log");
  Write(official / "web-settings.key", "0f0f0f\n");
  Write(official / "phone-camera/authority.crt", "crt");
  Write(official / "eboot.bin", "ELF");
  fs::create_symlink("/etc", official / "saves/E000ABCDEF012345/escape");
  fs::create_symlink(official / "network.conf", official / "skin/link.png");
  *unreadable = geteuid() != 0;
  if (*unreadable) {
    Write(official / "saves/E000ABCDEF012345/4D5307E6/00000001/Locked", "locked");
    fs::permissions(official / "saves/E000ABCDEF012345/4D5307E6/00000001/Locked", fs::perms::none);
  }
}

void FirstStart(const fs::path& root) {
  const fs::path official = root / "official", own = root / "reloaded";
  bool unreadable = false;
  MakeOfficial(official, &unreadable);
  // The title already has a network.conf of its own: it is kept.
  Write(own / "network.conf", "mode=offline\n");
  const auto facts = oi::Gather(official, own, Room(kBig));
  assert(facts.source && facts.source_saves && !facts.target_saves && !facts.marker && !facts.unsafe);
  assert(facts.saves_bytes == 4096 + 7 + 10000 + (unreadable ? 6 : 0));
  // vibration.conf, ui-dark.on, skin (2), game-art, dashboard, the icon; network.conf is had.
  assert(facts.small_bytes == 19 + 2 + 3 + 4 + 3 + 4 + 4);
  Chmod(official, fs::perms::owner_read | fs::perms::owner_exec | fs::perms::group_read | fs::perms::group_exec |
                      fs::perms::others_read | fs::perms::others_exec);
  const auto before = Snapshot(official);
  const auto result = oi::Run(official, own, Room(kBig));
  assert(Snapshot(official) == before);
  assert(result.plan.decision == Decision::kImport && result.saves_copied && result.error.empty());
  assert(Read(own / kSave) == std::string(4096, 's') && Read(own / kProfile) == "profile" &&
         Read(own / kDlc) == std::string(10000, 'd'));
  assert(!Exists(own / "saves/E000ABCDEF012345/escape"));
  assert(!Exists(own / "saves/E000ABCDEF012345/4D5307E6/00000001/Locked"));
  assert(Read(own / "network.conf") == "mode=offline\n");
  assert(Read(own / "vibration.conf") == "mode=1\nstrength=70\n" && Read(own / "ui-dark.on") == "1\n");
  assert(Read(own / "skin/home.png") == "png" && Read(own / "skin/sub/tile.png") == "tile");
  assert(!Exists(own / "skin/link.png"));
  assert(Read(own / "game-art/4D5307E6.jpg") == "jpg" && Read(own / "dashboard/dash.xex") == "XEX2");
  assert(Read(own / "cache/achievements/4D5307E6-1.png") == "icon");
  assert(!Exists(own / "cache/core") && !Exists(own / "logs") && !Exists(own / "web-settings.key") &&
         !Exists(own / "phone-camera") && !Exists(own / "eboot.bin") && !Exists(own / oi::kStaging));
  // Copies, not links: the title's saves are files of its own.
  assert(fs::symlink_status(own / kSave).type() == fs::file_type::regular);
  struct stat info {};
  assert(!lstat((own / kSave).c_str(), &info) && info.st_nlink == 1);
  // Files: 3 saves, vibration.conf, ui-dark.on, 2 skin, game art, dash.xex, the icon.
  assert(result.files == 10 && result.kept == 1);
  assert(result.skipped == 2u + (unreadable ? 1u : 0u));  // the two links (and the locked file)
  assert(result.marker_written && Exists(own / oi::kMarker));
  assert(Read(own / oi::kMarker).find("imported") != std::string::npos);
  // The second start: nothing at all.
  const auto mine = Snapshot(own);
  const auto again = oi::Run(official, own, Room(kBig));
  assert(again.plan.decision == Decision::kAlreadyDone && again.files == 0 && !again.marker_written);
  assert(Snapshot(own) == mine && Snapshot(official) == before);
  // Without the marker (deleted by hand): the title's saves are its own now and are kept, and
  // only missing small files come again.
  fs::remove(own / oi::kMarker);
  Write(own / kSave, "played since");
  fs::remove(own / "ui-dark.on");
  const auto third = oi::Run(official, own, Room(kBig));
  assert(third.plan.decision == Decision::kImport && !third.plan.copy_saves && !third.saves_copied);
  assert(Read(own / kSave) == "played since" && Read(own / "ui-dark.on") == "1\n");
  assert(third.files == 1 && Exists(own / oi::kMarker) && Snapshot(official) == before);
  Chmod(official, fs::perms::owner_all);
  std::printf("  first start: saves and settings copied (%llu files), links%s left out, the title's own "
              "network.conf kept, caches, logs and the key not copied; the official folder unchanged; "
              "the second start does nothing\n",
              static_cast<unsigned long long>(result.files), unreadable ? " and an unreadable file" : "");
}

void NoRoom(const fs::path& root) {
  const fs::path official = root / "official", own = root / "short";
  const auto before = Snapshot(official);
  const auto result = oi::Run(official, own, Room(1000));
  assert(result.plan.decision == Decision::kNoSpace && !result.saves_copied && result.files == 0);
  // Nothing but the marker: no saves, no staging, no settings.
  int entries = 0;
  for (const auto& entry : fs::directory_iterator(own)) {
    ++entries;
    assert(entry.path().filename() == oi::kMarker);
  }
  assert(entries == 1);
  assert(Read(own / oi::kMarker).find("not enough free space") != std::string::npos);
  assert(Snapshot(official) == before);
  // Decided: the next start does not ask again.
  assert(oi::Run(official, own, Room(kBig)).plan.decision == Decision::kAlreadyDone);
  std::printf("  short of room: nothing copied, nothing partial, the marker says why\n");
}

void Interrupted(const fs::path& root) {
  const fs::path official = root / "official", own = root / "interrupted";
  // A start that stopped while copying left its staging folder (and no marker, no saves).
  Write(own / oi::kStaging / "E000ABCDEF012345/half", "half");
  const auto result = oi::Run(official, own, Room(kBig));
  assert(result.plan.decision == Decision::kImport && result.saves_copied);
  assert(!Exists(own / oi::kStaging) && !Exists(own / "saves/E000ABCDEF012345/half"));
  assert(Read(own / kSave) == std::string(4096, 's'));
  std::printf("  interrupted start: its staging folder cleared, the saves copied again\n");
}

void NoOfficial(const fs::path& root) {
  const fs::path own = root / "alone";
  const auto result = oi::Run(root / "nowhere", own, Room(kBig));
  assert(result.plan.decision == Decision::kNothingToImport && result.marker_written);
  assert(!Exists(root / "nowhere") && !Exists(own / "saves"));
  std::printf("  no official folder: the marker only (the official folder is not made)\n");
}

void Unsafe(const fs::path& root) {
  const fs::path official = root / "official";
  Chmod(official, fs::perms::owner_all);
  const auto before = Snapshot(official);
  // The title's folder inside the official one, the same folder, or a link to it: refused.
  for (const fs::path& own : {official / "inside", official, root / "linked"}) {
    if (own == root / "linked") fs::create_directory_symlink(official, own);
    const auto result = oi::Run(official, own, Room(kBig));
    assert(result.plan.decision == Decision::kUnsafe && !result.marker_written && result.files == 0);
  }
  // Refused before anything is made: not even the missing title folder inside the official one.
  assert(!Exists(official / "inside") && Snapshot(official) == before);
  // A link in the title's folder that leads into the official one is not written through.
  const fs::path own = root / "planted";
  fs::create_directories(own);
  fs::create_directory_symlink(official / "skin", own / "skin");
  fs::create_directory_symlink(official / "cache", own / "cache");
  const auto skin = Snapshot(official / "skin");
  const auto cache = Snapshot(official / "cache");
  const auto result = oi::Run(official, own, Room(kBig));
  assert(result.plan.decision == Decision::kImport && result.saves_copied);
  assert(Snapshot(official / "skin") == skin && Snapshot(official / "cache") == cache);
  assert(!Exists(official / "skin/skin") && !Exists(official / "cache/achievements/achievements"));
  std::printf("  overlapping folders refused without a marker; a planted link into the official folder "
              "not written through\n");
}
}  // namespace

int main() {
  Decisions();
  const fs::path root = Temp();
  FirstStart(root);
  NoRoom(root);
  Interrupted(root);
  NoOfficial(root);
  Unsafe(root);
  {
    uint64_t free = 0;
    assert(oi::RealFreeSpace(root, &free) && free > 0);
  }
  std::error_code error;
  for (fs::recursive_directory_iterator it(root, error), end; !error && it != end; it.increment(error))
    if (it->symlink_status().type() == fs::file_type::directory) fs::permissions(it->path(), fs::perms::owner_all, error);
  fs::remove_all(root, error);
  std::puts("check-official-import: ok");
  return 0;
}
