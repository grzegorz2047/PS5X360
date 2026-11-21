// SPDX-License-Identifier: MIT
// Fix 91 (tools/source-1.0.2/fixes/91-readback-repeat.patch): the fast resolve readback gives
// every resolve of a key in its first frames after a pause its own copy. Tekken Tag Tournament 2
// (issue 14) resolves its costume colours to one 240 KiB key several times over a few frames as a
// match loads and reads them; the fast mode hands a copy over at the key's next resolve, so the
// game got each one's predecessor (zeros first) and the last never arrived: King's and Bob's
// clothes came out cyan and blue, right with the full mode.
//
// Drives xe::gpu::vulkan::readback_slots (the assembled core's readback_slots.h) as the fast
// mode in vulkan_command_processor.cc does (the GPU keeping up, so every earlier copy is done):
//  * Tekken's resolves frames apart (more than the 3 a log shows per length): each gets its own
//    copy (before: its predecessor's), and again at the next match after a pause;
//  * a key resolved every frame or twice a frame: its own copy in the first kFreshFrames frames,
//    then the fast mode's lateness (the previous resolve's copy);
//  * a key resolved rarely (pauses longer than kIdleFrames): always its own copy;
//  * a finished run is counted once for the log;
//  * and the assembled vulkan_command_processor.cc keeps the run and asks the rule in fast mode
//    only, never for the automatic copy, and logs the runs and the small keys.
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
// gets (resolves are numbered from 1); `ended` is what a finished run hands to the log.
struct Key {
  uint64_t submissions[rs::kCount] = {};
  uint32_t valid[rs::kCount] = {};
  int copy_of[rs::kCount] = {};
  uint64_t submission = 0, last_used_frame = 0;
  rs::FreshRun run;
  uint32_t ended = 0;
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
    if (fix91 && rs::OwnCopyAfterPause(previous, frame, run, &ended) && newest >= 0) miss = true;
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
  CHECK(rs::kIdleFrames == 30 && rs::kFreshFrames == 20);
  // Tekken's costume colours: resolves of 0FE7C000 frames apart as a match loads, each its own
  // copy; the next match after a pause again, and the first match's run is counted when the
  // second starts.
  {
    Key fixed, before;
    before.fix91 = false;
    const uint64_t load[] = {100, 102, 104, 105, 106, 108, 112};
    int n = 0;
    for (uint64_t frame : load) {
      ++n;
      CHECK(fixed.Resolve(frame) == n);
      const int old = before.Resolve(frame);
      CHECK(n == 1 ? old == 1 : old == n - 1);  // the bug: each one's predecessor
    }
    CHECK(fixed.Resolve(900) == 8 && fixed.ended == 7);
    CHECK(fixed.Resolve(901) == 9 && fixed.Resolve(903) == 10 && fixed.ended == 0);
  }
  // A key resolved every frame (bloom, luminance): its own copy in frames 1-21, then the
  // previous one's; its run is counted once, when the window passes.
  {
    Key k;
    for (uint64_t frame = 1; frame <= 1 + rs::kFreshFrames; ++frame) CHECK(k.Resolve(frame) == int(frame));
    CHECK(k.Resolve(22) == 21 && k.ended == 21);
    for (uint64_t frame = 23; frame <= 60; ++frame) {
      CHECK(k.Resolve(frame) == int(frame) - 1);
      CHECK(k.ended == 0);
    }
  }
  // Twice a frame through a fight (the keys 1.0.5-work-fix91 waited on 125 times): own copies
  // only in the first 21 frames (42), then late by one.
  {
    Key k;
    int own = 0;
    for (uint64_t frame = 1; frame <= 300; ++frame)
      for (int twice = 0; twice < 2; ++twice) {
        const int given = k.Resolve(frame, 4096);
        own += given == k.resolves;
        if (frame > 1 + rs::kFreshFrames) CHECK(given == k.resolves - 1);
      }
    CHECK(own == 42);
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
    CHECK(h.find("readback_slots::FreshRun fresh;  // fix 91") != std::string::npos);
    // The previous use read before it is overwritten.
    const size_t previous = cp.find("const uint64_t previous_use_frame = rb.last_used_frame;  // fix 91");
    CHECK(previous != std::string::npos &&
          cp.find("rb.last_used_frame = frame_current_;", previous) == previous + 71);
    const size_t miss = cp.find("bool read_cache_miss = readback_mode == ReadbackResolveMode::kFast &&");
    const size_t gate = cp.find("if (readback_mode == ReadbackResolveMode::kFast && !auto_readback) {", miss);
    const size_t rule = cp.find("readback_slots::OwnCopyAfterPause(previous_use_frame, frame_current_,", miss);
    const size_t scaled = cp.find("if (is_scaled) {", miss);
    CHECK(previous < miss && miss != std::string::npos && miss < gate && gate < rule && rule < scaled);
    CHECK(rule != std::string::npos && cp.find("rb.fresh, &ended);", rule) < scaled);
    CHECK(rule != std::string::npos && cp.find("if (own && newest_copy >= 0) {", rule) < scaled);
    CHECK(rule != std::string::npos && cp.find("read_cache_miss = true;", rule) < scaled);
    // The fast mode's miss path still gives this copy after waiting; the earlier rules are gone.
    CHECK(cp.find("} else if (read_cache_miss) {\n      // Delayed sync, but the previous buffer doesn't exist") !=
          std::string::npos);
    CHECK(cp.find("RepeatedInFrame") == std::string::npos && cp.find("fresh_run") == std::string::npos);
    // The log: each finished run and the small keys one by one.
    CHECK(cp.find("had {} resolves given their own copy in its") != std::string::npos);
    CHECK(cp.find("if (readback_length <= kAutoReadbackMaxLength && small_shown < 64) {") != std::string::npos);
    std::printf("tree: the fast readback gives a key's resolves in its first frames after a pause their own copy\n");
  }
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-readback-repeat: all passed\n");
  return 0;
}
