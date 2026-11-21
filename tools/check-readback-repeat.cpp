// SPDX-License-Identifier: MIT
// Fix 91 (tools/source-1.0.2/fixes/91-readback-repeat.patch): the fast resolve readback gives a
// resolve the previous frame's copy of its own pass. Tekken Tag Tournament 2 (issue 14) resolves
// several keys twice a frame through its fights (two passes drawn into the same place); the fast
// mode gave each pass the copy one resolve late, the other pass's, and King's and Bob's clothes
// came out cyan and blue, right with the full mode.
//
// Drives xe::gpu::vulkan::readback_slots (the assembled core's readback_slots.h) as the fast
// mode in vulkan_command_processor.cc does, each copy labelled with its frame and pass:
//  * twice a frame: pass A gets the previous frame's A, pass B the previous B (before: A got
//    the previous B, B the same frame's A); each pass's first frame gets its own copy;
//  * three passes a frame fit the four slots the same way; five degrade to own copies (never
//    another pass's copy);
//  * once a frame, every other frame and a few times at load: exactly what fix 35 gave;
//  * and the assembled vulkan_command_processor.cc counts the pass, stamps and clears it, and
//    asks the pass-aware choices.
// Built against the assembled tree and run from its root (run-fix-checks.sh).
#include "xenia/gpu/vulkan/readback_slots.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace rs = xe::gpu::vulkan::readback_slots;

static int failures = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
      ++failures;                                                  \
    }                                                              \
  } while (0)

// A copy's label: frame * 10 + pass.
constexpr int Label(uint64_t frame, uint32_t pass) { return int(frame) * 10 + int(pass); }

// One key in fast mode (the GPU keeping up). Resolve() returns the label of the copy the guest
// gets; `fix91` false runs fix 35's choices instead.
struct Key {
  uint64_t submissions[rs::kCount] = {};
  uint32_t valid[rs::kCount] = {};
  uint32_t passes[rs::kCount] = {};
  uint64_t frames[rs::kCount] = {};
  int label[rs::kCount] = {};
  uint64_t submission = 0, pass_frame = 0;
  uint32_t pass = 0;
  bool fix91 = true;
  int Resolve(uint64_t frame, uint32_t length = 4096) {
    if (pass_frame != frame) {
      pass_frame = frame;
      pass = 0;
    }
    const uint32_t this_pass = ++pass;
    ++submission;
    const uint64_t completed = submission - 1;
    uint32_t write;
    int newest;
    if (fix91) {
      write = rs::ChooseWrite(submissions, valid, passes, rs::kCount, length);
      newest = rs::ChooseRead(submissions, valid, passes, frames, rs::kCount, int(write), length, this_pass,
                              frame, completed, false);
    } else {
      write = rs::ChooseWrite(submissions, valid, rs::kCount, length, completed);
      newest = rs::ChooseRead(submissions, valid, rs::kCount, int(write), length, completed, false);
    }
    submissions[write] = submission;
    valid[write] = length;
    passes[write] = this_pass;
    frames[write] = frame;
    label[write] = Label(frame, this_pass);
    return newest < 0 ? Label(frame, this_pass) : label[newest];  // a miss: its own copy
  }
};

std::string ReadText(const char* path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

int main() {
  // Twice a frame: each pass the previous frame's copy of itself.
  {
    Key fixed, before;
    before.fix91 = false;
    CHECK(fixed.Resolve(1) == Label(1, 1) && fixed.Resolve(1) == Label(1, 2));  // first frame: own
    for (uint64_t f = 2; f <= 40; ++f) {
      CHECK(fixed.Resolve(f) == Label(f - 1, 1));
      CHECK(fixed.Resolve(f) == Label(f - 1, 2));
    }
    before.Resolve(1);
    before.Resolve(1);
    for (uint64_t f = 2; f <= 10; ++f) {
      CHECK(before.Resolve(f) == Label(f - 1, 2));  // the bug: A got the previous B
      CHECK(before.Resolve(f) == Label(f, 1));      // and B the same frame's A
    }
  }
  // Three passes a frame fit the four slots.
  {
    Key k;
    for (uint32_t p = 1; p <= 3; ++p) CHECK(k.Resolve(1) == Label(1, p));
    for (uint64_t f = 2; f <= 30; ++f)
      for (uint32_t p = 1; p <= 3; ++p) CHECK(k.Resolve(f) == Label(f - 1, p));
  }
  // Five passes (more than the slots): some get their own copy, none another pass's.
  {
    Key k;
    int own = 0;
    for (uint64_t f = 1; f <= 30; ++f)
      for (uint32_t p = 1; p <= 5; ++p) {
        const int given = k.Resolve(f);
        CHECK(given % 10 == int(p));
        CHECK(given == Label(f, p) || given / 10 < int(f));
        own += given == Label(f, p);
      }
    CHECK(own > 0);
  }
  // Once a frame, every other frame, and Tekken's costume key at load (one resolve a frame,
  // frames apart): exactly what fix 35 gave.
  {
    for (uint64_t step : {1u, 2u, 3u}) {
      Key fixed, before;
      before.fix91 = false;
      for (uint64_t f = 100; f <= 160; f += step) CHECK(fixed.Resolve(f, 245760) == before.Resolve(f, 245760));
    }
  }

  const std::string cp = ReadText(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_command_processor.cc");
  const std::string h = ReadText(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_command_processor.h");
  if (cp.empty() || h.empty()) {
    std::printf("no assembled tree here; the command processor not checked\n");
  } else {
    CHECK(h.find("uint32_t passes[readback_slots::kCount] = {};") != std::string::npos);
    CHECK(h.find("uint64_t frames[readback_slots::kCount] = {};") != std::string::npos);
    // The pass counted per frame before the slot is chosen.
    const size_t counted = cp.find("const uint32_t pass = ++rb.pass;");
    const size_t write = cp.find("uint32_t write_index = readback_slots::ChooseWrite(\n"
                                 "        rb.submissions, rb.valid_sizes, rb.passes, readback_slots::kCount,");
    const size_t read = cp.find("rb.passes, rb.frames,\n                                         readback_slots::kCount,");
    CHECK(cp.find("if (rb.pass_frame != frame_current_) {") < counted);
    CHECK(counted != std::string::npos && counted < write && write != std::string::npos && write < read &&
          read != std::string::npos);
    CHECK(cp.find("pass, frame_current_,\n                                         readback_completed, auto_readback)", read) !=
          std::string::npos);
    // Stamped with the copy, cleared with a new buffer.
    CHECK(cp.find("rb.passes[write_index] = pass;  // fix 91\n    rb.frames[write_index] = frame_current_;") !=
          std::string::npos);
    CHECK(cp.find("rb.passes[write_index] = 0;  // fix 91\n      rb.frames[write_index] = 0;") != std::string::npos);
    CHECK(cp.find("is resolved {} times in a frame: each resolve") != std::string::npos);
    // The earlier tries are gone.
    CHECK(cp.find("OwnCopyAfterPause") == std::string::npos && cp.find("RepeatedInFrame") == std::string::npos);
    std::printf("tree: the fast readback gives each pass the previous frame's copy of itself\n");
  }
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-readback-repeat: all passed\n");
  return 0;
}
