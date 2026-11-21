// SPDX-License-Identifier: MIT
// Fix 91 (tools/source-1.0.2/fixes/91-readback-repeat.patch): the fast resolve readback gives
// the first resolves of a key after a pause their own copy. Tekken Tag Tournament 2 (issue 14)
// resolves its costume colours to one 240 KiB key three times over a few frames as a match
// loads and reads the last; the fast mode hands a copy over at the key's next resolve, so the
// last one never arrived and the game read the second's (zeros): King's and Bob's clothes came
// out cyan and blue, right with the full mode.
//
// Drives xe::gpu::vulkan::readback_slots (the assembled core's readback_slots.h) as the fast
// mode in vulkan_command_processor.cc does (the GPU keeping up, so every earlier copy is done):
//  * Tekken's three resolves frames apart: each gets its own copy (before: the previous one's),
//    and again at the next match after a pause;
//  * a key resolved every frame or twice a frame: its own copy for the first kFreshResolves,
//    then the fast mode's lateness (the previous resolve's copy);
//  * a key resolved rarely (pauses longer than kIdleFrames): always its own copy;
//  * and the assembled vulkan_command_processor.cc keeps the run and asks the rule in fast mode
//    only, never for the automatic copy.
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

// One key's slots in fast mode. Resolve() returns the number of the resolve whose copy the guest
// gets (resolves are numbered from 1).
struct Key {
  uint64_t submissions[rs::kCount] = {};
  uint32_t valid[rs::kCount] = {};
  int copy_of[rs::kCount] = {};
  uint64_t submission = 0, last_used_frame = 0;
  uint32_t run = 0;
  int resolves = 0;
  bool fix91 = true;
  int Resolve(uint64_t frame, uint32_t length = 245760) {
    const int number = ++resolves;
    ++submission;
    const uint64_t completed = submission - 1;  // the GPU keeps up
    const uint64_t previous = last_used_frame;
    last_used_frame = frame;
    const uint32_t write = rs::ChooseWrite(submissions, valid, rs::kCount, length, completed);
    const int newest = rs::ChooseRead(submissions, valid, rs::kCount, int(write), length, completed, false);
    bool miss = newest < 0;
    if (fix91 && rs::OwnCopyAfterPause(previous, frame, run) && newest >= 0) miss = true;
    submissions[write] = submission;
    valid[write] = length;
    copy_of[write] = number;
    return miss ? number : copy_of[newest];
  }
};

std::string ReadText(const char* path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

int main() {
  CHECK(rs::kFreshResolves == 4 && rs::kIdleFrames == 30);
  // The rule: never used (frame 0) or a pause resets the run; kFreshResolves own copies.
  {
    uint32_t run = 0;
    CHECK(rs::OwnCopyAfterPause(0, 5, run) && run == 1);
    CHECK(rs::OwnCopyAfterPause(5, 6, run) && rs::OwnCopyAfterPause(6, 7, run) &&
          rs::OwnCopyAfterPause(7, 8, run) && run == 4);
    CHECK(!rs::OwnCopyAfterPause(8, 9, run) && run == 4);
    CHECK(!rs::OwnCopyAfterPause(9, 9 + rs::kIdleFrames, run));      // not a pause yet
    CHECK(rs::OwnCopyAfterPause(39, 40 + rs::kIdleFrames, run) && run == 1);  // a pause
  }
  // Tekken's costume colours: three resolves of 0FE7C000 frames apart as a match loads (the
  // 1.0.5-work-fix91 log: lines 425, 433, 435), then the next match after a pause.
  {
    Key fixed, before;
    before.fix91 = false;
    CHECK(fixed.Resolve(100) == 1 && fixed.Resolve(102) == 2 && fixed.Resolve(104) == 3);
    CHECK(before.Resolve(100) == 1 && before.Resolve(102) == 1 && before.Resolve(104) == 2);  // the bug
    CHECK(fixed.Resolve(900) == 4 && fixed.Resolve(901) == 5 && fixed.Resolve(903) == 6);
  }
  // A key resolved every frame (bloom, luminance): its own copy 4 times, then the previous one's.
  {
    Key k;
    for (uint64_t frame = 1; frame <= 4; ++frame) CHECK(k.Resolve(frame) == int(frame));
    for (uint64_t frame = 5; frame <= 40; ++frame) CHECK(k.Resolve(frame) == int(frame) - 1);
  }
  // Twice a frame through a fight (the keys 1.0.5-work-fix91 waited on 125 times): 4 waits in
  // all, then late by one.
  {
    Key k;
    int own = 0;
    for (uint64_t frame = 1; frame <= 50; ++frame)
      for (int twice = 0; twice < 2; ++twice) {
        const int given = k.Resolve(frame, 4096);
        own += given == k.resolves;
        if (k.resolves > 4) CHECK(given == k.resolves - 1);
      }
    CHECK(own == 4);
  }
  // A key resolved every 40 frames (each time after a pause): always its own copy.
  {
    Key k;
    for (int n = 1; n <= 6; ++n) CHECK(k.Resolve(uint64_t(n) * 40) == n);
  }

  const std::string cp = ReadText(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_command_processor.cc");
  const std::string h = ReadText(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_command_processor.h");
  if (cp.empty() || h.empty()) {
    std::printf("no assembled tree here; the command processor not checked\n");
  } else {
    CHECK(h.find("uint32_t fresh_run = 0;  // fix 91") != std::string::npos);
    // The previous use read before it is overwritten.
    const size_t previous = cp.find("const uint64_t previous_use_frame = rb.last_used_frame;  // fix 91");
    CHECK(previous != std::string::npos &&
          cp.find("rb.last_used_frame = frame_current_;", previous) == previous + 71);
    const size_t miss = cp.find("bool read_cache_miss = readback_mode == ReadbackResolveMode::kFast &&");
    const size_t rule = cp.find("readback_slots::OwnCopyAfterPause(previous_use_frame, frame_current_, rb.fresh_run)");
    const size_t scaled = cp.find("if (is_scaled) {", miss);
    CHECK(previous < miss && miss != std::string::npos && miss < rule && rule < scaled);
    // Fast mode only, never the automatic copy (it never waits); the run counts the first resolve.
    const std::string guard =
        rule == std::string::npos ? std::string() : cp.substr(rule > 120 ? rule - 120 : 0, 280);
    CHECK(guard.find("readback_mode == ReadbackResolveMode::kFast && !auto_readback &&") != std::string::npos);
    CHECK(guard.find("rb.fresh_run) &&\n        newest_copy >= 0)") != std::string::npos);
    CHECK(rule != std::string::npos && cp.find("read_cache_miss = true;", rule) < scaled);
    // The fast mode's miss path still gives this copy after waiting; the old same-frame rule is gone.
    CHECK(cp.find("} else if (read_cache_miss) {\n      // Delayed sync, but the previous buffer doesn't exist") !=
          std::string::npos);
    CHECK(cp.find("RepeatedInFrame") == std::string::npos);
    std::printf("tree: the fast readback gives a key's first resolves after a pause their own copy\n");
  }
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-readback-repeat: all passed\n");
  return 0;
}
