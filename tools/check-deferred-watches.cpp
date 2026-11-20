// SPDX-License-Identifier: MIT
// Fix 68 (tools/source-1.0.2/fixes/68-deferred-watches.patch): "Deferred memory watches".
//  (a) include/xbox360ps5/deferred_watch.hpp: pending units are sorted, one record per unit kept
//      (the latest upload's hash), each run of consecutive units protected with one call, all
//      runs before any comparison (protect first, compare after: no write unseen), and a unit
//      whose bytes differ from its recorded hash reported as changed; the statistics count
//      records, units, calls, changes and batches; kMax pending makes it full;
//  (b) a simulated upload cycle: guest bytes hashed before the copy, a guest write between the
//      copy and the flush invalidates that unit only, an unchanged unit stays valid;
//  (c) the assembled tree: the per-game option (off by default, live) and its translations key,
//      Modern Warfare 2's built-in preset, the switch for the core; SharedMemory: uploads record
//      their units only while deferring and not for GPU-written data, the flush protects the runs
//      and invalidates changed units exactly, a cache clear drops what is pending, the summary
//      line; UploadRanges defers around its MakeRangeValid only; EndSubmission flushes first.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "xbox360ps5/deferred_watch.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
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
namespace dw = xbox360ps5::deferred_watch;

void CheckPending() {
  dw::Pending pending;
  // Units 10, 11, 12 (12 twice: the later hash counts), 20, 21, 5.
  pending.Add(12, 0x120);
  pending.Add(20, 0x200);
  pending.Add(10, 0x100);
  pending.Add(5, 0x50);
  pending.Add(11, 0x110);
  pending.Add(21, 0x210);
  pending.Add(12, 0x121);
  Expect(pending.size() == 7 && !pending.Full(), "seven records");
  std::vector<std::string> calls;
  std::map<uint32_t, uint64_t> memory = {{5, 0x50}, {10, 0x100}, {11, 0x999}, {12, 0x121}, {20, 0x200}, {21, 0x210}};
  std::vector<uint32_t> changed;
  const auto stats = pending.Flush(
      [&](uint32_t first, uint32_t last) { calls.push_back("arm " + std::to_string(first) + "-" + std::to_string(last)); },
      [&](uint32_t unit) {
        calls.push_back("hash " + std::to_string(unit));
        return memory[unit];
      },
      [&](uint32_t unit) { changed.push_back(unit); });
  std::string joined;
  for (const auto& call : calls) joined += call + "; ";
  std::printf("   %s\n", joined.c_str());
  Expect(calls.size() == 9 && calls[0] == "arm 5-5" && calls[1] == "arm 10-12" && calls[2] == "arm 20-21",
         "runs of consecutive units, one call each, in address order");
  Expect(calls[3] == "hash 5", "every run protected before any comparison");
  Expect(changed == std::vector<uint32_t>{11}, "only the unit written since its copy is changed (12's latest hash counts)");
  Expect(stats.records == 7 && stats.units == 6 && stats.runs == 3 && stats.changed == 1 && stats.flushes == 1,
         "the statistics");
  Expect(pending.size() == 0, "nothing pending after a flush");
  const auto none = pending.Flush([&](uint32_t, uint32_t) { calls.push_back("x"); }, [](uint32_t) { return 0ull; },
                                  [](uint32_t) {});
  Expect(none.flushes == 0 && calls.size() == 9, "an empty flush does nothing");
  for (size_t n = 0; n < dw::Pending::kMax; ++n) pending.Add(uint32_t(n), 0);
  Expect(pending.Full(), "full at kMax");
  pending.Clear();
  Expect(pending.size() == 0, "cleared");
  dw::Stats sum;
  sum.Add(stats);
  sum.Add(stats);
  Expect(sum.runs == 6 && sum.changed == 2 && sum.flushes == 2, "statistics add up");
}

// A guest memory of 8 units of 16 bytes, its GPU copy and validity.
uint64_t Hash(const unsigned char* bytes) {
  uint64_t h = 1469598103934665603ull;
  for (int n = 0; n < 16; ++n) h = (h ^ bytes[n]) * 1099511628211ull;
  return h;
}
void CheckCycle() {
  unsigned char guest[8][16] = {}, gpu[8][16] = {};
  bool valid[8] = {}, armed[8] = {};
  for (int u = 0; u < 8; ++u) std::memset(guest[u], u + 1, 16);
  dw::Pending pending;
  // Upload units 2..5: record (hash before the copy), copy.
  for (uint32_t u = 2; u <= 5; ++u) {
    valid[u] = true;
    pending.Add(u, Hash(guest[u]));
    std::memcpy(gpu[u], guest[u], 16);
  }
  guest[4][3] = 0xEE;  // the game writes unit 4 before the submission ends
  const auto stats = pending.Flush(
      [&](uint32_t first, uint32_t last) {
        for (uint32_t u = first; u <= last; ++u) armed[u] = true;
      },
      [&](uint32_t u) { return Hash(guest[u]); }, [&](uint32_t u) { valid[u] = false; });
  Expect(stats.runs == 1 && armed[2] && armed[5] && !armed[1] && !armed[6], "one protection call for 2..5");
  Expect(valid[2] && valid[3] && !valid[4] && valid[5], "the unit written in between is invalidated, the others stay valid");
  Expect(std::memcmp(gpu[2], guest[2], 16) == 0 && std::memcmp(gpu[4], guest[4], 16) != 0,
         "a valid unit's copy equals the guest's bytes; the stale one is the invalid one");
}

void CheckTree() {
  const std::string launcher = Read("src/launcher.cpp");
  Expect(launcher.find("{\"deferred_watches\", &S::deferred_watches, 1, W::live, 0, true, \"Deferred memory watches\",") !=
             std::string::npos,
         "the per-game option, live, off by default");
  Expect(launcher.find("gpu_diag::deferred_watches = deferred_watches != 0;  // fix 68") != std::string::npos,
         "the option switches the core");
  Expect(launcher.find("{\"41560817\", {{\"deferred_watches\", 1}}},") != std::string::npos,
         "Modern Warfare 2's built-in preset turns it on");
  Expect(Read("include/xbox360ps5/launcher.hpp").find("int deferred_watches = 0;") != std::string::npos, "off by default");
  Expect(Read("include/xbox360ps5/gpu_diagnostics.hpp").find("inline std::atomic<bool> deferred_watches{false};") !=
             std::string::npos,
         "the core's switch, off");

  const std::string g = ".deps/xenia-canary/src/xenia/gpu/";
  const std::string shared = Read(g + "shared_memory.cc");
  const size_t valid = shared.find("void SharedMemory::MakeRangeValid(");
  const size_t defer =
      shared.find("if (deferring_upload_watch_ && !written_by_gpu && memory_invalidation_callback_handle_) {", valid);
  const size_t record = shared.find("deferred_watches_.Add(", defer);
  const size_t early = shared.find("return;", record);
  const size_t arm_now = shared.find("memory().EnablePhysicalMemoryAccessCallbacks(", valid);
  Expect(valid != std::string::npos && defer != std::string::npos && record != std::string::npos && early < arm_now,
         "an upload while deferring records its units instead of protecting them; GPU-written data is protected at once");
  Expect(shared.find("if (deferred_watches_.Full()) FlushDeferredWatches();", defer) < early, "full: flushed at once");
  const size_t flush = shared.find("void SharedMemory::FlushDeferredWatches() {");
  const size_t protect = shared.find("memory().EnablePhysicalMemoryAccessCallbacks(first << page_size_log2_,", flush);
  const size_t invalidate =
      shared.find("MemoryInvalidationCallback(unit << page_size_log2_, uint32_t(unit_bytes), true);", flush);
  Expect(flush != std::string::npos && protect != std::string::npos && invalidate != std::string::npos,
         "the flush protects the runs and invalidates a changed unit exactly, as its write fault would");
  Expect(shared.find("deferred_watches_.Clear();  // fix 68") > shared.find("void SharedMemory::ClearCache() {"),
         "a cache clear drops what is pending");
  Expect(shared.find("Performance deferred watches ({}):") != std::string::npos, "the summary line");
  const std::string header = Read(g + "shared_memory.h");
  Expect(header.find("void FlushDeferredWatches();") != std::string::npos &&
             header.find("bool deferring_upload_watch_ = false;") != std::string::npos,
         "SharedMemory's flush and flag");

  const std::string upload = Read(g + "vulkan/vulkan_shared_memory.cc");
  const size_t on = upload.find("deferring_upload_watch_ =\n          xbox360ps5::gpu_diag::deferred_watches.load(");
  const size_t call = upload.find("MakeRangeValid(upload_range_start << page_size_log2(),", on);
  const size_t off = upload.find("deferring_upload_watch_ = false;", call);
  const size_t copy = upload.find("std::memcpy(", call);
  Expect(on != std::string::npos && call != std::string::npos && off != std::string::npos && off < copy,
         "UploadRanges defers around its MakeRangeValid only (the hash is taken before the copy)");
  const std::string processor = Read(g + "vulkan/vulkan_command_processor.cc");
  const size_t end = processor.find("bool VulkanCommandProcessor::EndSubmission(bool is_swap) {");
  const size_t flushed = processor.find("if (shared_memory_) shared_memory_->FlushDeferredWatches();", end);
  const size_t open = processor.find("if (submission_open_) {", end);
  Expect(end != std::string::npos && flushed != std::string::npos && flushed < open,
         "the submission's deferred watches are made before its work is handed over");
}

}  // namespace

int main() {
  CheckPending();
  CheckCycle();
  CheckTree();
  if (failures) {
    std::printf("check-deferred-watches: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-deferred-watches: ok\n");
  return 0;
}
