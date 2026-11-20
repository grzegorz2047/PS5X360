// SPDX-License-Identifier: MIT
// Fix 48 (tools/source-1.0.2/fixes/48-memexport-loss.patch): exported data that guest memory
// lacks (a memory export not read back) is lost when the GPU's copy of its 4 KiB unit is
// uploaded again from guest memory; Call of Duty: Black Ops (41560855, issue #10) drew black,
// patterned skin that way before fix 30, and its hair is wrong in every build.
//  (a) include/xbox360ps5/memexport_loss.hpp: the readback decision (fix 30's option and small
//      exports first, then the learned one), unit bitmaps across 64-unit words, an upload over
//      units holding GPU-only data (lost, marked lossy once, no longer GPU-only; other units and
//      repeated uploads lose nothing), the first losses and the 30-second summary.
//  (b) a model of a frame: a 100 KiB export not read back, the CPU writing one of its units (an
//      upload of that unit), and the next frame's same export read back as learned; a small
//      export stays "small"; an export read back clears its units first, so fix 30's own
//      refresh upload is no loss.
//  (d) 1.0.4_candidate_5: fix 30's refresh (an exact invalidation of the ranges before the draw)
//      only for fix 30's own readbacks (the option, small exports): on a learned readback of
//      Black Ops' 0.7-1.4 MB export ranges it threw away resolved textures there (candidate 2:
//      blocky corrupted textures, while the hair was right).
//  (c) the assembled tree has the hooks: SharedMemory's bitmaps and NoteUpload before
//      UploadRanges, the Vulkan command processor's decision, ClearGpuOnly before the readback's
//      refresh and before an exporting draw requests its own ranges (it writes them again: an
//      upload for it loses nothing), MarkGpuOnly after a draw whose exports are not read back,
//      and the summary.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "../include/xbox360ps5/memexport_loss.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace ml = xbox360ps5::memexport_loss;

namespace {

int failures = 0;
void Expect(bool condition, const std::string& what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what.c_str());
    ++failures;
  }
}
bool Has(const std::vector<std::string>& lines, const std::string& part) {
  for (const auto& line : lines)
    if (line.find(part) != std::string::npos) return true;
  return false;
}

void CheckDecide() {
  using R = ml::Reason;
  Expect(ml::Decide(false, 0, 65536, true) == R::kNone, "nothing exported: nothing read back");
  Expect(ml::Decide(true, 1 << 20, 65536, false) == R::kOption, "the option reads back all");
  Expect(ml::Decide(false, 65536, 65536, false) == R::kSmall, "64 KiB is small (fix 30)");
  Expect(ml::Decide(false, 65537, 65536, false) == R::kNone, "above 64 KiB is not read back");
  Expect(ml::Decide(false, 65537, 65536, true) == R::kLearned, "above 64 KiB where data was lost: learned");
  Expect(ml::Decide(false, 4096, 0, false) == R::kNone, "small readback switched off");
  Expect(ml::Decide(false, 4096, 0, true) == R::kLearned, "learned readback with small readback off");
  Expect(ml::Decide(false, 4096, 65536, true) == R::kSmall, "small wins over learned (same readback)");
}

void CheckBits() {
  std::vector<uint64_t> bits(4, 0);
  ml::SetUnits(bits, 60, 70);
  Expect(bits[0] == (UINT64_MAX << 60) && bits[1] == 0x7F, "a range across two words");
  Expect(ml::AnyUnits(bits, 70, 80) && !ml::AnyUnits(bits, 71, 200) && !ml::AnyUnits(bits, 0, 59), "AnyUnits");
  ml::ClearUnits(bits, 64, 64);
  Expect(bits[1] == 0x7E, "clear one unit");
  ml::SetUnits(bits, 0, 255);
  Expect(bits[0] == UINT64_MAX && bits[3] == UINT64_MAX, "whole words");
  ml::SetUnits(bits, 250, 400);  // past the end: no write out of range
  Expect(bits.size() == 4, "the bitmap does not grow");
}

void CheckModel() {
  std::vector<uint64_t> gpu_only(64, 0), lossy(64, 0);
  constexpr uint32_t kLog2 = 12;
  const auto range_units = [](uint32_t start, uint32_t bytes, uint32_t& first, uint32_t& last) {
    first = start >> kLog2;
    last = (start + bytes - 1) >> kLog2;
  };
  // Frame 1: a 100 KiB export at 16F78000 (Black Ops' exports are near 16F34000-17010000),
  // not read back.
  const uint32_t start = 0x00078000, bytes = 100 * 1024;  // inside the model's 256 units
  uint32_t first, last;
  range_units(start, bytes, first, last);
  const auto reason1 = ml::Decide(false, bytes, 65536, ml::AnyUnits(lossy, first, last));
  Expect(reason1 == ml::Reason::kNone, "frame 1: not read back");
  ml::NoteExport(bytes, reason1, 0xABCDEF0123456789ull);
  ml::SetUnits(gpu_only, first, last);  // MarkGpuOnly after the draw
  // An upload of units outside the export loses nothing.
  Expect(ml::NoteUpload(gpu_only, lossy, last + 1, 4, kLog2) == 0, "an upload elsewhere loses nothing");
  // The CPU writes into the export's third unit: that unit is uploaded again.
  Expect(ml::NoteUpload(gpu_only, lossy, first + 2, 1, kLog2) == 1, "one unit of exported data lost");
  Expect(ml::NoteUpload(gpu_only, lossy, first + 2, 1, kLog2) == 0, "uploaded again: nothing more to lose");
  Expect(ml::counters.lost_units.load() == 1 && ml::counters.learned_units.load() == 1, "counted once");
  // Frame 2: the same export is now read back (learned); before it, its units are cleared, so
  // fix 30's refresh upload of the ranges loses nothing.
  const auto reason2 = ml::Decide(false, bytes, 65536, ml::AnyUnits(lossy, first, last));
  Expect(reason2 == ml::Reason::kLearned, "frame 2: learned readback");
  ml::NoteExport(bytes, reason2, 0xABCDEF0123456789ull);
  ml::ClearUnits(gpu_only, first, last);
  Expect(ml::NoteUpload(gpu_only, lossy, first, last - first + 1, kLog2) == 0, "the readback's refresh loses nothing");
  // A small export elsewhere stays small.
  Expect(ml::Decide(false, 20480, 65536, ml::AnyUnits(lossy, 2, 6)) == ml::Reason::kSmall, "a small export stays small");
  ml::NoteExport(20480, ml::Reason::kSmall, 1);
  // A second loss in another place, a run of 3 units across a word boundary.
  ml::SetUnits(gpu_only, 62, 66);
  Expect(ml::NoteUpload(gpu_only, lossy, 63, 3, kLog2) == 3, "three units across words");

  ml::Reporter reporter;
  const auto lines = reporter.Lines(30);
  for (const auto& line : lines) std::printf("   %s\n", line.c_str());
  Expect(Has(lines, "Memexport: 0.1 exporting draws/s"), "rate of exporting draws");
  Expect(Has(lines, "largest 102400 bytes, shader ABCDEF0123456789"), "the largest export not read back and its shader");
  Expect(Has(lines, "4 units of exported data lost to uploads (2 uploads), 4 units newly learned (4 in all)"),
         "units lost and learned");
  Expect(Has(lines, "lost at physical 0007A000 (1 units"), "the first loss's address");
  Expect(Has(lines, "lost at physical 0003F000 (3 units"), "the second loss's address");
  Expect(reporter.Lines(30).empty(), "nothing new: no line");
}

std::string Read(const char* path) {
  std::ifstream file(path, std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}
void ExpectIn(const std::string& path, const std::vector<std::string>& parts) {
  const std::string text = Read(path.c_str());
  Expect(!text.empty(), "read " + path);
  for (const auto& part : parts) Expect(text.find(part) != std::string::npos, path + " has: " + part);
}

void CheckTree() {
  const std::string gpu = ".deps/xenia-canary/src/xenia/gpu/";
  ExpectIn(gpu + "shared_memory.h", {"void MarkGpuOnly(uint32_t start, uint32_t length);",
                                     "bool AnyLossy(uint32_t start, uint32_t length) const;",
                                     "std::vector<uint64_t> gpu_only_bits_;"});
  const std::string shared = Read((gpu + "shared_memory.cc").c_str());
  const size_t note = shared.find("xbox360ps5::memexport_loss::NoteUpload(gpu_only_bits_, lossy_bits_,");
  const size_t upload = shared.find("bool usable = !current_upload_range || UploadRanges(uploads, current_upload_range);");
  Expect(note != std::string::npos && upload != std::string::npos && note < upload, "losses noted before the upload");
  Expect(shared.find("gpu_only_bits_.assign(num_system_page_flags_, 0);") != std::string::npos, "bitmaps sized");
  const std::string vk = Read((gpu + "vulkan/vulkan_command_processor.cc").c_str());
  const size_t decide = vk.find("xbox360ps5::memexport_loss::Decide(");
  const size_t clear = vk.find("shared_memory_->ClearGpuOnly(", decide);
  const size_t refresh = vk.find("xbox360ps5::memexport_merge::RefreshRanges(*shared_memory_, ranges);", decide);
  const size_t mark = vk.find("shared_memory_->MarkGpuOnly(");
  const size_t written = vk.rfind("shared_memory_->RangeWrittenByGpu(memexport_range.base_address_dwords << 2,", mark);
  Expect(decide != std::string::npos, "the decision");
  Expect(clear != std::string::npos && refresh != std::string::npos && clear < refresh, "units cleared before the refresh");
  // 1.0.4_candidate_5: only fix 30's own readbacks refresh the ranges (an exact invalidation of a
  // learned export's large ranges corrupted Black Ops' textures in candidate 2).
  const size_t only30 = vk.rfind("if (memexport_fix30) {", refresh);
  Expect(only30 != std::string::npos && only30 > decide && refresh - only30 < 120,
         "the refresh only for fix 30's own readbacks");
  const size_t own = vk.find("shared_memory_->ClearGpuOnly(memexport_range_base_bytes,");
  const size_t own_request = vk.find("shared_memory_->RequestRange(memexport_range_base_bytes,", own);
  Expect(own != std::string::npos && own_request != std::string::npos && own_request - own < 200,
         "an exporting draw's own request of its ranges is no loss");
  Expect(mark != std::string::npos && written != std::string::npos && mark - written < 400 &&
             vk.find("if (!memexport_readback) {", written) < mark,
         "GPU-only marked with RangeWrittenByGpu, only when not read back");
  ExpectIn("src/canary_game_main.cpp", {"static xbox360ps5::memexport_loss::Reporter memexport_reporter;"});
}

}  // namespace

int main() {
  CheckDecide();
  CheckBits();
  CheckModel();
  CheckTree();
  if (failures) {
    std::printf("check-memexport-loss: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-memexport-loss: ok\n");
  return 0;
}
