// SPDX-License-Identifier: MIT
// Fix 27 (tools/source-1.0.2/fixes/27-save-device-choice.patch): when a game asks where to save
// (XamShowDeviceSelectorUI), every writable drive the console has mounted is offered besides the
// internal folder, each as a storage device of its own with its saves in <drive>/PS5X360/saves
// (or below a writable folder of the drive). A "Save drive" setting names one drive to use
// without asking. The decisions live in the core's header-only xenia/kernel/xam/storage_devices.h,
// which the content manager, the device exports and the window call; this check drives them with
// a fake filesystem laid out as the console's /mnt was seen over FTP and in candidate 13's log
// (usb0, usb1 and usb2 mounted, usb2 possibly a second partition; ext1 the M.2/extended storage
// whose top the app cannot write but whose xbox360 folder it can; usb3-usb7 and ext0 empty
// placeholders; every path answering 16 GiB free of 64 GiB inside the sandbox), then the real
// PosixFs on the host. Run from the assembled tree's root: it also checks that the title
// translates every text (CoreUiText, the option in launcher.cpp, the language files).
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
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace stdfs = std::filesystem;
namespace storage = xe::kernel::xam::storage;
using Why = storage::Skipped::Why;

constexpr const char* kInternal = "/data/homebrew/PPSA50011/saves";
constexpr const char* kDash = "\xE2\x80\x93";  // the label's en dash
constexpr uint64_t kSandboxCapacity = 68719476736ull;  // what candidate 13 logged for every path
constexpr uint64_t kSandboxFree = 17179869184ull;

// The filesystem as Scan sees it. Directories exist only when listed; a probe fails in a
// read-only directory; Info answers for the paths given one. Nothing is ever created.
struct FakeFs {
  std::set<std::string> mounts;
  std::set<std::string> dirs;
  std::set<std::string> read_only;
  std::map<std::string, storage::FsInfo> info;
  std::vector<std::string> probes;

  bool IsMountPoint(const stdfs::path& path) { return mounts.count(path.string()) != 0; }
  bool IsDirectory(const stdfs::path& path) {
    return dirs.count(path.string()) != 0 || mounts.count(path.string()) != 0;
  }
  bool ProbeWritable(const stdfs::path& path) {
    probes.push_back(path.string());
    if (read_only.count(path.string())) return false;
    return IsDirectory(path);
  }
  storage::FsInfo Info(const stdfs::path& path) {
    const auto found = info.find(path.string());
    return found == info.end() ? storage::FsInfo{} : found->second;
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
  void Mount(const std::string& mount, uint64_t dev, const std::string& source, uint64_t capacity,
             uint64_t free) {
    mounts.insert(mount);
    AddDir(mount);
    info[mount] = {true, dev, "", source, "exfatfs", true, capacity, free};
  }
};

// The console of 2026-11-19 with the figures a drive really has.
FakeFs Console() {
  FakeFs fs;
  fs.AddDir(kInternal);
  fs.info[kInternal] = {true, 100, "", "/dev/ssd0.user", "ufs", true, kSandboxCapacity, kSandboxFree};
  for (const char* placeholder : {"/mnt/ext0", "/mnt/usb3", "/mnt/usb4", "/mnt/usb5", "/mnt/usb6",
                                  "/mnt/usb7", "/mnt/disc", "/mnt/pfs"}) {
    fs.AddDir(placeholder);
  }
  fs.Mount("/mnt/usb0", 201, "/dev/da0s1", 2000000000000ull, 500000000000ull);
  fs.Mount("/mnt/usb1", 202, "/dev/da1s1", 256000000000ull, 120000000000ull);
  fs.Mount("/mnt/usb2", 203, "/dev/da1s2", 32000000000ull, 31000000000ull);  // da1's 2nd partition
  fs.Mount("/mnt/ext1", 301, "/dev/nvme0", 1000000000000ull, 400000000000ull);
  fs.info["/mnt/ext1"].type = "ufs";
  // ext1: the system's top; its homebrew folder root's (drwxr-xr-x); xbox360 open to all.
  fs.read_only.insert("/mnt/ext1");
  fs.AddDir("/mnt/ext1/homebrew");
  fs.read_only.insert("/mnt/ext1/homebrew");
  fs.AddDir("/mnt/ext1/xbox360");
  fs.AddDir("/mnt/ext1/user");
  return fs;
}

// The same console inside the sandbox of candidate 13: every path gives the same figures.
FakeFs Sandbox() {
  FakeFs fs = Console();
  for (auto& [path, info] : fs.info) {
    info.capacity = kSandboxCapacity;
    info.free = kSandboxFree;
  }
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
      {"Internal storage", "Armazenamento interno"},
      {"M.2 SSD / extended storage", "SSD M.2 / armazenamento estendido"},
      {"USB %s", "USB %s"},
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
  if (!std::strcmp(text, "USB %s")) return "%n%s%x";
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
  assert(storage::FindSlot(0) == nullptr && storage::FindSlot(99) == nullptr);
  assert(storage::DiskOf("/dev/da1s2") == "da1" && storage::DiskOf("/dev/da0p1") == "da0");
  assert(storage::DiskOf("/dev/da0") == "da0" && storage::DiskOf("/dev/nvme0") == "nvme0");
  assert(storage::DiskOf("").empty() && storage::DiskOf("tmpfs").empty());
  assert(storage::DiskOf("/dev/ssd").empty() && storage::DiskOf("/dev/0").empty());
  std::puts("ok slots: fixed IDs 3-14, none of the hard drive's or the disc's, names fit; disks");
}

void CheckScan() {
  FakeFs fs = Console();
  std::vector<storage::Skipped> skipped;
  const auto devices = storage::Scan(fs, kInternal, "/mnt", &skipped);
  // The internal folder first, then the extended storage, then the USB drives.
  assert((Ids(devices) == std::vector<uint32_t>{1, 4, 7, 8, 9}));
  assert(devices[0].kind == storage::Kind::kInternal && devices[0].root == kInternal);
  // ext1: its top and homebrew are not writable, xbox360 is.
  assert(devices[1].kind == storage::Kind::kExtended && devices[1].name == "ext1");
  assert(devices[1].mount == "/mnt/ext1" && devices[1].root == "/mnt/ext1/xbox360/PS5X360/saves");
  assert(devices[2].root == "/mnt/usb0/PS5X360/saves" && devices[2].free == 500000000000ull);
  assert(devices[2].space_known && devices[2].capacity == 2000000000000ull);
  // USB numbers by disk: usb0 is da0, usb1 and usb2 are partitions of da1.
  assert(devices[2].number == 1 && devices[3].number == 2 && devices[4].number == 2);
  assert(skipped.empty());
  // Probes: ext1's top, homebrew, xbox360; each USB drive's top. Never a placeholder or a
  // folder the drive does not have.
  assert((fs.probes == std::vector<std::string>{"/mnt/ext1", "/mnt/ext1/homebrew", "/mnt/ext1/xbox360",
                                                "/mnt/usb0", "/mnt/usb1", "/mnt/usb2"}));
  assert(!fs.IsDirectory("/mnt/usb0/PS5X360") && !fs.IsDirectory("/mnt/ext1/xbox360/PS5X360"));

  // A saves folder already there is kept (the same game finds its saves), probed in itself.
  FakeFs again = Console();
  again.AddDir("/mnt/ext1/xbox360/PS5X360/saves");
  again.AddDir("/mnt/usb0/PS5X360/saves");
  const auto second = storage::Scan(again, kInternal, "/mnt");
  assert(Ids(second) == Ids(devices));
  for (size_t i = 0; i < devices.size(); ++i) assert(second[i].root == devices[i].root);
  assert(again.probes[0] == "/mnt/ext1/PS5X360/saves" || again.probes[0] == "/mnt/ext1/xbox360/PS5X360/saves");
  // ...and wins over a base that became writable since.
  FakeFs opened = Console();
  opened.read_only.clear();
  opened.AddDir("/mnt/ext1/xbox360/PS5X360/saves");
  assert(storage::Scan(opened, kInternal, "/mnt")[1].root == "/mnt/ext1/xbox360/PS5X360/saves");
  // A writable top is used first.
  FakeFs root_ok = Console();
  root_ok.read_only.clear();
  assert(storage::Scan(root_ok, kInternal, "/mnt")[1].root == "/mnt/ext1/PS5X360/saves");

  // A mount that is the same filesystem as an offered one (same st_dev) is offered once.
  FakeFs twice = Console();
  twice.Mount("/mnt/usb3", 201, "/dev/da0s1", 2000000000000ull, 500000000000ull);  // usb0 again
  twice.Mount("/mnt/ext0", 100, "", 1, 1);  // the internal folder's filesystem
  skipped.clear();
  const auto once = storage::Scan(twice, kInternal, "/mnt", &skipped);
  assert((Ids(once) == std::vector<uint32_t>{1, 4, 7, 8, 9}));
  assert(skipped.size() == 2);
  assert(skipped[0].mount == "/mnt/ext0" && skipped[0].id == 3 && skipped[0].why == Why::kSameFilesystem);
  assert(skipped[0].same_as == 1 && skipped[0].detail == kInternal);
  assert(skipped[1].mount == "/mnt/usb3" && skipped[1].id == 10 && skipped[1].same_as == 7);
  assert(skipped[1].detail == "/mnt/usb0");
  // Different st_dev but the same statfs source and fsid: still two (statfs is not trusted for
  // identity; the sandbox could give every path the same answer).
  FakeFs echo = Console();
  echo.info["/mnt/usb2"].source = "/dev/da1s1";
  assert(storage::Scan(echo, kInternal, "/mnt").size() == 5);

  // Mounted but not writable anywhere: skipped with the folders tried.
  FakeFs stick = Console();
  stick.read_only.insert("/mnt/usb1");
  skipped.clear();
  const auto without = storage::Scan(stick, kInternal, "/mnt", &skipped);
  assert((Ids(without) == std::vector<uint32_t>{1, 4, 7, 9}));
  assert(skipped.size() == 1 && skipped[0].mount == "/mnt/usb1" && skipped[0].why == Why::kNotWritable);
  assert(skipped[0].detail == "/mnt/usb1");
  FakeFs ext_closed = Console();
  ext_closed.read_only.insert("/mnt/ext1/xbox360");
  skipped.clear();
  storage::Scan(ext_closed, kInternal, "/mnt", &skipped);
  assert(skipped.size() == 1 && skipped[0].id == 4);
  assert(Same(skipped[0].detail, "/mnt/ext1, /mnt/ext1/homebrew, /mnt/ext1/xbox360"));

  // An ID belongs to its mount point: usb0 unplugged, the others keep theirs and usb1 is USB 1.
  FakeFs changed = Console();
  changed.mounts.erase("/mnt/usb0");
  const auto third = storage::Scan(changed, kInternal, "/mnt");
  assert((Ids(third) == std::vector<uint32_t>{1, 4, 8, 9}) && third[2].number == 1 && third[3].number == 1);

  // No mount root: the internal folder only, as before; no probe anywhere.
  FakeFs plain = Console();
  assert((Ids(storage::Scan(plain, kInternal, "")) == std::vector<uint32_t>{1}) && plain.probes.empty());
  std::puts("ok scan: internal, ext1 (in its writable xbox360 folder), usb0, usb1, usb2; same st_dev "
            "offered once; not writable skipped with the folders tried; IDs per mount point");
}

void CheckSpace() {
  // Inside the sandbox every path gave 16 GiB free of 64 GiB: no figure is believed.
  FakeFs fs = Sandbox();
  const auto devices = storage::Scan(fs, kInternal, "/mnt");
  for (const auto& device : devices) assert(!device.space_known && device.fs.space_ok);
  const auto prompt = storage::Decide(devices, 100000000000ull, [](const storage::Device&) { return false; });
  assert(prompt.shown.size() == devices.size());  // unknown space keeps every drive offered
  const std::string text = storage::Describe({devices[2]});
  assert(text.find("free space not reported") != std::string::npos);
  assert(text.find("[/dev/da0s1 exfatfs]") != std::string::npos);
  // Only the drives that share figures lose them.
  FakeFs pair = Console();
  pair.info["/mnt/usb2"].capacity = pair.info["/mnt/usb1"].capacity;
  pair.info["/mnt/usb2"].free = pair.info["/mnt/usb1"].free;
  const auto some = storage::Scan(pair, kInternal, "/mnt");
  assert(some[0].space_known && some[1].space_known && some[2].space_known);
  assert(!some[3].space_known && !some[4].space_known);
  // No statfs answer: unknown.
  FakeFs none = Console();
  none.info["/mnt/usb0"].space_ok = false;
  const auto missing = storage::Scan(none, kInternal, "/mnt");
  assert(!missing[2].space_known);
  assert(storage::Describe({missing[2]}).find("free space unknown") != std::string::npos);
  // The real figures, each its own.
  const auto real = storage::Scan(*new FakeFs(Console()), kInternal, "/mnt");
  for (const auto& device : real) assert(device.space_known);
  assert(real[1].free == 400000000000ull && real[4].free == 31000000000ull);
  // Sources all alike: USB drives numbered one by one.
  FakeFs alike = Console();
  for (auto& [path, info] : alike.info) info.source = "/dev/lvd0";
  const auto numbered = storage::Scan(alike, kInternal, "/mnt");
  assert(numbered[2].number == 1 && numbered[3].number == 2 && numbered[4].number == 3);
  std::puts("ok space: the sandbox's shared figures not believed, real ones per drive; partitions "
            "share a USB number only when sources differ");
}

void CheckFolders() {
  FakeFs fs = Console();
  const auto devices = storage::Scan(fs, kInternal, "/mnt");
  assert(storage::RootFor(devices, 7) == "/mnt/usb0/PS5X360/saves");
  assert(storage::RootFor(devices, 4) == "/mnt/ext1/xbox360/PS5X360/saves");
  assert(storage::RootFor(devices, 9) == "/mnt/usb2/PS5X360/saves");
  for (uint32_t id : {1u, 0u, 2u, 10u, 99u, 0xFFFFFFFFu}) assert(storage::RootFor(devices, id) == kInternal);
  assert(storage::MetadataId(devices, 7) == 7 && storage::MetadataId(devices, 4) == 4);
  for (uint32_t id : {1u, 0u, 2u, 10u, 99u}) assert(storage::MetadataId(devices, id) == 1);
  assert(Same(storage::PackageRoot(storage::RootFor(devices, 7), 0xE00012345678ABCDull, 0x573607E4, 1).string(),
              "/mnt/usb0/PS5X360/saves/E00012345678ABCD/573607E4/00000001"));
  assert(Same(storage::PackageRoot(storage::RootFor(devices, 1), 0, 0x4D5307E6, 2).string(),
              std::string(kInternal) + "/0000000000000000/4D5307E6/00000002"));
  assert((storage::ListFor(devices, 0) == std::vector<uint32_t>{1, 4, 7, 8, 9}));
  assert((storage::ListFor(devices, 7) == std::vector<uint32_t>{7}));
  assert(storage::ListFor(devices, 2).empty() && storage::ListFor(devices, 10).empty());
  std::puts("ok folders: <saves root>/<xuid>/<title>/<type>; unknown IDs inside");
}

void CheckHoldsTitle() {
  FakeFs fs = Console();
  fs.AddDir("/mnt/usb0/PS5X360/saves/E00012345678ABCD/573607E4/00000001");
  fs.AddDir("/mnt/usb0/PS5X360/saves/notes/4D5307E6");  // not a profile folder
  fs.AddDir("/mnt/usb2/PS5X360/saves/0000000000000000/4D5307E6");  // common content
  const auto devices = storage::Scan(fs, kInternal, "/mnt");
  assert(storage::HoldsTitle(fs, devices[2], 0x573607E4));
  assert(!storage::HoldsTitle(fs, devices[2], 0x4D5307E6));
  assert(storage::HoldsTitle(fs, devices[4], 0x4D5307E6));
  assert(!storage::HoldsTitle(fs, devices[0], 0x573607E4));
  std::puts("ok holds title: a profile's or the common folder's <title> folder");
}

void CheckPrompt() {
  FakeFs fs = Console();
  const auto devices = storage::Scan(fs, kInternal, "/mnt");
  const auto none = [](const storage::Device&) { return false; };
  auto prompt = storage::Decide(devices, 0, none);
  assert((prompt.shown == std::vector<size_t>{0, 1, 2, 3, 4}) && prompt.focus == 0 && prompt.list);
  assert(storage::Chosen(prompt, 2) == 2 && storage::Chosen(prompt, 5) == -1);
  prompt = storage::Decide(devices, 0, [](const storage::Device& d) { return d.id == 7; });
  assert(prompt.focus == 2);
  // 100 GB asked for: the internal folder (17.2 GB) and usb2 (31 GB) are left out.
  prompt = storage::Decide(devices, 100000000000ull, none);
  assert((prompt.shown == std::vector<size_t>{1, 2, 3}));
  prompt = storage::Decide(devices, 10000000000000ull, none);  // more than any: all
  assert(prompt.shown.size() == 5);
  FakeFs plain = Console();
  prompt = storage::Decide(storage::Scan(plain, kInternal, ""), 0, none);
  assert((prompt.shown == std::vector<size_t>{0}) && !prompt.list);
  std::puts("ok prompt: offered drives, space filter, first selection, Cancel, one location");
}

void CheckLabels() {
  FakeFs fs = Console();
  const auto devices = storage::Scan(fs, kInternal, "/mnt");
  const std::string dash = std::string(" ") + kDash + " ";
  assert(Same(storage::Label(devices[0], false, English), "Hard drive"));
  assert(Same(storage::Label(devices[0], true, English), "Internal storage" + dash + "17.2 GB free"));
  assert(Same(storage::Label(devices[1], true, English),
              "M.2 SSD / extended storage (ext1)" + dash + "400 GB free"));
  assert(Same(storage::Label(devices[2], true, English), "USB 1 (usb0)" + dash + "500 GB free"));
  assert(Same(storage::Label(devices[3], true, English), "USB 2 (usb1)" + dash + "120 GB free"));
  assert(Same(storage::Label(devices[4], true, English), "USB 2 (usb2)" + dash + "31.0 GB free"));
  assert(Same(storage::Label(devices[2], false, English), "USB 1 (usb0)"));
  assert(Same(storage::Label(devices[1], true, Portuguese),
              "SSD M.2 / armazenamento estendido (ext1)" + dash + "400 GB livres"));
  assert(Same(storage::Label(devices[0], true, Portuguese), "Armazenamento interno" + dash + "17.2 GB livres"));
  assert(Same(storage::Label(devices[2], true, Hostile), "%n1%x (usb0)" + dash + "GB 500"));
  // The sandbox's figures: names only.
  FakeFs sandbox = Sandbox();
  const auto hidden = storage::Scan(sandbox, kInternal, "/mnt");
  assert(Same(storage::Label(hidden[0], true, English), "Internal storage"));
  assert(Same(storage::Label(hidden[2], true, English), "USB 1 (usb0)"));
  assert(Same(storage::Gigabytes(0), "0.0") && Same(storage::Gigabytes(100000000000ull), "100"));
  std::puts("ok labels: Hard drive alone; Internal storage, M.2 SSD / extended storage, USB n (port), "
            "free space when believed; translated, never a format");
}

void CheckPosix() {
  storage::PosixFs fs;
  const stdfs::path base = stdfs::temp_directory_path() / "check-save-devices";
  stdfs::remove_all(base);
  stdfs::create_directories(base / "internal");
  stdfs::create_directories(base / "mnt" / "usb0");
  std::ofstream(base / "mnt" / "file.txt") << "x";
  assert(fs.IsMountPoint("/"));
  if (stdfs::exists("/proc/self")) assert(fs.IsMountPoint("/proc"));
  assert(!fs.IsMountPoint(base / "mnt" / "usb0") && !fs.IsMountPoint(base / "missing"));
  assert(fs.ProbeWritable(base / "mnt" / "usb0") && stdfs::is_empty(base / "mnt" / "usb0"));
  assert(!fs.ProbeWritable(base / "missing"));
  if (::geteuid() != 0) {
    stdfs::create_directories(base / "read-only");
    ::chmod((base / "read-only").c_str(), 0555);
    assert(!fs.ProbeWritable(base / "read-only"));
    ::chmod((base / "read-only").c_str(), 0755);
  }
  const auto info = fs.Info(base);
  assert(info.ok && info.space_ok && info.capacity > 0 && info.free <= info.capacity);
  struct stat self {};
  ::stat(base.c_str(), &self);
  assert(info.dev == static_cast<uint64_t>(self.st_dev));
  assert(fs.Info(base / "internal").dev == info.dev);  // one filesystem
  assert(!fs.Info(base / "missing").ok);
  if (stdfs::exists("/proc/self")) assert(fs.Info("/proc").dev != info.dev || fs.Info("/").dev != info.dev);
  auto names = fs.ListDirectories(base / "mnt");
  std::sort(names.begin(), names.end());
  assert((names == std::vector<std::string>{"usb0"}));
  std::vector<storage::Skipped> skipped;
  const auto devices = storage::Scan(fs, base / "internal", base / "mnt", &skipped);
  assert((Ids(devices) == std::vector<uint32_t>{1}) && skipped.empty() && devices[0].space_known);
  assert(stdfs::is_empty(base / "mnt" / "usb0"));
  stdfs::remove_all(base);
  std::puts("ok posix: mount points by device, Info's st_dev and space, probe removed, read-only refused");
}

// The real filesystem under odd mount points: a missing one, one the process may not read, an
// empty one, a file, a dangling symlink and a symlink loop. Nothing throws, nothing is offered,
// nothing is written, and each step is traced before it is taken.
void CheckPosixOddMounts() {
  storage::PosixFs fs;
  const stdfs::path base = stdfs::temp_directory_path() / "check-save-devices-odd";
  stdfs::remove_all(base);
  stdfs::create_directories(base / "internal");
  stdfs::create_directories(base / "mnt" / "usb1");
  stdfs::create_directories(base / "mnt" / "usb2");                  // empty
  std::ofstream(base / "mnt" / "usb3") << "not a folder";           // a file
  stdfs::create_symlink(base / "nowhere", base / "mnt" / "usb4");   // dangling
  stdfs::create_symlink(base / "mnt" / "usb5", base / "mnt" / "usb5");  // a loop
  ::chmod((base / "mnt" / "usb1").c_str(), 0);                      // unreadable
  for (const char* name : {"usb0", "usb1", "usb2", "usb3", "usb4", "usb5"}) {
    const auto mount = base / "mnt" / name;
    assert(!fs.IsMountPoint(mount));
    const auto info = fs.Info(mount);  // never throws
    if (!std::strcmp(name, "usb0") || !std::strcmp(name, "usb4") || !std::strcmp(name, "usb5")) {
      assert(!info.ok);
    }
    fs.ListDirectories(mount);
    fs.IsDirectory(mount);
  }
  std::vector<std::string> steps;
  std::vector<storage::Skipped> skipped;
  const auto devices = storage::Scan(fs, base / "internal", base / "mnt", &skipped,
                                     [&steps](const std::string& step) { steps.push_back(step); });
  assert((Ids(devices) == std::vector<uint32_t>{1}) && skipped.empty());
  assert(steps.size() == 1 + storage::kSlotCount);  // the internal folder, then each slot
  assert(steps[0] == "info " + (base / "internal").string());
  assert(steps[1] == "mount point? " + (base / "mnt" / "ext0").string());
  assert(steps[6] == "mount point? " + (base / "mnt" / "usb1").string());
  ::chmod((base / "mnt" / "usb1").c_str(), 0755);
  assert(stdfs::is_empty(base / "mnt" / "usb1") && stdfs::is_empty(base / "mnt" / "usb2"));
  stdfs::remove_all(base);
  std::puts("ok odd mounts: missing, unreadable, empty, file, dangling and looping symlinks are not "
            "drives; no throw, no write; each step traced first");
}

// A filesystem whose answers throw: the slot is left out, the rest is scanned.
struct ThrowingFs : FakeFs {
  std::string throw_at;
  storage::FsInfo Info(const stdfs::path& path) {
    if (path.string() == throw_at) throw std::runtime_error("statvfs blew up");
    return FakeFs::Info(path);
  }
};

void CheckThrowing() {
  ThrowingFs fs;
  static_cast<FakeFs&>(fs) = Console();
  fs.throw_at = "/mnt/usb1";
  std::vector<storage::Skipped> skipped;
  std::vector<std::string> steps;
  const auto devices = storage::Scan(fs, kInternal, "/mnt", &skipped,
                                     [&steps](const std::string& step) { steps.push_back(step); });
  assert((Ids(devices) == std::vector<uint32_t>{1, 4, 7, 9}));
  assert(skipped.size() == 1 && skipped[0].mount == "/mnt/usb1" && skipped[0].id == 8);
  assert(skipped[0].why == Why::kError);
  // The last step before the throw names it.
  const auto at = std::find(steps.begin(), steps.end(), "info /mnt/usb1");
  assert(at != steps.end() && *(at + 1) == "mount point? /mnt/usb2");
  // The internal folder's answer throwing: it is still offered, its space unknown.
  ThrowingFs inner;
  static_cast<FakeFs&>(inner) = Console();
  inner.throw_at = kInternal;
  const auto still = storage::Scan(inner, kInternal, "/mnt");
  assert(still[0].id == 1 && still[0].root == kInternal && !still[0].space_known);
  // CheckSaved: a drive left out by an error is "not mounted" for the setting.
  const auto saved = storage::CheckSaved(devices, skipped, 8, 0, "/mnt");
  assert(saved.state == storage::Saved::kNotMounted && saved.mount == "/mnt/usb1");
  std::puts("ok errors: a step that throws leaves its slot out (logged), the scan goes on");
}

void CheckSettingValues() {
  assert(storage::kSaveDriveChoices == 14);
  assert(storage::SaveDriveDeviceId(0) == 0 && storage::SaveDriveDeviceId(1) == storage::kInternalDeviceId);
  for (int choice = 2; choice < storage::kSaveDriveChoices; ++choice) {
    assert(storage::SaveDriveDeviceId(choice) == storage::kSlots[choice - 2].device_id);
  }
  assert(storage::SaveDriveDeviceId(3) == 4 && storage::SaveDriveDeviceId(6) == 7);  // ext1, usb0
  for (int bad : {-1, 14, 99}) assert(storage::SaveDriveDeviceId(bad) == 0);
  std::puts("ok setting values: 0 ask, 1 internal, 2-5 ext0-ext3, 6-13 usb0-usb7, else ask");
}

void CheckSavedDrive() {
  FakeFs fs = Console();
  std::vector<storage::Skipped> skipped;
  const auto devices = storage::Scan(fs, kInternal, "/mnt", &skipped);
  const auto check = [&](uint32_t id, uint64_t requested = 0) {
    return storage::CheckSaved(devices, skipped, id, requested, "/mnt");
  };
  auto saved = check(0);  // no setting
  assert(saved.state == storage::Saved::kAsk && storage::Notice(saved, English).empty());
  saved = check(7);  // valid: usb0
  assert(saved.state == storage::Saved::kUse && saved.index == 2 && saved.mount == "/mnt/usb0");
  saved = check(4);  // valid: ext1 in its xbox360 folder
  assert(saved.state == storage::Saved::kUse && devices[saved.index].root == "/mnt/ext1/xbox360/PS5X360/saves");
  saved = check(1);
  assert(saved.state == storage::Saved::kUse && saved.index == 0 && saved.mount == kInternal);
  // Stale: usb0 unplugged.
  FakeFs unplugged = Console();
  unplugged.mounts.erase("/mnt/usb0");
  std::vector<storage::Skipped> unplugged_skipped;
  const auto after = storage::Scan(unplugged, kInternal, "/mnt", &unplugged_skipped);
  saved = storage::CheckSaved(after, unplugged_skipped, 7, 0, "/mnt");
  assert(saved.state == storage::Saved::kNotMounted && saved.mount == "/mnt/usb0");
  assert(Same(storage::Notice(saved, English), "The saved drive (/mnt/usb0) is not mounted; choose another."));
  assert(Same(storage::Notice(saved, Portuguese),
              "A unidade definida nas configurações (/mnt/usb0) não está montada; escolha outra."));
  assert(check(10).state == storage::Saved::kNotMounted);  // usb3, a placeholder
  // Read-only mount.
  FakeFs stick = Console();
  stick.read_only.insert("/mnt/usb1");
  std::vector<storage::Skipped> stick_skipped;
  const auto with_stick = storage::Scan(stick, kInternal, "/mnt", &stick_skipped);
  saved = storage::CheckSaved(with_stick, stick_skipped, 8, 0, "/mnt");
  assert(saved.state == storage::Saved::kReadOnly && saved.mount == "/mnt/usb1");
  assert(Same(storage::Notice(saved, English), "The saved drive (/mnt/usb1) cannot be written; choose another."));
  // A saved mount that is the same filesystem as an offered one: that one.
  FakeFs twice = Console();
  twice.Mount("/mnt/usb3", 201, "/dev/da0s1", 1, 1);
  std::vector<storage::Skipped> twice_skipped;
  const auto once = storage::Scan(twice, kInternal, "/mnt", &twice_skipped);
  saved = storage::CheckSaved(once, twice_skipped, 10, 0, "/mnt");
  assert(saved.state == storage::Saved::kUse && once[saved.index].id == 7);
  // Too little room (usb2: 31 GB), exactly enough, unknown space.
  saved = check(9, 100000000000ull);
  assert(saved.state == storage::Saved::kNoSpace && saved.mount == "/mnt/usb2");
  assert(Same(storage::Notice(saved, English),
              "The saved drive (/mnt/usb2) does not have enough free space; choose another."));
  assert(check(9, 31000000000ull).state == storage::Saved::kUse);
  FakeFs sandbox = Sandbox();
  const auto hidden = storage::Scan(sandbox, kInternal, "/mnt");
  assert(storage::CheckSaved(hidden, {}, 9, 100000000000ull, "/mnt").state == storage::Saved::kUse);
  assert(check(2).state == storage::Saved::kAsk && check(99).state == storage::Saved::kAsk);
  FakeFs plain = Console();
  saved = storage::CheckSaved(storage::Scan(plain, kInternal, ""), {}, 7, 0, "");
  assert(saved.state == storage::Saved::kNotMounted && saved.mount == "/mnt/usb0");
  std::puts("ok saved drive: none asks; valid used without the window (ext1's subfolder too); "
            "unplugged, placeholder, read-only and full drives ask with a notice; a second mount "
            "of an offered drive is that drive");
}

std::string ReadTreeFile(const char* rel) {
  std::ifstream file(rel, std::ios::binary);
  assert(file && "run from the assembled tree's root");
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}

std::string DriveChoice(const storage::Slot& slot) {
  return slot.kind == storage::Kind::kExtended
             ? std::string("M.2 SSD / extended storage (") + slot.mount + ")"
             : std::string("USB drive ") + slot.mount;
}

void CheckOption() {
  const std::string source = ReadTreeFile("src/launcher.cpp");
  std::string choices = "{\"Ask every time\", \"Console storage\"";
  for (const auto& slot : storage::kSlots) choices += ", \"" + DriveChoice(slot) + "\"";
  choices += "}";
  const size_t option = source.find("{\"save_drive\", &S::save_drive, 4, W::live, 0, true, \"Save drive\",");
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

void CheckLanguageFiles(const char* folder) {
  std::vector<std::string> keys = {
      "Internal storage", "M.2 SSD / extended storage", "USB %s", "%s GB free", "Console storage",
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
}  // namespace

int main(int argc, char** argv) {
  CheckSlots();
  CheckScan();
  CheckSpace();
  CheckFolders();
  CheckHoldsTitle();
  CheckPrompt();
  CheckLabels();
  CheckPosix();
  CheckPosixOddMounts();
  CheckThrowing();
  CheckSettingValues();
  CheckSavedDrive();
  CheckTranslations();
  CheckOption();
  if (argc > 1) CheckLanguageFiles(argv[1]);
  std::puts("check-save-devices: all passed");
  return 0;
}
