// SPDX-License-Identifier: MIT
// Fix 42 (tools/source-1.0.2/fixes/42-network-self-test.patch): online mode's network self-test
// (NetworkSelfTest, canary_game_main.cpp) logs what the official 1.0.2's does: libcurl's trace
// and each step of the request timed from the test's start, the resolver's report a line at a
// time, and a plain TCP connection to 166.117.125.185:443.
// Drives xbox360ps5::network_self_test (port/network_self_test.hpp): the elapsed milliseconds,
// the trace text, the report's lines, the outcome text and the TCP probe against host sockets
// (a listener on the loopback address, a closed port, an address that never answers: bounded by
// the timeout), then checks the assembled NetworkSelfTest uses them with 1.0.2's format strings.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "../port/network_self_test.hpp"  // the assembled tree's port/ (run from <tree>/tools)

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace nst = xbox360ps5::network_self_test;
using std::chrono::milliseconds;
using std::chrono::nanoseconds;

void Elapsed() {
  const nst::Clock::time_point start{};
  assert(nst::ElapsedMs(start, start) == 0);
  assert(nst::ElapsedMs(start, start + nanoseconds(999999)) == 0);  // truncated, not rounded
  assert(nst::ElapsedMs(start, start + nanoseconds(1000000)) == 1);
  assert(nst::ElapsedMs(start, start + nanoseconds(1999999)) == 1);
  assert(nst::ElapsedMs(start, start + milliseconds(20000) + nanoseconds(5)) == 20000);
  // 1.0.2's (now - start) / 1000000 on signed nanoseconds truncates toward zero.
  assert(nst::ElapsedMs(start + nanoseconds(1500000), start) == -1);
  const nst::Clock::time_point real = nst::Clock::now();
  assert(nst::ElapsedMs(real, nst::Clock::now()) >= 0);
  std::printf("  elapsed: whole milliseconds, truncated toward zero\n");
}

void Trace() {
  const char record[] = "  Trying 166.117.125.185:443...\n";
  assert(nst::TraceText(record, sizeof(record) - 1) == "  Trying 166.117.125.185:443...");
  const char crlf[] = "Connected to xenia\r\n\r\n";
  assert(nst::TraceText(crlf, sizeof(crlf) - 1) == "Connected to xenia");
  // Only the record's `size` bytes: libcurl's buffer is not NUL-terminated.
  const char longer[] = "ALPN: offers h2\nnext record";
  assert(nst::TraceText(longer, 15) == "ALPN: offers h2");
  assert(nst::TraceText(longer, 16) == "ALPN: offers h2");
  assert(nst::TraceText("\n", 1).empty());
  assert(nst::TraceText("", 0).empty());
  assert(nst::TraceText(nullptr, 5).empty());
  const char inner[] = "a\nb\n";
  assert(nst::TraceText(inner, 4) == "a\nb");  // line ends inside are kept, as 1.0.2
  std::printf("  curl trace: the record without its trailing line ends\n");
}

std::vector<std::string> Lines(const char* report) {
  std::vector<std::string> lines;
  for (const std::string_view line : nst::ReportLines(report)) lines.emplace_back(line);
  return lines;
}

void Report() {
  using List = std::vector<std::string>;
  assert(Lines(nullptr).empty());
  assert(Lines("").empty());
  assert(Lines("\n").empty());
  assert(Lines("\n\n\n").empty());
  // ps5_resolver.cpp's Note: one "name: step; step" line per lookup, each ending in '\n'.
  const char report[] =
      "xenia.example: system resolver 3 ms; ok 166.117.125.185\n"
      "socket mode: descriptor flag set, console option refused (errno 42)\n";
  assert((Lines(report) == List{"xenia.example: system resolver 3 ms; ok 166.117.125.185",
                                "socket mode: descriptor flag set, console option refused (errno 42)"}));
  assert((Lines("only line, no end") == List{"only line, no end"}));
  assert((Lines("a\n\nb\n\n") == List{"a", "b"}));  // empty lines left out
  assert((Lines("\nfirst\nsecond") == List{"first", "second"}));
  // The views point into the buffer, which the self-test keeps until it has logged them.
  const char* buffer = "one\ntwo\n";
  const auto views = nst::ReportLines(buffer);
  assert(views.size() == 2 && views[0].data() == buffer && views[1].data() == buffer + 4);
  // The 0x640-byte buffer as ps5x_resolver_report fills it (snprintf: always NUL-terminated).
  char full[nst::kResolverReportSize];
  std::string many;
  for (int n = 0; n < 8; ++n) many += "host" + std::to_string(n) + ": " + std::string(240, 'x') + "\n";
  std::snprintf(full, sizeof(full), "%s", many.c_str());
  const auto cut = Lines(full);
  assert(cut.size() == 7 && cut[0].rfind("host0: ", 0) == 0 && cut[6].rfind("host6: ", 0) == 0);
  assert(cut[6].size() < 247);  // the line the buffer's end cut short
  static_assert(nst::kResolverReportSize == 1600);
  std::printf("  resolver report: one line per lookup, empty lines left out, 0x640 bytes\n");
}

void Text() {
  nst::TcpProbeResult result;
  assert(std::strcmp(nst::TcpProbeText(result), "failed") == 0);
  result.connected = true;
  assert(std::strcmp(nst::TcpProbeText(result), "connected") == 0);
  std::printf("  outcome: \"connected\" / \"failed\", 1.0.2's words\n");
}

// A listener on the loopback address; its port.
int Listen(int* port) {
  const int sock = socket(AF_INET, SOCK_STREAM, 0);
  assert(sock >= 0);
  sockaddr_in at{};
  at.sin_family = AF_INET;
  at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(bind(sock, reinterpret_cast<sockaddr*>(&at), sizeof(at)) == 0);
  assert(listen(sock, 4) == 0);
  socklen_t length = sizeof(at);
  assert(getsockname(sock, reinterpret_cast<sockaddr*>(&at), &length) == 0);
  *port = ntohs(at.sin_port);
  return sock;
}

long long Timed(nst::TcpProbeResult* result, const char* address, int port, int timeout_ms) {
  const auto start = nst::Clock::now();
  *result = nst::ProbeTcp(address, std::uint16_t(port), timeout_ms);
  return nst::ElapsedMs(start, nst::Clock::now());
}

void Probe() {
  int port = 0;
  const int listener = Listen(&port);
  nst::TcpProbeResult result;
  long long took = Timed(&result, "127.0.0.1", port, 2000);
  assert(result.connected && result.error == 0);
  assert(took < 2000);
  close(listener);
  std::printf("  TCP probe: connected to a listener (%lld ms), errno 0\n", took);

  // The same port with nothing listening: refused at once.
  took = Timed(&result, "127.0.0.1", port, 2000);
  assert(!result.connected && result.error == ECONNREFUSED);
  assert(took < 2000);
  std::printf("  TCP probe: closed port failed with ECONNREFUSED (%lld ms)\n", took);

  // TEST-NET-1 (RFC 5737) never answers: the probe gives up at its timeout, or sooner where the
  // host has no route. Either way it fails and never waits much past the limit.
  took = Timed(&result, "192.0.2.1", 443, 300);
  assert(!result.connected && result.error != 0);
  assert(took < 300 + 700);
  std::printf("  TCP probe: unanswered address failed after %lld ms (errno %d, limit 300 ms)\n", took,
              result.error);

  result = nst::ProbeTcp("not an address", 443, 300);
  assert(!result.connected && result.error == EINVAL);
  result = nst::ProbeTcp(nullptr, 443, 300);
  assert(!result.connected && result.error == EINVAL);

  // The address and the bound the self-test uses.
  in_addr official{};
  assert(inet_pton(AF_INET, nst::kTcpProbeAddress, &official) == 1);
  assert(std::strcmp(nst::kTcpProbeAddress, "166.117.125.185") == 0);
  assert(nst::kTcpProbePort == 443);
  // 1.0.2's sockaddr dword 0xbb010210 (stored little-endian: bytes 10 02 01 bb): sin_len 16,
  // AF_INET (2), then the port's bytes 01 bb in network order, 443.
  constexpr unsigned kOfficialHead = 0xbb010210u;
  static_assert((kOfficialHead & 0xff) == 16 && ((kOfficialHead >> 8) & 0xff) == 2);
  static_assert(((((kOfficialHead >> 16) & 0xff) << 8) | (kOfficialHead >> 24)) == 443);
  assert(nst::kTcpProbeTimeoutMs > 0 && nst::kTcpProbeTimeoutMs <= 5000);
  std::printf("  TCP probe: 166.117.125.185:443, at most %d ms\n", nst::kTcpProbeTimeoutMs);
}

std::string ReadTree(const char* path) {
  std::ifstream file(path, std::ios::binary);
  assert(file && "run from the assembled tree's root");
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}

bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// The official 1.0.2's six strings that the published source lacked (eboot-v1.0.2.elf virtual
// addresses), and the ones it kept.
void SelfTestUsesIt() {
  std::string source = ReadTree("src/canary_game_main.cpp");
  std::string flat;
  for (char c : source)
    if (c != '\r') flat += c;
  source = flat;
  assert(Has(source, "#include \"network_self_test.hpp\""));
  assert(Has(source, "extern \"C\" void ps5x_resolver_report(char* out, size_t size);"));
  const auto trace = source.find("int NetSelfTestTrace(CURL*, curl_infotype type, char* data, size_t size, void*) {");
  const auto begin = source.find("void NetworkSelfTest(const std::string& server) {");
  assert(trace != std::string::npos && begin != std::string::npos && trace < begin);
  const auto end = source.find("\n}\n", begin);
  assert(end != std::string::npos);
  const std::string callback = source.substr(trace, begin - trace);
  const std::string body = source.substr(begin, end - begin);

  // 0x1dd2963, the trace callback 0x82f40: CURLINFO_TEXT only, timed.
  assert(Has(callback, "if (type != CURLINFO_TEXT) return 0;"));
  assert(Has(callback, "XELOGW(\"Network test: curl +{} ms: {}\", NetSelfTestMs(), line);"));
  assert(Has(callback, "network_self_test::TraceText(data, size)"));
  assert(!Has(source, "\"Network test: curl: {}\""));  // the published format is gone
  // The start, first thing in the test (0x73d27, before the pipe line).
  const auto started = body.find("net_self_test_start = nst::Clock::now();");
  assert(started != std::string::npos && started < body.find("pipe(ends)"));
  // 0x1dc9e80, 0x1d52f77, 0x1d12669 around curl_easy_cleanup.
  const auto request = body.find("XELOGW(\"Network test: +{} ms {} -> {} ({}; {}), HTTP {}\", NetSelfTestMs(), url, int(result),");
  const auto closing = body.find("XELOGW(\"Network test: +{} ms closing the request\", NetSelfTestMs());");
  const auto cleanup = body.find("curl_easy_cleanup(curl);");
  const auto closed = body.find("XELOGW(\"Network test: +{} ms request closed\", NetSelfTestMs());");
  assert(request != std::string::npos && closing != std::string::npos && cleanup != std::string::npos &&
         closed != std::string::npos);
  assert(request < closing && closing < cleanup && cleanup < closed);
  assert(body.find("curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);") < request);
  assert(!Has(source, "\"Network test: {} -> {} ({}; {}), HTTP {}\""));
  // 0x1dafd0b: ps5x_resolver_report into 0x640 bytes, a line each, after the request.
  const auto report = body.find("ps5x_resolver_report(lookups, sizeof(lookups));");
  assert(report != std::string::npos && closed < report);
  assert(Has(body, "char lookups[nst::kResolverReportSize] = {};"));
  const auto lookup = body.find("for (const std::string_view line : nst::ReportLines(lookups)) {\n"
                                "    XELOGW(\"Network test: lookup {}\", line);\n  }");
  assert(lookup != std::string::npos && report < lookup);
  // 0x1dc9eb0: the TCP probe last.
  const auto probe = body.find("nst::ProbeTcp(nst::kTcpProbeAddress, nst::kTcpProbePort, nst::kTcpProbeTimeoutMs);");
  const auto tcp = body.find("XELOGW(\"Network test: TCP 166.117.125.185:443 {} (errno {})\", nst::TcpProbeText(tcp), tcp.error);");
  assert(probe != std::string::npos && tcp != std::string::npos && lookup < probe && probe < tcp);
  // 1.0.2's earlier lines unchanged.
  assert(Has(body, "\"Network test: pipe {} (errno {})\""));
  assert(Has(body, "\"Network test: wake-up channel {} (errno {})\""));
  assert(Has(body, "\"Network test: curl multi handle {}\""));
  assert(Has(body, "\"Network test: curl easy handle failed\""));
  assert(Has(body, "curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, NetSelfTestTrace);"));
  assert(Has(body, "curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);"));
  // Still started on a thread of its own, never on the interface's.
  assert(Has(source, "std::thread(NetworkSelfTest, std::string(cvars::api_address)).detach();"));
  // The resolver report is the title's (ps5_resolver.cpp), with the signature declared above.
  const std::string resolver = ReadTree("port/ps5_resolver.cpp");
  assert(Has(resolver, "extern \"C\" void ps5x_resolver_report(char* out, size_t size) {"));
  std::printf("  NetworkSelfTest: 1.0.2's six lines in its order, the probe on the test's own thread\n");
}
}  // namespace

int main() {
  Elapsed();
  Trace();
  Report();
  Text();
  Probe();
  SelfTestUsesIt();
  std::puts("PASS: network self-test as the official 1.0.2 (fix 42)");
}
