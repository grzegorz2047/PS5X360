// SPDX-License-Identifier: MIT
// Fix 24, candidate 21 (tools/source-1.0.2/fixes/24-stall-code-words.patch;
// research/bf3/ANALYSIS.md): the STALL report's decode of the command buffer whose label the GPU
// waits on (xenia/gpu/cp_trace.h), on the ring words Battlefield 3 left in candidate 19's log;
// each case of the run/finish handshake by name; the JIT's entry probes
// (xenia/cpu/entry_probe.h): inserted only for the expected code, counted always, logged only
// when enabled and only the first lines; and the scan for Battlefield 3's render jobs
// (port/stall_dump.hpp FindWord). Built against the assembled 1.0.2 tree.
#undef NDEBUG
#include "../port/stall_dump.hpp"  // the assembled tree's port/ (run from <tree>/tools)
#include "xenia/cpu/entry_probe.h"
#include "xenia/gpu/cp_trace.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {
namespace ct = xbox360ps5::cp_trace;
namespace ep = xbox360ps5::entry_probe;
namespace sd = xbox360ps5::stall_dump;

bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
bool AnyHas(const std::vector<std::string>& lines, const std::string& part) {
  for (const auto& line : lines)
    if (Has(line, part)) return true;
  return false;
}

// Physical memory (as the GPU reads it) and virtual memory (as the CPU sees it), word by word.
struct FakeMemory {
  std::map<uint32_t, uint32_t> physical, virtual_words;
  std::set<uint32_t> virtual_pages;
  int reads = 0;
  void PutVirtual(uint32_t address, uint32_t value) {
    virtual_pages.insert(address >> 12);
    virtual_words[address] = value;
  }
  bool Physical(uint32_t address, uint32_t& value) {
    auto found = physical.find(address);
    if (found == physical.end()) return false;
    value = found->second;
    return true;
  }
  bool Virtual(uint32_t address, uint32_t& value) {
    if (!virtual_pages.count(address >> 12)) return false;
    auto found = virtual_words.find(address);
    value = found == virtual_words.end() ? 0 : found->second;
    return true;
  }
  // For stall_dump.
  bool Readable(uint32_t address) { return virtual_pages.count(address >> 12) != 0; }
  uint32_t Word(uint32_t address) {
    assert(Readable(address));
    ++reads;
    auto found = virtual_words.find(address);
    return found == virtual_words.end() ? 0 : found->second;
  }
};

constexpr uint32_t kPacket = 0x1FA944F0, kAwaited = 0x1F9BA0FC, kObject = 0x51708A60;

// The ring at the hang (bf3-c19.log, "STALL gpu packets at physical ...") and the command
// buffer with the given handshake fields.
FakeMemory Hang(uint32_t flags, uint32_t finish_count, uint32_t label) {
  FakeMemory memory;
  const uint32_t ring[] = {0x1F9B9980, 0x00000030, 0x00022080, 0x00000000, 0x00000000, 0x02C00500,
                           0x0000057A, 0x945C2298, 0xC0043C00, 0x00000013, 0x1F9BA0FE, 0xFFFFFFFF,
                           0xFFFFFFFF, 0x00000100, 0x0000057A, 0x00000000, 0x0000057B, 0x51708A62,
                           0xC0006300, 0x00000000, 0xC0003B00, 0x00007FFF, 0xC0001000, 0x77777777};
  for (uint32_t n = 0; n < sizeof(ring) / 4; ++n) memory.physical[0x1FA944D0 + n * 4] = ring[n];
  memory.physical[kAwaited] = 0;
  for (uint32_t n = 0; n < 40; ++n) memory.PutVirtual(kObject + n * 4, 0x11110000 + n);
  memory.PutVirtual(kObject + 0x08, 0x0000050F);
  memory.PutVirtual(kObject + 0x68, flags);
  memory.PutVirtual(kObject + 0x7C, finish_count);
  memory.PutVirtual(kObject + 0x80, label);
  return memory;
}

void DecodesTheStuckRing() {
  // The tag: 80000000 or C0000000 | object >> 2 (82FD6AE0 inserts object >> 2 into bits 2-31).
  assert(ct::IsLabelTag(0x0000057A, 0x945C2298) && ct::IsLabelTag(0x0000057A, 0xD45C2298));
  assert(!ct::IsLabelTag(0x0000057B, 0x945C2298) && !ct::IsLabelTag(0x0000057A, 0x145C2298));
  assert(ct::LabelObject(0x945C2298) == kObject && ct::LabelObject(0xD45C2298) == kObject);
  // The label's views: A0000000/C0000000 as is, E0000000 0x1000 above (as the emitter at 82FD6B00).
  assert(ct::LabelPhysical(0xBF9BA0FC) == kAwaited && ct::LabelPhysical(0xDF9BA0FC) == kAwaited);
  assert(ct::LabelPhysical(0xFF9B90FC) == kAwaited);

  FakeMemory memory = Hang(0x20, 0, 0xBF9BA0FC);
  const ct::LabelOwner owner = ct::FindLabelOwner(memory, kPacket, kAwaited);
  assert(owner.tagged && owner.readable && owner.tag == 0x945C2298 && owner.object == kObject);
  assert(owner.fence == 0x50F && owner.flags == 0x20 && owner.finish_count == 0 && owner.label == 0xBF9BA0FC);
  assert(owner.label_matches && owner.words.size() == 40 && owner.words[1] == 0x11110001);
  assert(owner.label_case == ct::LabelCase::kFinishNeverStarted);
  const auto lines = ct::LabelOwnerLines(owner);
  assert(lines.size() == 2 + 5);
  assert(Has(lines[0], "tag 945C2298, command buffer 51708A60") && Has(lines[0], "+68 flags 00000020"));
  assert(Has(lines[0], "+80 label BF9BA0FC (the awaited dword)"));
  assert(Has(lines[1], "never reached the finish"));
  assert(Has(lines[2], "label owner 51708A60: 11110000 11110001 0000050F"));
  assert(Has(lines[6], "label owner 51708AE0:"));
}

void NamesEachCase() {
  struct Row {
    uint32_t flags, finish_count, label;
    ct::LabelCase expected;
    const char* text;
  };
  const Row rows[] = {
      {0x00, 0, 0xBF9BA0FC, ct::LabelCase::kNotRun, "never marked it run"},
      {0x40, 1, 0xBF9BA0FC, ct::LabelCase::kNotRun, "never marked it run"},
      {0x20, 0, 0xBF9BA0FC, ct::LabelCase::kFinishNeverStarted, "never reached the finish"},
      {0x20, 1, 0xBF9BA0FC, ct::LabelCase::kFinishStarted, "a finish began"},
      {0x60, 1, 0xBF9BA0FC, ct::LabelCase::kFinishedNotReleased, "lost the release"},
      {0x60, 1, 0, ct::LabelCase::kReleased, "written back"},
      {0x62, 3, 0, ct::LabelCase::kReleased, "written back"},  // other bits do not matter
  };
  for (const Row& row : rows) {
    assert(ct::ClassifyLabel(row.flags, row.finish_count, row.label) == row.expected);
    FakeMemory memory = Hang(row.flags, row.finish_count, row.label);
    const auto owner = ct::FindLabelOwner(memory, kPacket, kAwaited);
    assert(owner.label_case == row.expected);
    assert(Has(ct::LabelOwnerLines(owner)[1], row.text));
  }
  // A label elsewhere, and a cleared one, are told apart from the awaited dword.
  FakeMemory elsewhere = Hang(0x20, 0, 0xBF9BA100);
  assert(Has(ct::LabelOwnerLines(ct::FindLabelOwner(elsewhere, kPacket, kAwaited))[0], "NOT the awaited dword"));
  FakeMemory cleared = Hang(0x60, 1, 0);
  assert(Has(ct::LabelOwnerLines(ct::FindLabelOwner(cleared, kPacket, kAwaited))[0], "(cleared)"));
}

void NoTagOrUnreadable() {
  // Not after a SCRATCH_REG2 write (the other WAIT_REG_MEMs: fences, registers).
  FakeMemory memory = Hang(0x20, 0, 0xBF9BA0FC);
  memory.physical[kPacket - 8] = 0xC0025800;
  auto owner = ct::FindLabelOwner(memory, kPacket, kAwaited);
  assert(!owner.tagged && Has(ct::LabelOwnerLines(owner)[0], "not a SCRATCH_REG2 tag"));
  // The packet words cannot be read, or the packet is at the start of memory.
  FakeMemory empty;
  assert(!ct::FindLabelOwner(empty, kPacket, kAwaited).tagged);
  assert(!ct::FindLabelOwner(empty, 4, kAwaited).tagged);
  // The command buffer's page cannot be read: shown, not guessed.
  FakeMemory unreadable = Hang(0x20, 0, 0xBF9BA0FC);
  unreadable.virtual_pages.clear();
  owner = ct::FindLabelOwner(unreadable, kPacket, kAwaited);
  assert(owner.tagged && !owner.readable);
  const auto lines = ct::LabelOwnerLines(owner);
  assert(lines.size() == 1 && Has(lines[0], "command buffer 51708A60 unreadable"));
}

// Code words of the decoded BF.Main.xex (research/bf3/disc2-BF.Main.bin) at the probes.
std::map<uint32_t, uint32_t> Bf3Code() {
  std::map<uint32_t, uint32_t> code;
  for (const auto& probe : ep::Probes())
    for (int n = 0; n < ep::kWords; ++n) code[probe.address + 4u * n] = probe.words[n];
  return code;
}

void ProbesOnlyForTheirCode() {
  auto code = Bf3Code();
  int reads = 0;
  auto read = [&](uint32_t address) {
    ++reads;
    auto found = code.find(address);
    return found == code.end() ? 0u : found->second;
  };
  const uint32_t addresses[] = {0x83D2C8A8, 0x82FD54A0, 0x82FD7398, 0x82FD6B78, 0x82FD6CF0,
                               0x8346ED68, 0x8346E3B0, 0x8346D590};
  static_assert(ep::kProbes == 8);
  for (int n = 0; n < ep::kProbes; ++n) {
    assert(ep::Probes()[n].address == addresses[n]);
    assert(ep::Find(addresses[n], read) == n);
  }
  // Any other function: no code read at all.
  reads = 0;
  assert(ep::Find(0x82FD54A4, read) == -1 && ep::Find(0x82000000, read) == -1 && reads == 0);
  // Another executable at the same address (any word differs, the last one too): no probe, and
  // the reads stay within the function's page.
  for (int word = 0; word < ep::kWords; ++word) {
    code = Bf3Code();
    code[0x83D2C8A8 + 4u * word] ^= 1;
    reads = 0;
    assert(ep::Find(0x83D2C8A8, read) == -1);
    assert(reads == word + 1);
  }
  for (const auto& probe : ep::Probes())
    assert((probe.address >> 12) == ((probe.address + 4u * (ep::kWords - 1)) >> 12));
  // The command buffer's register: r4 for begin, release and run, r3 for finish; the job's
  // context r5 at +79C (the recorder reads +79C, +7A0, +7A4).
  assert(ep::Probes()[0].object_reg == -1 && ep::Probes()[0].context_reg == 5 && ep::Probes()[0].context_offset == 0x79C);
  assert(ep::Probes()[1].object_reg == 4 && ep::Probes()[2].object_reg == 3);
  assert(ep::Probes()[3].object_reg == 4 && ep::Probes()[4].object_reg == 4);
  // Candidate 31, the job manager: the dependency's handle (r3+0), the pushed job (r4+18:
  // function, +1C, pending dependencies; r4+50: sequence, priority, affinity), the first
  // dependency link (r3+0: state, next, callback; r3+C: argument, pool).
  for (int n = 0; n < 5; ++n)
    assert(ep::Probes()[n].extra_reg == -1 && ep::Probes()[n].lines == ep::kLinesPerProbe && !ep::Probes()[n].after_render_job);
  assert(ep::Probes()[5].object_reg == -1 && ep::Probes()[5].context_reg == 3 && ep::Probes()[5].context_offset == 0 &&
         ep::Probes()[5].extra_reg == -1);
  assert(ep::Probes()[6].context_reg == 4 && ep::Probes()[6].context_offset == 0x18 && ep::Probes()[6].extra_reg == 4 &&
         ep::Probes()[6].extra_offset == 0x50);
  assert(ep::Probes()[7].context_reg == 3 && ep::Probes()[7].context_offset == 0 && ep::Probes()[7].extra_reg == 3 &&
         ep::Probes()[7].extra_offset == 0xC);
  for (int n = 5; n < 8; ++n) assert(ep::Probes()[n].lines == ep::kJobLines && ep::Probes()[n].after_render_job);
}

void CountsAndLines() {
  ep::Counters counters;
  uint64_t number = 0;
  // Not enabled (another title, or before the launcher knows it): counted, not logged.
  assert(!counters.Enter(2, 100, 7, 0x51708A60, 0x7000, 0x83D23928, &number) && number == 1);
  assert(!counters.IsInstalled(2));
  counters.Installed(2);
  assert(counters.IsInstalled(2) && !counters.IsInstalled(0));
  counters.Enable(true);
  int logged = 0;
  for (int n = 0; n < 100; ++n)
    if (counters.Enter(2, 200 + n, 9, 0x51708A60, 0x7010, 0x83D23928, &number)) ++logged;
  assert(logged == ep::kLinesPerProbe && number == 101);
  const ep::Count count = counters.Get(2);
  assert(count.entered == 101 && count.last_ms == 299 && count.last_thread == 9 && count.last_r3 == 0x51708A60);
  assert(count.last_r4 == 0x7010 && count.last_lr == 0x83D23928 && counters.Get(1).entered == 0);

  assert(ep::FlagNames(0) == "neither run nor finished" && ep::FlagNames(0x20) == "run");
  assert(ep::FlagNames(0x40) == "finished" && ep::FlagNames(0x62) == "run+finished");
  ep::Entry entry;
  entry.number = 3;
  entry.thread = 26;
  entry.lr = 0x8346DF48;
  entry.r = {0x40218A00, 0x51708A60, 0, 1};
  entry.object = 0x51708A60;
  entry.object_read = true;
  entry.object_words = {0x50F, 0x20, 0, 0xBF9BA0FC};
  std::string line = ep::Line(ep::Probes()[4], entry);
  assert(Has(line, "PROBE 82FD6CF0 run command buffer entered (#3) thread id 26 lr 8346DF48"));
  assert(Has(line, "r3 40218A00 r4 51708A60 r5 00000000 r6 00000001"));
  assert(Has(line, "command buffer r4 51708A60: +08 0000050F +68 00000020 (run) +7C 00000000 +80 BF9BA0FC"));
  entry.object_read = false;
  assert(Has(ep::Line(ep::Probes()[4], entry), "command buffer r4 51708A60 unreadable"));
  entry.context = 0x7038F000;
  entry.context_read = true;
  entry.context_words = {0x40218A00, 0x51708A60, 0x51708B00};
  line = ep::Line(ep::Probes()[0], entry);
  assert(Has(line, "render job") && Has(line, "; r5+79C: 40218A00 51708A60 51708B00") && !Has(line, "command buffer"));
  entry.context_read = false;
  assert(Has(ep::Line(ep::Probes()[0], entry), "r5+79C unreadable"));

  assert(Has(ep::Summary(ep::Probes()[0], false, ep::Count{}, 1000), "83D2C8A8 render job: not compiled yet"));
  assert(Has(ep::Summary(ep::Probes()[0], true, ep::Count{}, 1000), "compiled, never entered"));
  assert(Has(ep::Summary(ep::Probes()[2], true, count, 1299),
             "entered 101 times, last -1000 ms by thread id 9 from lr 83D23928, r3 51708A60 r4 00007010"));
}

// The job manager's probes log only once the render job has run, and then up to kJobLines.
void JobProbesAfterTheRenderJob() {
  ep::Counters counters;
  counters.Enable(true);
  uint64_t number = 0;
  // The loading screen's jobs before any render job: counted, not logged.
  for (int n = 0; n < 1000; ++n) assert(!counters.Enter(6, n, 30, 0xFFC49088, 0xFFC4A000, 0x8346EE48, &number));
  assert(number == 1000 && counters.Get(6).entered == 1000);
  // The render job runs (logged: its own probe has no such condition).
  assert(counters.Enter(0, 2000, 30, 0x43AABE00, 0x43AAC330, 0x8346DF44, &number));
  int logged = 0;
  for (int n = 0; n < 1000; ++n)
    if (counters.Enter(6, 3000 + n, 30, 0xFFC49088, 0xFFC4A000, 0x8346EE48, &number)) ++logged;
  assert(logged == ep::kJobLines);
  // Each job probe has its own budget; the D3D probes keep theirs.
  logged = 0;
  for (int n = 0; n < 1000; ++n)
    if (counters.Enter(7, 5000 + n, 6, 0xFFC4F730, 0, 0x8346E234, &number)) ++logged;
  assert(logged == ep::kJobLines);
  // Not enabled (another title): nothing logged whatever ran.
  ep::Counters other;
  assert(!other.Enter(0, 1, 1, 0, 0, 0, &number) && !other.Enter(7, 2, 1, 0, 0, 0, &number));

  // The push line: the job's function, its pending dependencies and its sequence/affinity.
  ep::Entry entry;
  entry.number = 2;
  entry.thread = 6;
  entry.lr = 0x8346EE48;
  entry.r = {0xFFC49088, 0xFFC4A780, 0, 0};
  entry.context = 0xFFC4A780;
  entry.context_read = true;
  entry.context_words = {0, 2, 1};
  entry.extra = 0xFFC4A780;
  entry.extra_read = true;
  entry.extra_words = {0x20FA003F, 0, 3};
  const std::string line = ep::Line(ep::Probes()[6], entry);
  assert(Has(line, "PROBE 8346E3B0 push job entered (#2) thread id 6 lr 8346EE48 r3 FFC49088 r4 FFC4A780"));
  assert(Has(line, "; r4+18: 00000000 00000002 00000001; r4+50: 20FA003F 00000000 00000003"));
  assert(!Has(line, "command buffer"));
  entry.extra_read = false;
  assert(Has(ep::Line(ep::Probes()[6], entry), "; r4+50 unreadable"));
  // The link line (c28's crash: the callback already 0).
  entry.r = {0xFFC4F730, 0xFFC490C0, 0, 0};
  entry.context_words = {0x000002B2, 0xFFC509F0, 0};
  entry.extra_read = true;
  entry.extra_words = {0xFFC4C600, 0xFFC49180, 0};
  assert(Has(ep::Line(ep::Probes()[7], entry),
             "run dependency links entered (#2) thread id 6 lr 8346EE48 r3 FFC4F730 r4 FFC490C0 r5 00000000 r6 00000000"
             "; r3+0: 000002B2 FFC509F0 00000000; r3+C: FFC4C600 FFC49180 00000000"));
}

void FindsJobsByFunction() {
  FakeMemory memory;
  // Two readable pages with a hit each, one page in between that cannot be read (its word is
  // never read), and a hit before the range.
  memory.PutVirtual(0xFFC40018, 0x83D2C8A8);
  memory.PutVirtual(0xFFC40FFC, 0x12345678);
  memory.virtual_words[0xFFC41010] = 0x83D2C8A8;  // page not readable
  memory.PutVirtual(0xFFC42400, 0x83D2C8A8);
  memory.PutVirtual(0xFFC3FFF0, 0x83D2C8A8);
  auto found = sd::FindWord(memory, 0xFFC40000, 0xFFC50000, 0x83D2C8A8, 8);
  assert((found == std::vector<uint32_t>{0xFFC40018, 0xFFC42400}));
  assert(memory.reads == 2 * 1024);
  // The limit, and a range that starts inside a page (and unaligned).
  assert((sd::FindWord(memory, 0xFFC40000, 0xFFC50000, 0x83D2C8A8, 1) == std::vector<uint32_t>{0xFFC40018}));
  assert((sd::FindWord(memory, 0xFFC4001A, 0xFFC50000, 0x83D2C8A8, 8) == std::vector<uint32_t>{0xFFC40018, 0xFFC42400}));
  assert((sd::FindWord(memory, 0xFFC4001C, 0xFFC50000, 0x83D2C8A8, 8) == std::vector<uint32_t>{0xFFC42400}));
  // The end of the address space.
  memory.PutVirtual(0xFFFFFFFC, 0x83D2C8A8);
  assert((sd::FindWord(memory, 0xFFFFF000, 0xFFFFFFFF, 0x83D2C8A8, 8) == std::vector<uint32_t>{0xFFFFFFFC}));
}

}  // namespace

int main() {
  DecodesTheStuckRing();
  NamesEachCase();
  NoTagOrUnreadable();
  ProbesOnlyForTheirCode();
  CountsAndLines();
  JobProbesAfterTheRenderJob();
  FindsJobsByFunction();
  std::printf(
      "PASS: the GPU's awaited label is traced to its command buffer and the missing side of the "
      "handshake is named; entry probes only for Battlefield 3's code (fix 24, candidate 21)\n");
  return 0;
}
