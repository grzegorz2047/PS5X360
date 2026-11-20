// SPDX-License-Identifier: MIT
// Fix 70 (tools/source-1.0.2/fixes/70-deferred-watch-checks.patch): "Deferred memory watches"
// without stale draws. On 1.0.4_candidate_27 Modern Warfare 2 ran faster with fix 68's setting
// but showed a film-grain flicker over the whole screen: a draw later in a submission used a copy
// the game had rewritten after its upload.
//  (a) include/xbox360ps5/deferred_watch.hpp: TopChanged names the units changed most often,
//      by count (then address), at most the number asked, as physical addresses;
//  (b) the assembled tree: only vertex-data requests defer (others are protected at once and
//      counted); a deferred unit is marked waiting with its latest hash; every request first
//      compares the waiting units of its range with their hashes and invalidates a changed one
//      (before the lock-free answer); the flush clears the waiting marks and counts changes; a
//      cache clear drops the marks; the summary is written every 120 frames without needing GPU
//      diagnostics, with the units changed most; Modern Warfare 2's preset leaves it off.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "xbox360ps5/deferred_watch.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>

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

void CheckTop() {
  Expect(dw::TopChanged({}, 12, 6) == "none", "nothing changed");
  const std::unordered_map<uint32_t, uint32_t> counts = {{0x1C984, 40}, {0x1C985, 40}, {0x00010, 3}, {0x0FE2C, 90}};
  const std::string top = dw::TopChanged(counts, 12, 3);
  std::printf("   %s\n", top.c_str());
  Expect(top == "0FE2C000 (90), 1C984000 (40), 1C985000 (40)", "by count, then address; at most three");
}

void CheckTree() {
  const std::string g = ".deps/xenia-canary/src/xenia/gpu/";
  const std::string shared = Read(g + "shared_memory.cc");
  const size_t valid = shared.find("void SharedMemory::MakeRangeValid(");
  const size_t other = shared.find(
      "if (deferring_upload_watch_ && !written_by_gpu && memory_invalidation_callback_handle_ &&\n      !request_vertex_) {",
      valid);
  const size_t counted = shared.find("deferred_immediate_units_ += valid_page_last - valid_page_first + 1;", other);
  const size_t vertex = shared.find(
      "} else if (deferring_upload_watch_ && !written_by_gpu && memory_invalidation_callback_handle_) {", counted);
  const size_t marked = shared.find("SetUnitBit(deferred_pending_bits_, unit);", vertex);
  const size_t hashed = shared.find("deferred_hash_[unit] = hash;", vertex);
  Expect(other != std::string::npos && counted != std::string::npos && vertex != std::string::npos &&
             marked != std::string::npos && hashed != std::string::npos,
         "only vertex data defers, with its waiting mark and latest hash; other data is protected at once");

  const size_t request = shared.find("bool SharedMemory::RequestRange(uint32_t start, uint32_t length,");
  const size_t kind = shared.find("request_vertex_ = allow_unwatched;", request);
  const size_t check = shared.find("if (deferred_watches_.size()) CheckDeferredInRange(start, length);", kind);
  const size_t fast = shared.find("xbox360ps5::gpu_diag::lock_free_valid.load(std::memory_order_relaxed) &&", request);
  Expect(request != std::string::npos && kind != std::string::npos && check != std::string::npos && check < fast,
         "every request checks its range's waiting units before any answer, the lock-free one included");
  const size_t fn = shared.find("void SharedMemory::CheckDeferredInRange(uint32_t start, uint32_t length) {");
  const size_t compare = shared.find("== deferred_hash_[unit])", fn);
  const size_t invalid = shared.find("MemoryInvalidationCallback(unit << page_size_log2_, uint32_t(unit_bytes), true);", compare);
  const size_t next_fn = shared.find("void SharedMemory::ReportDeferredWatches() {", fn);
  Expect(fn != std::string::npos && compare != std::string::npos && invalid < next_fn,
         "a waiting unit written since its upload is invalidated, as its write fault would have");

  const size_t flush = shared.find("void SharedMemory::FlushDeferredWatches() {");
  Expect(shared.find("ClearUnitBit(deferred_pending_bits_, unit);  // fix 70", flush) < fn &&
             shared.find("++deferred_change_counts_[unit];  // fix 70", flush) < fn,
         "the flush clears the waiting marks and counts the changed units");
  Expect(shared.find("std::fill(deferred_pending_bits_.begin(), deferred_pending_bits_.end(), uint64_t(0));  // fix 70") >
             shared.find("void SharedMemory::ClearCache() {"),
         "a cache clear drops the marks");
  Expect(shared.find("changed most: {}") > next_fn && shared.find("TopChanged(deferred_change_counts_, page_size_log2_, 6)") > next_fn,
         "the summary names the units changed most");
  Expect(shared.find("Performance deferred watches ({}):") > next_fn, "the summary line is ReportDeferredWatches'");

  const std::string processor = Read(g + "vulkan/vulkan_command_processor.cc");
  Expect(processor.find("if (frame_current_ && (frame_current_ % 120) == 0) shared_memory_->ReportDeferredWatches();") !=
             std::string::npos,
         "every 120 frames, without GPU diagnostics");
  const std::string launcher = Read("src/launcher.cpp");
  Expect(launcher.find("{\"41560817\", {{\"deferred_watches\", 1}}},") == std::string::npos,
         "Modern Warfare 2's preset leaves it off");
  Expect(launcher.find("{\"deferred_watches\", &S::deferred_watches, 1, W::live, 0, true,") != std::string::npos,
         "the setting stays, off by default");
}

}  // namespace

int main() {
  CheckTop();
  CheckTree();
  if (failures) {
    std::printf("check-deferred-checks: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-deferred-checks: ok\n");
  return 0;
}
