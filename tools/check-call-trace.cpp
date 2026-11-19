// SPDX-License-Identifier: MIT
// Fix 26, diagnostic for Handball 16 (573607E4), 1.0.3_candidate_18: after the storage prompt
// the game draws at 60 frames a second and ignores START. xam/call_trace.h logs what the game
// asks of input, sign-in, content and notifications, each call and key when its answer first
// appears and when it changes, capped, with the time since the storage prompt; and
// canary_game_main.cpp dumps the threads 20 s and 45 s after the prompt.
//
// Checks call_trace.h, and from the assembled tree that every hook is in place.
// Built and run from the tree's root by tools/source-1.0.2/run-fix-checks.sh.
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "xenia/kernel/xam/call_trace.h"

static int failures = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::cout << "FAIL " << __LINE__ << ": " #cond << std::endl; \
      ++failures;                                                  \
    }                                                              \
  } while (0)

static std::string Read(const char* path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}
static bool Has(const std::string& text, const char* what) {
  return text.find(what) != std::string::npos;
}

int main() {
  using namespace xe::kernel::xam::call_trace;
  // Before the prompt, then relative to it.
  CHECK(When(5000, 0) == "(before the storage prompt)");
  CHECK(When(5000, 3000) == "(+2000 ms after the storage prompt)");
  CHECK(When(1000, 3000) == "(+0 ms after the storage prompt)");

  // The input answer names START and what blanked the buttons.
  CHECK(InputAnswer(0, 0x0010, 0, false) == "result 00000000 buttons 0010 START");
  CHECK(InputAnswer(0x48F, 0, 0, false) == "result 0000048F buttons 0000");
  CHECK(Has(InputAnswer(0, 0x1010, 1, false), "START, system UI active (XAM dialog) (state discarded)"));
  CHECK(Has(InputAnswer(0, 0, 2, false), "system UI active (guide) (state discarded)"));
  CHECK(Has(InputAnswer(0, 0, 3, false), "system UI active (XAM dialog and guide)"));
  CHECK(Has(InputAnswer(0, 0, 0, true), "controller keyboard suppressing"));

  // Change log: first answer, then only changes, per call and key.
  Changes changes;
  const std::string first = changes.Note("XamInputGetState", 0x100, "result 00000000 buttons 0000", 10);
  std::cout << first << "\n";
  CHECK(first == "Calls: XamInputGetState[00000100] result 00000000 buttons 0000 (before the storage prompt)");
  CHECK(changes.Note("XamInputGetState", 0x100, "result 00000000 buttons 0000", 20).empty());
  CHECK(!changes.Note("XamInputGetState", 0x101, "result 00000000 buttons 0000", 20).empty());
  CHECK(!changes.Note("XamUserGetSigninState", 0x100, "result 00000000 buttons 0000", 20).empty());
  CHECK(!InWindow(500));  // no prompt yet
  CHECK(&For(500) == &Changes::Get());
  MarkPrompt(1000);
  // The minute after the prompt has its own, larger log (candidate_20).
  CHECK(InWindow(1000) && InWindow(60999) && !InWindow(61000) && !InWindow(999));
  CHECK(&For(30000) == &AfterPrompt());
  CHECK(&For(70000) == &Changes::Get());
  {
    int presses = 0;
    for (int i = 0; i < 300; ++i) {
      presses += !AfterPrompt().Note("XamInputGetState", 0, (i & 1) ? "buttons 0010 START" : "buttons 0000", 2000).empty();
    }
    CHECK(presses == 300);  // 150 START presses and releases, all logged
  }
  const std::string start = changes.Note("XamInputGetState", 0x100, "result 00000000 buttons 0010 START", 4500);
  std::cout << start << "\n";
  CHECK(Has(start, "buttons 0010 START (+3500 ms after the storage prompt)"));
  CHECK(!changes.Note("XamInputGetState", 0x100, "result 00000000 buttons 0000", 4600).empty());

  // A key that keeps changing stops at kPerKey lines and says so once.
  Changes per_key;
  std::string last;
  int lines = 0;
  for (int i = 0; i < 100; ++i) {
    const auto line = per_key.Note("XNotifyGetNext", 9, "param " + std::to_string(i));
    if (!line.empty()) {
      ++lines;
      last = line;
    }
  }
  CHECK(lines == int(Changes::kPerKey));
  CHECK(Has(last, "no more changes of this one are logged"));

  // All keys together stop at kTotal lines.
  Changes total;
  int all = 0;
  std::string final_line;
  for (uint32_t key = 0; key < 2000; ++key) {
    const auto line = total.Note("BroadcastNotification", key, "data 00000000");
    if (!line.empty()) {
      ++all;
      final_line = line;
    }
  }
  CHECK(all == int(Changes::kTotal));
  CHECK(Has(final_line, "the trace has used its lines"));

  // The hooks in the assembled tree.
  const std::string k = ".deps/xenia-canary/src/xenia/kernel/";
  const std::string input = Read((k + "xam/xam_input.cc").c_str());
  const std::string user = Read((k + "xam/xam_user.cc").c_str());
  const std::string content = Read((k + "xam/xam_content.cc").c_str());
  const std::string device = Read((k + "xam/xam_content_device.cc").c_str());
  const std::string notify = Read((k + "xam/xam_notify.cc").c_str());
  const std::string ui = Read((k + "xam/xam_ui.cc").c_str());
  const std::string kernel = Read((k + "kernel_state.cc").c_str());
  const std::string game = Read("src/canary_game_main.cpp");
  CHECK(Has(input, "\"XamInputGetState\""));
  CHECK(Has(input, "the buttons the game did not get"));
  CHECK(Has(input, "\"XamInputGetCapabilities\""));
  CHECK(Has(input, "\"XamInputGetKeystroke\""));
  CHECK(Has(user, "\"XamUserGetSigninState\""));
  CHECK(Has(user, "\"XamUserGetSigninInfo\""));
  CHECK(Has(content, "\"XamContentCreate\""));
  CHECK(Has(device, "\"XamContentGetDeviceState\""));
  CHECK(Has(notify, "\"XNotifyGetNext\""));
  CHECK(Has(kernel, "\"BroadcastNotification\""));
  CHECK(Has(ui, "call_trace::MarkPrompt();"));
  CHECK(Has(game, "ReportStall(emulator, 10 + dumps);"));
  CHECK(Has(game, "since >= (dumps ? 45000u : 20000u)"));
  // candidate_20: the minute after the prompt, every open and the system interface's owners.
  CHECK(Has(Read((k + "xboxkrnl/xboxkrnl_io.cc").c_str()), "Files: NtCreateFile"));
  CHECK(Has(input, "call_trace::For().Note("));
  CHECK(Has(Read((k + "xam/xam_state.cc").c_str()), "System UI: {} -> {}"));

  if (failures) {
    std::cout << failures << " check(s) failed\n";
    return 1;
  }
  std::cout << "PASS: call trace logs first answers and changes (capped, timed from the storage "
               "prompt); hooks for input, sign-in, content, notifications and the thread dumps\n";
  return 0;
}
