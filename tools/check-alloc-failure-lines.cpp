// SPDX-License-Identifier: MIT
// Fix 76 (tools/source-1.0.2/fixes/76-alloc-failure-lines.patch): on the v1.0.4 draft Fable II
// (4D5307F1) stopped at start. Its game thread asks NtAllocateVirtualMemory for a 5 MiB reserve
// of 64 KiB pages; when that fails it asks again at every 64 KiB from 0x40000000 until one is
// free (about 3,900 tries with 245 MiB of the heap taken). Fix 47 wrote a hang-watch line, a
// write and an fsync, for every failed allocation: 3,856 lines in 61 s, and the game never got
// past it. Now only the first kFailureLines failures get a line, then one says the rest are only
// counted; a big allocation still gets its line, and the beat still has every failure.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "xbox360ps5/hang_watch.hpp"

#include <cstdio>
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
namespace hw = xbox360ps5::hang_watch;
std::vector<std::string> raw;

void CheckLines() {
  hw::raw_sink.store(+[](const char* text, size_t size) { raw.emplace_back(text, size); });
  // Fable II's search: the reserve anywhere, then at every 64 KiB from 0x40000000.
  hw::GuestAllocation("NtAllocateVirtualMemory", false, 0, 0x540000, 0x20002000, false);
  for (uint32_t address = 0x40000000; address < 0x4F500000; address += 0x10000)
    hw::GuestAllocation("NtAllocateVirtualMemory", false, address, 0x540000, 0x20002000, false);
  const uint64_t tries = 1 + (0x4F500000 - 0x40000000) / 0x10000;
  Expect(hw::guest_memory.failures == tries, "every failure counted (" + std::to_string(hw::guest_memory.failures) + ")");
  Expect(raw.size() == hw::kFailureLines + 1, "a line for the first " + std::to_string(hw::kFailureLines) +
                                                   " failures and one more, not one each (" + std::to_string(raw.size()) + ")");
  Expect(!raw.empty() && raw.front().find("GUEST NtAllocateVirtualMemory FAILED: 00000000, 5505024 bytes (5 MiB), type 20002000") == 0,
         "the first failure's line");
  Expect(raw.size() > hw::kFailureLines &&
             raw[hw::kFailureLines] == "GUEST failed allocations: 16 lines written, the rest only counted in the beat",
         "the line that says no more follow");
  // The reserve found room: a small success, no line.
  Expect(hw::GuestAllocation("NtAllocateVirtualMemory", false, 0x4F500000, 0x540000, 0x20002000, true).empty(),
         "a small allocation: no line");
  const size_t before = raw.size();
  hw::GuestAllocation("MmAllocatePhysicalMemoryEx", true, 0xA0000000, 96u << 20, 0x20000000, true);
  Expect(raw.size() == before + 1, "a big allocation still gets its line");
  hw::BeatInput in;
  Expect(hw::HeartbeatLine(in).find(", " + std::to_string(tries) + " failed") != std::string::npos,
         "the beat has every failure");
  hw::raw_sink.store(nullptr);
}

void CheckTree() {
  const std::string code = Read(".deps/xenia-canary/src/xenia/kernel/xboxkrnl/xboxkrnl_memory.cc");
  Expect(code.find("if (!ok || (alloc_type & X_MEM_COMMIT)) {\n    xbox360ps5::hang_watch::GuestAllocation(") != std::string::npos ||
             code.find("if (!ok || (alloc_type & X_MEM_COMMIT)) {\r\n    xbox360ps5::hang_watch::GuestAllocation(") != std::string::npos,
         "NtAllocateVirtualMemory still hands every failure to GuestAllocation");
}

}  // namespace

int main() {
  CheckLines();
  CheckTree();
  if (failures) {
    std::printf("check-alloc-failure-lines: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-alloc-failure-lines: ok\n");
  return 0;
}
