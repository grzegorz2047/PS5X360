// SPDX-License-Identifier: MIT
// Fix 26 (Handball 16, 573607E4): who holds the system interface up. While anyone
// does, XamInputGetState gives the game a pad at rest. In 1.0.2 one bool held it, written by the
// XAM dialogs and by the PS5X360 guide, and a dialog the UI thread could not show left it set.
// candidate_18 on the console: START reached the game before the storage prompt, then every
// read was discarded while the device selector was up; whether that went on after the prompt
// was hidden by the trace's cap.
//
// Checks xam/system_ui.h (owners: XAM dialog, guide; the hook on every change) through the
// sequences that matter, and in the assembled tree the uses: XamState's member and hook, the
// dialogs' failure paths, the guide's own bit in canary_game_main.cpp.
// Built and run from the tree's root by tools/source-1.0.2/run-fix-checks.sh.
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "xenia/kernel/xam/system_ui.h"

static int failures = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::cout << "FAIL " << __LINE__ << ": " #cond << std::endl; \
      ++failures;                                                  \
    }                                                              \
  } while (0)

static std::vector<std::pair<uint32_t, uint32_t>> changes;
static void Hook(uint32_t before, uint32_t after) { changes.emplace_back(before, after); }

static std::string Read(const char* path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}
static size_t Count(const std::string& text, const std::string& what) {
  size_t n = 0;
  for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) ++n;
  return n;
}

int main() {
  using xe::kernel::xam::SystemUiFlag;
  {
    // Handball 16 before the prompt: a dialog at START (closed with A), then the device
    // selector (closed with the drive). Afterwards the game's input passes.
    SystemUiFlag ui;
    ui.set_hook(Hook);
    CHECK(!ui.load());
    ui.store(true);
    CHECK(ui.load() && ui.owners() == SystemUiFlag::kXamDialog);
    ui.store(false);
    ui.store(true);
    ui.store(false);
    CHECK(!ui.load());
    CHECK(changes.size() == 4);
    CHECK(changes.back() == std::make_pair(1u, 0u));
  }
  changes.clear();
  {
    // The guide over a dialog: closing the guide leaves the dialog's hold (1.0.2's bool let
    // the game's input through under the dialog), and closing the dialog leaves the guide's.
    SystemUiFlag ui;
    ui.set_hook(Hook);
    ui.store(true);
    ui.Set(SystemUiFlag::kGuide, true);
    CHECK(ui.owners() == 3u);
    ui.Set(SystemUiFlag::kGuide, false);
    CHECK(ui.load() && ui.owners() == SystemUiFlag::kXamDialog);
    ui.Set(SystemUiFlag::kGuide, true);
    ui.store(false);
    CHECK(ui.load() && ui.owners() == SystemUiFlag::kGuide);
    ui.Set(SystemUiFlag::kGuide, false);
    CHECK(!ui.load());
  }
  changes.clear();
  {
    // A dialog answered without a window (or one the UI thread could not show): set, then
    // released on the failure path; the guide's own toggling does not touch it.
    SystemUiFlag ui;
    ui.set_hook(Hook);
    ui.store(true);
    ui.store(false);  // xeXamDispatchDialog's failure path (fix 26)
    CHECK(!ui.load());
    ui.Set(SystemUiFlag::kGuide, true);
    ui.Set(SystemUiFlag::kGuide, false);
    CHECK(!ui.load());
    // The same owner set twice is one hold, and the hook hears only real changes.
    ui.store(true);
    ui.store(true);
    ui.store(false);
    CHECK(!ui.load());
    CHECK(changes.size() == 6);
  }
  CHECK(std::string(SystemUiFlag::OwnersText(0)) == "none");
  CHECK(std::string(SystemUiFlag::OwnersText(1)) == "XAM dialog");
  CHECK(std::string(SystemUiFlag::OwnersText(2)) == "guide");
  CHECK(std::string(SystemUiFlag::OwnersText(3)) == "XAM dialog and guide");

  // The assembled tree uses it.
  const std::string k = ".deps/xenia-canary/src/xenia/kernel/";
  const std::string state_h = Read((k + "xam/xam_state.h").c_str());
  const std::string state_cc = Read((k + "xam/xam_state.cc").c_str());
  const std::string ui_cc = Read((k + "xam/xam_ui.cc").c_str());
  const std::string game = Read("src/canary_game_main.cpp");
  CHECK(Count(state_h, "SystemUiFlag is_xam_dialog_present_;") == 1);
  CHECK(Count(state_cc, "is_xam_dialog_present_.set_hook(") == 1);
  CHECK(Count(ui_cc, "a dialog could not be shown on the UI thread; released") == 2);
  // ... and the game gets a failure there, not 1.0.2's uninitialized result; every closed
  // dialog's result is logged.
  CHECK(Count(ui_cc, "      result = X_ERROR_FUNCTION_FAILED;\n      delete dialog;") == 2);
  CHECK(Count(ui_cc, "System UI: dialog closed, result {:08X}") == 2);
  CHECK(Count(game, "SystemUiFlag::kGuide, ui_now") == 1);
  CHECK(Count(game, "is_xam_dialog_present_.store(ui_now)") == 0);

  if (failures) {
    std::cout << failures << " check(s) failed\n";
    return 1;
  }
  std::cout << "PASS: the system interface is held per owner (XAM dialog, guide); nested, "
               "auto-answered and failed dialogs release it; every change is reported\n";
  return 0;
}
