// SPDX-License-Identifier: MIT
// Fix 66 (tools/source-1.0.2/fixes/66-phone-latency.patch): the phone camera page's delay by
// stage reaches the game log. tools/check-phone-latency.mjs checks the page; this checks:
//  (a) include/xbox360ps5/phone_latency.hpp: the page's report turns into one line with each
//      stage's median and p95 ("none" for a stage without samples), the frame rate and the
//      queue; a body that is too large, not the report, out of range or without any sample gives
//      none; at most one line every 1.5 s;
//  (b) port/phone_camera_tls.cpp: /latency.mjs is served from the title, /api/motion/latency is
//      answered after the key check and logs the line through Due and Line.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "xbox360ps5/phone_latency.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

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
namespace pl = xbox360ps5::phone_latency;

void CheckLine() {
  const std::string report =
      "{\"fps\":29.8,\"frames\":60,\"queue\":1,\"stages\":{\"camera\":[12.4,20.1,60],\"detect\":[24,31.5,60],"
      "\"track\":[3,5,30],\"network\":[-1,-1,0],\"smoothing\":[90.2,106.1,60]}}";
  const auto line = pl::Line(report);
  Expect(line.has_value(), "the page's report gives a line");
  if (line) std::printf("   %s\n", line->c_str());
  Expect(line && *line ==
                     "Motion: phone stages camera 12 ms (p95 20), detection 24 ms (p95 32), tracking to send 3 ms "
                     "(p95 5), network (half the round trip) none, hand smoothing lag 90 ms (p95 106); 29.8 fps, "
                     "queue 1",
         "each stage's median and p95, none without samples, the rate and the queue");
  Expect(!pl::Line(""), "empty: none");
  Expect(!pl::Line("[1,2]"), "not an object: none");
  Expect(!pl::Line("{\"fps\":30}"), "without the stages: none");
  Expect(!pl::Line(std::string(3000, '{')), "too large: none");
  std::string far = report;
  far.replace(far.find("12.4"), 4, "9e99");
  Expect(!pl::Line(far), "out of range: none");
  const std::string empty =
      "{\"fps\":0,\"frames\":0,\"queue\":0,\"stages\":{\"camera\":[-1,-1,0],\"detect\":[-1,-1,0],\"track\":[-1,-1,0],"
      "\"network\":[-1,-1,0],\"smoothing\":[-1,-1,0]}}";
  Expect(!pl::Line(empty), "no sample at all: no line");
}

void CheckRate() {
  Expect(pl::Due(10000), "the first line");
  Expect(!pl::Due(10500) && !pl::Due(11499), "then none for 1.5 s");
  Expect(pl::Due(11500), "then the next");
}

void CheckTree() {
  const std::string code = Read("port/phone_camera_tls.cpp");
  Expect(code.find("if (method == \"GET\" && target == \"/latency.mjs\")  // fix 66") != std::string::npos &&
             code.find("phone_camera::kLatencyModule") != std::string::npos,
         "latency.mjs is served from the title");
  const size_t key = code.find("if (given != key) return Response(\"403 Forbidden\"");
  const size_t latency = code.find("if (method == \"POST\" && target == \"/api/motion/latency\") {");
  const size_t logged = code.find("if (const auto line = phone_latency::Line(body)) XELOGW(\"{}\", *line);", latency);
  const size_t due = code.find("if (phone_latency::Due(phone_latency::NowMs()))", latency);
  Expect(key != std::string::npos && latency != std::string::npos && key < latency && due < logged &&
             logged != std::string::npos,
         "the report is taken after the key check and logged at most every 1.5 s");
  const std::string header = Read("port/phone_camera_page.hpp");
  Expect(header.find("inline constexpr char kLatencyModule[] = R\"LATENCYJS(") != std::string::npos,
         "the page's header holds latency.mjs");
}

}  // namespace

int main() {
  CheckLine();
  CheckRate();
  CheckTree();
  if (failures) {
    std::printf("check-phone-latency: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-phone-latency: ok\n");
  return 0;
}
