// SPDX-License-Identifier: MIT
// Fix 26 (tools/source-1.0.2/fixes/26-gdfx-win1252-names.patch): a GDFX disc whose file names
// hold Windows-1252 letters must mount. Handball 16 (Europe, 573607E4) stopped every launch
// with "Not enough space": utfcpp's utf8::not_enough_room, thrown by the UTF-8 path code for a
// name like "caf\xE9" that xe::win1252_to_utf8 copied unconverted on POSIX.
//
// Builds a synthetic GDFX image (30,000+ entries in 31 directories, balanced directory trees
// like a mastered disc, and names with cp1252 bytes in every position) and mounts it with the
// tree's DiscImageDevice, reading through pread as on the console (XE_MAPPED_READ_FILE).
// Checks the entry count, lookups by the UTF-8 name and by the guest's cp1252 bytes (as
// TranslateAnsiPath now converts them), the names given back to the guest, the full
// Windows-1252 table, and a bound on the heap the mount takes. Guest paths (guest_path.h, used
// by TranslateAnsiPath, IsValidPath and the file calls' failure reports): converted, accepted
// with UTF-8 (1.0.2's IsValidPath refused every byte from 0x80 up), never empty unless the
// game's name is, and each failure reported once with its raw bytes.
//
// check-gdfx-names.sh builds this against an assembled tree, and also against the tree with fix
// 24's core changes reversed; that build must fail with "Not enough space", as on the console.
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <new>
#include <string>
#include <vector>

#include <unistd.h>

#include "third_party/utfcpp/source/utf8.h"
#include "xenia/base/string.h"
#include "xenia/kernel/util/guest_path.h"
#include "xenia/vfs/devices/disc_image_device.h"
#include "xenia/vfs/entry.h"

// Heap accounting for the mount (all of operator new goes through here).
static size_t g_live = 0, g_peak = 0, g_count = 0;
static bool g_track = false;
void* operator new(size_t size) {
  void* p = std::malloc(size + 16);
  if (!p) throw std::bad_alloc();
  *static_cast<size_t*>(p) = size;
  if (g_track) {
    g_live += size;
    g_count++;
    if (g_live > g_peak) g_peak = g_live;
  }
  return static_cast<char*>(p) + 16;
}
void operator delete(void* p) noexcept {
  if (!p) return;
  void* base = static_cast<char*>(p) - 16;
  if (g_track) g_live -= *static_cast<size_t*>(base);
  std::free(base);
}
void operator delete(void* p, size_t) noexcept { operator delete(p); }
void* operator new[](size_t size) { return operator new(size); }
void operator delete[](void* p) noexcept { operator delete(p); }
void operator delete[](void* p, size_t) noexcept { operator delete(p); }

static int failures = 0;
#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::cout << "FAIL " << __LINE__ << ": " #cond << std::endl;         \
      ++failures;                                                          \
    }                                                                      \
  } while (0)

namespace {
constexpr size_t kSector = 2048;

struct Node {
  std::string name;  // raw cp1252 bytes, as on the disc
  bool directory = false;
  std::vector<Node> children;
  uint32_t sector = 0, length = 0;  // filled while laying out
  std::string content;              // file data
};

// GDFX directory table for `entries` (sorted), as a balanced binary tree like a mastered
// disc: each entry is u16 left, u16 right (ordinals = byte offset / 4), u32 sector, u32 length,
// u8 attributes, u8 name length, name; 4-byte aligned; unused bytes 0xFF.
std::vector<uint8_t> DirectoryTable(const std::vector<Node*>& entries) {
  std::vector<uint8_t> table;
  std::function<uint16_t(size_t, size_t)> emit = [&](size_t lo, size_t hi) -> uint16_t {
    if (lo >= hi) return 0;
    const size_t mid = (lo + hi) / 2;
    const size_t at = table.size();
    const Node& n = *entries[mid];
    table.resize(at + ((14 + n.name.size() + 3) & ~size_t(3)), 0xFF);
    uint8_t* p = &table[at];
    const uint32_t sector = n.sector, length = n.length;
    std::memcpy(p + 4, &sector, 4);
    std::memcpy(p + 8, &length, 4);
    p[12] = n.directory ? 0x10 : 0x80;
    p[13] = uint8_t(n.name.size());
    std::memcpy(p + 14, n.name.data(), n.name.size());
    const uint16_t left = emit(lo, mid);
    const uint16_t right = emit(mid + 1, hi);
    std::memcpy(&table[at + 0], &left, 2);
    std::memcpy(&table[at + 2], &right, 2);
    return uint16_t(at / 4);
  };
  // The root entry of a table has ordinal 0: emit the middle first.
  emit(0, entries.size());
  return table;
}

struct Image {
  std::vector<uint8_t> bytes;
  void Put(size_t sector, const void* data, size_t size) {
    if (bytes.size() < sector * kSector + size) bytes.resize(sector * kSector + size, 0);
    std::memcpy(&bytes[sector * kSector], data, size);
  }
  size_t Sectors(size_t size) const { return (size + kSector - 1) / kSector; }
};

// Lays out a directory's children (files and subdirectories first, then the table that points
// at them) from sector `next`; returns the table's sector and size.
std::pair<uint32_t, uint32_t> Layout(Image& image, Node& dir, size_t& next) {
  std::vector<Node*> entries;
  for (auto& child : dir.children) entries.push_back(&child);
  for (Node* child : entries) {
    if (child->directory) {
      auto [sector, size] = Layout(image, *child, next);
      child->sector = sector;
      child->length = size;
    } else {
      child->sector = uint32_t(next);
      child->length = uint32_t(child->content.size());
      image.Put(next, child->content.data(), child->content.size());
      next += std::max<size_t>(1, image.Sectors(child->content.size()));
    }
  }
  // GDFX sorts names case-insensitively; any fixed order works for the parser.
  std::sort(entries.begin(), entries.end(),
            [](const Node* a, const Node* b) { return a->name < b->name; });
  const auto table = DirectoryTable(entries);
  const size_t sector = next;
  image.Put(sector, table.data(), table.size());
  next += image.Sectors(table.size());
  return {uint32_t(sector), uint32_t(table.size())};
}

size_t CountTree(const Node& dir) {
  size_t n = 0;
  for (const auto& c : dir.children) n += 1 + (c.directory ? CountTree(c) : 0);
  return n;
}

size_t CountEntries(const xe::vfs::Entry* e) {
  size_t n = 0;
  for (const auto& c : e->children()) n += 1 + CountEntries(c.get());
  return n;
}

bool ValidUtf8(const std::string& s) { return utf8::is_valid(s.begin(), s.end()); }

// The names that broke the mount: cp1252 letters at the end, the start, the middle, the euro
// sign, the bytes Windows-1252 leaves undefined, and a lead byte with ASCII after it.
const std::vector<std::string> kAccented = {
    "caf\xE9",          "\xC9quipe.dat",   "na\xEFve\xA0x.bin", "\x80uro.txt",
    "\x81\x8D\x8F\x90\x9D", "Stra\xDF" "e",     "\xC3(",          "Espa\xF1" "a.xml",
    "\xFF",             "j\xFC" "rgen.cfg",
};
}  // namespace

static void CheckTable() {
  // Every byte gives valid UTF-8, and comes back.
  for (int b = 1; b < 256; ++b) {
    const std::string raw(1, char(b));
    const std::string utf = xe::win1252_to_utf8(raw);
    CHECK(ValidUtf8(utf));
    CHECK(xe::utf8_to_win1252(utf) == raw);
    if (b < 0x80) CHECK(utf == raw);
  }
  CHECK(xe::win1252_to_utf8("\x80") == "\xE2\x82\xAC");    // euro sign
  CHECK(xe::win1252_to_utf8("\xE9") == "\xC3\xA9");        // e acute
  CHECK(xe::win1252_to_utf8("\x9F") == "\xC5\xB8");        // Y diaeresis
  CHECK(xe::win1252_to_utf8("\x81") == "\xC2\x81");        // undefined: U+0081, as Windows
  CHECK(xe::win1252_to_utf8("caf\xE9") == "caf\xC3\xA9");
  CHECK(xe::win1252_to_utf8("plain/ascii.txt") == "plain/ascii.txt");
  // Not in Windows-1252, and invalid UTF-8: '_' and no exception.
  bool threw = false;
  try {
    CHECK(xe::utf8_to_win1252("\xE4\xB8\xAD") == "_");     // U+4E2D
    CHECK(xe::utf8_to_win1252("a\xE9") == "a_");           // truncated sequence
    CHECK(xe::utf8_to_win1252("\xC3(") == "_(");           // lead byte, then ASCII
  } catch (const std::exception& e) {
    threw = true;
    std::cout << "utf8_to_win1252 threw " << e.what() << "\n";
  }
  CHECK(!threw);
}

// 1.0.2's xfile.h IsValidPath, the part that refused the bytes (a signed char).
static bool IsValidPath102(std::string_view s) {
  for (const char c : s) {
    if (c <= 31 || c >= 127) return false;
  }
  return true;
}

static void CheckGuestPaths() {
  using namespace xe::kernel::util;
  // Conversion as TranslateAnsiPath does it: white space around goes, nothing else.
  CHECK(GuestAnsiPathToUtf8("").empty());
  CHECK(GuestAnsiPathToUtf8(" \t\r\n").empty());
  CHECK(GuestAnsiPathToUtf8(" game:\\Media\\a.dat\r\n") == "game:\\Media\\a.dat");
  CHECK(GuestAnsiPathToUtf8("game:\\Fran\xE7" "ais\\Options.dat") ==
        "game:\\Fran\xC3\xA7" "ais\\Options.dat");
  CHECK(GuestAnsiPathToUtf8("x\xA0") == "x\xC2\xA0");  // no-break space is a character
  for (int b = 1; b < 256; ++b) {
    const std::string raw(1, char(b));
    const bool space = b == ' ' || (b >= '\t' && b <= '\r');
    const std::string path = GuestAnsiPathToUtf8(raw);
    // An empty result only for an empty or white-space name, never from the conversion.
    CHECK(path.empty() == space);
    CHECK(IsUtf8(path));
  }
  // Validity: UTF-8 accepted now, refused by 1.0.2.
  const std::string accented = GuestAnsiPathToUtf8("game:\\Donn\xE9" "es\\caf\xE9");
  CHECK(IsValidGuestPath(accented, false));
  CHECK(!IsValidPath102(accented));
  CHECK(IsValidGuestPath("game:\\Media\\Data\\Stadium_1.li2", false));
  CHECK(!IsValidGuestPath("game:\\Fran\xE7" "ais", false));  // raw cp1252 is not UTF-8
  CHECK(!IsValidGuestPath("a\x01", false));
  CHECK(!IsValidGuestPath("a\x7F", false));
  CHECK(!IsValidGuestPath("a|b", false));
  CHECK(!IsValidGuestPath("a*.b", false));
  CHECK(IsValidGuestPath("*.dat", true));
  CHECK(!IsValidGuestPath("*_X3.rkv", true));
  CHECK(!IsValidGuestPath("a?b", false));
  CHECK(IsValidGuestPath("a?b", true));
  CHECK(IsValidGuestPath("", false));

  // Failure reports: raw bytes, the conversion, empty names said out loud, once each.
  GuestPathFailures reports;
  const uint32_t callers[3] = {0x82001234, 0x82005678, 0};
  const std::string empty =
      reports.Report("NtCreateFile", 0xC0000034, 0xFFFFFFFD, callers, "", "");
  std::cout << empty << "\n";
  CHECK(empty.find("NtCreateFile C0000034, root FFFFFFFD, caller 82001234 < 82005678") !=
        std::string::npos);
  CHECK(empty.find("0 bytes \"\" (empty name)") != std::string::npos);
  CHECK(reports.Report("NtCreateFile", 0xC0000034, 0xFFFFFFFD, callers, "", "").empty());
  CHECK(!reports.Report("NtCreateFile", 0xC0000034, 0, callers, "", "").empty());
  const std::string raw = "game:\\Fran\xE7" "ais";
  const std::string line = reports.Report("NtCreateFile", 0xC0000033, 0xFFFFFFFD, callers,
                                           raw, GuestAnsiPathToUtf8(raw));
  std::cout << line << "\n";
  // Shown escaped (a backslash doubled, a high byte as \xNN), then byte for byte.
  CHECK(line.find("14 bytes \"game:\\\\Fran\\xE7" "ais\"") != std::string::npos);
  CHECK(line.find("[67 61 6D 65 3A 5C 46 72 61 6E E7 61 69 73]") != std::string::npos);
  CHECK(line.find("-> \"game:\\Fran\xC3\xA7" "ais\"") != std::string::npos);
  CHECK(line.find("empty after conversion") == std::string::npos);
  const std::string lost = GuestPathFailures::Describe("NtCreateFile", 0xC0000034, 0, callers,
                                                       "x", "");
  CHECK(lost.find("(empty after conversion)") != std::string::npos);
  size_t reported = 0;
  for (int i = 0; i < 400; ++i) {
    reported += !reports.Report("NtQueryFullAttributesFile", 0xC000000F, 0, callers,
                                 "file" + std::to_string(i), "").empty();
  }
  CHECK(reported == GuestPathFailures::kMaxReports - 3);  // three reported above
}

int main() {
  CheckTable();
  CheckGuestPaths();

  // 31 directories: one named with cp1252 letters holding the accented names, and 30 of
  // 1,000 files each (30,000 entries, Handball 16 has 27,811).
  Node root;
  root.directory = true;
  Node donnees;
  donnees.name = "Donn\xE9" "es";
  donnees.directory = true;
  for (const auto& name : kAccented) {
    Node f;
    f.name = name;
    f.content = "file " + name;
    donnees.children.push_back(f);
  }
  Node nested;
  nested.name = "\xC9" "lite";
  nested.directory = true;
  Node deep;
  deep.name = "caf\xE9";
  deep.content = "deep";
  nested.children.push_back(deep);
  donnees.children.push_back(nested);
  root.children.push_back(donnees);
  for (int d = 0; d < 30; ++d) {
    Node dir;
    dir.name = "data" + std::to_string(d);
    dir.directory = true;
    for (int f = 0; f < 1000; ++f) {
      Node file;
      file.name = "asset_" + std::to_string(f) + (f % 7 == 0 ? "_\xE9t\xE9" : "") + ".pak";
      file.content = std::string(1, char('a' + f % 26));
      dir.children.push_back(file);
    }
    root.children.push_back(dir);
  }
  const size_t expected = CountTree(root);

  Image image;
  const char magic[] = "MICROSOFT*XBOX*MEDIA";
  size_t next = 33;
  auto [root_sector, root_size] = Layout(image, root, next);
  uint8_t descriptor[32] = {};
  std::memcpy(descriptor, magic, 20);
  std::memcpy(descriptor + 20, &root_sector, 4);
  std::memcpy(descriptor + 24, &root_size, 4);
  image.Put(32, descriptor, sizeof(descriptor));
  image.bytes.resize(image.bytes.size() + kSector, 0);

  const auto path = std::filesystem::temp_directory_path() /
                     ("check-gdfx-names-" + std::to_string(::getpid()) + ".iso");
  {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(image.bytes.data()), image.bytes.size());
  }
  std::cout << "Image: " << image.bytes.size() << " bytes, " << expected << " entries\n";

  // As on the console: the image is read with pread, not through a mapping.
  setenv("XE_MAPPED_READ_FILE", "1", 1);
  int status = 0;
  {
    xe::vfs::DiscImageDevice device("\\Device\\Cdrom0", path);
    bool mounted = false;
    g_track = true;
    try {
      mounted = device.Initialize();
    } catch (const std::exception& e) {
      g_track = false;
      std::cout << "FAIL: mount threw: " << e.what() << "\n";
      std::filesystem::remove(path);
      return 2;
    }
    g_track = false;
    CHECK(mounted);
    if (!mounted) {
      std::filesystem::remove(path);
      return 1;
    }
    std::cout << "Mount: peak heap " << g_peak << " bytes, " << g_count << " allocations, "
              << (g_peak / expected) << " bytes per entry\n";
    // Bounds: the entries themselves (a few strings and a vector each) and one 32 KiB-ish
    // directory table at a time; Handball 16's 27,811 entries stay far below the title's heap.
    CHECK(g_peak / expected < 1024);
    CHECK(g_peak < 32u * 1024 * 1024);

    auto* root_entry = device.ResolvePath("");
    CHECK(root_entry != nullptr);
    if (root_entry) {
      const size_t mounted_entries = CountEntries(root_entry);
      std::cout << "Entries mounted: " << mounted_entries << "\n";
      CHECK(mounted_entries == expected);
    }

    // Every accented name: found by its UTF-8 name and by the guest's cp1252 bytes converted
    // as TranslateAnsiPath does; given back to the guest as the bytes on the disc.
    for (const auto& name : kAccented) {
      const std::string guest = "Donn\xE9" "es\\" + name;
      auto* by_utf8 = device.ResolvePath(xe::win1252_to_utf8(guest));
      CHECK(by_utf8 != nullptr);
      if (!by_utf8) {
        std::cout << "  not found: " << xe::win1252_to_utf8(guest) << "\n";
        continue;
      }
      CHECK(ValidUtf8(by_utf8->name()));
      CHECK(xe::utf8_to_win1252(by_utf8->name()) == name);
      CHECK(by_utf8->size() == ("file " + name).size());
      // The game's own path, as the file calls see it: converted, valid, and the same entry.
      const std::string kernel_path =
          xe::kernel::util::GuestAnsiPathToUtf8(" " + guest + "\r\n");
      CHECK(!kernel_path.empty());
      CHECK(xe::kernel::util::IsValidGuestPath(kernel_path, false));
      CHECK(device.ResolvePath(kernel_path) == by_utf8);
    }
    auto* deep_entry = device.ResolvePath(xe::win1252_to_utf8("Donn\xE9" "es\\\xC9lite\\caf\xE9"));
    CHECK(deep_entry != nullptr);
    if (deep_entry) CHECK(deep_entry->size() == 4);
    auto* plain = device.ResolvePath("data29\\asset_999.pak");
    CHECK(plain != nullptr);
    auto* mixed = device.ResolvePath(xe::win1252_to_utf8("data3\\asset_7_\xE9t\xE9.pak"));
    CHECK(mixed != nullptr);
    // Case-insensitive lookup still works for ASCII.
    CHECK(device.ResolvePath("DATA0\\ASSET_1.PAK") != nullptr);
  }

  // A damaged table (an entry pointing past the table) fails the mount without an exception.
  {
    std::vector<uint8_t> broken = image.bytes;
    uint8_t* root_table = &broken[size_t(root_sector) * kSector];
    root_table[0] = root_table[1] = 0xFF;
    const auto broken_path = path.string() + ".broken";
    {
      std::ofstream out(broken_path, std::ios::binary);
      out.write(reinterpret_cast<const char*>(broken.data()), broken.size());
    }
    xe::vfs::DiscImageDevice device("\\Device\\Cdrom0", broken_path);
    bool mounted = true, threw = false;
    try {
      mounted = device.Initialize();
    } catch (const std::exception& e) {
      threw = true;
      std::cout << "damaged image threw " << e.what() << "\n";
    }
    CHECK(!threw);
    CHECK(!mounted);
    std::filesystem::remove(broken_path);
  }

  std::filesystem::remove(path);
  if (failures) {
    std::cout << failures << " check(s) failed\n";
    return 1;
  }
  std::cout << "PASS: GDFX names in Windows-1252 mount, resolve and round-trip ("
            << expected << " entries)\n";
  return status;
}
