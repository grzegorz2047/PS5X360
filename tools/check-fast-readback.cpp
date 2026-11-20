// SPDX-License-Identifier: MIT
// Fix 59 (tools/source-1.0.2/fixes/59-fast-readback-no-wait.patch): the fast resolve readback
// (readback=1) no longer waits for the GPU to finish the previous copy. MW2 on 1.0.4_candidate_20
// waited there 138 ms/s in 55/s (fix 58 named the line in IssueCopy); the copy is handed over at a
// later resolve instead, as automatic copies already were.
//  (a) include/xbox360ps5/frame_cost.hpp: NoteLateReadback is counted and fix 58's summary line
//      ends with the rate of readbacks left for the next resolve, since its previous line;
//  (b) the assembled tree: in IssueCopy the previous slot's producer not yet finished returns
//      without CheckSubmissionCompletionAndDeviceLoss in every mode (auto_readback only decides
//      whether it is counted); the non-fast modes still await all work before they get there.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "xbox360ps5/frame_cost.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

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

namespace fc = xbox360ps5::frame_cost;

void CheckCount() {
  fc::PlaceReporter reporter;
  Expect(reporter.Line(1.0).find("; fast resolve readbacks left for the next resolve: 0.0/s") != std::string::npos,
         "none yet");
  for (int n = 0; n < 30; ++n) fc::NoteLateReadback();
  const std::string line = reporter.Line(2.0);
  std::printf("   %s\n", line.c_str());
  Expect(line.find("; fast resolve readbacks left for the next resolve: 15.0/s") != std::string::npos, "per second");
  Expect(reporter.Line(1.0).find("next resolve: 0.0/s") != std::string::npos, "only since the previous line");
}

void CheckTree() {
  const std::string code = Read(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_command_processor.cc");
  const size_t slot = code.find("const uint64_t read_submission = rb.submissions[read_index];");
  const size_t late = code.find("if (!auto_readback) xbox360ps5::frame_cost::NoteLateReadback();", slot);
  const size_t wait = code.find("CheckSubmissionCompletionAndDeviceLoss(read_submission);", slot);
  Expect(slot != std::string::npos && late != std::string::npos && wait != std::string::npos && late < wait,
         "the late copy is counted before the wait that is no longer reached");
  const std::string between = code.substr(slot, late - slot);
  Expect(between.find("if (auto_readback &&") == std::string::npos &&
             between.find("if (read_submission >\n          completion_timeline_.UpdateAndGetCompletedSubmission()) {") !=
                 std::string::npos,
         "no wait for the previous copy in any mode");
  Expect(code.find("if (readback_mode != ReadbackResolveMode::kFast) {") != std::string::npos,
         "the other modes still await all work first");
}

}  // namespace

int main() {
  CheckCount();
  CheckTree();
  if (failures) {
    std::printf("check-fast-readback: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-fast-readback: ok\n");
  return 0;
}
