// SPDX-License-Identifier: MIT
// Fix 91 diagnostic (issue 14): the resolve readback mode per key from readback-keys.txt
// (include/xbox360ps5/readback_keys.hpp) and the keys in the log.
//
//   check-readback-keys        run from an assembled tree's root
//
// The file's lines (default, address, address/length, comments, nonsense), which line wins, and
// the command processor's use: read again when the file changes, only the fast and full settings
// changed (never readback off or its automatic copy), the keys logged.
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "xbox360ps5/readback_keys.hpp"

namespace rk = xbox360ps5::readback_keys;

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

int main() {
  // Nothing in the file: every key as set.
  {
    const rk::Rules none = rk::Parse("");
    CHECK(none.default_mode == rk::Mode::kAsSet && none.keys.empty() && none.not_understood == 0);
    CHECK(rk::Lookup(none, 0x0FE7C000, 245760) == rk::Mode::kAsSet);
  }
  // One key full (the first bisect step), the rest as set.
  {
    const rk::Rules one = rk::Parse("0FE7C000 full\n");
    CHECK(rk::Lookup(one, 0x0FE7C000, 245760) == rk::Mode::kFull);
    CHECK(rk::Lookup(one, 0x0FE7C000, 4096) == rk::Mode::kFull);  // any length
    CHECK(rk::Lookup(one, 0x0E338000, 4227072) == rk::Mode::kAsSet);
  }
  // "All but one": default full, one key fast; a length line wins over the address line.
  {
    const rk::Rules rules = rk::Parse(
        "# Tekken, step 3\r\n"
        "default full\n"
        "0E338000 fast   # the 4 MiB one\n"
        "0x0FD7C000/4096 fast\n"
        "0FD7C000 full\n"
        "10020000/0x1000 fast\n"
        "\n"
        "   \t\n");
    CHECK(rules.default_mode == rk::Mode::kFull && rules.keys.size() == 4 && rules.not_understood == 0);
    CHECK(rk::Lookup(rules, 0x0E338000, 4227072) == rk::Mode::kFast);
    CHECK(rk::Lookup(rules, 0x0FD7C000, 4096) == rk::Mode::kFast);
    CHECK(rk::Lookup(rules, 0x0FD7C000, 65536) == rk::Mode::kFull);
    CHECK(rk::Lookup(rules, 0x10020000, 4096) == rk::Mode::kFast);
    CHECK(rk::Lookup(rules, 0x10020000, 16384) == rk::Mode::kFull);  // the default
    CHECK(rk::Lookup(rules, 0x0D0F0000, 3768320) == rk::Mode::kFull);
  }
  // Lines that are not rules are counted and ignored.
  {
    const rk::Rules bad = rk::Parse("0FE7C000\nzzz full\n0FE7C000 slow\n0FE7C000/0 full\n0FE7C000 full extra\n"
                                    "default\n1FFFFFFFF full\n0D0F0000/3768320 fast\n");
    CHECK(bad.not_understood == 7 && bad.keys.size() == 1);
    CHECK(rk::Lookup(bad, 0x0D0F0000, 3768320) == rk::Mode::kFast);
  }
  CHECK(std::string(rk::Name(rk::Mode::kFull)) == "full" && std::string(rk::Name(rk::Mode::kFast)) == "fast");
  // 1.0.5-work-fix91g: the read watch forced on keys, and the simpler rule switched on.
  {
    const rk::Rules rules = rk::Parse("watch 0F05C000\nwatch 0x0E338000\noccasional own\ndefault fast\n");
    CHECK(rules.not_understood == 0 && rules.watch.size() == 2 && rules.occasional_own &&
          rules.default_mode == rk::Mode::kFast && rules.keys.empty());
    CHECK(rk::Watched(rules, 0x0F05C000) && rk::Watched(rules, 0x0E338000) && !rk::Watched(rules, 0x10020000));
    CHECK(!rk::Parse("occasional off\n").occasional_own && !rk::Parse("").occasional_own);
    const rk::Rules bad = rk::Parse("watch\nwatch zz\nwatch 0F05C000 full\noccasional\noccasional maybe\n");
    CHECK(bad.not_understood == 5 && bad.watch.empty() && !bad.occasional_own);
  }

  const std::string cp = ReadText(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_command_processor.cc");
  if (cp.empty()) {
    std::printf("no assembled tree here; the command processor not checked\n");
  } else {
    CHECK(cp.find("#include \"xbox360ps5/readback_keys.hpp\"  // fix 91 diagnostic") != std::string::npos);
    // The file's places, read again when it changes.
    CHECK(cp.find("XBOX360PS5_DATA_ROOT \"/readback-keys.txt\"") != std::string::npos);
    CHECK(cp.find("\"/download0/xbox360ps5/readback-keys.txt\"") != std::string::npos);
    CHECK(cp.find("if (found != loaded_from || stamp != loaded_stamp) {") != std::string::npos);
    // Only the fast and full settings, never the automatic copy.
    CHECK(cp.find("if (!automatic && length &&\n      (mode == ReadbackResolveMode::kFast || mode == "
                  "ReadbackResolveMode::kFull)) {") != std::string::npos);
    // Called with the resolve's key before the mode is used.
    const size_t call = cp.find("ReadbackKeysDiagnostic(written_address, written_length,");
    const size_t auto_set = cp.find("readback_mode = ReadbackResolveMode::kFast;\n  }");
    const size_t used = cp.find("if (readback_mode == ReadbackResolveMode::kDisabled || !written_length) {");
    CHECK(auto_set != std::string::npos && auto_set < call && call < used && used != std::string::npos);
    CHECK(cp.find("first seen (fix 91") != std::string::npos);
    CHECK(cp.find("\"Readback keys, last 30 s: {}\"") != std::string::npos);
    // The switches reach the command processor (fix 91g).
    CHECK(cp.find("xbox360ps5::readback_keys::Rules g_readback_rules;") != std::string::npos);
    CHECK(cp.find("xbox360ps5::readback_keys::Watched(g_readback_rules, written_address);  // fix 91g") !=
          std::string::npos);
    CHECK(cp.find("(rb.cpu_reads || g_readback_rules.occasional_own)") != std::string::npos);
    std::printf("tree: the readback mode per key from readback-keys.txt, and the keys logged\n");
  }
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-readback-keys: all passed\n");
  return 0;
}
