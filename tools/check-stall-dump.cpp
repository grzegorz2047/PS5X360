// SPDX-License-Identifier: MIT
// Fix 24 (tools/source-1.0.2/fixes/24-stall-code-words.patch): drives the STALL report's guest
// memory reading (port/stall_dump.hpp) with fake memory: the 64-byte code window around lr, the
// stack's back chain with each frame's saved return address and first words, unreadable pages,
// and the ends of the address space. Built against the assembled 1.0.2 tree.
#undef NDEBUG
#include "../port/stall_dump.hpp"  // the assembled tree's port/ (run from <tree>/tools)

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
  assert(!sd::LooksLikeData(0x7062FCB0));  // a stack
  assert(sd::LooksLikeData(0x30083010) && sd::LooksLikeData(0x437F8D80) && sd::LooksLikeData(0x89014FA0));
  assert(!sd::LooksLikeData(0xFFFFFFFF));
  std::printf("  register values dumped: data addresses, not small numbers or stacks\n");
}
}  // namespace

int main() {
  CodeWindowAroundLr();
  UnreadableWordsShown();
  BackChain();
  BackChainStops();
  DataPointers();
  std::puts("PASS: STALL report guest memory (fix 24)");
}
