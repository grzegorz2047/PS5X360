// SPDX-License-Identifier: MIT
// Fix 28 (tools/source-1.0.2/fixes/28-notify-first-poll.patch), Naruto: The Broken Bond (55530825).
//
// 1. xenia/kernel/notify_queue.h, the queue of an XNotifyListener: the sign-in change the core
//    sends every System|Live listener when it is created is held until the listener's first
//    XNotifyGetNext has returned. A model of the game's start-up (research/naruto/CAUSE.md: init
//    827EECE8 creates the listener in 8277AB30, polls it once in 8277A4F0, and only then makes the
//    object at 833A3400 that the sign-in handler passes to 827F9D48) crashes with the 1.0.2 queue
//    and does not with the fix; the frame loop's poll then handles the notification.
// 2. xenia/kernel/util/socket_option_posix.h: SO_DONTLINGER (guest ~0x80) and SO_EXCLUSIVEADDRUSE
//    (guest ~0x04), Winsock-only options the 1.0.2 core passed to setsockopt as ~SO_LINGER /
//    ~SO_REUSEADDR (ENOPROTOOPT), on a real host socket.
// 3. The assembled core calls them (kernel_state.cc, xnotifylistener.{h,cc}, xsocket.cc).
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "xenia/kernel/notify_queue.h"
#include "xenia/kernel/util/socket_option_posix.h"

#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
namespace nq = xe::kernel::notify;
namespace so = xe::kernel::socket_option;

constexpr uint32_t kSystemUI = 0x00000009;             // XN_SYS_UI
constexpr uint32_t kSignInChanged = 0x0000000A;        // XN_SYS_SIGNINCHANGED
constexpr uint32_t kLiveConnectionChanged = 0x02000001;  // XN_LIVE_CONNECTIONCHANGED
using Queue = nq::Queue<uint32_t>;

// The 1.0.2 queue: everything available at once (the netplay core's System|Live sign-in too).
struct OldQueue {
  std::vector<std::pair<uint32_t, uint32_t>> items;
  void PushAfterFirstPoll(uint32_t id, uint32_t data) { items.emplace_back(id, data); }
  bool Next(uint32_t* id, uint32_t* data) {
    if (items.empty()) return false;
    *id = items.front().first;
    *data = items.front().second;
    items.erase(items.begin());
    return true;
  }
};

// The game's start-up, as the disassembly of default.xex shows it.
struct NarutoModel {
  bool manager_created = false;  // the global at 833A3400 (made by 827F9EA8)
  bool crashed = false;
  int sign_in_handled = 0;
  // 8277A4F0: one XNotifyGetNext(listener, 0); for XN_SYS_SIGNINCHANGED (and [833A3448] == 0)
  // it calls 827F9D48([833A3400], 0), which reads +0x10 of that object.
  template <typename Q>
  void Poll(Q& queue) {
    uint32_t id = 0, data = 0;
    if (!queue.Next(&id, &data)) return;
    if (id == kSignInChanged) {
      if (!manager_created) {
        crashed = true;  // access violation reading 0x10 at 827F9D74
        return;
      }
      ++sign_in_handled;
    }
  }
  template <typename Q>
  void Run(Q& queue, uint32_t signed_in_players) {
    // KernelState::RegisterNotifyListener for a mask of exactly System|Live, when the listener is
    // not the first System or Live listener (the c20 log: the first notification it got was 0A).
    queue.PushAfterFirstPoll(kSignInChanged, signed_in_players);
    Poll(queue);  // 8277AB30 -> 8277A4F0, inside the call that created the listener
    if (crashed) return;
    manager_created = true;  // 827EED44 -> 827F9EA8
    Poll(queue);             // 827EEAB0 -> 8277A4F0, the frame loop
  }
};

void NarutoStartup() {
  {
    OldQueue old_queue;
    NarutoModel game;
    game.Run(old_queue, 1);
    assert(game.crashed);  // 1.0.2: the crash of the candidate 20 log
  }
  for (uint32_t players : {0u, 1u, 0xFu}) {  // the handler ignores the players: crash either way
    OldQueue old_queue;
    NarutoModel game;
    game.Run(old_queue, players);
    assert(game.crashed);
  }
  {
    Queue queue;
    NarutoModel game;
    game.Run(queue, 1);
    assert(!game.crashed);
    assert(game.sign_in_handled == 1);  // still delivered, by the frame loop's poll
    assert(!queue.pending());
  }
  std::printf("  Naruto's start-up: 1.0.2 queue crashes, fix 28's handles the sign-in a poll later\n");
}

void HeldUntilFirstPoll() {
  Queue queue;
  assert(!queue.pending() && !queue.polled());
  assert(queue.PushAfterFirstPoll(kSignInChanged, 1));  // held
  assert(queue.held() == 1 && queue.available() == 0);
  assert(queue.pending());  // the listener's event is set: a waiting title wakes
  uint32_t id = 0xDEAD, data = 0xDEAD;
  size_t released = 99;
  assert(!queue.Next(&id, &data, &released));  // the first poll gets nothing
  assert(id == 0xDEAD && data == 0xDEAD);      // and writes nothing
  assert(released == 1);
  assert(queue.polled() && queue.held() == 0 && queue.available() == 1);
  assert(queue.pending());  // the event stays set for the next poll
  assert(queue.Next(&id, &data, &released));
  assert(id == kSignInChanged && data == 1 && released == 0);
  assert(!queue.pending());
  assert(!queue.Next(&id, &data, &released) && released == 0);
  // After the first poll nothing is held any more.
  assert(!queue.PushAfterFirstPoll(kSignInChanged, 3));
  assert(queue.Next(&id, &data) && id == kSignInChanged && data == 3);
  std::printf("  held until the first poll, then available; later ones never held\n");
}

void FirstListenerKeepsUpstreamOrder() {
  // The first System and Live listener: upstream's start-up notifications come at once, the
  // netplay core's extra sign-in after the first poll, behind them.
  Queue queue;
  queue.Push(kSystemUI, 0);
  queue.Push(kSignInChanged, 1);
  queue.Push(kLiveConnectionChanged, 0x001510F1);
  assert(queue.PushAfterFirstPoll(kSignInChanged, 1));
  uint32_t id = 0, data = 0;
  size_t released = 0;
  assert(queue.Next(&id, &data, &released) && id == kSystemUI && data == 0 && released == 1);
  assert(queue.Next(&id, &data, &released) && id == kSignInChanged && data == 1 && released == 0);
  assert(queue.Next(&id, &data) && id == kLiveConnectionChanged && data == 0x001510F1);
  assert(queue.Next(&id, &data) && id == kSignInChanged && data == 1);
  assert(!queue.Next(&id, &data) && !queue.pending());
  std::printf("  first listener: upstream's notifications first, the held sign-in last\n");
}

void MatchCountsAsAPoll() {
  Queue queue;
  queue.PushAfterFirstPoll(kSignInChanged, 1);
  uint32_t data = 0xDEAD;
  size_t released = 0;
  assert(!queue.Match(kSignInChanged, &data, &released) && data == 0xDEAD && released == 1);
  assert(queue.Match(kSignInChanged, &data, &released) && data == 1 && released == 0);
  // Match takes the oldest with that id and leaves the others in order.
  queue.Push(kSystemUI, 1);
  queue.Push(kSignInChanged, 2);
  queue.Push(kSystemUI, 0);
  queue.Push(kSignInChanged, 4);
  assert(queue.Match(kSignInChanged, &data) && data == 2);
  assert(!queue.Match(kLiveConnectionChanged, &data));
  uint32_t id = 0;
  assert(queue.Next(&id, &data) && id == kSystemUI && data == 1);
  assert(queue.Next(&id, &data) && id == kSystemUI && data == 0);
  assert(queue.Next(&id, &data) && id == kSignInChanged && data == 4);
  assert(!queue.pending());
  std::printf("  XNotifyGetNext with an id: counts as the first poll, oldest match taken\n");
}

void SaveKeepsEverything() {
  Queue queue;
  queue.Push(kSystemUI, 1);
  queue.PushAfterFirstPoll(kSignInChanged, 1);
  const auto all = queue.All();
  assert(all.size() == 2);
  assert(all[0].first == kSystemUI && all[1].first == kSignInChanged);
  std::printf("  Save lists available then held notifications\n");
}

void SocketOptionsClassified() {
  static_assert(so::kGuestDontLinger == 0xFFFFFF7Fu);
  static_assert(so::kGuestExclusiveAddrUse == 0xFFFFFFFBu);
  // Naruto's call at 82BBE9E0: level 0xFFFF, option -0x81.
  static_assert(so::Classify(0xFFFF, uint32_t(-0x81)) == so::Kind::kDontLinger);
  static_assert(so::Classify(0xFFFF, ~uint32_t(0x0004)) == so::Kind::kExclusiveAddrUse);
  for (uint32_t option : {0x0004u, 0x0020u, 0x0080u, 0x1001u, 0x1002u, 0x1005u, 0x1006u})
    assert(so::Classify(0xFFFF, option) == so::Kind::kPassThrough);
  assert(so::Classify(6, uint32_t(-0x81)) == so::Kind::kPassThrough);  // IPPROTO_TCP
  assert(so::Classify(6, 1) == so::Kind::kPassThrough);                // TCP_NODELAY
  std::printf("  options: SO_DONTLINGER and SO_EXCLUSIVEADDRUSE recognised, the rest unchanged\n");
}

linger Linger(int fd) {
  linger value{};
  socklen_t length = sizeof(value);
  assert(getsockopt(fd, SOL_SOCKET, SO_LINGER, &value, &length) == 0);
  return value;
}

void DontLingerOnAHostSocket() {
  for (int type : {SOCK_DGRAM, SOCK_STREAM}) {
    const int fd = socket(AF_INET, type, 0);
    assert(fd >= 0);
    // The 1.0.2 mapping: ~SO_LINGER is no option on a POSIX host.
    int one = 1;
    errno = 0;
    assert(setsockopt(fd, SOL_SOCKET, ~SO_LINGER, &one, sizeof(one)) != 0);
    assert(errno == ENOPROTOOPT);
    // SO_DONTLINGER TRUE (Naruto passes 1): lingering off.
    assert(so::SetDontLinger(fd, 1) == 0);
    assert(Linger(fd).l_onoff == 0);
    // FALSE: lingering on, the timeout the socket had kept.
    linger seven{1, 7};
    assert(setsockopt(fd, SOL_SOCKET, SO_LINGER, &seven, sizeof(seven)) == 0);
    assert(so::SetDontLinger(fd, 1) == 0);
    assert(Linger(fd).l_onoff == 0 && Linger(fd).l_linger == 7);
    assert(so::SetDontLinger(fd, 0) == 0);
    assert(Linger(fd).l_onoff != 0 && Linger(fd).l_linger == 7);
    // Any non-zero value is TRUE (a big-endian 1 read unswapped included).
    assert(so::SetDontLinger(fd, 0x01000000) == 0);
    assert(Linger(fd).l_onoff == 0);
    close(fd);
  }
  // Not a socket: the error comes back.
  errno = 0;
  assert(so::SetDontLinger(-1, 1) != 0 && errno == EBADF);
  std::printf("  SO_DONTLINGER on UDP and TCP host sockets: SO_LINGER on/off, timeout kept\n");
}

std::string Read(const char* path) {
  std::ifstream file(path, std::ios::binary);
  assert(file && "run from the assembled tree's root");
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}

void CoreUsesThem() {
  const std::string kernel_state = Read(".deps/xenia-canary/src/xenia/kernel/kernel_state.cc");
  const auto block = kernel_state.find("listener->mask() == (kXNotifySystem | kXNotifyLive)");
  assert(block != std::string::npos);
  const auto end = kernel_state.find("\n}\n", block);
  const std::string body = kernel_state.substr(block, end - block);
  assert(body.find("EnqueueNotificationAfterFirstPoll(") != std::string::npos);
  assert(body.find("EnqueueNotification(") == std::string::npos);
  // Upstream's start-up notifications stay immediate.
  const std::string startup = kernel_state.substr(0, block);
  assert(startup.find("listener->EnqueueNotification(kXNotificationSystemSignInChanged, 1);") !=
         std::string::npos);

  const std::string header = Read(".deps/xenia-canary/src/xenia/kernel/xnotifylistener.h");
  assert(header.find("notify::Queue<XNotificationID> notifications_;") != std::string::npos);
  const std::string listener = Read(".deps/xenia-canary/src/xenia/kernel/xnotifylistener.cc");
  assert(listener.find("notifications_.Next(out_id, out_data, &released)") != std::string::npos);
  assert(listener.find("notifications_.Match(id, out_data, &released)") != std::string::npos);
  assert(listener.find("notifications_.pending()") != std::string::npos);

  const std::string socket = Read(".deps/xenia-canary/src/xenia/kernel/xsocket.cc");
  const auto set_option = socket.find("XSocket::SetOption(");
  const auto classify = socket.find("socket_option::Classify(level, optname)");
  const auto generic = socket.find("int ret = setsockopt(native_handle_", set_option);
  assert(set_option != std::string::npos && classify != std::string::npos);
  assert(set_option < classify && classify < generic);  // before the generic mapping
  assert(socket.find("socket_option::SetDontLinger(int(native_handle_), value)") !=
         std::string::npos);
  std::printf("  the core's listener, RegisterNotifyListener and SetOption use them\n");
}
}  // namespace

int main() {
  NarutoStartup();
  HeldUntilFirstPoll();
  FirstListenerKeepsUpstreamOrder();
  MatchCountsAsAPoll();
  SaveKeepsEverything();
  SocketOptionsClassified();
  DontLingerOnAHostSocket();
  CoreUsesThem();
  std::puts("PASS: notifications after the first poll, Winsock-only socket options (fix 28)");
}
