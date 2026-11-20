// SPDX-License-Identifier: MIT
// Fix 47 (tools/source-1.0.2/fixes/47-hang-watch.patch): a game whose process the system ends
// without a crash report (Ninja Gaiden II, issue #11: no STALL report and no Performance line,
// so the UI loop that writes them stood still, then the app was gone ~30 s later).
//  (a) include/xbox360ps5/hang_watch.hpp: which thread is in which Vulkan call since when (also
//      through gpu_diag::Call), nested calls, the submission a fence wait is for, slots freed by
//      exiting threads (more threads than slots over a game), the ring of recent draws and
//      resolves (oldest first, the newest 32 kept).
//  (b) the Watcher, stepped with a clock of its own: nothing while the UI loop turns and calls
//      return; a fence wait of the "gpu" thread that lasts 4 s gives one report (UI and GPU
//      heartbeats, the queue, each watched thread, the draws of the open submissions, the GPU
//      hang verdict) with a probe and the shaders of the draws in flight to write out; a line
//      every 5 s while it lasts; the report again after 20 s; "HANG over" when it ends. A UI loop
//      stopped outside any call is reported with the other verdict; one measuring on purpose
//      is not; pipeline creation only after 20 s. Every line fits Stage's 200-byte buffer.
//  (c) the assembled tree has the hooks: gpu_diag::Call marks its calls, the completion timeline
//      names the awaited submission and marks the queue submit, the presenter marks its acquire,
//      the command processor names its thread and beats, the Vulkan command processor records
//      draws, resolves and submissions, canary_game_main.cpp runs the watch thread around the
//      game loop only (stopped before the exit's waits) with the UI loop's beats, and
//      crash_report.cpp reports the signals that end a process without a fault (SIGPIPE then
//      ignored).
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "../include/xbox360ps5/gpu_diagnostics.hpp"  // includes hang_watch.hpp (fix 47)

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace hw = xbox360ps5::hang_watch;

namespace {

int failures = 0;
void Expect(bool condition, const std::string& what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what.c_str());
    ++failures;
  }
}
bool Has(const std::vector<std::string>& lines, const std::string& part) {
  for (const auto& line : lines)
    if (line.find(part) != std::string::npos) return true;
  return false;
}
void Show(const char* title, const std::vector<std::string>& lines) {
  std::printf("-- %s\n", title);
  for (const auto& line : lines) std::printf("   %s\n", line.c_str());
}
void ExpectFits(const std::vector<std::string>& lines) {
  // Stage: "[X360] " + text + "\n" in 200 bytes.
  for (const auto& line : lines) Expect(line.size() <= 191, "line fits Stage: " + line);
}

// A thread that stays inside a watched call until released.
class Blocked {
 public:
  Blocked(const char* role, const char* call, uint64_t awaited, bool through_gpu_diag = false) {
    thread_ = std::thread([=, this] {
      if (role) hw::NameThread(role);
      if (awaited) hw::SetAwaitedSubmission(awaited);
      const auto wait = [this] {
        std::unique_lock<std::mutex> lock(mutex_);
        inside_ = true;
        changed_.notify_all();
        changed_.wait(lock, [this] { return released_; });
        return 0;
      };
      if (through_gpu_diag) {
        xbox360ps5::gpu_diag::Call(xbox360ps5::gpu_diag::Kind::fence, wait);
      } else {
        hw::InCall in_call(call);
        hw::InCall nested("submit");  // nested: the outer call is what is reported
        wait();
      }
    });
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait(lock, [this] { return inside_; });
  }
  void Release() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      released_ = true;
    }
    changed_.notify_all();
    if (thread_.joinable()) thread_.join();
  }
  ~Blocked() { Release(); }

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  bool inside_ = false, released_ = false;
  std::thread thread_;
};

const hw::SlotView* Find(const std::vector<hw::SlotView>& slots, const char* role) {
  for (const auto& slot : slots)
    if (slot.role && std::string(slot.role) == role) return &slot;
  return nullptr;
}

void CheckSlots() {
  {
    Blocked gpu("gpu", "fence", 7, true);
    const auto busy = hw::BusySlots();
    const auto* slot = Find(busy, "gpu");
    Expect(slot && slot->call && std::string(slot->call) == "fence", "gpu_diag::Call marks a fence wait");
    Expect(slot && slot->detail == 8, "the awaited submission is kept (+1)");
    gpu.Release();
    const auto after = hw::BusySlots();
    Expect(!Find(after, "gpu"), "an exited thread frees its slot");
  }
  {
    Blocked nested("ui", "acquire", 0);
    const auto* slot = Find(hw::BusySlots(), "ui");
    Expect(slot && slot->call && std::string(slot->call) == "acquire", "a nested call keeps the outer call's name");
  }
  // More threads over a game than slots: each exits and frees its slot.
  for (int round = 0; round < 3 * int(hw::kSlots); ++round) {
    std::thread([] { hw::InCall call("pipeline"); }).join();
  }
  {
    Blocked late("late", "fence", 0);
    Expect(Find(hw::BusySlots(), "late") != nullptr, "slots are still free after many threads came and went");
  }
}

void CheckWork() {
  int vs_object = 0, ps_object = 0;
  for (uint32_t n = 0; n < 40; ++n) {
    if (n % 5 == 4)
      hw::RecordResolve(100 + n / 10, 0x1F778000 + n, 5447680, 32);
    else
      hw::RecordDraw(100 + n / 10, 0x1000 + n, &vs_object, 0x2000 + n, &ps_object, 4, 3 * n, n % 2 ? hw::kIndexed : 0);
  }
  const auto recent = hw::RecentWork();
  Expect(recent.size() == hw::kWork, "the ring keeps the newest 32");
  bool ordered = true;
  for (size_t i = 1; i < recent.size(); ++i) ordered = ordered && recent[i - 1].serial < recent[i].serial;
  Expect(ordered, "recent work is oldest first");
  Expect(!recent.empty() && recent.back().vs == 0x1000 + 39 && recent.back().submission == 103, "the newest draw is last");
  Expect(!recent.empty() && recent.front().serial == hw::work_serial.load() - hw::kWork + 1, "the oldest kept is 32 back");
}

void CheckWatcher() {
  namespace gd = xbox360ps5::gpu_diag;
  hw::Watcher watcher;
  const uint64_t t0 = hw::NowMs();
  hw::ui_loop.Mark(hw::kUiInput);
  Expect(watcher.Observe(t0).lines.empty(), "first look: nothing");
  hw::ui_loop.Mark(hw::kUiPaint);
  Expect(watcher.Observe(t0 + 1000).lines.empty(), "a turning UI loop: nothing");
  // The draws of submission 7 (open: 6 is complete), then the GPU thread waits for 7 and the
  // UI loop stands in Paint.
  int vs_object = 0, ps_object = 0, other = 0;
  hw::RecordDraw(6, 0xAAAA, &other, 0, nullptr, 4, 3, 0);
  hw::RecordResolve(7, 0x1F778000, 5447680, 32);
  hw::RecordDraw(7, 0x0123456789ABCDEFull, &vs_object, 0xFEDCBA9876543210ull, &ps_object, 8, 1200, hw::kIndexed);
  hw::Submitted(7, 6);
  hw::ui_loop.Mark(hw::kUiPaint);
  {
    Blocked gpu("gpu", nullptr, 7, true);
    Blocked ui("ui", "fence", 0);
    const uint64_t t1 = hw::NowMs();
    Expect(watcher.Observe(t1 + 1000).lines.empty(), "1 s in: nothing yet");
    auto step = watcher.Observe(t1 + 4500);
    Show("first report", step.lines);
    ExpectFits(step.lines);
    Expect(step.probe, "the first report asks where the threads are");
    Expect(Has(step.lines, "HANG report: UI loop unchanged for"), "headline");
    Expect(Has(step.lines, "(last paint)"), "the UI loop's last phase");
    Expect(Has(step.lines, "last GPU submission 7"), "the queue");
    Expect(Has(step.lines, "HANG gpu ") && Has(step.lines, "in fence for") && Has(step.lines, "for submission 7"),
           "the gpu thread's fence wait and its submission");
    Expect(Has(step.lines, "sub 7 (open) draw vs 0123456789ABCDEF ps FEDCBA9876543210 prim 8 vertices 1200 indexed"),
           "the draw in flight");
    Expect(Has(step.lines, "sub 7 (open) resolve to 1F778000, 5447680 bytes, format 32"), "the resolve in flight");
    Expect(Has(step.lines, "HANG verdict: the GPU does not finish a submission (gpu thread's wait for submission 7"),
           "the GPU hang verdict");
    bool vs_dumped = false, ps_dumped = false;
    for (const void* object : step.dump) {
      vs_dumped = vs_dumped || object == &vs_object;
      ps_dumped = ps_dumped || object == &ps_object;
    }
    Expect(vs_dumped && ps_dumped, "the shaders of the open submission are written out");
    Expect(watcher.reports() == 1 && watcher.hung(), "one report");
    Expect(watcher.Observe(t1 + 5500).lines.empty(), "no line between the 5 s marks");
    auto still = watcher.Observe(t1 + 9600);
    Show("still", still.lines);
    ExpectFits(still.lines);
    Expect(Has(still.lines, "HANG still +5 s") && Has(still.lines, "gpu:fence"), "a line every 5 s");
    auto again = watcher.Observe(t1 + 25000);
    Expect(Has(again.lines, "HANG report again") && again.probe && again.dump.empty(), "the report again after 20 s, no dump");
    gpu.Release();
    ui.Release();
  }
  hw::ui_loop.Mark(hw::kUiInput);
  auto over = watcher.Observe(hw::NowMs() + 26000);
  Show("over", over.lines);
  Expect(Has(over.lines, "HANG over after"), "the end is said");
  Expect(!watcher.hung(), "watching again");

  // A UI loop stopped outside any watched call (a lock, a log mutex): the other verdict.
  hw::Watcher stopped;
  hw::ui_loop.Mark(hw::kUiSample);
  const uint64_t t2 = hw::NowMs();
  stopped.Observe(t2);
  Expect(stopped.Observe(t2 + 3000).lines.empty(), "3 s: nothing");
  auto step = stopped.Observe(t2 + 4100);
  Show("UI loop stopped", step.lines);
  Expect(Has(step.lines, "HANG verdict: the UI loop is stopped outside a GPU wait"), "UI stop verdict");

  // Measuring on purpose: no report.
  hw::Watcher measuring;
  hw::ui_loop.Mark(hw::kUiMeasure);
  const uint64_t t3 = hw::NowMs();
  measuring.Observe(t3);
  Expect(measuring.Observe(t3 + 6000).lines.empty(), "a measurement is not a hang");
  hw::ui_loop.Mark(hw::kUiInput);

  // Pipeline creation: 5 s is no hang, 21 s is.
  hw::Watcher pipeline;
  {
    Blocked compiler("compiler", "pipeline", 0);
    const uint64_t t4 = hw::NowMs();
    pipeline.Observe(t4);
    for (uint64_t at = 1000; at <= 5000; at += 1000) {
      hw::ui_loop.Mark(hw::kUiInput);
      Expect(pipeline.Observe(t4 + at).lines.empty(), "a slow pipeline is no hang");
    }
    hw::ui_loop.Mark(hw::kUiInput);
    auto late = pipeline.Observe(t4 + 21000);
    Expect(Has(late.lines, "HANG report") && Has(late.lines, "in pipeline for"), "a pipeline stuck for 20 s is reported");
    compiler.Release();
  }
  (void)gd::names;
}

std::string Read(const char* path) {
  std::ifstream file(path, std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}
void ExpectIn(const char* path, const std::vector<std::string>& parts) {
  const std::string text = Read(path);
  Expect(!text.empty(), std::string("read ") + path);
  for (const auto& part : parts) Expect(text.find(part) != std::string::npos, std::string(path) + " has: " + part);
}

void CheckTree() {
  const char* core = ".deps/xenia-canary/src/xenia/";
  ExpectIn("include/xbox360ps5/gpu_diagnostics.hpp",
           {"#include \"xbox360ps5/hang_watch.hpp\"", "hang_watch::InCall in_call(names[static_cast<unsigned>(kind)]);"});
  ExpectIn((std::string(core) + "ui/vulkan/vulkan_gpu_completion_timeline.cc").c_str(),
           {"xbox360ps5::hang_watch::SetAwaitedSubmission(awaited_submission);",
            "xbox360ps5::hang_watch::InCall in_call(\"submit\");"});
  ExpectIn((std::string(core) + "ui/vulkan/vulkan_presenter.cc").c_str(),
           {"xbox360ps5::hang_watch::InCall in_call(\"acquire\");"});
  ExpectIn((std::string(core) + "gpu/command_processor.cc").c_str(),
           {"xbox360ps5::hang_watch::NameThread(\"gpu\");", "gpu_thread.Mark(xbox360ps5::hang_watch::kGpuIdle);",
            "gpu_thread.Mark(xbox360ps5::hang_watch::kGpuExecute);"});
  ExpectIn((std::string(core) + "gpu/vulkan/vulkan_command_processor.cc").c_str(),
           {"xbox360ps5::hang_watch::RecordDraw(", "xbox360ps5::hang_watch::RecordResolve(",
            "xbox360ps5::hang_watch::Submitted(submission_index,"});
  const std::string main_text = Read("src/canary_game_main.cpp");
  Expect(main_text.find("class HangWatchThread") != std::string::npos, "the watch thread");
  const size_t start = main_text.find("auto hang_watch_thread = std::make_unique<HangWatchThread>(log_root);");
  const size_t loop = main_text.find("while (!context.HasQuitFromUIThread()) {", start);
  const size_t reset = main_text.find("hang_watch_thread.reset();", start);
  const size_t cancel = main_text.find("xe::ui::controller_keyboard::Cancel();", start);
  Expect(start != std::string::npos && loop != std::string::npos && loop > start, "the watch starts before the game loop");
  Expect(reset != std::string::npos && cancel != std::string::npos && reset < cancel,
         "the watch stops when the loop ends, before the exit's waits");
  Expect(main_text.find("ui_loop.Mark(xbox360ps5::hang_watch::kUiPaint);\r\n          context.Tick(); window.Paint();") !=
                 std::string::npos ||
             main_text.find("ui_loop.Mark(xbox360ps5::hang_watch::kUiPaint);\n          context.Tick(); window.Paint();") !=
                 std::string::npos,
         "the UI loop marks its Paint");
  Expect(main_text.find("ui_loop.Mark(xbox360ps5::hang_watch::kUiMeasure);") != std::string::npos, "a measurement is marked");
  ExpectIn("platform/ps5/crash_report.cpp",
           {"constexpr int kEndingSignals[] = {SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGXCPU, SIGXFSZ, SIGPIPE};",
            "for (int signal : kEndingSignals) sigaction(signal, &ending, nullptr);",
            "if (number == SIGPIPE) return;", "raise(number);"});
}

}  // namespace

int main() {
  CheckSlots();
  CheckWork();
  CheckWatcher();
  CheckTree();
  if (failures) {
    std::printf("check-hang-watch: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-hang-watch: ok\n");
  return 0;
}
