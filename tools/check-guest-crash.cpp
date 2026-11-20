// SPDX-License-Identifier: MIT
// Fix 45 (tools/source-1.0.2/fixes/45-guest-crash-notice.patch): a guest crash no longer ends in
// a silent picture at 0 FPS (Minecraft, 584111F7, on the v1.0.3 draft: thread F8000104 read guest
// address 0 at PC 824F849C, Emulator::ExceptionCallback called Pause() and Pause() never returned;
// research/minecraft-online-hang/VERDICT.md).
//  (a) port/guest_crash.hpp: the first crash is recorded (pc, thread, fault address, time) and
//      later ones only counted, also from threads at once; the STALL report's first line names it
//      ("STALL after a guest crash on thread F8000104 at PC 824F849C, 16 s ago; the emulator paused
//      itself ...") and is the line it always was when nothing crashed; the crashed thread's state
//      and the overlay's detail. In the assembled tree: the core declares the weak hook and calls
//      it in ExceptionCallback before Pause(), canary_game_main.cpp defines it, logs the headline
//      and names the crashed thread in ReportStall, and the overlay shows the notice.
//  (b) the overlay's two notices (English keys) are in every language file of
//      tools/source-1.0.2/english/lang (the argument), Polish translated.
//  (c) a model of XmaDecoder's pause handshake: a worker that sleeps on an auto-reset event and
//      checks paused_ after each pass over the contexts (signal pause_fence_, wait on
//      resume_fence_). With the fix, Pause() returns within 100 ms with the worker idle and no
//      context kicked, over and over, and without a worker loop (xma_inline) at once; the 1.0.2
//      Pause (no Set) is still waiting after 300 ms in both cases and only a kick (or a signal
//      nobody sends) lets it go: the frozen crash handler. The tree's XmaDecoder::Pause,
//      WorkerThreadMain and Resume are checked to be what the model runs.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "../port/guest_crash.hpp"  // the assembled tree's port/ (run from <tree>/tools)

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {
namespace fs = std::filesystem;
namespace gc = xbox360ps5::guest_crash;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

std::atomic<bool> finished{false};

// A file of the tree with its line ends as "\n" (canary_game_main.cpp keeps the drop's CRLF).
std::string ReadTree(const char* path) {
  std::ifstream file(path, std::ios::binary);
  assert(file && "run from the assembled tree's root");
  std::stringstream text;
  text << file.rdbuf();
  std::string out;
  for (const char c : text.str()) {
    if (c != '\r') out += c;
  }
  return out;
}

bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// The text from `start` to the first "\n}\n" after it (a function's body at namespace level).
std::string Body(const std::string& text, const std::string& start) {
  const auto at = text.find(start);
  assert(at != std::string::npos && "function not found in the tree");
  const auto end = text.find("\n}\n", at);
  assert(end != std::string::npos);
  return text.substr(at, end + 3 - at);
}

// `parts` appear in `text` in this order.
bool InOrder(const std::string& text, std::initializer_list<const char*> parts) {
  size_t at = 0;
  for (const char* part : parts) {
    const auto found = text.find(part, at);
    if (found == std::string::npos) {
      std::fprintf(stderr, "  missing (in order): %s\n", part);
      return false;
    }
    at = found + std::string(part).size();
  }
  return true;
}

// ---------------------------------------------------------------- (a) the record and the text

void NothingRecorded() {
  gc::Recorder recorder;
  const gc::Crash crash = recorder.Get();
  assert(!crash.recorded && crash.pc == 0 && crash.thread == 0 && crash.count == 0);
  // The headline the frame watch always logged (fmt's "{:08X} '{}' after {:.0f}s, {} ... {}").
  assert(gc::StallHeadline(crash, 50000, 0x584111F7, "Minecraft", 30.4, 707, 1414) ==
         "STALL title 584111F7 'Minecraft' after 30s, 707 refreshed outputs, 1414 submitted swaps; "
         "no output is not proof of deadlock");
  assert(gc::ThreadNote(crash, 0xF8000104, 50000).empty());
  assert(gc::NoticeDetail(crash).empty());
  std::printf("  nothing recorded: the STALL headline as before, no thread note, no notice detail\n");
}

void MinecraftCrash() {
  gc::Recorder recorder;
  // draft:194-198: thread F8000104, PC 824F849C, access violation reading 0x0000001000000000.
  assert(recorder.Record(0x824F849C, 0xF8000104, 0x0000001000000000ull, 120000));
  gc::Crash crash = recorder.Get();
  assert(crash.recorded && crash.pc == 0x824F849C && crash.thread == 0xF8000104);
  assert(crash.fault_address == 0x0000001000000000ull && crash.at_ms == 120000 && crash.count == 1);
  const std::string headline =
      gc::StallHeadline(crash, 136900, 0x584111F7, "Minecraft", 15.0, 707, 707);
  assert(headline ==
         "STALL after a guest crash on thread F8000104 at PC 824F849C, 16 s ago; the emulator paused "
         "itself (title 584111F7 'Minecraft' after 15s, 707 refreshed outputs, 707 submitted swaps)");
  assert(headline.rfind("STALL after a guest crash on thread F8000104 at PC 824F849C, ", 0) == 0);
  assert(!Has(headline, "no output is not proof of deadlock"));
  assert(gc::SecondsAgo(crash, 119000) == 0);  // a clock read before the record: never negative
  assert(gc::ThreadNote(crash, 0xF8000104, 136900) ==
         "crashed at PC 824F849C (access violation at 0000001000000000), 16 s ago");
  assert(gc::ThreadNote(crash, 0xF8000110, 136900).empty());  // the Server thread keeps its wait text
  assert(gc::NoticeDetail(crash) == " (PC 824F849C)");

  // A second crash (another thread faulting later) is counted; the first stays the one named.
  assert(!recorder.Record(0x82000000, 0xF8000110, 0, 130000));
  crash = recorder.Get();
  assert(crash.pc == 0x824F849C && crash.thread == 0xF8000104 && crash.at_ms == 120000 && crash.count == 2);
  assert(gc::StallHeadline(crash, 136900, 0x584111F7, "Minecraft", 15.0, 707, 707) ==
         "STALL after a guest crash on thread F8000104 at PC 824F849C, 16 s ago; the emulator paused "
         "itself (title 584111F7 'Minecraft' after 15s, 707 refreshed outputs, 707 submitted swaps; "
         "2 guest crashes in all)");

  // An illegal instruction: no fault address.
  gc::Recorder other;
  other.Record(0x82100004, 0xF8000020, 0, 1000);
  assert(gc::ThreadNote(other.Get(), 0xF8000020, 3500) == "crashed at PC 82100004, 2 s ago");
  std::printf("  Minecraft's crash: the STALL headline, the crashed thread's state, a later crash counted\n");
}

void ConcurrentCrashes() {
  gc::Recorder recorder;
  std::atomic<int> kept{0};
  std::vector<std::thread> threads;
  for (uint32_t n = 0; n < 8; ++n) {
    threads.emplace_back([&, n] {
      if (recorder.Record(0x82000000 + n * 4, 0xF8000100 + n * 4, n, 1000 + n)) ++kept;
    });
  }
  for (auto& thread : threads) thread.join();
  const gc::Crash crash = recorder.Get();
  assert(kept == 1 && crash.count == 8);
  // The one kept is whole: its pc, thread, fault address and time belong together.
  const uint32_t n = (crash.pc - 0x82000000) / 4;
  assert(n < 8 && crash.thread == 0xF8000100 + n * 4 && crash.fault_address == n &&
         crash.at_ms == int64_t(1000 + n));
  assert(!gc::Global().Get().recorded);  // the process-wide recorder is a separate one
  std::printf("  8 threads crashing at once: one crash kept whole, 8 counted\n");
}

void TreeUsesIt() {
  // The hook's signature, in the core (weak) and in the port.
  const std::string hook =
      "extern \"C\" void Xbox360PS5GuestCrashed(uint32_t guest_pc, uint32_t thread_handle,\n" +
      std::string(39, ' ') + "uint64_t fault_address)";
  const std::string emulator = ReadTree(".deps/xenia-canary/src/xenia/emulator.cc");
  assert(Has(emulator, hook + " __attribute__((weak));"));
  // In ExceptionCallback: the crash dump is logged, the port told, then Pause().
  const std::string callback = Body(emulator, "bool Emulator::ExceptionCallback(Exception* ex) {");
  assert(InOrder(callback, {"XELOGE(\"{}\", crash_msg);", "if (Xbox360PS5GuestCrashed) {",
                            "guest_function->MapMachineCodeToGuestAddress(ex->pc());",
                            "Xbox360PS5GuestCrashed(guest_pc, current_thread->handle(), fault_address);",
                            "  Pause();", "current_thread->Suspend(nullptr);"}));
  assert(Has(callback, "ex->code() == Exception::Code::kAccessViolation ? ex->fault_address()"));

  const std::string main = ReadTree("src/canary_game_main.cpp");
  assert(Has(main, "#include \"guest_crash.hpp\""));
  assert(Has(main, hook + " {"));
  assert(Has(main, "xbox360ps5::guest_crash::Global().Record(guest_pc, thread_handle, fault_address,"));
  // The frame watch's first STALL line comes from StallHeadline; the old text only there.
  assert(Has(main, "XELOGW(\"{}\", xbox360ps5::guest_crash::StallHeadline("));
  assert(!Has(main, "no output is not proof of deadlock"));
  // ReportStall names the crashed thread instead of its wait text.
  const std::string report = Body(main, "void ReportStall(xe::Emulator& emulator, int pass) {");
  assert(InOrder(report, {"xbox360ps5::guest_crash::Global().Get()",
                          "xbox360ps5::guest_crash::ThreadNote(crash, thread->handle(), crash_now)",
                          "crashed.empty() ? WaitText(*thread) : crashed"}));
  // The overlay: the notice whenever a crash is recorded, with the PC after it.
  assert(Has(main, "const auto crash = xbox360ps5::guest_crash::Global().Get();"));
  assert(Has(main, "notice += xbox360ps5::guest_crash::NoticeDetail(crash);"));
  std::printf("  the tree: hook declared weak and called before Pause(), defined by the port, used by "
              "the STALL report and the overlay\n");
}

// ---------------------------------------------------------------- (b) the language files

// "\s" in a language file is a space, "\n" a line break (rekey.py's escape()).
std::string Unescape(const std::string& text) {
  std::string out;
  for (size_t at = 0; at < text.size(); ++at) {
    if (text[at] == '\\' && at + 1 < text.size()) {
      const char next = text[++at];
      out += next == 's' ? ' ' : next == 'n' ? '\n' : next == 't' ? '\t' : next;
    } else {
      out += text[at];
    }
  }
  return out;
}

void LanguageFiles(const fs::path& folder) {
  const std::string keys[] = {gc::kNoticeTouchpad, gc::kNoticeOptions};
  std::map<std::string, std::map<std::string, std::string>> texts;  // code -> key -> text
  for (const auto& entry : fs::directory_iterator(folder)) {
    if (entry.path().extension() != ".txt") continue;
    std::ifstream file(entry.path(), std::ios::binary);
    for (std::string line; std::getline(file, line);) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      const auto split = line.find(" = ");
      if (split == std::string::npos) continue;
      const std::string key = Unescape(line.substr(0, split));
      for (const auto& wanted : keys) {
        if (key == wanted) texts[entry.path().stem().string()][key] = Unescape(line.substr(split + 3));
      }
    }
  }
  const char* codes[] = {"en", "pt-BR", "pt", "es", "es-419", "de", "fr", "it", "ja", "ko", "nl", "pl",
                         "ru", "tr", "zh-Hans", "zh-Hant"};
  for (const char* code : codes) {
    for (const auto& key : keys) {
      const auto& file = texts[code];
      const auto found = file.find(key);
      if (found == file.end() || found->second.empty()) {
        std::fprintf(stderr, "  %s.txt lacks: %s\n", code, key.c_str());
        assert(!"the crash notice is in every language file");
      }
    }
  }
  assert(texts["en"][keys[0]] == keys[0] && texts["en"][keys[1]] == keys[1]);
  assert(texts["pl"][keys[0]] ==
         "Gra uległa awarii i emulator ją wstrzymał. Touchpad: przewodnik, potem Zamknij grę i "
         "wróć do menu");
  assert(texts["pl"][keys[1]] ==
         "Gra uległa awarii i emulator ją wstrzymał. OPTIONS + touchpad: przewodnik, potem Zamknij "
         "grę i wróć do menu");
  // The overlay asks for exactly these keys.
  const std::string main = ReadTree("src/canary_game_main.cpp");
  assert(Has(main, "Tr(\"" + keys[0] + "\")") && Has(main, "Tr(\"" + keys[1] + "\")"));
  std::printf("  both notices in all %zu language files (Polish translated), asked for by the overlay\n",
              sizeof(codes) / sizeof(codes[0]));
}

// ---------------------------------------------------------------- (c) the XMA pause handshake

// xe::threading's auto-reset event (CreateAutoResetEvent): Set wakes one waiter, or the next Wait.
class AutoResetEvent {
 public:
  void Set() {
    std::lock_guard<std::mutex> lock(mutex_);
    signaled_ = true;
    cond_.notify_one();
  }
  void Wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    ++waiting_;
    cond_.wait(lock, [this] { return signaled_; });
    --waiting_;
    signaled_ = false;
  }
  int waiting() {
    std::lock_guard<std::mutex> lock(mutex_);
    return waiting_;
  }

 private:
  std::mutex mutex_;
  std::condition_variable cond_;
  bool signaled_ = false;
  int waiting_ = 0;
};

// xe::threading::Fence (base/threading.h) for one waiter: Signal sets it, Wait waits and clears it.
class Fence {
 public:
  void Signal() {
    std::lock_guard<std::mutex> lock(mutex_);
    signaled_ = true;
    cond_.notify_all();
  }
  void Wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    cond_.wait(lock, [this] { return signaled_; });
    signaled_ = false;
  }

 private:
  std::mutex mutex_;
  std::condition_variable cond_;
  bool signaled_ = false;
};

// XmaDecoder's worker, Pause and Resume (apu/xma_decoder.cc), with the contexts' work reduced to
// a count of kicks.
struct XmaModel {
  const bool fix;        // fix 45's Pause, else 1.0.2's
  const bool dedicated;  // cvars::use_dedicated_xma_thread (off with the game option xma_inline)
  std::atomic<bool> worker_running{true}, paused{false};
  std::atomic<int> kicks{0}, passes_with_work{0};
  AutoResetEvent work_event;
  Fence pause_fence, resume_fence;
  std::thread worker;

  XmaModel(bool fix, bool dedicated) : fix(fix), dedicated(dedicated) {
    // XmaDecoder::Setup's XHostThread: WorkerThreadMain only with a dedicated thread.
    if (dedicated) worker = std::thread([this] { WorkerThreadMain(); });
  }
  void WorkerThreadMain() {
    while (worker_running) {
      const bool did_work = kicks.exchange(0) != 0;  // the pass over the contexts
      if (did_work) ++passes_with_work;
      if (paused) {
        pause_fence.Signal();
        resume_fence.Wait();
      }
      if (did_work) continue;
      work_event.Wait();
    }
  }
  void Pause() {
    if (paused) return;
    paused = true;
    if (fix) {
      if (!dedicated) return;  // no worker loop: nobody would signal the fence
      work_event.Set();        // an idle worker wakes and reaches the paused check
    }
    pause_fence.Wait();
  }
  void Resume() {
    if (!paused) return;
    paused = false;
    resume_fence.Signal();
  }
  void Kick() {  // WriteRegister's context kick: work_event_->SetBoostPriority()
    ++kicks;
    work_event.Set();
  }
  void Shutdown() {
    worker_running = false;
    work_event.Set();
    if (paused) Resume();
    if (worker.joinable()) worker.join();
  }
  void WaitIdle() {  // the worker asleep on work_event_
    const auto until = Clock::now() + 5s;
    while (work_event.waiting() == 0) {
      assert(Clock::now() < until && "the worker never went idle");
      std::this_thread::sleep_for(1ms);
    }
  }
};

// Runs Pause() on its own thread (the crashing guest thread in ExceptionCallback).
struct PauseCall {
  std::atomic<bool> returned{false};
  std::thread thread;
  explicit PauseCall(XmaModel& model) : thread([this, &model] {
    model.Pause();
    returned = true;
  }) {}
  bool ReturnsWithin(std::chrono::milliseconds limit) {
    const auto until = Clock::now() + limit;
    while (!returned && Clock::now() < until) std::this_thread::sleep_for(1ms);
    return returned;
  }
  ~PauseCall() { thread.join(); }
};

void FixedPauseWithIdleWorker() {
  XmaModel model(true, true);
  // Some audio first, as before the crash; then the guest's audio stops (AudioSystem paused).
  model.Kick();
  model.WaitIdle();
  for (int round = 0; round < 50; ++round) {
    const int kicks_before = model.kicks.load();
    {
      PauseCall call(model);
      assert(call.ReturnsWithin(100ms) && "fix 45: Pause() returns with the worker idle, no kick");
      assert(model.kicks == kicks_before);  // nothing kicked the worker
    }
    // While paused, a kick is not decoded; after Resume it is.
    const int worked = model.passes_with_work.load();
    model.Kick();
    std::this_thread::sleep_for(round == 0 ? 20ms : 0ms);
    assert(model.passes_with_work == worked);
    model.Resume();
    const auto until = Clock::now() + 2s;
    while (model.passes_with_work == worked) {
      assert(Clock::now() < until && "the worker decodes again after Resume");
      std::this_thread::sleep_for(1ms);
    }
    model.WaitIdle();
  }
  model.Shutdown();
  std::printf("  fix 45, worker idle: Pause() returns within 100 ms without a kick (50 pauses, each resumed)\n");
}

void FixedPauseWithBusyWorker() {
  // The worker in the middle of a pass when Pause() comes: it still stops after that pass.
  XmaModel model(true, true);
  std::atomic<bool> stop{false};
  std::thread kicker([&] {
    while (!stop) {
      model.Kick();
      std::this_thread::sleep_for(200us);
    }
  });
  std::this_thread::sleep_for(20ms);
  {
    PauseCall call(model);
    assert(call.ReturnsWithin(100ms));
  }
  stop = true;
  kicker.join();
  model.Resume();
  model.Shutdown();
  std::printf("  fix 45, worker busy: Pause() returns within 100 ms\n");
}

void FixedPauseWithoutWorker() {
  XmaModel model(true, false);  // xma_inline: the XMA thread returns at once
  {
    PauseCall call(model);
    assert(call.ReturnsWithin(100ms) && "fix 45: Pause() does not wait for a worker that does not run");
  }
  model.Resume();
  model.Shutdown();
  std::printf("  fix 45, no worker loop (xma_inline): Pause() returns at once\n");
}

void OldPauseHangs() {
  {
    XmaModel model(false, true);
    model.WaitIdle();
    {
      PauseCall call(model);
      assert(!call.ReturnsWithin(300ms) && "1.0.2's Pause() waits for an idle worker");
      // Only a context kick wakes the worker; after a crash nothing kicks (the guest's audio is
      // paused and its threads are about to be).
      model.Kick();
      assert(call.ReturnsWithin(2000ms));
    }
    model.Resume();
    model.Shutdown();
  }
  {
    XmaModel model(false, false);
    {
      PauseCall call(model);
      assert(!call.ReturnsWithin(300ms) && "1.0.2's Pause() waits for a worker loop that never runs");
      model.pause_fence.Signal();  // what nothing in 1.0.2 does: let the check end
      assert(call.ReturnsWithin(2000ms));
    }
    model.Resume();
    model.Shutdown();
  }
  std::printf("  1.0.2's Pause() still waiting after 300 ms, idle worker or no worker loop: the frozen crash handler\n");
}

void TreeXmaDecoder() {
  const std::string xma = ReadTree(".deps/xenia-canary/src/xenia/apu/xma_decoder.cc");
  assert(Has(xma, "work_event_ = xe::threading::Event::CreateAutoResetEvent(false);"));
  assert(InOrder(xma, {"[this]() {", "if (cvars::use_dedicated_xma_thread) {", "WorkerThreadMain();"}));
  const std::string worker = Body(xma, "void XmaDecoder::WorkerThreadMain() {");
  assert(InOrder(worker, {"while (worker_running_) {", "contexts_[n]->Work();", "if (paused_) {",
                          "pause_fence_.Signal();", "resume_fence_.Wait();", "if (did_work) {",
                          "xe::threading::Wait(work_event_.get(), false);"}));
  const std::string pause = Body(xma, "void XmaDecoder::Pause() {");
  assert(InOrder(pause, {"if (paused_) {", "paused_ = true;", "if (!cvars::use_dedicated_xma_thread) {",
                         "return;", "work_event_->Set();", "pause_fence_.Wait();"}));
  const std::string resume = Body(xma, "void XmaDecoder::Resume() {");
  assert(InOrder(resume, {"paused_ = false;", "resume_fence_.Signal();"}));
  // The audio worker's pause, which the fix mirrors, wakes its worker the same way.
  const std::string audio = ReadTree(".deps/xenia-canary/src/xenia/apu/audio_system.cc");
  assert(InOrder(Body(audio, "void AudioSystem::Pause() {"),
                 {"paused_ = true;", "pending_work_event_->Set();", "pause_fence_.Wait();",
                  "xma_decoder_->Pause();"}));
  std::printf("  the tree's XmaDecoder::Pause, WorkerThreadMain and Resume are the model's\n");
}
}  // namespace

int main(int argc, char** argv) {
  assert(argc == 2 && "usage: check-guest-crash <english/lang folder>");
  std::thread([] {
    const auto until = Clock::now() + 120s;
    while (Clock::now() < until) {
      if (finished) return;
      std::this_thread::sleep_for(100ms);
    }
    std::printf("FAIL: still running after 120 s (a Pause() that never returned?)\n");
    std::fflush(stdout);
    std::_Exit(2);
  }).detach();
  std::printf("check-guest-crash\n");
  NothingRecorded();
  MinecraftCrash();
  ConcurrentCrashes();
  TreeUsesIt();
  LanguageFiles(argv[1]);
  FixedPauseWithIdleWorker();
  FixedPauseWithBusyWorker();
  FixedPauseWithoutWorker();
  OldPauseHangs();
  TreeXmaDecoder();
  finished = true;
  std::printf("check-guest-crash: all passed\n");
  return 0;
}
