// SPDX-License-Identifier: MIT
// Fix 39 (tools/source-1.0.2/fixes/39-net-log-drain.patch): the log stream's DrainNetLog (called
// at every exit, title switch and restart, and from the crash report) waits only when a log
// client is connected. Builds the assembled tree's platform/ps5/net_log.cpp into this check and
// runs it on a local TCP port:
//  * with lines buffered and no client: fix 39 returns at once; 1.0.2's loop (replayed on the same
//    buffer) took 100 x 10 ms + 50 ms, since nothing empties the buffer without a client;
//  * with a client connected: the buffered lines reach it and DrainNetLog returns once they have;
//  * after the client leaves: at once again.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "../platform/ps5/net_log.cpp"  // the assembled tree's (run from <tree>/tools)

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

namespace {
using Clock = std::chrono::steady_clock;

long long Ms(Clock::time_point since) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - since).count();
}

// 1.0.2's DrainNetLog on the same buffer.
void OldDrain() {
  for (int n = 0; n < 100 && xbox360ps5::listener >= 0; ++n) {
    if (xbox360ps5::guard.try_lock()) {
      const bool empty = xbox360ps5::pending.empty();
      xbox360ps5::guard.unlock();
      if (empty) break;
    }
    usleep(10000);
  }
  usleep(50000);
}

void Log(const std::string& line) { xbox360ps5::NetLog(line.data(), line.size()); }

std::string Read(const char* path) {
  std::ifstream file(path, std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}
}  // namespace

int main() {
  // A send to the client that left must not end the check (the console's runtime has no SIGPIPE).
  std::signal(SIGPIPE, SIG_IGN);
  unsigned short port = 0;
  for (unsigned short p = 39100; p < 39200 && !port; ++p)
    if (xbox360ps5::StartNetLog(p)) port = p;
  assert(port);

  // No client: lines stay buffered.
  Log("[X360] boot line\n");
  auto start = Clock::now();
  xbox360ps5::DrainNetLog();
  const long long fixed = Ms(start);
  start = Clock::now();
  OldDrain();
  const long long old = Ms(start);
  std::printf("no client: fix 39 %lld ms, 1.0.2 %lld ms\n", fixed, old);
  assert(fixed < 50);
  assert(old >= 1000);

  // A client connects: it gets what was buffered, and the drain waits for it.
  const int client = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
  std::string received;
  char buffer[4096];
  start = Clock::now();
  while (received.find("boot line") == std::string::npos && Ms(start) < 3000) {
    const ssize_t got = recv(client, buffer, sizeof(buffer), MSG_DONTWAIT);
    if (got > 0) received.append(buffer, size_t(got));
    else usleep(5000);
  }
  assert(received.find("[X360] boot line\n") != std::string::npos);
  assert(xbox360ps5::client_connected.load());
  const std::string big(256 * 1024, 'x');
  Log(big + "\n[X360] exit line\n");
  start = Clock::now();
  xbox360ps5::DrainNetLog();
  const long long with_client = Ms(start);
  {
    std::lock_guard lock(xbox360ps5::guard);
    assert(xbox360ps5::pending.empty());  // the sender took it all before the drain returned
  }
  start = Clock::now();
  while (received.find("exit line") == std::string::npos && Ms(start) < 3000) {
    const ssize_t got = recv(client, buffer, sizeof(buffer), MSG_DONTWAIT);
    if (got > 0) received.append(buffer, size_t(got));
    else usleep(5000);
  }
  assert(received.find("[X360] exit line\n") != std::string::npos);
  std::printf("with a client: drained in %lld ms, %zu bytes received\n", with_client, received.size());
  assert(with_client < 1100);

  // The client leaves: Serve notices at its next send; then the drain is immediate again.
  close(client);
  start = Clock::now();
  while (xbox360ps5::client_connected.load() && Ms(start) < 3000) {
    Log("[X360] after the client left\n");
    usleep(20000);
  }
  assert(!xbox360ps5::client_connected.load());
  start = Clock::now();
  xbox360ps5::DrainNetLog();
  assert(Ms(start) < 50);

  const std::string source = Read("platform/ps5/net_log.cpp");
  assert(source.find("if (!client_connected.load()) return;") != std::string::npos);
  std::puts("check-net-log-drain: all passed");
  return 0;
}
