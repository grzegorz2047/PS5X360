// SPDX-License-Identifier: MIT
// Fix 27 (tools/source-1.0.2/fixes/27-save-device-choice.patch): when a game asks where to save
// (XamShowDeviceSelectorUI), every writable drive the console has mounted is offered besides the
// internal folder, each as a storage device of its own with its saves in <drive>/PS5X360/saves.
// The decisions live in the core's header-only xenia/kernel/xam/storage_devices.h, which the
// content manager, the device exports and the window call; this check drives them with a fake
// filesystem laid out as the console's /mnt was seen over FTP (usb0 and usb2 exFAT drives, usb1
// a read-only installer stick, ext1 the extended storage, usb3-usb7 and ext0 empty placeholders),
// then the real PosixFs on the host. Run from the assembled tree's root: it also checks that the
// title translates every text of the window (CoreUiText in src/canary_game_main.cpp).
#undef NDEBUG
#include "xenia/kernel/xam/storage_devices.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {
namespace stdfs = std::filesystem;
namespace storage = xe::kernel::xam::storage;

constexpr const char* kInternal = "/data/homebrew/PPSA50011/saves";
constexpr const char* kDash = "\xE2\x80\x93";  // the label's en dash

// The filesystem as Scan sees it. Directories exist only when listed; a probe fails under a
// read-only path; space is known only where set. Nothing is ever created.
struct FakeFs {
  std::set<std::string> mounts;
  std::set<std::string> dirs;
  std::set<std::string> read_only;
  std::map<std::string, std::pair<uint64_t, uint64_t>> space;  // capacity, free
  std::vector<std::string> probes;

  bool IsMountPoint(const stdfs::path& path) { return mounts.count(path.string()) != 0; }
  bool IsDirectory(const stdfs::path& path) {
    return dirs.count(path.string()) != 0 || mounts.count(path.string()) != 0;
  }
  bool ProbeWritable(const stdfs::path& path) {
    probes.push_back(path.string());
    for (const auto& prefix : read_only) {
      if (path.string().rfind(prefix, 0) == 0) return false;
    }
    return IsDirectory(path);
  }
  bool Space(const stdfs::path& path, uint64_t& capacity, uint64_t& free) {
    const auto found = space.find(path.string());
    if (found == space.end()) return false;
    capacity = found->second.first;
    free = found->second.second;
    return true;
  }
  std::vector<std::string> ListDirectories(const stdfs::path& path) {
    std::vector<std::string> names;
    for (const auto& dir : dirs) {
      if (stdfs::path(dir).parent_path() == path) names.push_back(stdfs::path(dir).filename().string());
    }
    return names;
  }
  void AddDir(const std::string& path) {
    for (stdfs::path p = path; p.has_relative_path(); p = p.parent_path()) dirs.insert(p.string());
  }
};

// The console of 2026-11-19 (FTP listing of /mnt).
FakeFs Console() {
  FakeFs fs;
  fs.AddDir(kInternal);
  fs.space[kInternal] = {68719476736ull, 17179869184ull};  // the game log's figures
  for (const char* placeholder : {"/mnt/ext0", "/mnt/usb3", "/mnt/usb4", "/mnt/usb5", "/mnt/usb6",
                                  "/mnt/usb7", "/mnt/disc", "/mnt/pfs"}) {
    fs.AddDir(placeholder);
  }
  for (const char* drive : {"/mnt/usb0", "/mnt/usb1", "/mnt/usb2", "/mnt/ext1"}) {
    fs.mounts.insert(drive);
    fs.AddDir(drive);
  }
  fs.read_only.insert("/mnt/usb1");
  fs.AddDir("/mnt/ext1/PS5X360/saves");  // the extended storage already has saves
  fs.space["/mnt/usb0"] = {2000000000000ull, 500000000000ull};
  fs.space["/mnt/usb1"] = {8000000000ull, 0};
  fs.space["/mnt/usb2"] = {32000000000ull, 31000000000ull};
  fs.space["/mnt/ext1"] = {1000000000000ull, 400000000000ull};
  return fs;
}

bool Same(const std::string& got, const std::string& want) {
  if (got == want) return true;
  std::fprintf(stderr, "  got \"%s\", want \"%s\"\n", got.c_str(), want.c_str());
  return false;
}

std::vector<uint32_t> Ids(const std::vector<storage::Device>& devices) {
  std::vector<uint32_t> ids;
  for (const auto& device : devices) ids.push_back(device.id);
  return ids;
}

const char* English(const char* text) { return text; }
const char* Portuguese(const char* text) {
  static const std::map<std::string, std::string> table = {
      {"Hard drive", "Disco rígido"},
      {"Console storage", "Armazenamento do console"},
      {"Extended storage %s", "Armazenamento estendido %s"},
      {"USB drive %s", "Unidade USB %s"},
      {"%s GB free", "%s GB livres"},
      {"The saved drive (%s) is not mounted; choose another.",
       "A unidade definida nas configurações (%s) não está montada; escolha outra."},
  };
  const auto found = table.find(text);
  return found == table.end() ? text : found->second.c_str();
}
// A language file can hold anything: printf specifiers stay text, a text without %s gets the
// value after it.
const char* Hostile(const char* text) {
  if (!std::strcmp(text, "USB drive %s")) return "%n%s%x";
  if (!std::strcmp(text, "%s GB free")) return "GB";
  return text;
}

void CheckSlots() {
  std::set<uint32_t> ids;
  for (const auto& slot : storage::kSlots) {
    assert(slot.device_id > storage::kDiscDeviceId);  // not 0, the hard drive or the disc
    assert(ids.insert(slot.device_id).second);
    assert(std::char_traits<char16_t>::length(slot.guest_name) <= 27);  // X_CONTENT_DEVICE_DATA
    assert(storage::FindSlot(slot.device_id) == &slot);
  }
  assert(storage::FindSlot(storage::kInternalDeviceId) == nullptr);
  assert(storage::FindSlot(storage::kDiscDeviceId) == nullptr);
  assert(storage::FindSlot(0) == nullptr);
  assert(storage::FindSlot(99) == nullptr);
  std::puts("ok slots: fixed IDs 3-14, none of the hard drive's or the disc's, names fit");
}

void CheckScan() {
  FakeFs fs = Console();
  std::vector<std::string> skipped;
  const auto devices = storage::Scan(fs, kInternal, "/mnt", &skipped);
  // The internal folder first, then the extended storage, then the USB drives.
  assert((Ids(devices) == std::vector<uint32_t>{1, 4, 7, 9}));
  assert(devices[0].kind == storage::Kind::kInternal && devices[0].root == kInternal);
  assert(devices[0].mount.empty() && devices[0].space_known);
  assert(devices[0].free == 17179869184ull);
  assert(devices[1].kind == storage::Kind::kExtended && devices[1].name == "ext1");
  assert(devices[1].mount == "/mnt/ext1" && devices[1].root == "/mnt/ext1/PS5X360/saves");
  assert(devices[2].kind == storage::Kind::kUsb && devices[2].name == "usb0");
  assert(devices[2].root == "/mnt/usb0/PS5X360/saves");
  assert(devices[2].capacity == 2000000000000ull && devices[2].free == 500000000000ull);
  assert(devices[3].name == "usb2" && devices[3].free == 31000000000ull);
  // The read-only stick is reported, not offered.
  assert((skipped == std::vector<std::string>{"/mnt/usb1"}));
  // Probes: in the saves folder where there is one (ext1), else at the drive's top; never in a
  // placeholder (/mnt/usb3, /mnt/ext0: not mount points, they are the system's filesystem).
  assert((fs.probes == std::vector<std::string>{"/mnt/ext1/PS5X360/saves", "/mnt/usb0",
                                                "/mnt/usb1", "/mnt/usb2"}));
  // Scanning made no folder (no PS5X360 on usb0 or usb2).
  assert(!fs.IsDirectory("/mnt/usb0/PS5X360") && !fs.IsDirectory("/mnt/usb2/PS5X360"));

  // The same drives give the same IDs and folders next time (a game finds its saves again).
  FakeFs again = Console();
  const auto second = storage::Scan(again, kInternal, "/mnt");
  assert(Ids(second) == Ids(devices));
  for (size_t i = 0; i < devices.size(); ++i) assert(second[i].root == devices[i].root);

  // An ID belongs to its mount point: usb0 unplugged, usb2 keeps 9; a drive in usb3 gets 10.
  FakeFs changed = Console();
  changed.mounts.erase("/mnt/usb0");
  changed.mounts.insert("/mnt/usb3");
  const auto third = storage::Scan(changed, kInternal, "/mnt");
  assert((Ids(third) == std::vector<uint32_t>{1, 4, 9, 10}));
  assert(third[2].root == "/mnt/usb2/PS5X360/saves" && third[3].root == "/mnt/usb3/PS5X360/saves");

  // No mount root (a title that does not set one): the internal folder only, as before; no
  // probe anywhere.
  FakeFs plain = Console();
  const auto internal_only = storage::Scan(plain, kInternal, "");
  assert((Ids(internal_only) == std::vector<uint32_t>{1}) && plain.probes.empty());

  // A drive whose free space cannot be read is still offered (space unknown).
  FakeFs no_space = Console();
  no_space.space.erase("/mnt/usb2");
  const auto unknown = storage::Scan(no_space, kInternal, "/mnt");
  assert(unknown[3].id == 9 && !unknown[3].space_known);
  std::puts("ok scan: internal, ext1, usb0, usb2; usb1 read-only skipped; placeholders never "
            "probed; no folder made; IDs fixed per mount point");
}

void CheckFolders() {
  FakeFs fs = Console();
  const auto devices = storage::Scan(fs, kInternal, "/mnt");
  assert(storage::RootFor(devices, 7) == "/mnt/usb0/PS5X360/saves");
  assert(storage::RootFor(devices, 4) == "/mnt/ext1/PS5X360/saves");
  assert(storage::RootFor(devices, 9) == "/mnt/usb2/PS5X360/saves");
  // The hard drive, 0, the disc, the read-only stick and made-up IDs: the internal folder
  // (1.0.2's answer for every device).
  for (uint32_t id : {1u, 0u, 2u, 8u, 10u, 99u, 0xFFFFFFFFu}) {
    assert(storage::RootFor(devices, id) == kInternal);
  }
  assert(storage::MetadataId(devices, 7) == 7 && storage::MetadataId(devices, 4) == 4);
  for (uint32_t id : {1u, 0u, 2u, 8u, 99u}) assert(storage::MetadataId(devices, id) == 1);

  // A save's folder: <drive>/PS5X360/saves/<xuid>/<title>/<content type>/<name>, as inside.
  assert(Same(storage::PackageRoot(storage::RootFor(devices, 7), 0xE00012345678ABCDull,
                                   0x573607E4, 1)
                  .string(),
              "/mnt/usb0/PS5X360/saves/E00012345678ABCD/573607E4/00000001"));
  assert(Same(storage::PackageRoot(storage::RootFor(devices, 1), 0, 0x4D5307E6, 2).string(),
              std::string(kInternal) + "/0000000000000000/4D5307E6/00000002"));

  // Enumeration: all devices for 0, the one asked for, none for the disc or a gone drive.
  assert((storage::ListFor(devices, 0) == std::vector<uint32_t>{1, 4, 7, 9}));
  assert((storage::ListFor(devices, 7) == std::vector<uint32_t>{7}));
  assert((storage::ListFor(devices, 1) == std::vector<uint32_t>{1}));
  assert(storage::ListFor(devices, 2).empty() && storage::ListFor(devices, 8).empty());
  std::puts("ok folders: <drive>/PS5X360/saves/<xuid>/<title>/<type>; unknown IDs inside");
}

void CheckHoldsTitle() {
  FakeFs fs = Console();
  fs.AddDir("/mnt/usb0/PS5X360/saves/E00012345678ABCD/573607E4/00000001");
  fs.AddDir("/mnt/usb0/PS5X360/saves/notes/4D5307E6");  // not a profile folder
  fs.AddDir("/mnt/usb2/PS5X360/saves/0000000000000000/4D5307E6");  // common content
  const auto devices = storage::Scan(fs, kInternal, "/mnt");
  assert(storage::HoldsTitle(fs, devices[2], 0x573607E4));
  assert(!storage::HoldsTitle(fs, devices[2], 0x4D5307E6));
  assert(storage::HoldsTitle(fs, devices[3], 0x4D5307E6));
  assert(!storage::HoldsTitle(fs, devices[0], 0x573607E4));
  assert(!storage::HoldsTitle(fs, devices[1], 0x573607E4));
  std::puts("ok holds title: a profile's or the common folder's <title> folder");
}

void CheckPrompt() {
  FakeFs fs = Console();
  const auto devices = storage::Scan(fs, kInternal, "/mnt");
  const auto none = [](const storage::Device&) { return false; };
  const auto on_usb0 = [](const storage::Device& d) { return d.id == 7; };

  auto prompt = storage::Decide(devices, 0, none);
  assert((prompt.shown == std::vector<size_t>{0, 1, 2, 3}) && prompt.focus == 0 && prompt.list);
  assert(storage::Chosen(prompt, 2) == 2 && storage::Chosen(prompt, 4) == -1);  // 4: Cancel
  assert(storage::Chosen(prompt, 99) == -1);

  // The drive that keeps the game's saves is selected first.
  prompt = storage::Decide(devices, 0, on_usb0);
  assert(prompt.focus == 2);

  // 100 GB asked for: the internal folder (17.2 GB) and usb2 (31 GB) are left out.
  prompt = storage::Decide(devices, 100000000000ull, on_usb0);
  assert((prompt.shown == std::vector<size_t>{1, 2}) && prompt.focus == 1 && prompt.list);
  assert(storage::Chosen(prompt, 0) == 1 && storage::Chosen(prompt, 1) == 2);
  assert(storage::Chosen(prompt, 2) == -1);

  // More than any drive has: all offered, as before (the game reports the full drive).
  prompt = storage::Decide(devices, 10000000000000ull, none);
  assert((prompt.shown == std::vector<size_t>{0, 1, 2, 3}));

  // Exactly the free space fits.
  prompt = storage::Decide(devices, 31000000000ull, none);
  assert((prompt.shown == std::vector<size_t>{1, 2, 3}));

  // Unknown free space is offered.
  FakeFs no_space = Console();
  no_space.space.erase("/mnt/usb2");
  const auto unknown = storage::Scan(no_space, kInternal, "/mnt");
  prompt = storage::Decide(unknown, 100000000000ull, none);
  assert((prompt.shown == std::vector<size_t>{1, 2, 3}));

  // One location (no drive plugged in, or the title sets no mount root): 1.0.2's window.
  FakeFs plain = Console();
  const auto internal_only = storage::Scan(plain, kInternal, "");
  prompt = storage::Decide(internal_only, 0, none);
  assert((prompt.shown == std::vector<size_t>{0}) && prompt.focus == 0 && !prompt.list);
  assert(storage::Chosen(prompt, 0) == 0 && storage::Chosen(prompt, 1) == -1);
  // Only one drive with room: one button, the drive's own name.
  prompt = storage::Decide(devices, 450000000000ull, none);
  assert((prompt.shown == std::vector<size_t>{2}) && !prompt.list);
  std::puts("ok prompt: offered drives, space filter, first selection, Cancel, one location");
}

void CheckLabels() {
  FakeFs fs = Console();
  const auto devices = storage::Scan(fs, kInternal, "/mnt");
  const std::string dash = std::string(" ") + kDash + " ";
  // One location: 1.0.2's "Hard drive".
  assert(Same(storage::Label(devices[0], false, English), "Hard drive"));
  assert(Same(storage::Label(devices[0], false, Portuguese), "Disco rígido"));
  assert(Same(storage::Label(devices[2], false, English), "USB drive usb0"));
  // Several: each with its free space.
  assert(Same(storage::Label(devices[0], true, English), "Console storage" + dash + "17.2 GB free"));
  assert(Same(storage::Label(devices[1], true, English),
              "Extended storage ext1" + dash + "400 GB free"));
  assert(Same(storage::Label(devices[2], true, English), "USB drive usb0" + dash + "500 GB free"));
  assert(Same(storage::Label(devices[3], true, English), "USB drive usb2" + dash + "31.0 GB free"));
  assert(Same(storage::Label(devices[2], true, Portuguese),
              "Unidade USB usb0" + dash + "500 GB livres"));
  assert(Same(storage::Label(devices[1], true, Portuguese),
              "Armazenamento estendido ext1" + dash + "400 GB livres"));
  // A language file's text is never a printf format.
  assert(Same(storage::Label(devices[2], true, Hostile), "%nusb0%x" + dash + "GB 500"));
  // Unknown space: no figure.
  FakeFs no_space = Console();
  no_space.space.erase("/mnt/usb2");
  const auto unknown = storage::Scan(no_space, kInternal, "/mnt");
  assert(Same(storage::Label(unknown[3], true, English), "USB drive usb2"));
  assert(Same(storage::Gigabytes(0), "0.0") && Same(storage::Gigabytes(16000000000ull), "16.0"));
  assert(Same(storage::Gigabytes(100000000000ull), "100"));
  assert(Same(storage::Describe({devices[0], devices[2]}),
              std::string("1 ") + kInternal + " (17.2 GB free), 7 /mnt/usb0/PS5X360/saves (500 GB free)"));
  std::puts("ok labels: Hard drive alone, drive names and free space, translated, never a format");
}

void CheckPosix() {
  storage::PosixFs fs;
  const stdfs::path base = stdfs::temp_directory_path() / "check-save-devices";
  stdfs::remove_all(base);
  stdfs::create_directories(base / "internal");
  stdfs::create_directories(base / "mnt" / "usb0");
  stdfs::create_directories(base / "mnt" / "ext1" / "PS5X360" / "saves");
  std::ofstream(base / "mnt" / "file.txt") << "x";

  assert(fs.IsMountPoint("/"));
  if (stdfs::exists("/proc/self")) assert(fs.IsMountPoint("/proc"));
  assert(!fs.IsMountPoint(base / "mnt" / "usb0"));  // a folder, not a mount
  assert(!fs.IsMountPoint(base / "mnt" / "file.txt"));
  assert(!fs.IsMountPoint(base / "missing"));
  assert(fs.IsDirectory(base / "mnt") && !fs.IsDirectory(base / "mnt" / "file.txt"));

  assert(fs.ProbeWritable(base / "mnt" / "usb0"));
  assert(stdfs::is_empty(base / "mnt" / "usb0"));  // the probe is removed again
  assert(!fs.ProbeWritable(base / "missing"));
  if (::geteuid() != 0) {  // root writes anywhere
    stdfs::create_directories(base / "read-only");
    ::chmod((base / "read-only").c_str(), 0555);
    assert(!fs.ProbeWritable(base / "read-only"));
    ::chmod((base / "read-only").c_str(), 0755);
  }
  uint64_t capacity = 0, free = 0;
  assert(fs.Space(base, capacity, free) && capacity > 0 && free <= capacity);
  assert(!fs.Space(base / "missing", capacity, free));

  auto names = fs.ListDirectories(base / "mnt");
  std::sort(names.begin(), names.end());
  assert((names == std::vector<std::string>{"ext1", "usb0"}));  // not file.txt
  assert(fs.ListDirectories(base / "missing").empty());

  // Folders that are not mount points are not drives: only the internal folder, and nothing
  // written under them.
  std::vector<std::string> skipped;
  const auto devices = storage::Scan(fs, base / "internal", base / "mnt", &skipped);
  assert((Ids(devices) == std::vector<uint32_t>{1}) && skipped.empty());
  assert(devices[0].space_known);
  assert(stdfs::is_empty(base / "mnt" / "usb0"));
  assert(storage::HoldsTitle(fs, devices[0], 0x573607E4) == false);
  stdfs::create_directories(base / "internal" / "E00012345678ABCD" / "573607E4");
  assert(storage::HoldsTitle(fs, devices[0], 0x573607E4));
  stdfs::remove_all(base);
  std::puts("ok posix: mount points by device, probe removed, read-only refused, plain folders "
            "are not drives");
}

// The save drive setting: its value is the index of a choice, one mount point each.
void CheckSettingValues() {
  assert(storage::kSaveDriveChoices == 14);
  assert(storage::SaveDriveDeviceId(0) == 0);  // ask every time (the default)
  assert(storage::SaveDriveDeviceId(1) == storage::kInternalDeviceId);
  for (int choice = 2; choice < storage::kSaveDriveChoices; ++choice) {
    assert(storage::SaveDriveDeviceId(choice) == storage::kSlots[choice - 2].device_id);
  }
  assert(storage::SaveDriveDeviceId(2) == 3 && storage::SaveDriveDeviceId(5) == 6);   // ext0, ext3
  assert(storage::SaveDriveDeviceId(6) == 7 && storage::SaveDriveDeviceId(13) == 14);  // usb0, usb7
  for (int bad : {-1, 14, 99}) assert(storage::SaveDriveDeviceId(bad) == 0);  // out of range: ask
  std::puts("ok setting values: 0 ask, 1 console, 2-5 ext0-ext3, 6-13 usb0-usb7, else ask");
}

void CheckSavedDrive() {
  FakeFs fs = Console();
  std::vector<std::string> unwritable;
  const auto devices = storage::Scan(fs, kInternal, "/mnt", &unwritable);
  const auto check = [&](uint32_t id, uint64_t requested = 0) {
    return storage::CheckSaved(devices, unwritable, id, requested, "/mnt");
  };
  // No setting (ask every time): the window, no notice.
  auto saved = check(0);
  assert(saved.state == storage::Saved::kAsk && storage::Notice(saved, English).empty());
  // A valid setting: that drive without the window.
  saved = check(7);  // usb0
  assert(saved.state == storage::Saved::kUse && saved.index == 2 && saved.mount == "/mnt/usb0");
  assert(devices[saved.index].root == "/mnt/usb0/PS5X360/saves");
  saved = check(4);  // ext1, its saves folder already there
  assert(saved.state == storage::Saved::kUse && saved.index == 1);
  saved = check(1);  // the console's storage
  assert(saved.state == storage::Saved::kUse && saved.index == 0 && saved.mount == kInternal);
  // A stale setting: the drive was unplugged (usb0 gone), or never there (usb3, a placeholder).
  FakeFs unplugged = Console();
  unplugged.mounts.erase("/mnt/usb0");
  std::vector<std::string> unplugged_unwritable;
  const auto after = storage::Scan(unplugged, kInternal, "/mnt", &unplugged_unwritable);
  saved = storage::CheckSaved(after, unplugged_unwritable, 7, 0, "/mnt");
  assert(saved.state == storage::Saved::kNotMounted && saved.mount == "/mnt/usb0");
  assert(Same(storage::Notice(saved, English),
              "The saved drive (/mnt/usb0) is not mounted; choose another."));
  assert(Same(storage::Notice(saved, Portuguese),
              "A unidade definida nas configurações (/mnt/usb0) não está montada; escolha outra."));
  saved = check(10);  // usb3
  assert(saved.state == storage::Saved::kNotMounted && saved.mount == "/mnt/usb3");
  // Mounted but read-only (the installer stick in usb1).
  saved = check(8);
  assert(saved.state == storage::Saved::kReadOnly && saved.mount == "/mnt/usb1");
  assert(Same(storage::Notice(saved, English),
              "The saved drive (/mnt/usb1) cannot be written; choose another."));
  // Less free space than the game asks for (usb2: 31 GB).
  saved = check(9, 100000000000ull);
  assert(saved.state == storage::Saved::kNoSpace && saved.index == 3 && saved.mount == "/mnt/usb2");
  assert(Same(storage::Notice(saved, English),
              "The saved drive (/mnt/usb2) does not have enough free space; choose another."));
  assert(check(9, 31000000000ull).state == storage::Saved::kUse);  // exactly enough
  // Unknown space counts as enough, as in the window.
  FakeFs no_space = Console();
  no_space.space.erase("/mnt/usb2");
  const auto unknown = storage::Scan(no_space, kInternal, "/mnt");
  assert(storage::CheckSaved(unknown, {}, 9, 100000000000ull, "/mnt").state == storage::Saved::kUse);
  // IDs of no device: as if no drive were saved (the disc, 99).
  assert(check(2).state == storage::Saved::kAsk && check(99).state == storage::Saved::kAsk);
  // A title without a mount root: a saved drive is reported as not mounted, never replaced.
  FakeFs plain = Console();
  const auto internal_only = storage::Scan(plain, kInternal, "");
  saved = storage::CheckSaved(internal_only, {}, 7, 0, "");
  assert(saved.state == storage::Saved::kNotMounted && saved.mount == "/mnt/usb0");
  std::puts("ok saved drive: none asks; valid used without the window; unplugged, placeholder, "
            "read-only and full drives ask with a notice; unknown IDs ask");
}

std::string ReadTreeFile(const char* rel) {
  std::ifstream file(rel, std::ios::binary);
  assert(file && "run from the assembled tree's root");
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}

std::string DriveChoice(const storage::Slot& slot) {
  return std::string(slot.kind == storage::Kind::kExtended ? "Extended storage " : "USB drive ") +
         slot.mount;
}

// The option's choices in launcher.cpp, in SaveDriveDeviceId's order.
void CheckOption() {
  const std::string source = ReadTreeFile("src/launcher.cpp");
  std::string choices = "{\"Ask every time\", \"Console storage\"";
  for (const auto& slot : storage::kSlots) choices += ", \"" + DriveChoice(slot) + "\"";
  choices += "}";
  const size_t option =
      source.find("{\"save_drive\", &S::save_drive, 4, W::live, 0, true, \"Save drive\",");
  assert(option != std::string::npos && "save_drive: System, live, ask by default, per game");
  if (source.find(choices, option) == std::string::npos) {
    std::fprintf(stderr, "  launcher.cpp lacks the choices %s\n", choices.c_str());
    assert(false);
  }
  assert(source.find("cvars::saved_storage_device = "
                     "xe::kernel::xam::storage::SaveDriveDeviceId(save_drive);") != std::string::npos);
  std::puts("ok option: save_drive in System, per game, live, Ask every time by default, choices "
            "in slot order, applied to the core");
}

// Language files keyed by English (fix 25, tools/source-1.0.2/english/lang): every new text of
// fix 27 is translated in each language.
void CheckLanguageFiles(const char* folder) {
  std::vector<std::string> keys = {
      "Console storage", "Extended storage %s", "USB drive %s", "%s GB free",
      "Each drive keeps its saves in its own PS5X360/saves folder.",
      "The saved drive (%s) is not mounted; choose another.",
      "The saved drive (%s) cannot be written; choose another.",
      "The saved drive (%s) does not have enough free space; choose another.",
      "Save drive", "Ask every time"};
  for (const auto& slot : storage::kSlots) keys.push_back(DriveChoice(slot));
  const char* codes[] = {"pt-BR", "pt", "es", "es-419", "de", "fr", "it", "ja",
                         "ko", "nl", "pl", "ru", "tr", "zh-Hans", "zh-Hant", "en"};
  for (const char* code : codes) {
    std::ifstream file(std::string(folder) + "/" + code + ".txt", std::ios::binary);
    assert(file);
    std::stringstream text;
    text << file.rdbuf();
    const std::string data = "\n" + text.str();
    for (const auto& key : keys) {
      std::string encoded;
      for (char c : key) encoded += c == ' ' ? std::string("\\s") : std::string(1, c);
      if (data.find("\n" + encoded + " = ") == std::string::npos) {
        std::fprintf(stderr, "  %s.txt lacks \"%s\"\n", code, key.c_str());
        assert(false);
      }
    }
  }
  std::printf("ok language files: %zu texts of fix 27 in 16 files\n", keys.size());
}

void CheckTranslations() {
  const std::string source = ReadTreeFile("src/canary_game_main.cpp");
  for (const char* english : storage::kUiTexts) {
    const std::string entry = std::string("{\"") + english + "\", \"";
    const std::string entry_wrapped = std::string("{\"") + english + "\",";
    if (source.find(entry) == std::string::npos && source.find(entry_wrapped) == std::string::npos) {
      std::fprintf(stderr, "  CoreUiText lacks \"%s\"\n", english);
      assert(false);
    }
  }
  assert(source.find("xe::kernel::xam::external_storage_root = \"/mnt\";") != std::string::npos);
  std::puts("ok translations: every text of the window is in CoreUiText; drives under /mnt");
}
}  // namespace

int main(int argc, char** argv) {
  CheckSlots();
  CheckScan();
  CheckFolders();
  CheckHoldsTitle();
  CheckPrompt();
  CheckLabels();
  CheckPosix();
  CheckSettingValues();
  CheckSavedDrive();
  CheckTranslations();
  CheckOption();
  if (argc > 1) CheckLanguageFiles(argv[1]);
  std::puts("check-save-devices: all passed");
  return 0;
}
