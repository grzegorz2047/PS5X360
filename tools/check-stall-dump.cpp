// SPDX-License-Identifier: MIT
// Fix 24 (tools/source-1.0.2/fixes/24-stall-code-words.patch): drives the STALL report's guest
// memory reading (port/stall_dump.hpp) with fake memory: the 64-byte code window around lr, the
// stack's back chain with each frame's saved return address and first words, unreadable pages,
// the ends of the address space, thread states, call targets, changes over time and the kernel
// objects' wait and release history (xenia/kernel/object_history.h). Built against the assembled 1.0.2 tree.
#undef NDEBUG
#include "../port/stall_dump.hpp"  // the assembled tree's port/ (run from <tree>/tools)
#include "xenia/kernel/object_history.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <string>

namespace {
namespace sd = xbox360ps5::stall_dump;

// Words by address; pages (4 KiB) not listed in `readable` cannot be read.
struct FakeMemory {
  std::map<uint32_t, uint32_t> words;
  std::set<uint32_t> readable_pages;
  int reads = 0;
  void Page(uint32_t address) { readable_pages.insert(address >> 12); }
  void Put(uint32_t address, uint32_t value) {
    Page(address);
    words[address] = value;
  }
  bool Readable(uint32_t address) { return readable_pages.count(address >> 12) != 0; }
  uint32_t Word(uint32_t address) {
    assert(Readable(address));  // never read where the host could not
    ++reads;
    auto found = words.find(address);
    return found == words.end() ? 0 : found->second;
  }
};

void CodeWindowAroundLr() {
  FakeMemory memory;
  // Battlefield 3's loaders spin at lr 838D4514.
  for (uint32_t n = 0; n < 16; ++n) memory.Put(0x838D44F4 + n * 4, 0x60000000 + n);
  const std::string text = sd::CodeWindow(memory, 0x838D4514);
  // 16 words from lr - 32; lr's own word is the ninth.
  assert(text ==
         " 60000000 60000001 60000002 60000003 60000004 60000005 60000006 60000007"
         " 60000008 60000009 6000000A 6000000B 6000000C 6000000D 6000000E 6000000F");
  assert(memory.reads == 16);
  std::printf("  code window: 64 bytes, lr's word the ninth\n");
}

void UnreadableWordsShown() {
  FakeMemory memory;
  memory.Put(0x89014FA0, 0x7D8802A6);  // only ctr's page readable; the window crosses into the one before
  const std::string text = sd::Words(memory, 0x89014FF8, 4);
  assert(text == " 00000000 00000000 ???????? ????????");
  assert(sd::CodeWindow(memory, 0x00000010).size() == 16 * 9);  // near 0: starts at 0, never wraps
  assert(sd::Words(memory, 0xFFFFFFF8, 4) == " ???????? ???????? ???????? ????????");
  std::printf("  unreadable pages and the address space's ends: '????????', no read\n");
}

void BackChain() {
  FakeMemory memory;
  // r1 7062FCB0 -> 7062FD00 -> 7062FE00 -> 0 (end of the chain).
  memory.Put(0x7062FCB0, 0x7062FD00);
  memory.Put(0x7062FCB4, 0x11111111);
  memory.Put(0x7062FD00, 0x7062FE00);
  memory.Put(0x7062FCF8, 0x838B8510);  // saved at the caller's frame - 8
  memory.Put(0x7062FE00, 0x00000000);
  memory.Put(0x7062FDF8, 0x838B85D4);
  const auto frames = sd::StackFrames(memory, 0x7062FCB0);
  assert(frames.size() == 3);
  assert(frames[0].address == 0x7062FCB0 && frames[0].back == 0x7062FD00);
  assert(frames[0].lr_read && frames[0].saved_lr == 0x838B8510);
  assert(frames[0].words.rfind(" 7062FD00 11111111", 0) == 0);
  assert(frames[1].address == 0x7062FD00 && frames[1].saved_lr == 0x838B85D4);
  assert(frames[2].address == 0x7062FE00 && frames[2].back == 0 && !frames[2].lr_read);
  std::printf("  back chain: frames, saved return addresses, first words, end of chain\n");
}

void BackChainStops() {
  {
    FakeMemory memory;  // a frame that points down the stack (a loop) ends the walk
    memory.Put(0x70001000, 0x70000F00);
    assert(sd::StackFrames(memory, 0x70001000).size() == 1);
  }
  {
    FakeMemory memory;  // a jump of more than 64 KiB ends it
    memory.Put(0x70001000, 0x70100000);
    const auto frames = sd::StackFrames(memory, 0x70001000);
    assert(frames.size() == 1 && !frames[0].lr_read);
  }
  {
    FakeMemory memory;  // the caller's frame not readable: listed with no return address, then stop
    memory.Put(0x70001000, 0x70003000);
    const auto frames = sd::StackFrames(memory, 0x70001000);
    assert(frames.size() == 1 && !frames[0].lr_read);
  }
  {
    FakeMemory memory;  // misaligned, null, unreadable r1: nothing
    memory.Page(0x70001000);
    assert(sd::StackFrames(memory, 0x70001002).empty());
    assert(sd::StackFrames(memory, 0).empty());
    assert(sd::StackFrames(memory, 0x50000000).empty());
  }
  {
    FakeMemory memory;  // at most `limit` frames
    for (uint32_t n = 0; n < 20; ++n) memory.Put(0x70010000 + n * 0x100, 0x70010000 + (n + 1) * 0x100);
    assert(sd::StackFrames(memory, 0x70010000).size() == 8);
    assert(sd::StackFrames(memory, 0x70010000, 3).size() == 3);
  }
  std::printf("  back chain stops: down the stack, a big jump, unreadable, misaligned, the limit\n");
}

void DataPointers() {
  assert(!sd::LooksLikeData(3) && !sd::LooksLikeData(0x33));
  assert(sd::LooksLikeData(0x7018D5A0));  // Battlefield 3's main thread spins on an object in its frame
  assert(sd::LooksLikeData(0x30083010) && sd::LooksLikeData(0x437F8D80) && sd::LooksLikeData(0x89014FA0));
  assert(!sd::LooksLikeData(0xF8000090) && !sd::LooksLikeData(0xFFFFFFFF));
  assert(sd::LooksLikeHandle(0xF8000090) && sd::LooksLikeHandle(0xF80001A8));
  assert(!sd::LooksLikeHandle(0xF8000000) && !sd::LooksLikeHandle(0xF8000091) && !sd::LooksLikeHandle(0x30083010));
  assert(!sd::LooksLikeHandle(0xFFC49088));
  std::printf("  register values: data addresses (stacks too) dumped, handles told apart\n");
}
void ThreadStates() {
  using sd::ThreadState;
  assert(sd::Classify(true, false, false) == ThreadState::kRunning);
  // Battlefield 3's ClientLoad on candidate_9: exited (terminated, header signalled), object kept.
  assert(sd::Classify(false, true, true) == ThreadState::kExited);
  assert(sd::Classify(false, false, true) == ThreadState::kExited);  // terminated from outside
  assert(sd::Classify(true, true, true) == ThreadState::kExited);    // between its exit and its end
  assert(sd::Classify(false, false, false) == ThreadState::kNotStarted);
  assert(std::string(sd::ThreadStateName(ThreadState::kExited)) == "exited (object still referenced)");
  std::printf("  thread states: running, exited (object still referenced), not started\n");
}

void CallsDecoded() {
  FakeMemory memory;
  // JobManager1's poll wrapper (candidate_9): li r4,0 then bl to the import thunk 83ECE554.
  memory.Put(0x838D16B4, 0x38800000);
  memory.Put(0x838D16B8, 0x485FCE9D);
  memory.Put(0x838D16BC, 0x2C030000);
  // A bl backwards (offset -0x80); a plain b (no link) and a bla (absolute) are not calls here.
  memory.Put(0x838D16C0, 0x4BFFFF81);
  memory.Put(0x838D16C4, 0x48000010);
  memory.Put(0x838D16C8, 0x48000013);
  const auto calls = sd::Calls(memory, 0x838D16B4, 6);
  assert(calls.size() == 2);
  assert(calls[0].site == 0x838D16B8 && calls[0].target == 0x83ECE554);
  assert(calls[1].site == 0x838D16C0 && calls[1].target == 0x838D1640);
  assert(sd::Calls(memory, 0x838D2000, 4).empty());  // unreadable: nothing, no read
  std::printf("  calls: bl targets decoded (838D16B8 -> 83ECE554), b/bla and unreadable skipped\n");
}

void ChangesOverTime() {
  sd::ChangeLog steady;
  for (uint32_t ms = 0; ms < 800; ms += 100) steady.Add(ms, " 00000001 00000000");
  assert(!steady.Changed() && steady.samples.size() == 1 && steady.last_ms == 700);
  sd::ChangeLog moving;
  moving.Add(0, " 00000005");
  moving.Add(100, " 00000005");
  moving.Add(200, " 00000004");
  moving.Add(300, " 00000004");
  moving.Add(400, " 00000003");
  assert(moving.Changed() && moving.samples.size() == 3);
  assert(moving.samples[1].ms == 200 && moving.samples[2].text == " 00000003");
  std::printf("  memory over time: only the changes kept\n");
}

void ObjectHistory() {
  namespace oh = xbox360ps5::object_history;
  oh::Ring ring;
  // A JobManager polling with a zero timeout: one entry, counted.
  for (uint32_t n = 0; n < 5000; ++n) ring.Record(1000 + n, 0xF800008C, oh::Op::kWait, 0, 0x102);
  // The main thread releases twice; the poll then succeeds once.
  ring.Record(7000, 0xF8000008, oh::Op::kRelease, 1, 0);
  ring.Record(7001, 0xF8000008, oh::Op::kRelease, 1, 1);
  ring.Record(7002, 0xF800008C, oh::Op::kWait, 0, 0);
  auto entries = ring.Entries();
  assert(ring.total() == 5003 && entries.size() == 4);
  assert(entries[0].repeats == 5000 && entries[0].first_ms == 1000 && entries[0].last_ms == 5999);
  assert(entries[1].op == oh::Op::kRelease && entries[1].result == 0 && entries[2].result == 1);
  assert(entries[3].thread == 0xF800008C && entries[3].result == 0);
  const std::string text = oh::Format(entries, 7002);
  assert(text.find(" [-1003ms F800008C wait 0 -> 00000102 x5000 over 4999ms]") == 0);
  assert(text.find(" [-2ms F8000008 release 1 -> 00000000 x1]") != std::string::npos);
  // More than kSize different operations: the oldest drop out, newest last.
  oh::Ring full;
  for (uint32_t n = 0; n < 40; ++n) full.Record(n, 0xF8000008, oh::Op::kSet, int32_t(n), 0);
  entries = full.Entries();
  assert(entries.size() == oh::Ring::kSize && full.total() == 40);
  assert(entries.front().arg == 40 - int32_t(oh::Ring::kSize) && entries.back().arg == 39);
  assert(oh::Format({}, 0).empty() && std::string(oh::OperationName(oh::Op::kMutantRelease)) == "mutant-release");
  std::printf("  object history: repeats folded, releases kept, ring of %zu, text\n", oh::Ring::kSize);
}
}  // namespace

int main() {
  CodeWindowAroundLr();
  UnreadableWordsShown();
  BackChain();
  BackChainStops();
  DataPointers();
  ThreadStates();
  CallsDecoded();
  ChangesOverTime();
  ObjectHistory();
  std::puts("PASS: STALL report guest memory (fix 24)");
}
