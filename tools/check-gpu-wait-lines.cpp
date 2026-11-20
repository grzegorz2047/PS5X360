// SPDX-License-Identifier: MIT
// Fix 58 (tools/source-1.0.2/fixes/58-gpu-wait-lines.patch): the GPU command thread's waits for the
// GPU that have no cause of their own ("other" in fix 49's Frame time line: MW2 on
// 1.0.4_candidate_18, 91-141 ms/s) are counted by the source line that waited.
//  (a) include/xbox360ps5/frame_cost.hpp: a wait of the GPU thread with no cause is counted under
//      the line its LineScope names (nested scopes restore the outer line), a wait with a cause or
//      on another thread is not, a wait with no line is "no line", lines past the table's size
//      fall into its last place; PlaceReporter's line lists them largest first per second and
//      counts only what came since its previous line;
//  (b) the assembled tree: CheckSubmissionCompletionAndDeviceLoss and
//      AwaitAllQueueOperationsCompletion take their caller's line (__builtin_LINE()), the first
//      sets the LineScope, and the 30-s summary logs the line.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "xbox360ps5/frame_cost.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

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
bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

namespace fc = xbox360ps5::frame_cost;

void CheckPlaces() {
  fc::PlaceReporter reporter;
  Expect(reporter.Line(1.0).find("none") != std::string::npos, "nothing yet");
  fc::MarkGpuThread();
  {
    fc::LineScope at(3035);
    fc::AddFenceWait(2'000'000);
    fc::AddFenceWait(2'000'000);
    {
      fc::LineScope inner(4049);
      fc::AddFenceWait(1'000'000);
    }
    fc::AddFenceWait(2'000'000);  // the outer line again
    fc::CauseScope memexport(fc::kMemexport);
    fc::AddFenceWait(9'000'000);  // has a cause: not here
  }
  fc::AddFenceWait(600'000);  // no line
  std::thread([] { fc::LineScope at(7777); fc::AddFenceWait(5'000'000); }).join();  // not the GPU thread
  const std::string line = reporter.Line(2.0);
  std::printf("   %s\n", line.c_str());
  // Fix 59 may end the line with its own count: the start is fix 58's.
  Expect(line.rfind("Frame time other waits (GPU thread) by the line of vulkan_command_processor.cc that waited: "
                 "line 3035 3.0 ms/s in 1.5/s, line 4049 0.5 ms/s in 0.5/s, no line 0.3 ms/s in 0.5/s", 0) == 0,
         "by line, largest first, per second");
  Expect(!Has(line, "7777") && !Has(line, "4.5"), "other threads and caused waits left out");
  Expect(Has(reporter.Line(1.0), "none"), "only what came since the previous line");
  for (int n = 0; n < 20; ++n) {
    fc::LineScope at(100 + n);
    fc::AddFenceWait(1'000'000);
  }
  const std::string full = reporter.Line(1.0);
  Expect(Has(full, "line 100 1.0 ms/s in 1.0/s") && Has(full, "line 108 12.0 ms/s in 12.0/s") && !Has(full, "line 119"),
         "past the table: the last place takes the rest");
}

void CheckTree() {
  const std::string header = Read(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_command_processor.h");
  Expect(Has(header, "int frame_cost_line = __builtin_LINE());") &&
             Has(header, "bool AwaitAllQueueOperationsCompletion(int frame_cost_line = __builtin_LINE()) {") &&
             Has(header, "CheckSubmissionCompletionAndDeviceLoss(GetCurrentSubmission(), frame_cost_line);"),
         "the two waits take their caller's line");
  const std::string code = Read(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_command_processor.cc");
  const size_t at = code.find("void VulkanCommandProcessor::CheckSubmissionCompletionAndDeviceLoss(\n    uint64_t await_submission, int frame_cost_line) {");
  const size_t scope = code.find("xbox360ps5::frame_cost::LineScope frame_cost_line_scope(frame_cost_line);", at);
  Expect(at != std::string::npos && scope != std::string::npos && scope - at < 400, "the wait sets the line");
  const std::string main_text = Read("src/canary_game_main.cpp");
  Expect(Has(main_text, "XELOGW(\"{}\", frame_cost_places.Line(double(summary_samples)));"), "the summary logs it");
}

}  // namespace

int main() {
  CheckPlaces();
  CheckTree();
  if (failures) {
    std::printf("check-gpu-wait-lines: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-gpu-wait-lines: ok\n");
  return 0;
}
