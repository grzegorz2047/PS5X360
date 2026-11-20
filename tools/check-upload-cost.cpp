// SPDX-License-Identifier: MIT
// Fix 62 (tools/source-1.0.2/fixes/62-upload-cost.patch): what an upload's time goes to.
// 1.0.4_candidate_22: Modern Warfare 2's GPU command thread spent 248 ms/s in uploads, 9k a
// second at 27 us each; the summary now splits that into the ordering barrier, the upload
// buffer, marking the pages valid and the copy, with the bytes and the copy rate.
//  (a) include/xbox360ps5/frame_cost.hpp: UploadTimer counts on the GPU thread only, each mark
//      takes the time since the previous one, Skip drops it; UploadReporter gives the parts per
//      second, the pieces, their average size, MB/s and the copy rate, since its previous line;
//  (b) the assembled tree: the marks in SharedMemory::UploadRanges in order (barrier after
//      SubmitBarriers, buffer after RequestPartial, valid after MakeRangeValid, copy after the
//      copy) and the line in the 30-second summary after fix 58's.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "xbox360ps5/frame_cost.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

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
namespace fc = xbox360ps5::frame_cost;
void Spin(uint64_t ns) {
  const uint64_t end = fc::NowNs() + ns;
  while (fc::NowNs() < end) {
  }
}
double After(const std::string& line, const char* name) {
  const size_t at = line.find(name);
  return at == std::string::npos ? -1.0 : std::atof(line.c_str() + at + std::strlen(name));
}

void CheckTimer() {
  fc::UploadReporter reporter;
  (void)reporter.Line(1);  // starts the window
  {
    fc::UploadTimer not_gpu;
    Spin(1000000);
    not_gpu.Mark(fc::kUploadCopy);
    not_gpu.Copied(4096);
  }
  Expect(fc::upload_calls == 0 && fc::upload_bytes == 0, "counted on the GPU thread only");
  const uint64_t start = fc::NowNs();
  std::thread([] {
    fc::MarkGpuThread();
    fc::UploadTimer timer;
    Spin(10000000);  // 10 ms barrier
    timer.Mark(fc::kUploadBarrier);
    for (int piece = 0; piece < 2; ++piece) {
      Spin(20000000);  // 20 ms that is none of the parts
      timer.Skip();
      Spin(5000000);  // 5 ms buffer
      timer.Mark(fc::kUploadBuffer);
      Spin(15000000);  // 15 ms marking valid
      timer.Mark(fc::kUploadValid);
      Spin(10000000);  // 10 ms copying 1 MiB
      timer.Mark(fc::kUploadCopy);
      timer.Copied(1 << 20);
    }
  }).join();
  const double seconds = double(fc::NowNs() - start) / 1e9;
  const std::string line = reporter.Line(seconds);
  std::printf("   %s\n", line.c_str());
  const auto ms = [&](const char* name) { return After(line, name) * seconds; };
  const double barrier = ms("barriers "), buffer = ms("upload buffers "), valid = ms("marking valid "),
               copy = ms("copying ");
  Expect(barrier > 8 && barrier < 14, "the barrier: 10 ms (" + std::to_string(barrier) + ")");
  Expect(buffer > 8 && buffer < 14, "the buffers: 2 x 5 ms, the skipped time not in it (" + std::to_string(buffer) + ")");
  Expect(valid > 27 && valid < 36, "marking valid: 2 x 15 ms (" + std::to_string(valid) + ")");
  Expect(copy > 17 && copy < 25, "copying: 2 x 10 ms (" + std::to_string(copy) + ")");
  Expect(line.find("Frame time uploads (GPU thread): ") == 0, "the line");
  Expect(After(line, "pieces/s of ") > 1023 && After(line, "pieces/s of ") < 1025, "1 MiB pieces on average");
  const double rate = After(line, "ms/s (");
  Expect(rate > 0.08 && rate < 0.12, "the copy rate: 2 MiB in 20 ms, about 0.1 GB/s (" + std::to_string(rate) + ")");
  const std::string next = reporter.Line(1);
  Expect(next.find(": 0/s, 0 pieces/s") != std::string::npos && next.find("copying 0.0 ms/s") != std::string::npos,
         "only since the previous line");
}

void CheckTree() {
  const std::string code = Read(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_shared_memory.cc");
  const size_t upload = code.find("bool VulkanSharedMemory::UploadRanges(");
  const auto after = [&](const std::string& what, size_t from) {
    const size_t at = code.find(what, from);
    return at == std::string::npos ? std::string::npos : at + what.size();
  };
  const size_t timer = after("xbox360ps5::frame_cost::UploadTimer upload_timer;", upload);
  const size_t barriers = after("command_processor_.SubmitBarriers(true);", timer);
  const size_t barrier = after("upload_timer.Mark(xbox360ps5::frame_cost::kUploadBarrier);", barriers);
  const size_t skip = after("upload_timer.Skip();", barrier);
  const size_t request = after("upload_buffer_pool_->RequestPartial(", skip);
  const size_t buffer = after("upload_timer.Mark(xbox360ps5::frame_cost::kUploadBuffer);", request);
  const size_t valid_call = after("MakeRangeValid(upload_range_start << page_size_log2(),", buffer);
  const size_t valid = after("upload_timer.Mark(xbox360ps5::frame_cost::kUploadValid);", valid_call);
  const size_t copy_call = after("std::memcpy(", valid);
  const size_t copy = after("upload_timer.Mark(xbox360ps5::frame_cost::kUploadCopy);", copy_call);
  const size_t copied = after("upload_timer.Copied(upload_buffer_size);", copy);
  Expect(upload != std::string::npos && copied != std::string::npos,
         "UploadRanges marks the barrier, the buffer, marking valid and the copy, in that order");
  const std::string main_code = Read("src/canary_game_main.cpp");
  const size_t places = main_code.find("XELOGW(\"{}\", frame_cost_places.Line(double(summary_samples)));");
  const size_t uploads = main_code.find("XELOGW(\"{}\", frame_cost_uploads.Line(double(summary_samples)));");
  Expect(places != std::string::npos && uploads != std::string::npos && places < uploads,
         "the summary gives the line after fix 58's");
}

}  // namespace

int main() {
  CheckTimer();
  CheckTree();
  if (failures) {
    std::printf("check-upload-cost: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-upload-cost: ok\n");
  return 0;
}
