// SPDX-License-Identifier: MIT
// Fix 99 (issue 15): XamAvatarGetAssetsResultSize gives room for an empty XAVATAR_ASSETS while
// the avatar asset calls are stubs, and XamAvatarGetAssets clears the set's header before it
// fails (include/xbox360ps5/avatar_assets.hpp).
//
//   check-avatar-assets        run from an assembled tree's root
//
// Kinect Sports' use (0x82524388 / 0x82524770) on a model heap: with sizes 0 its component count
// came from the next heap block and its loop wrote a 32-byte header per "component" over the heap;
// with the fix the count reads 0 and nothing is written. Then the tree's stubs.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "xbox360ps5/avatar_assets.hpp"

namespace a = xbox360ps5::avatar_assets;

static int failures = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
      ++failures;                                                  \
    }                                                              \
  } while (0)

std::string ReadText(const char* path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

// The title's flow on a model heap: a block of `result_size` bytes, then the next block's bytes
// (another object, here a free-list header with a big word at +8). Returns how many 32-byte
// headers its loop writes; the stub clears kClearedBytes when `fixed`.
uint32_t TitleFlow(uint32_t result_size, bool fixed) {
  std::vector<uint8_t> heap(0x400, 0);
  const uint8_t next_block[] = {0x40, 0x10, 0x00, 0x00, 0x00, 0x00, 0x01, 0x40,
                                0x00, 0x01, 0x20, 0x00, 0x40, 0x44, 0x83, 0x20};
  std::memcpy(heap.data() + result_size, next_block, sizeof(next_block));  // right after the block
  uint8_t* assets = heap.data();
  std::memset(assets, 0, result_size);  // 0x825244C8: memset(assets, 0, result size)
  if (fixed) std::memset(assets, 0, a::kClearedBytes);  // XamAvatarGetAssets, before it fails
  return a::ComponentCount(assets);  // 0x825247F0: the loop's count
}

int main() {
  CHECK(a::kResultBytes >= 0x20 && a::kGpuBytes > 0 && a::kClearedBytes <= a::kResultBytes);
  // Before: sizes 0, the count is the next block's word at +8: thousands of headers over the heap.
  CHECK(TitleFlow(0, false) == 0x00012000);
  // Now: the title clears what it asked for, and the stub clears the header: no component.
  CHECK(TitleFlow(a::kResultBytes, false) == 0);
  CHECK(TitleFlow(a::kResultBytes, true) == 0);
  // The header words the title reads.
  {
    uint8_t set[0x20] = {};
    set[8] = 0x00, set[9] = 0x00, set[10] = 0x00, set[11] = 0x03;
    set[16] = 0x40, set[17] = 0x25, set[18] = 0xA0, set[19] = 0x40;
    CHECK(a::ComponentCount(set) == 3 && a::ComponentArray(set) == 0x4025A040);
  }

  const std::string xam = ReadText(".deps/xenia-canary/src/xenia/kernel/xam/xam_avatar.cc");
  if (xam.empty()) {
    std::printf("no assembled tree here; the stubs not checked\n");
  } else {
    CHECK(xam.find("#include \"xbox360ps5/avatar_assets.hpp\"  // fix 99") != std::string::npos);
    const size_t size = xam.find("dword_result_t XamAvatarGetAssetsResultSize_entry(");
    const size_t get = xam.find("dword_result_t XamAvatarGetAssets_entry(");
    CHECK(size != std::string::npos && get != std::string::npos && size < get);
    const std::string size_body = xam.substr(size, get - size);
    CHECK(size_body.find("*result_buffer_size_ptr = xbox360ps5::avatar_assets::kResultBytes;") != std::string::npos);
    CHECK(size_body.find("*gpu_resource_buffer_size_ptr = xbox360ps5::avatar_assets::kGpuBytes;") !=
          std::string::npos);
    CHECK(size_body.find("*result_buffer_size_ptr = 0;") == std::string::npos);
    CHECK(size_body.find("(fix 99)") != std::string::npos);
    const std::string get_body = xam.substr(get, 900);
    const size_t clear = get_body.find("xbox360ps5::avatar_assets::kClearedBytes);");
    const size_t fail = get_body.find("X_ERROR_FUNCTION_FAILED");
    CHECK(clear != std::string::npos && fail != std::string::npos && clear < fail);
    CHECK(get_body.find("if (result_buffer_ptr) {") < clear);
    std::printf("tree: the avatar asset sizes leave room for an empty set\n");
  }
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-avatar-assets: all passed\n");
  return 0;
}
