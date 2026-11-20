// SPDX-License-Identifier: MIT
// Fix 53 (tools/source-1.0.2/fixes/53-old-data-probe.patch), a diagnostic: whether
// PS5X360Reloaded can see the official app's /download0 (its library, settings, game settings and
// patches, which fix 46's import from /data/homebrew does not carry over).
//  (a) include/xbox360ps5/old_data_probe.hpp on a temporary folder: a file with the UFS2 magic at
//      64 KiB + 1372 is reported as a UFS2 image, a file without it (and one too short) as not,
//      a missing path with stat's errno, a folder as opened, an unreadable file with open's errno
//      (unless run as root); the paths looked at are the old app's image, folder and sandbox, this
//      title's own image and the folders above;
//  (b) the probe only reads: no lstat, realpath, write, create, mount or remove;
//  (c) the assembled canary_game_main.cpp logs each line ("Official app data: ...") after the
//      import's summary, with the titles from app_identity.hpp.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "xbox360ps5/old_data_probe.hpp"

#include <sys/stat.h>
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

namespace probe = xbox360ps5::old_data_probe;

void CheckLook() {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / ("check-old-data-" + std::to_string(::getpid()));
  fs::remove_all(dir);
  fs::create_directories(dir / "folder");
  {
    std::string image(70000, '\0');
    const uint32_t magic = probe::kUfs2Magic;
    std::memcpy(&image[probe::kSuperblock + probe::kMagicOffset], &magic, sizeof(magic));
    std::ofstream(dir / "download0.dat", std::ios::binary) << image;
    std::ofstream(dir / "other.dat", std::ios::binary) << std::string(70000, 'x');
    std::ofstream(dir / "short.dat", std::ios::binary) << "tiny";
    std::ofstream(dir / "locked.dat", std::ios::binary) << "secret";
  }
  ::chmod((dir / "locked.dat").c_str(), 0);
  const auto image = probe::Look((dir / "download0.dat").string());
  const std::string image_line = probe::Line(image);
  std::printf("   %s\n", image_line.c_str());
  Expect(image.stat_error == 0 && image.open_error == 0 && image.read_error == 0 && image.ufs2 && image.size == 70000,
         "a UFS2 image");
  Expect(image_line.find(": a file of 70000 bytes, opened, a UFS2 image (superblock magic found)") != std::string::npos,
         "the image's line");
  const auto other = probe::Look((dir / "other.dat").string());
  Expect(other.open_error == 0 && !other.ufs2 && probe::Line(other).find("no UFS2 superblock magic") != std::string::npos,
         "a file without the magic");
  const auto tiny = probe::Look((dir / "short.dat").string());
  Expect(tiny.open_error == 0 && !tiny.ufs2 && tiny.read_error == 0, "a file shorter than the superblock: no magic");
  const auto missing = probe::Look((dir / "none.dat").string());
  const std::string missing_line = probe::Line(missing);
  std::printf("   %s\n", missing_line.c_str());
  Expect(missing.stat_error == ENOENT && missing.open_error == -1 && missing_line.find("not seen (stat errno 2,") != std::string::npos,
         "a missing path: stat's errno");
  const auto folder = probe::Look((dir / "folder").string());
  Expect(folder.directory && folder.open_error == 0 && probe::Line(folder).find(": a folder, opened") != std::string::npos,
         "a folder");
  if (::geteuid() != 0) {
    const auto locked = probe::Look((dir / "locked.dat").string());
    const std::string locked_line = probe::Line(locked);
    std::printf("   %s\n", locked_line.c_str());
    Expect(locked.open_error == EACCES && locked_line.find("open refused (errno 13,") != std::string::npos,
           "an unreadable file: open's errno");
  }
  ::chmod((dir / "locked.dat").c_str(), 0600);
  fs::remove_all(dir);

  const auto paths = probe::Paths("PPSA50011", "PPSA83600");
  Expect(paths.size() == 8 && paths[0] == "/user/download/PPSA50011/download0.dat" &&
             paths[2] == "/user/download/PPSA83600/download0.dat" &&
             paths[4] == "/mnt/sandbox/PPSA50011_000/download0/xbox360ps5/settings.txt" &&
             paths[7] == "/download0/xbox360ps5/settings.txt",
         "the paths looked at");
  Expect(probe::Report({"/nonexistent-check-old-data"}).size() == 1, "one line a path");
}

void CheckTree() {
  const std::string header = Read("include/xbox360ps5/old_data_probe.hpp");
  Expect(!header.empty(), "the probe's header");
  for (const char* part : {"lstat", "realpath", "canonical", "O_WRONLY", "O_RDWR", "O_CREAT", "::write(", "mount(",
                           "unlink(", "remove", "rename("})
    Expect(header.find(part) == std::string::npos, std::string("the probe never uses: ") + part);
  const std::string main_text = Read("src/canary_game_main.cpp");
  Expect(main_text.find("#include \"xbox360ps5/old_data_probe.hpp\"") != std::string::npos, "the main includes the probe");
  const size_t summary = main_text.find("XELOGW(\"Import from PS5X360: {}\", xbox360ps5::official_import::Summary(imported));");
  const size_t call = main_text.find(
      "xbox360ps5::old_data_probe::Report(xbox360ps5::old_data_probe::Paths(", summary);
  const size_t titles = main_text.find("xbox360ps5::app_identity::kOfficialTitleId, xbox360ps5::app_identity::kTitleId", call);
  const size_t line = main_text.find("XELOGW(\"Official app data: {}\", line);", call);
  Expect(summary != std::string::npos && call != std::string::npos && titles != std::string::npos && line != std::string::npos,
         "the main logs the probe after the import's summary, with app_identity's titles");
}

}  // namespace

int main() {
  CheckLook();
  CheckTree();
  if (failures) {
    std::printf("check-old-data-probe: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-old-data-probe: ok\n");
  return 0;
}
