// SPDX-License-Identifier: MIT
// Fix 30 (tools/source-1.0.2/fixes/30-memexport-small-readback.patch): which draws' memory
// exports are read back to guest memory (xenia/gpu/memexport_merge.h ShouldReadBack: all with
// the option, small ones always, none of size 0), and how (Merge): Battlefield 3's job queue
// with the GPU's export that makes a job runnable (0x3F in the slot's affinity byte, candidate
// 21's log) reaches guest memory while the slots the CPU changed meanwhile keep the CPU's
// values (a whole-range copy would lose them); a dword both changed keeps the CPU's; tails and
// unaligned ranges; and a worker thread updating other slots with atomics while merges run
// loses none of its updates; the GPU's copy refreshed from guest memory before the draw
// (PrepareReadback), without which a stale copy wrote Battlefield 3's old dependency links back
// (candidate 25's crash at 8346D64C). Built against the assembled 1.0.2 tree.
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
  // Off: the small ones (Battlefield 3's queue), not large buffers, and none if disabled.
  assert(mm::ShouldReadBack(false, 4, mm::kDefaultSmallBytes));
  assert(mm::ShouldReadBack(false, 0x1000, mm::kDefaultSmallBytes));
  assert(mm::ShouldReadBack(false, 64 * 1024, mm::kDefaultSmallBytes));
  assert(!mm::ShouldReadBack(false, 64 * 1024 + 4, mm::kDefaultSmallBytes));
  assert(!mm::ShouldReadBack(false, 4, 0));
  // No export: nothing to read.
  assert(!mm::ShouldReadBack(true, 0, mm::kDefaultSmallBytes) && !mm::ShouldReadBack(false, 0, 64));
}

// The job queue: 16 slots of 8 bytes (job pointer; affinity, priority, sequence), as 8346E3B0
// pushes them. The sync job of a render job is pushed with affinity 0, so no worker takes it
// until the GPU exports 3F|priority|sequence into the slot's second word.
std::vector<uint8_t> Queue() {
  std::vector<uint8_t> queue(16 * 8);
  for (size_t slot = 0; slot < 16; ++slot) {
    Store(queue, slot * 8, 0xFFC4A000u + uint32_t(slot) * 0x60);
    Store(queue, slot * 8 + 4, 0x3F002000u + uint32_t(slot));
  }
  Store(queue, 5 * 8 + 4, 0x00002066);  // the sync job, not runnable yet
  return queue;
}

void ExportReachesTheQueue() {
  const std::vector<uint8_t> before = Queue();
  std::vector<uint8_t> after = before;  // the GPU's copy once the draw is done
  Store(after, 5 * 8 + 4, 0x3F002066);  // the export
  std::vector<uint8_t> guest = before;
  // Meanwhile workers took slot 2 and pushed into slot 9 (CPU only; the GPU's copy is older).
  Store(guest, 2 * 8, 0);
  Store(guest, 9 * 8, 0xFFC4C000);
  Store(guest, 9 * 8 + 4, 0x3F0020AA);

  // A whole-range copy, as the readback did before: the CPU's changes are lost.
  std::vector<uint8_t> copied = guest;
  std::memcpy(copied.data(), after.data(), after.size());
  assert(Load(copied, 2 * 8) != 0 && Load(copied, 9 * 8) != 0xFFC4C000);

  const mm::Merged merged = mm::Merge(guest.data(), before.data(), after.data(), guest.size());
  assert(merged.written == 1 && merged.kept == 0);
  assert(Load(guest, 5 * 8 + 4) == 0x3F002066);  // runnable now
  assert(Load(guest, 2 * 8) == 0 && Load(guest, 9 * 8) == 0xFFC4C000 && Load(guest, 9 * 8 + 4) == 0x3F0020AA);
  for (size_t slot = 0; slot < 16; ++slot) {
    if (slot == 2 || slot == 5 || slot == 9) continue;
    assert(Load(guest, slot * 8) == Load(before, slot * 8) && Load(guest, slot * 8 + 4) == Load(before, slot * 8 + 4));
  }
  // Merging again changes nothing (the GPU's copy and guest memory agree on the export).
  const mm::Merged again = mm::Merge(guest.data(), guest.data(), after.data(), 0);
  assert(again.written == 0 && again.kept == 0);
}

void BothChangedKeepsTheCpu() {
  const std::vector<uint8_t> before = Queue();
  std::vector<uint8_t> after = before;
  Store(after, 5 * 8 + 4, 0x3F002066);
  std::vector<uint8_t> guest = before;
  Store(guest, 5 * 8 + 4, 0x01002066);  // the CPU changed the same dword meanwhile
  const mm::Merged merged = mm::Merge(guest.data(), before.data(), after.data(), guest.size());
  assert(merged.written == 0 && merged.kept == 1 && Load(guest, 5 * 8 + 4) == 0x01002066);
  // Nothing exported: nothing written, whatever the CPU did.
  Store(guest, 0, 0x12345678);
  const mm::Merged none = mm::Merge(guest.data(), before.data(), before.data(), guest.size());
  assert(none.written == 0 && none.kept == 0 && Load(guest, 0) == 0x12345678);
}

void TailsAndUnaligned() {
  // 10 bytes: two dwords and a 2-byte tail; the export changes dword 1 and tail byte 9.
  std::vector<uint8_t> before = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  std::vector<uint8_t> after = {1, 2, 3, 4, 0x3F, 6, 7, 8, 9, 0xAA};
  std::vector<uint8_t> guest = before;
  guest[8] = 0x77;  // the CPU's change in the tail, not exported
  mm::Merged merged = mm::Merge(guest.data(), before.data(), after.data(), 10);
  assert(merged.written == 2 && merged.kept == 0);
  assert((guest == std::vector<uint8_t>{1, 2, 3, 4, 0x3F, 6, 7, 8, 0x77, 0xAA}));
  // A tail byte both changed keeps the CPU's.
  guest = before;
  guest[9] = 0x55;
  merged = mm::Merge(guest.data(), before.data(), after.data(), 10);
  assert(merged.written == 1 && merged.kept == 1 && guest[9] == 0x55 && guest[4] == 0x3F);
  // A range at an odd host address (no 32-bit atomic there): the same result.
  std::vector<uint8_t> buffer(16, 0);
  uint8_t* odd = buffer.data() + 1;
  std::memcpy(odd, before.data(), 10);
  merged = mm::Merge(odd, before.data(), after.data(), 10);
  assert(merged.written == 2 && odd[4] == 0x3F && odd[9] == 0xAA && odd[0] == 1 && buffer[0] == 0);
  assert(mm::SwapIfUnchanged(odd, 0x01020304, 0x04030201) == false);  // host order differs from bytes
  uint32_t host_value;
  std::memcpy(&host_value, odd, 4);
  assert(mm::SwapIfUnchanged(odd, host_value, 0xDEADBEEF));
  std::memcpy(&host_value, odd, 4);
  assert(host_value == 0xDEADBEEF);
}

// A worker updating other dwords of the range with atomics (as ldarx/stdcx. do) while the GPU
// thread merges exports: none of the worker's updates is lost, and every export lands.
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
    uint8_t before[sizeof(guest)], after[sizeof(guest)];
    // The snapshot before the draw, then the GPU's copy: dword 10 exported, the rest as of the
    // snapshot (older than the worker's later updates).
    std::memcpy(before, guest, sizeof(guest));
    std::memcpy(after, before, sizeof(guest));
    uint32_t value = 0x3F000000u + exports;
    std::memcpy(after + 10 * 4, &value, 4);
    const mm::Merged merged = mm::Merge(reinterpret_cast<uint8_t*>(guest), before, after, sizeof(guest));
    assert(merged.written <= 1);
    assert(guest[10] == value || merged.kept == 1);
    ++exports;
  }
  worker.join();
  assert(guest[3] == uint32_t(kIncrements) && guest[17] == uint32_t(kIncrements));
}

// The GPU's copy of guest memory as SharedMemory keeps it, in 64-byte pages: a valid page is
// not uploaded again by RequestRange; MemoryInvalidationCallback(exact) makes pages invalid.
// A "kept" page is valid but older than guest memory: what the fork's kept GPU-written units
// leave after CPU writes next to data the GPU wrote (the job arena after fix 30's readback).
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
    events.push_back("request");
    for (uint32_t page = start / kPage; page <= (start + length - 1) / kPage; ++page) {
      if (valid[page]) continue;
      std::memcpy(gpu.data() + page * kPage, guest.data() + page * kPage, kPage);
      valid[page] = true;
    }
  }
};

// Battlefield 3's arena: the queue (slot 5's sync job not runnable yet) and, in the next page,
// a dependency link whose callback (+8) the worker cleared after running it. The GPU's copy of
// that page is kept from before: the callback still set.
std::vector<uint8_t> Arena() {
  std::vector<uint8_t> arena(4 * FakeShared::kPage, 0);
  std::vector<uint8_t> queue = Queue();
  std::memcpy(arena.data(), queue.data(), queue.size());  // pages 0-1
  Store(arena, 2 * FakeShared::kPage + 0, 0xFFC50270);   // link: next
  Store(arena, 2 * FakeShared::kPage + 8, 0);            // callback, already run
  Store(arena, 2 * FakeShared::kPage + 12, 0xFFC4AA40);  // its job
  return arena;
}
void MakeStale(FakeShared& shared) {
  Store(shared.gpu, 2 * FakeShared::kPage + 8, 0x8346EC28);  // the copy from before the run
  Store(shared.gpu, 2 * FakeShared::kPage + 0, 0xFFC50100);
}
void Export(FakeShared& shared) { Store(shared.gpu, 5 * 8 + 4, 0x3F002066); }

void StaleCopyNeedsTheRefresh() {
  // Without the refresh (candidate 25): the snapshot is current, the GPU's copy is not, and
  // the merge writes the stale link back besides the export: the crash at 8346D64C.
  {
    std::vector<uint8_t> guest = Arena();
    FakeShared shared(guest);
    MakeStale(shared);
    std::vector<uint8_t> before = guest;
    shared.RequestRange(0, uint32_t(guest.size()));
    Export(shared);
    const mm::Merged merged = mm::Merge(guest.data(), before.data(), shared.gpu.data(), guest.size());
    assert(merged.written == 3 && Load(guest, 5 * 8 + 4) == 0x3F002066);
    assert(Load(guest, 2 * FakeShared::kPage + 8) == 0x8346EC28);  // a run callback is back
  }
  // With it (candidate 28): snapshot, exact invalidation, upload, export: only the export lands.
  {
    std::vector<uint8_t> guest = Arena();
    const std::vector<uint8_t> arena = guest;
    FakeShared shared(guest);
    MakeStale(shared);
    std::vector<uint8_t> before;
    const std::vector<mm::Range> ranges = {{0, uint32_t(guest.size())}};
    mm::PrepareReadback(
        shared,
        [&](uint32_t start, uint8_t* destination, uint32_t length) {
          shared.events.push_back("snapshot");
          std::memcpy(destination, guest.data() + start, length);
        },
        ranges, before);
    // The snapshot first, then one exact invalidation of the whole range.
    assert((shared.events == std::vector<std::string>{"snapshot", "invalidate 0+256"}));
    assert(before == arena);
    shared.RequestRange(0, uint32_t(guest.size()));
    assert(shared.gpu == arena);  // the copy is guest memory again
    Export(shared);
    const mm::Merged merged = mm::Merge(guest.data(), before.data(), shared.gpu.data(), guest.size());
    assert(merged.written == 1 && merged.kept == 0);
    std::vector<uint8_t> expected = arena;
    Store(expected, 5 * 8 + 4, 0x3F002066);
    assert(guest == expected);
  }
  // A worker writing between the snapshot and the upload (its value is in the copy too), and
  // one writing during the draw (not in the copy): both keep the worker's value.
  {
    std::vector<uint8_t> guest = Arena();
    FakeShared shared(guest);
    MakeStale(shared);
    std::vector<uint8_t> before;
    mm::PrepareReadback(
        shared, [&](uint32_t start, uint8_t* destination, uint32_t length) {
          std::memcpy(destination, guest.data() + start, length);
        },
        {{0, uint32_t(guest.size())}}, before);
    Store(guest, 9 * 8, 0xFFC4C000);  // before the upload
    shared.RequestRange(0, uint32_t(guest.size()));
    Export(shared);
    Store(guest, 2 * 8, 0);  // during the draw
    const mm::Merged merged = mm::Merge(guest.data(), before.data(), shared.gpu.data(), guest.size());
    assert(merged.written == 1 && merged.kept == 1);
    assert(Load(guest, 9 * 8) == 0xFFC4C000 && Load(guest, 2 * 8) == 0 && Load(guest, 5 * 8 + 4) == 0x3F002066);
    assert(Load(guest, 2 * FakeShared::kPage + 8) == 0);
  }
  // Several ranges: one snapshot buffer in order, each range invalidated.
  {
    std::vector<uint8_t> guest = Arena();
    FakeShared shared(guest);
    std::vector<uint8_t> before;
    mm::PrepareReadback(
        shared, [&](uint32_t start, uint8_t* destination, uint32_t length) {
          std::memcpy(destination, guest.data() + start, length);
        },
        {{128, 16}, {0, 8}}, before);
    assert(before.size() == 24 && std::memcmp(before.data(), guest.data() + 128, 16) == 0);
    assert(std::memcmp(before.data() + 16, guest.data(), 8) == 0);
    assert((shared.events == std::vector<std::string>{"invalidate 128+16", "invalidate 0+8"}));
    assert(!shared.valid[2] && !shared.valid[0] && shared.valid[1] && shared.valid[3]);
  }
}

}  // namespace

int main() {
  WhichDrawsAreReadBack();
  ExportReachesTheQueue();
  BothChangedKeepsTheCpu();
  TailsAndUnaligned();
  ConcurrentWorkerKeepsItsUpdates();
  StaleCopyNeedsTheRefresh();
  std::printf(
      "PASS: small memory exports reach guest memory, only the dwords the draw changed and only "
      "where the CPU did not change them meanwhile (fix 30)\n");
  return 0;
}
