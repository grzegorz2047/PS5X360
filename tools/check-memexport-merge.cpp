// SPDX-License-Identifier: MIT
// Fix 30 (tools/source-1.0.2/fixes/30-memexport-small-readback.patch): which draws' memory
// exports are read back to guest memory (xenia/gpu/memexport_merge.h ShouldReadBack: all with
// the option, small ones always, none of size 0), and what of them reaches guest memory
// (ApplyExports): only the dwords in which the GPU's copy after the draw differs from its copy
// just before it, stored as the GPU stores them, so the CPU keeps everything else of Battlefield
// 3's job arena. Candidate 31's log: a link's next pointer set and cleared by the CPU around
// the upload was taken for an export by the earlier comparison with guest memory and restored
// (the crash at 8346D64C); here it stays cleared. Also the refresh of the GPU's copy before the
// draw (RefreshRanges), the readback buffer layout, the per-store records and log line, tails
// and unaligned ranges, and a worker updating other dwords with atomics while exports are
// stored loses none of its updates. Built against the assembled 1.0.2 tree.
#undef NDEBUG
#include "xenia/gpu/memexport_merge.h"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
namespace mm = xbox360ps5::memexport_merge;

void Store(std::vector<uint8_t>& memory, size_t at, uint32_t value) {  // big-endian, as the guest
  memory[at] = uint8_t(value >> 24);
  memory[at + 1] = uint8_t(value >> 16);
  memory[at + 2] = uint8_t(value >> 8);
  memory[at + 3] = uint8_t(value);
}
uint32_t Load(const std::vector<uint8_t>& memory, size_t at) {
  return uint32_t(memory[at]) << 24 | uint32_t(memory[at + 1]) << 16 | uint32_t(memory[at + 2]) << 8 |
         memory[at + 3];
}

void WhichDrawsAreReadBack() {
  assert(mm::kDefaultSmallBytes == 64 * 1024);
  // The option on: every exporting draw.
  assert(mm::ShouldReadBack(true, 4, mm::kDefaultSmallBytes));
  assert(mm::ShouldReadBack(true, 16u << 20, mm::kDefaultSmallBytes));
  assert(mm::ShouldReadBack(true, 16u << 20, 0));
  // Off: the small ones, not large buffers, and none if disabled.
  assert(mm::ShouldReadBack(false, 4, mm::kDefaultSmallBytes));
  assert(mm::ShouldReadBack(false, 64 * 1024, mm::kDefaultSmallBytes));
  assert(!mm::ShouldReadBack(false, 64 * 1024 + 4, mm::kDefaultSmallBytes));
  assert(!mm::ShouldReadBack(false, 4, 0));
  // No export: nothing to read.
  assert(!mm::ShouldReadBack(true, 0, mm::kDefaultSmallBytes) && !mm::ShouldReadBack(false, 0, 64));
  // The copy from before the draw goes after the copy from after it, 256-byte aligned
  // (Battlefield 3's arena: 393216 bytes).
  assert(mm::BeforeOffset(393216) == 393216 && mm::BeforeOffset(4) == 256 && mm::BeforeOffset(256) == 256);
  assert(mm::BeforeOffset(257) == 512 && mm::BeforeOffset(0) == 0);
}

// The job arena as candidate 31 saw it, in guest byte order: the queue (16 slots of 8 bytes,
// job pointer; affinity, priority, sequence), the sync job in slot 5 pushed with affinity 0,
// then in the next 64 bytes a dependency link (+0 state, +4 next, +8 callback, +C argument) and
// a counter the workers increment.
constexpr size_t kLink = 16 * 8, kCounter = kLink + 32;
std::vector<uint8_t> Arena() {
  std::vector<uint8_t> arena(kLink + 64, 0);
  for (size_t slot = 0; slot < 16; ++slot) {
    Store(arena, slot * 8, 0xFFC4A000u + uint32_t(slot) * 0x60);
    Store(arena, slot * 8 + 4, 0x3F002000u + uint32_t(slot));
  }
  Store(arena, 5 * 8 + 4, 0x00002045);  // the sync job, not runnable yet
  Store(arena, kLink + 0, 0x0000049A);
  Store(arena, kLink + 4, 0);           // next
  Store(arena, kLink + 8, 0x8346EC28);  // callback
  Store(arena, kLink + 12, 0xFFC4AAF0);
  Store(arena, kCounter, 0x00003A28);
  return arena;
}

// Candidate 31's draw: the GPU's copy before the draw was uploaded after the CPU had set the
// link's next pointer (FFC50210) and moved the counter; the shader wrote only slot 5's second
// word. Meanwhile the CPU cleared the next pointer again and moved the counter on.
void OnlyTheShadersWritesReachGuestMemory() {
  std::vector<uint8_t> guest = Arena();
  std::vector<uint8_t> gpu_before = guest;
  Store(gpu_before, kLink + 4, 0xFFC50210);
  Store(gpu_before, kCounter, 0x00003A31);
  std::vector<uint8_t> gpu_after = gpu_before;
  Store(gpu_after, 5 * 8 + 4, 0x3F002045);  // the export
  Store(guest, kLink + 4, 0);               // the CPU detached the link again
  Store(guest, kCounter, 0x00003A4D);
  Store(guest, 2 * 8, 0);                   // and a worker took slot 2

  const mm::Merged merged = mm::ApplyExports(guest.data(), gpu_before.data(), gpu_after.data(), guest.size());
  assert(merged.written == 1 && merged.differed == 0);
  assert(Load(guest, 5 * 8 + 4) == 0x3F002045);  // runnable now
  assert(Load(guest, kLink + 4) == 0);           // the link stays detached
  assert(Load(guest, kLink + 8) == 0x8346EC28 && Load(guest, kCounter) == 0x00003A4D && Load(guest, 2 * 8) == 0);
  const std::vector<uint8_t> arena = Arena();
  for (size_t at = 0; at < kLink; at += 4) {
    if (at == 5 * 8 + 4 || at == 2 * 8) continue;
    assert(Load(guest, at) == Load(arena, at));
  }
}

// The export lands even where guest memory held another value than the GPU's copy (the GPU
// stores, it does not compare), and is counted; nothing exported, nothing stored.
void ExportsAreStoresNotSwaps() {
  std::vector<uint8_t> guest = Arena();
  std::vector<uint8_t> gpu_before = guest;
  std::vector<uint8_t> gpu_after = guest;
  Store(gpu_after, 5 * 8 + 4, 0x3F002045);
  Store(guest, 5 * 8 + 4, 0x01002045);
  mm::Merged merged = mm::ApplyExports(guest.data(), gpu_before.data(), gpu_after.data(), guest.size());
  assert(merged.written == 1 && merged.differed == 1 && Load(guest, 5 * 8 + 4) == 0x3F002045);
  Store(guest, 0, 0x12345678);
  merged = mm::ApplyExports(guest.data(), gpu_before.data(), gpu_before.data(), guest.size());
  assert(merged.written == 0 && merged.differed == 0 && Load(guest, 0) == 0x12345678);
  // An export of the value the GPU's copy already held is not a change (nothing to store).
  merged = mm::ApplyExports(guest.data(), gpu_after.data(), gpu_after.data(), guest.size());
  assert(merged.written == 0);
}

void TailsAndUnaligned() {
  // 10 bytes: two dwords and a 2-byte tail; the shader changed dword 1 and tail byte 9.
  const std::vector<uint8_t> before = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  const std::vector<uint8_t> after = {1, 2, 3, 4, 0x3F, 6, 7, 8, 9, 0xAA};
  std::vector<uint8_t> guest = before;
  guest[8] = 0x77;  // the CPU's change in the tail, not exported: kept
  guest[0] = 0x55;  // and in dword 0: kept
  mm::Merged merged = mm::ApplyExports(guest.data(), before.data(), after.data(), 10);
  assert(merged.written == 2 && merged.differed == 0);
  assert((guest == std::vector<uint8_t>{0x55, 2, 3, 4, 0x3F, 6, 7, 8, 0x77, 0xAA}));
  // A range at an odd host address (no 32-bit atomic there): the same result.
  std::vector<uint8_t> buffer(16, 0);
  uint8_t* odd = buffer.data() + 1;
  std::memcpy(odd, before.data(), 10);
  merged = mm::ApplyExports(odd, before.data(), after.data(), 10);
  assert(merged.written == 2 && odd[4] == 0x3F && odd[9] == 0xAA && odd[0] == 1 && buffer[0] == 0);
  mm::StoreWord(odd, 0xDEADBEEF);
  uint32_t host_value;
  std::memcpy(&host_value, odd, 4);
  assert(host_value == 0xDEADBEEF);
}

// A worker updating other dwords with atomics (as ldarx/stdcx. do) while the GPU thread stores
// exports: none of the worker's updates is lost, and every export lands.
void ConcurrentWorkerKeepsItsUpdates() {
  alignas(8) static uint32_t guest[32];
  std::memset(guest, 0, sizeof(guest));
  std::atomic<bool> stop{false};
  constexpr int kIncrements = 200000;
  std::thread worker([&] {
    for (int n = 0; n < kIncrements; ++n) {
      __atomic_fetch_add(&guest[3], 1u, __ATOMIC_SEQ_CST);
      __atomic_fetch_add(&guest[17], 1u, __ATOMIC_SEQ_CST);
    }
    stop.store(true);
  });
  uint32_t exports = 0;
  while (!stop.load() || exports < 1000) {
    // The GPU's copies before and after the draw: older than the worker's later updates; the
    // shader wrote dword 10.
    uint8_t before[sizeof(guest)], after[sizeof(guest)];
    std::memcpy(before, guest, sizeof(guest));
    std::memcpy(after, before, sizeof(guest));
    uint32_t value = 0x3F000000u + exports;
    std::memcpy(after + 10 * 4, &value, 4);
    const mm::Merged merged = mm::ApplyExports(reinterpret_cast<uint8_t*>(guest), before, after, sizeof(guest));
    assert(merged.written <= 1);
    assert(guest[10] == value || merged.written == 0);
    ++exports;
  }
  worker.join();
  assert(guest[3] == uint32_t(kIncrements) && guest[17] == uint32_t(kIncrements));
}

// The GPU's copy of guest memory as SharedMemory keeps it, in 64-byte pages:
// MemoryInvalidationCallback(exact) makes pages invalid; RequestRange uploads invalid ones.
struct FakeShared {
  static constexpr uint32_t kPage = 64;
  std::vector<uint8_t>& guest;
  std::vector<uint8_t> gpu;
  std::vector<bool> valid;
  std::vector<std::string> events;
  explicit FakeShared(std::vector<uint8_t>& guest_memory)
      : guest(guest_memory), gpu(guest_memory), valid(guest_memory.size() / kPage, true) {}
  std::pair<uint32_t, uint32_t> MemoryInvalidationCallback(uint32_t start, uint32_t length, bool exact) {
    assert(exact);
    events.push_back("invalidate " + std::to_string(start) + "+" + std::to_string(length));
    for (uint32_t page = start / kPage; page <= (start + length - 1) / kPage; ++page) valid[page] = false;
    return {0, 0};
  }
  void RequestRange(uint32_t start, uint32_t length) {
    for (uint32_t page = start / kPage; page <= (start + length - 1) / kPage; ++page) {
      if (valid[page]) continue;
      std::memcpy(gpu.data() + page * kPage, guest.data() + page * kPage, kPage);
      valid[page] = true;
    }
  }
};

// The refresh: the shader reads current data, and a stale page of the GPU's copy is no export.
void RefreshAndStaleCopy() {
  std::vector<uint8_t> guest = Arena();
  FakeShared shared(guest);
  Store(shared.gpu, kLink + 8, 0);  // a kept page, older than guest memory
  mm::RefreshRanges(shared, {{0, uint32_t(guest.size())}, {64, 8}});
  assert((shared.events == std::vector<std::string>{"invalidate 0+192", "invalidate 64+8"}));
  shared.RequestRange(0, uint32_t(guest.size()));
  assert(shared.gpu == guest);
  // Even had the copy stayed stale, comparing it with itself before and after the draw finds
  // only what the shader wrote.
  std::vector<uint8_t> stale = guest;
  Store(stale, kLink + 8, 0);
  std::vector<uint8_t> after = stale;
  Store(after, 5 * 8 + 4, 0x3F002045);
  const mm::Merged merged = mm::ApplyExports(guest.data(), stale.data(), after.data(), guest.size());
  assert(merged.written == 1 && Load(guest, kLink + 8) == 0x8346EC28);
}

// The per-store records and the log line (Battlefield 3's queue at physical 1FC4E200).
void ChangesAreRecorded() {
  const std::vector<uint8_t> before = Arena();
  std::vector<uint8_t> after = before;
  Store(after, 5 * 8 + 4, 0x3F002045);
  Store(after, 7 * 8 + 4, 0x3F002033);
  Store(after, 9 * 8, 0x11111111);
  std::vector<uint8_t> guest = before;
  Store(guest, 7 * 8 + 4, 0x00002077);
  std::vector<mm::Change> changes;
  const mm::Merged merged = mm::ApplyExports(guest.data(), before.data(), after.data(), guest.size(), &changes, 2);
  assert(merged.written == 3 && merged.differed == 1 && changes.size() == 2);
  assert(changes[0].offset == 5 * 8 + 4 && changes[0].before == 0x00002045 && changes[0].after == 0x3F002045);
  assert(changes[0].guest == 0x00002045);
  assert(changes[1].offset == 7 * 8 + 4 && changes[1].before == 0x3F002007 && changes[1].after == 0x3F002033);
  assert(changes[1].guest == 0x00002077);
  assert(Load(guest, 9 * 8) == 0x11111111 && Load(guest, 7 * 8 + 4) == 0x3F002033);
  assert(mm::ChangeLine(0x1FC4E200, changes[0], 0xFFC4A1E0) ==
         "memexport 1FC4E22C (+2C): 00002045 -> 3F002045, guest was 00002045; other half of the doubleword "
         "FFC4A1E0");
  assert(mm::GuestWord(guest.data() + 5 * 8 + 4) == 0x3F002045);
  // The title's switch and the budgets.
  assert(!mm::DetailEnabled().load() && mm::DetailLines().load() == 0);
  assert(mm::kChangesPerRange == 64 && mm::kChangeLinesTotal == 800);
  // The exporting shader is dumped the first time per shader, for the first 3 shaders.
  assert(mm::FirstDumpOf(0xA) && !mm::FirstDumpOf(0xA) && mm::FirstDumpOf(0xB) && mm::FirstDumpOf(0xC));
  assert(!mm::FirstDumpOf(0xD) && !mm::FirstDumpOf(0xB));
}

}  // namespace

int main() {
  WhichDrawsAreReadBack();
  OnlyTheShadersWritesReachGuestMemory();
  ExportsAreStoresNotSwaps();
  TailsAndUnaligned();
  ConcurrentWorkerKeepsItsUpdates();
  RefreshAndStaleCopy();
  ChangesAreRecorded();
  std::printf(
      "PASS: small memory exports reach guest memory, only the dwords the shader wrote (the GPU's copy "
      "before and after the draw compared), stored as the GPU stores them (fix 30)\n");
  return 0;
}
