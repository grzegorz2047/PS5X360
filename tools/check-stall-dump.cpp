// SPDX-License-Identifier: MIT
// Fix 24 (tools/source-1.0.2/fixes/24-stall-code-words.patch): drives the STALL report's guest
// memory reading (port/stall_dump.hpp) with fake memory: the 64-byte code window around lr, the
// stack's back chain with each frame's saved return address and first words, unreadable pages,
// the ends of the address space, thread states, call targets, changes over time and the kernel
// objects' wait and release history (xenia/kernel/object_history.h), the pointers a watched
// object holds, and the GPU command processor's writes and waits (xenia/gpu/cp_trace.h). Built against the assembled 1.0.2 tree.
#undef NDEBUG
#include "../port/stall_dump.hpp"  // the assembled tree's port/ (run from <tree>/tools)
#include "xenia/kernel/object_history.h"
#include "xenia/gpu/cp_trace.h"

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
  assert(!sd::LooksLikeData(0xF8000090) && !sd::LooksLikeData(0xFFFFFFFF) && !sd::LooksLikeData(0xFFFF0000));
  assert(sd::LooksLikeData(0xFFC4A410));  // physical memory above the handles: Battlefield 3's job objects
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
void PointersFollowed() {
  FakeMemory memory;
  // Battlefield 3's main thread: its stack object at 7018D5A0 (candidate_10).
  const uint32_t words[8] = {0x40218A00, 3, 0x50B, 0, 0, 0xF0011096, 0, 0x4021C120};
  for (uint32_t n = 0; n < 8; ++n) memory.Put(0x7018D5A0 + n * 4, words[n]);
  auto pointers = sd::Pointers(memory, 0x7018D5A0, 8, 2);
  // The device (40218A00) and the next heap pointer; not 0x50B, not the misaligned F0011096.
  assert(pointers.size() == 2);
  assert(pointers[0].index == 0 && pointers[0].value == 0x40218A00);
  assert(pointers[1].index == 7 && pointers[1].value == 0x4021C120);
  assert(sd::Pointers(memory, 0x7018D5A0, 8, 1).size() == 1);
  // A stack pointer, one into the window itself and a misaligned one are not followed.
  memory.Put(0x7018D5A4, 0x7018D600);
  memory.Put(0x7018D5A8, 0x7018D5B0);
  memory.Put(0x7018D5AC, 0x40000002);
  pointers = sd::Pointers(memory, 0x7018D5A0, 8, 4);
  assert(pointers.size() == 2 && pointers[1].value == 0x4021C120);
  assert(sd::IsStack(0x7018D5A0) && !sd::IsStack(0x40218A00));
  // Its r29 object with readback full (candidate_10): the job memory at FFC4A410 is followed.
  FakeMemory job;
  const uint32_t r29[8] = {0x8401081C, 0xFFC4A410, 3, 0, 0x8401081C, 0xFFC4A780, 0x1E, 0};
  for (uint32_t n = 0; n < 8; ++n) job.Put(0x7018E2F0 + n * 4, r29[n]);
  pointers = sd::Pointers(job, 0x7018E2F0, 8, 2);
  assert(pointers.size() == 2 && pointers[0].value == 0x8401081C && pointers[1].value == 0xFFC4A410);
  std::printf("  pointers followed: heap objects only (the device at 40218A00)\n");
}

void GpuTrace() {
  namespace ct = xbox360ps5::cp_trace;
  ct::Trace trace;
  // A fence written by EVENT_WRITE_SHD with a rising value, and other writes around it.
  for (uint32_t fence = 0x500; fence <= 0x50B; ++fence) {
    trace.Record(1000 + fence, ct::Kind::kEventShd, 0x1FC4A010, fence);
    trace.Record(1000 + fence, ct::Kind::kMemWrite, 0x00100000 + fence * 4, 7);
  }
  trace.Record(3000, ct::Kind::kScratch, 0x1FC4A100, 0x77);
  assert(trace.Total(ct::Kind::kEventShd) == 12 && trace.Total(ct::Kind::kMemWrite) == 12);
  assert(trace.Total(ct::Kind::kScratch) == 1 && trace.Total(ct::Kind::kCondWrite) == 0);
  // The fence's address keeps one entry with its last value and count.
  const auto fence = trace.TargetsWithValue(0x50B);
  assert(fence.size() == 1 && fence[0].address == 0x1FC4A010 && fence[0].count == 12);
  assert(fence[0].kind == ct::Kind::kEventShd && fence[0].first_ms == 1000 + 0x500);
  assert(trace.TargetsWithValue(0x50F).empty());
  // The last writes, oldest first, newest last.
  const auto writes = trace.Writes();
  assert(writes.size() == 25 && writes.back().kind == ct::Kind::kScratch && writes.back().value == 0x77);
  // More addresses than kTargets: the one written longest ago gives way.
  ct::Trace many;
  for (uint32_t n = 0; n < ct::Trace::kTargets + 5; ++n) many.Record(n, ct::Kind::kMemWrite, 0x1000 + n * 4, n);
  const auto targets = many.Targets();
  assert(targets.size() == ct::Trace::kTargets);
  for (const auto& target : targets) assert(target.address >= 0x1000 + 5 * 4);
  assert(many.Writes().size() == ct::Trace::kTargets + 5);
  // More writes than kWrites: the ring keeps the newest.
  for (uint32_t n = 0; n < 100; ++n) many.Record(100 + n, ct::Kind::kScratch, 0x2000, n);
  assert(many.Writes().size() == ct::Trace::kWrites && many.Writes().back().value == 99);
  assert(many.Writes().front().value == 100 - ct::Trace::kWrites);
  // A WAIT_REG_MEM that does not match: its state while it lasts, cleared when it ends.
  trace.BeginWait(4000, true, 0x1FC4A020, 0x50F, 0xFFFFFFFF, 3);
  trace.Poll(0x50B);
  trace.Poll(0x50B);
  auto wait = trace.Wait();
  assert(wait.active && wait.memory && wait.address == 0x1FC4A020 && wait.ref == 0x50F);
  assert(wait.polls == 2 && wait.value == 0x50B && wait.function == 3 && wait.since_ms == 4000);
  assert(trace.Waits() == 1);
  trace.EndWait();
  assert(!trace.Wait().active);
  trace.SetPointers(0x120, 0x120);
  assert(trace.ReadPointer() == 0x120 && trace.WritePointer() == 0x120);
  assert(std::string(ct::KindName(ct::Kind::kEventShd)) == "EVENT_WRITE_SHD");
  std::printf("  gpu trace: fence address and value found, targets bounded, wait state, ring pointers\n");
}
// The GPU's view of physical memory and the CPU's three views of it, as separate maps, so a
// write that reached one and not the others can be shown.
struct FakeViews {
  std::map<uint32_t, uint32_t> physical, virt;
  bool Physical(uint32_t at, uint32_t& value) {
    auto found = physical.find(at);
    value = found == physical.end() ? 0 : found->second;
    return at < 0x20000000;
  }
  bool Virtual(uint32_t at, uint32_t& value) {
    auto found = virt.find(at);
    if (found != virt.end()) {
      value = found->second;
      return true;
    }
    // No CPU-side difference recorded: the view shows the physical memory.
    uint32_t physical_address = at >= 0xE0000000 ? at - 0xE0000000 + 0x1000
                                : at >= 0xC0000000 ? at - 0xC0000000 : at - 0xA0000000;
    return Physical(physical_address, value);
  }
};

void AwaitedDword() {
  namespace ct = xbox360ps5::cp_trace;
  // Battlefield 3 on candidate_16: WAIT_REG_MEM on memory 1F9BA0FE (endian 8in32) == FFFFFFFF.
  assert(ct::PacketAddress(0x1F9BA0FE) == 0x1F9BA0FC && std::string(ct::EndianName(0x1F9BA0FE)) == "8in32");
  assert(std::string(ct::EndianName(0x1F9BA0FC)) == "none" && std::string(ct::EndianName(0x1F9BA0FF)) == "16in32");
  const auto aliases = ct::Aliases(0x1F9BA0FC);
  assert(aliases.size() == 3);
  assert(aliases[0].address == 0xBF9BA0FC && aliases[1].address == 0xDF9BA0FC && aliases[2].address == 0xFF9B90FC);
  assert(ct::Aliases(0x800).size() == 2 && ct::Aliases(0x20000000).empty());
  {
    // (a) Nobody wrote it: all views agree on 0, the value is nowhere near.
    FakeViews views;
    const std::string text = ct::AliasReport(views, 0x1F9BA0FC, 0xFFFFFFFF, 0xFFFFFFFF);
    assert(text.find("physical 1F9BA0FC = 00000000") == 0);
    assert(text.find("E0000000 view FF9B90FC = 00000000") != std::string::npos);
    assert(text.find("the views agree") != std::string::npos);
    assert(text.find("nowhere within 8 KiB") != std::string::npos);
  }
  {
    // (b) A CPU write through the 0xE0000000 view without its 4 KiB offset: FFFFFFFF lands 0x1000
    // below the awaited dword, and that view shows something else at the awaited address.
    FakeViews views;
    views.physical[0x1F9BA0FC - 0x1000] = 0xFFFFFFFF;
    views.virt[0xFF9B90FC] = 0x12345678;
    const std::string text = ct::AliasReport(views, 0x1F9BA0FC, 0xFFFFFFFF, 0xFFFFFFFF);
    assert(text.find("THE VIEWS DIFFER") != std::string::npos);
    assert(text.find("near at 1F9B90FC(-4096)") != std::string::npos);
  }
  {
    // The masked comparison: only the masked bits must match.
    FakeViews views;
    views.physical[0x1F9BA100] = 0x000000FF;
    const std::string text = ct::AliasReport(views, 0x1F9BA0FC, 0x000000FF, 0x000000FF);
    assert(text.find("1F9BA100(+4)") != std::string::npos);
  }
  // A wait's first value, and its report once it has lasted 2 s (only once).
  ct::Trace trace;
  trace.BeginWait(1000, true, 0x1F9BA0FE, 0xFFFFFFFF, 0xFFFFFFFF, 3, 0x0E7F1000);
  trace.Poll(0);
  trace.Poll(5);
  assert(trace.Wait().first_value == 0 && trace.Wait().value == 5 && trace.Wait().packet == 0x0E7F1000);
  assert(!trace.DueReport(2999, 2000) && trace.DueReport(3000, 2000) && !trace.DueReport(9000, 2000));
  trace.EndWait();
  assert(!trace.DueReport(9000, 2000));
  std::printf("  awaited dword: address and endian, the three views, a write 4 KiB off found, 2 s report once\n");
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
  PointersFollowed();
  GpuTrace();
  AwaitedDword();
  std::puts("PASS: STALL report guest memory (fix 24)");
}
