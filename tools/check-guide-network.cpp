// SPDX-License-Identifier: MIT
// Fix 22 (tools/source-1.0.2/fixes/22-guide-network-mode.patch): drives
// xbox360ps5::network_switch::Run, the guide's network switch, with a fake emulator, and the
// sign-in rule UserProfile::signin_state uses. The fake's Select and Convert do what the
// netplay core's XLiveAPI::SelectNetworkMode and ProfileManager::ModifyAccount do with
// notifications (BroadcastNetworkStatus; a sign-out and a sign-in), so the whole sequence a
// game sees is checked. Built against the assembled 1.0.2 tree (port/ include root).
#undef NDEBUG
#include "../port/network_switch.hpp"  // the assembled tree's port/ (run from <tree>/tools)

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace {
namespace ns = xbox360ps5::network_switch;
constexpr uint32_t kLogonConnectionEstablished = 0x001510F0;  // X_ONLINE_S_LOGON_CONNECTION_ESTABLISHED
constexpr uint32_t kLogonDisconnected = 0x001510F1;           // X_ONLINE_S_LOGON_DISCONNECTED

struct Profile {
  uint64_t xuid;
  std::string name;
  bool live_enabled;
  int slot;  // -1: signed out
};

struct FakeEmulator {
  int network_mode = 0;
  bool server_reachable = true;
  bool server_prepared = false;  // the Xenia server set (System Link and online)
  ns::Queue* queue = nullptr;    // a press made while Select runs, when set
  int press_during_select = -1;
  std::vector<Profile> profiles;
  std::vector<std::pair<uint32_t, uint32_t>> notifications;
  std::vector<uint64_t> registered;
  std::string saved;
  int converts = 0;

  void BroadcastNetworkStatus() {
    notifications.push_back({ns::kLiveConnectionChanged,
                             network_mode == 2 ? kLogonConnectionEstablished : kLogonDisconnected});
    notifications.push_back({ns::kLiveLinkStateChanged, network_mode == 0 ? 0u : 1u});
  }
  uint32_t SlotMask() const {
    uint32_t mask = 0;
    for (const auto& profile : profiles)
      if (profile.slot >= 0) mask |= 1u << profile.slot;
    return mask;
  }
  // The sign-in state a game reads for a profile (UserProfile::signin_state).
  bool LiveFor(uint64_t xuid) const {
    for (const auto& profile : profiles)
      if (profile.xuid == xuid) return ns::SignedInToLive(profile.live_enabled, network_mode);
    return false;
  }
};

struct Backend {
  FakeEmulator& emulator;
  int Mode() { return emulator.network_mode; }
  void PrepareServer() { emulator.server_prepared = true; }
  // XLiveAPI::SelectNetworkMode: offline at once; System Link always ends in System Link;
  // online ends in System Link without the server, and then broadcasts nothing.
  bool Select(int mode) {
    if (emulator.queue && emulator.press_during_select >= 0) {
      emulator.queue->Ask(emulator.press_during_select);  // the user presses again meanwhile
      emulator.press_during_select = -1;
    }
    if (mode == 0) {
      emulator.network_mode = 0;
      emulator.BroadcastNetworkStatus();
      return true;
    }
    emulator.network_mode = mode == 2 && emulator.server_reachable ? 2 : 1;
    const bool switched = emulator.network_mode == mode;
    if (switched) emulator.BroadcastNetworkStatus();
    return switched;
  }
  std::vector<ns::Player> Players() {
    std::vector<ns::Player> players;
    for (const auto& profile : emulator.profiles)
      if (profile.slot >= 0)
        players.push_back({uint8_t(profile.slot), profile.xuid, profile.live_enabled, profile.name});
    return players;
  }
  // ProfileManager::ModifyAccount: out (notified), changed, in again (notified).
  bool Convert(uint64_t xuid) {
    ++emulator.converts;
    for (auto& profile : emulator.profiles) {
      if (profile.xuid != xuid) continue;
      const int slot = profile.slot;
      profile.slot = -1;
      emulator.notifications.push_back({ns::kSignInChanged, emulator.SlotMask()});
      profile.live_enabled = true;
      profile.slot = slot;
      emulator.notifications.push_back({ns::kSignInChanged, emulator.SlotMask()});
      if (slot == 0 && emulator.server_reachable) emulator.registered.push_back(xuid);  // Login registers slot 0
      return true;
    }
    return false;
  }
  bool Connected() { return emulator.network_mode != 0 && emulator.server_reachable; }
  void Register(uint64_t xuid) { emulator.registered.push_back(xuid); }
  uint32_t UsedSlots() { return emulator.SlotMask(); }
  void Broadcast(uint32_t id, uint32_t data) { emulator.notifications.push_back({id, data}); }
  bool Save(const char* mode) {
    emulator.saved = mode;
    return true;
  }
};

void SignInRule() {
  for (int mode : {0, 1, 2}) {
    assert(!ns::SignedInToLive(false, mode));
    assert(ns::SignedInToLive(true, mode) == (mode == 2));
  }
  std::printf("  sign-in state: Live only for a Live-enabled profile in mode 2 (0, 1, 2 checked)\n");
}

void OfflineToOnline() {
  FakeEmulator emulator;
  emulator.profiles = {{0xE000000000000001ull, "Player", false, 0}, {0xE000000000000002ull, "Guest", true, 1}};
  Backend backend{emulator};
  assert(!emulator.LiveFor(0xE000000000000001ull) && !emulator.LiveFor(0xE000000000000002ull));
  const ns::Result result = ns::Run(backend, 2);
  assert(result.asked == 2 && result.from == 0 && result.now == 2 && result.selected && result.saved);
  assert(emulator.network_mode == 2 && emulator.server_prepared && emulator.saved == "online");
  // Only the profile that was not Live-enabled is converted; the other was already.
  assert(emulator.converts == 1 && result.profiles == "Player now uses Xbox Live");
  assert(emulator.LiveFor(0xE000000000000001ull) && emulator.LiveFor(0xE000000000000002ull));
  // Slot 0 was converted, so it registered as it signed in again: not twice.
  assert(emulator.registered.size() == 1 && emulator.registered[0] == 0xE000000000000001ull);
  const std::vector<std::pair<uint32_t, uint32_t>> expected = {
      {ns::kLiveConnectionChanged, kLogonConnectionEstablished},
      {ns::kLiveLinkStateChanged, 1},
      {ns::kSignInChanged, 0b10},   // slot 0 signs out for the conversion
      {ns::kSignInChanged, 0b11},   // and in again
      {ns::kSignInChanged, 0b11}};  // the switch's own: read the sign-in state again
  assert(emulator.notifications == expected);
  std::printf("  offline -> online: cvar, conversion, registration, notifications, network.conf\n");
}

void OnlineWithoutServer() {
  FakeEmulator emulator;
  emulator.server_reachable = false;
  emulator.profiles = {{0xE000000000000003ull, "Player", false, 0}};
  Backend backend{emulator};
  const ns::Result result = ns::Run(backend, 2);
  assert(!result.selected && result.now == 1 && emulator.network_mode == 1);
  assert(emulator.converts == 0 && emulator.registered.empty() && emulator.saved == "systemlink");
  assert(!emulator.LiveFor(0xE000000000000003ull));
  // No Live notifications from the core (it did not switch); the sign-in re-read still goes out.
  assert(emulator.notifications.size() == 1 && emulator.notifications[0].first == ns::kSignInChanged);
  std::printf("  online without the server: System Link, nothing converted\n");
}

void OnlineToOfflineAndSystemLink() {
  FakeEmulator emulator;
  emulator.network_mode = 2;
  emulator.profiles = {{0xE000000000000004ull, "Player", true, 0}};
  Backend backend{emulator};
  assert(emulator.LiveFor(0xE000000000000004ull));
  ns::Result result = ns::Run(backend, 0);
  assert(result.from == 2 && result.now == 0 && result.selected && emulator.saved == "offline");
  assert(!emulator.LiveFor(0xE000000000000004ull) && emulator.converts == 0 && !emulator.server_prepared);
  std::vector<std::pair<uint32_t, uint32_t>> expected = {
      {ns::kLiveConnectionChanged, kLogonDisconnected}, {ns::kLiveLinkStateChanged, 0}, {ns::kSignInChanged, 0b1}};
  assert(emulator.notifications == expected);

  emulator.notifications.clear();
  result = ns::Run(backend, 1);
  assert(result.from == 0 && result.now == 1 && result.selected && emulator.saved == "systemlink");
  // System Link asks the configured (else public) server too, never the core's 192.168.0.1.
  assert(emulator.server_prepared);
  assert(!emulator.LiveFor(0xE000000000000004ull));
  expected = {{ns::kLiveConnectionChanged, kLogonDisconnected}, {ns::kLiveLinkStateChanged, 1}, {ns::kSignInChanged, 0b1}};
  assert(emulator.notifications == expected);

  // Back online with a profile that is Live-enabled already: no conversion, registered once.
  emulator.notifications.clear();
  result = ns::Run(backend, 2);
  assert(result.now == 2 && emulator.converts == 0 && result.profiles.empty());
  assert(emulator.registered.size() == 1 && emulator.LiveFor(0xE000000000000004ull));
  std::printf("  online -> offline -> System Link -> online: sign-in state follows, notified each time\n");
}

void NoPlayers() {
  FakeEmulator emulator;
  Backend backend{emulator};
  const ns::Result result = ns::Run(backend, 2);
  assert(result.now == 2 && emulator.converts == 0 && emulator.registered.empty());
  assert(emulator.notifications.back() == std::make_pair(ns::kSignInChanged, 0u));
  std::printf("  no one signed in: switched, nothing converted or registered\n");
}

void QueueNeverBlocks() {
  {
    ns::Queue queue;
    assert(!queue.Busy() && queue.Ask(1) && queue.Busy() && queue.Target() == 1);
    assert(!queue.Ask(2) && queue.Target() == 2);  // a press while switching: remembered
    assert(queue.Finished(1) == 2 && queue.Busy() && queue.Target() == 2);
    assert(queue.Finished(2) == -1 && !queue.Busy());
  }
  {
    ns::Queue queue;  // pressed past System Link to offline: only the last one runs
    assert(queue.Ask(1) && !queue.Ask(2) && !queue.Ask(0) && queue.Target() == 0);
    assert(queue.Finished(1) == 0 && queue.Finished(0) == -1);
  }
  {
    ns::Queue queue;  // online failed into System Link, System Link asked meanwhile: nothing more
    assert(queue.Ask(2) && !queue.Ask(1));
    assert(queue.Finished(1) == -1 && !queue.Busy());
  }
  std::printf("  queue: a press while switching is kept, intermediate modes skipped\n");
}

void DrainRunsTheLastWanted() {
  // Black Ops III on candidate_3: System Link pressed, then Online while it ran. Both run, in
  // order, and the row ends on Online.
  FakeEmulator emulator;
  emulator.profiles = {{0xE000000000000005ull, "Player", false, 0}};
  ns::Queue queue;
  emulator.queue = &queue;
  emulator.press_during_select = 2;
  Backend backend{emulator};
  std::vector<int> started;
  std::vector<ns::Outcome> outcomes;
  assert(queue.Ask(1));
  ns::Drain(backend, queue, 1, [&](int mode) { started.push_back(mode); },
            [&](const ns::Result& result) { outcomes.push_back(ns::OutcomeOf(result)); });
  assert((started == std::vector<int>{1, 2}));
  assert((outcomes == std::vector<ns::Outcome>{ns::Outcome::kSystemLink, ns::Outcome::kOnlineConnected}));
  assert(!queue.Busy() && emulator.network_mode == 2 && emulator.saved == "online");
  assert(emulator.LiveFor(0xE000000000000005ull));
  std::printf("  drain: System Link, then the Online pressed meanwhile; signed in to Live\n");
}

void RowTexts() {
  ns::Result result;
  result.asked = 2; result.now = 2;
  assert(ns::OutcomeOf(result) == ns::Outcome::kOnlineConnected);
  assert(std::string(ns::OutcomeLabel(ns::OutcomeOf(result))) == "Online – connected");  // English key (fix 25)
  result.now = 1;  // the server was not reached
  assert(std::string(ns::OutcomeLabel(ns::OutcomeOf(result))) == "Online – server not reached");  // English key (fix 25)
  result.asked = 1;
  assert(std::string(ns::OutcomeLabel(ns::OutcomeOf(result))) == "System Link");
  result.asked = 0; result.now = 0;
  assert(std::string(ns::OutcomeLabel(ns::OutcomeOf(result))) == "Off||feminine");  // English key (fix 25): "Desligada" in pt-BR.txt
  std::printf("  row: connected, server not reached, System Link, off\n");
}
}  // namespace

int main() {
  SignInRule();
  OfflineToOnline();
  OnlineWithoutServer();
  OnlineToOfflineAndSystemLink();
  NoPlayers();
  QueueNeverBlocks();
  DrainRunsTheLastWanted();
  RowTexts();
  std::puts("PASS: network mode from the guide (fix 22)");
}
