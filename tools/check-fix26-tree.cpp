// SPDX-License-Identifier: MIT
// Fix 26 in the assembled tree (Handball 16, 573607E4; research/handball16/ANALYSIS.md):
//  * NtCreateFile/NtOpenFile answer an empty name under the DOS devices root (XAPI's
//    DeleteFile("")) before the VFS is asked, and the VFS no longer logs an empty path. The
//    ~11 ms that call took let Handball 16's title screen issue a save request while the game's
//    save job was not idle yet; the job dropped it, "saving" stayed set, and START was ignored.
//  * The diagnostics of the Handball test builds (candidates 18-26: the call trace, the
//    post-prompt file log, the message box lines, the save-state watch and thread dumps) are not
//    in the shipping patch.
// Built and run from the tree's root by tools/source-1.0.2/run-fix-checks.sh.
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

static int failures = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::cout << "FAIL " << __LINE__ << ": " #cond << std::endl; \
      ++failures;                                                  \
    }                                                              \
  } while (0)

static std::string Read(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}
static bool Has(const std::string& text, const char* what) {
  return text.find(what) != std::string::npos;
}

int main() {
  const std::string k = ".deps/xenia-canary/src/xenia/";
  const std::string io = Read(k + "kernel/xboxkrnl/xboxkrnl_io.cc");
  const std::string vfs = Read(k + "vfs/virtual_file_system.cc");
  const std::string guest_path = Read(k + "kernel/util/guest_path.h");
  const std::string game = Read("src/canary_game_main.cpp");
  CHECK(!io.empty() && !vfs.empty() && !guest_path.empty() && !game.empty());

  // DeleteFile(""): answered in NtCreateFile before the VFS open, with the status it had.
  const size_t create = io.find("dword_result_t NtCreateFile_entry(");
  const size_t quiet = io.find("util::IsQuietEmptyOpen(target_path, object_attrs->root_directory)", create);
  const size_t open = io.find("kernel_state()->file_system()->OpenFile(", create);
  CHECK(create != std::string::npos && quiet != std::string::npos && open != std::string::npos);
  CHECK(quiet < open);
  const size_t ret = io.find("return X_STATUS_OBJECT_NAME_NOT_FOUND;", quiet);
  CHECK(ret != std::string::npos && ret < open);
  CHECK(Has(guest_path, "inline bool IsQuietEmptyOpen(std::string_view path, uint32_t root) {\n"
                        "  return path.empty() && (root == 0 || root == 0xFFFFFFFDu);"));
  CHECK(Has(vfs, "&& !path.empty()) {\n      XELOGE(\"ResolvePath({}) failed - device not found\", path);"));

  // The test builds' diagnostics are not shipped.
  for (const char* file : {"kernel/xboxkrnl/xboxkrnl_io.cc", "kernel/xam/xam_input.cc",
                           "kernel/xam/xam_user.cc", "kernel/xam/xam_content.cc",
                           "kernel/xam/xam_content_device.cc", "kernel/xam/xam_notify.cc",
                           "kernel/xam/xam_ui.cc", "kernel/xam/xam_state.cc", "kernel/kernel_state.cc"}) {
    const std::string text = Read(k + file);
    CHECK(!text.empty());
    if (Has(text, "call_trace") || Has(text, "Files: NtCreateFile") || Has(text, "Message box: ")) {
      std::cout << "diagnostic left in " << file << "\n";
      ++failures;
    }
  }
  CHECK(Read(k + "kernel/xam/call_trace.h").empty());
  CHECK(!Has(game, "WatchHandball"));
  CHECK(!Has(game, "call_trace"));
  CHECK(!Has(game, "STALL dump {} of 2 without a stall"));
  // ... while the guide keeps its own system UI bit (system_ui.h).
  CHECK(Has(game, "SystemUiFlag::kGuide, ui_now"));

  if (failures) {
    std::cout << failures << " check(s) failed\n";
    return 1;
  }
  std::cout << "PASS: DeleteFile(\"\") answered before the VFS, empty paths not logged, no Handball "
               "test-build diagnostics in the tree (fix 26)\n";
  return 0;
}
