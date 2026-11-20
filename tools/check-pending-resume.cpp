// SPDX-License-Identifier: MIT
// Fix 71 (tools/source-1.0.2/fixes/71-pending-resume.patch): a resume that reaches a thread just
// before it suspends itself is kept for that suspension (Ninja Gaiden II's black screen on
// 1.0.4_candidate_26: two job threads suspended by their own NtSuspendThread, never woken).
//  (a) Credit (include/xbox360ps5/pending_resume.hpp): taken once, within 10 ms of the resume;
//  (b) a stress test of XThread's SelfSuspend and Resume as the console runs them (an atomic count
//      raised without the lock, a condition variable under suspend_mutex_, a resume that finds 0
//      doing nothing) with the job pattern that hung: a worker says it is idle and suspends
//      itself, a master that sees it idle resumes it. Without the credit the pair hangs within
//      a second (a resume lands between the two; reproduced, not assumed); with it, the same
//      loop runs for a second without a hang, and a resume long before a suspension still lets
//      the thread sleep;
//  (c) the assembled tree: XThread::Resume notes the credit where it finds a count of 0, and
//      SelfSuspend takes it before raising the count and returns at once, noted in the trace.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "xbox360ps5/pending_resume.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

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
namespace pr = xbox360ps5::pending_resume;
int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void CheckCredit() {
  pr::Credit credit;
  Expect(!credit.Take(1000), "no credit without a resume");
  credit.Note(1000);
  Expect(credit.Take(1000 + pr::kWindowNs), "taken within the window");
  Expect(!credit.Take(1000 + pr::kWindowNs), "taken once");
  credit.Note(5000);
  Expect(!credit.Take(5000 + pr::kWindowNs + 1), "lapsed after the window");
  Expect(!credit.Take(5000 + 10), "a lapsed credit is gone");
}

// XThread's SelfSuspend and Resume (xthread.cc, !XE_PLATFORM_WIN32), for one guest thread.
struct ModelThread {
  bool use_credit = false;
  std::atomic<uint8_t> count{0};
  std::mutex suspend_mutex;
  std::condition_variable suspend_cv;
  pr::Credit credit;
  std::atomic<bool> stop{false};

  void SelfSuspend() {
    if (use_credit && credit.Take(NowNs())) return;  // fix 71
    count.fetch_add(1);
    std::unique_lock<std::mutex> lock(suspend_mutex);
    suspend_cv.wait(lock, [&] { return count.load() == 0 || stop.load(); });
  }
  uint8_t Resume() {
    std::lock_guard<std::mutex> lock(suspend_mutex);
    uint8_t previous = count.load();
    while (previous > 0 && !count.compare_exchange_weak(previous, previous - 1)) {
    }
    if (previous == 0 && use_credit) credit.Note(NowNs());  // fix 71
    suspend_cv.notify_all();
    return previous;
  }
};

// The job pattern: the worker sets idle and suspends itself (some guest code between the two);
// the master waits for idle, clears it and resumes the worker. Returns the rounds done and
// whether it hung (no round for 250 ms) within `seconds`.
struct Outcome {
  uint64_t rounds = 0;
  bool hung = false;
  double seconds = 0;
};
Outcome RunJobs(bool use_credit, double seconds) {
  ModelThread worker;
  worker.use_credit = use_credit;
  std::atomic<int> idle{0};
  std::atomic<uint64_t> rounds{0};
  std::atomic<bool> done{false};
  std::thread work([&] {
    while (!done.load()) {
      idle.store(1);
      for (volatile int spin = 0; spin < 50; ++spin) {
      }  // the guest's code between saying so and NtSuspendThread
      worker.SelfSuspend();
      rounds.fetch_add(1);
    }
  });
  const auto start = std::chrono::steady_clock::now();
  Outcome out;
  auto last_progress = start;
  uint64_t last_rounds = 0;
  while (true) {
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration<double>(now - start).count() >= seconds) break;
    if (idle.exchange(0)) worker.Resume();
    const uint64_t r = rounds.load();
    if (r != last_rounds) {
      last_rounds = r;
      last_progress = now;
    } else if (now - last_progress > std::chrono::milliseconds(250)) {
      out.hung = true;
      break;
    }
  }
  out.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  out.rounds = rounds.load();
  done.store(true);
  worker.stop.store(true);
  {
    std::lock_guard<std::mutex> lock(worker.suspend_mutex);
    worker.suspend_cv.notify_all();
  }
  work.join();
  return out;
}

void CheckStress() {
  // Without the credit: reproduced within a few tries of up to a second each.
  Outcome without;
  for (int attempt = 0; attempt < 5 && !without.hung; ++attempt) without = RunJobs(false, 1.0);
  std::printf("   without the credit: %s after %llu rounds in %.3f s\n", without.hung ? "HUNG" : "no hang",
              (unsigned long long)without.rounds, without.seconds);
  Expect(without.hung, "the lost wake-up reproduces without the credit");
  const Outcome with = RunJobs(true, 1.0);
  std::printf("   with the credit: %s, %llu rounds in %.3f s\n", with.hung ? "HUNG" : "no hang",
              (unsigned long long)with.rounds, with.seconds);
  Expect(!with.hung && with.rounds > 1000, "with the credit the same loop runs for a second");

  // A resume long before a suspension leaves no credit: the thread still sleeps until resumed.
  ModelThread late;
  late.use_credit = true;
  late.Resume();  // count 0: a credit
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  std::atomic<bool> woke{false};
  std::thread sleeper([&] {
    late.SelfSuspend();
    woke.store(true);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  Expect(!woke.load(), "a lapsed credit does not wake a later suspension");
  late.Resume();
  sleeper.join();
  Expect(woke.load(), "the resume after it does");
}

void CheckTree() {
  const std::string k = ".deps/xenia-canary/src/xenia/kernel/";
  const std::string code = Read(k + "xthread.cc");
  const size_t resume = code.find("X_STATUS XThread::Resume(uint32_t* out_suspend_count) {");
  const size_t zero = code.find("suspend_trace_.Note(xbox360ps5::suspend_trace::kResumeFoundRunning, resumer, 0, now);", resume);
  const size_t note = code.find("pending_resume_.Note(", zero);
  const size_t self = code.find("uint32_t XThread::SelfSuspend() {");
  Expect(resume != std::string::npos && zero != std::string::npos && note != std::string::npos && note < self,
         "a resume that finds a count of 0 leaves the credit");
  const size_t take = code.find("if (pending_resume_.Take(", self);
  const size_t cancelled = code.find("suspend_trace_.Note(xbox360ps5::suspend_trace::kSuspendCancelled,", take);
  const size_t early_return = code.find("return suspend_count.load();", cancelled);
  const size_t raise = code.find("const uint32_t previous = suspend_count.fetch_add(1);", self);
  Expect(self != std::string::npos && take != std::string::npos && cancelled != std::string::npos &&
             early_return < raise,
         "SelfSuspend takes it before raising the count and returns at once, noted in the trace");
  Expect(Read(k + "xthread.h").find("xbox360ps5::pending_resume::Credit pending_resume_;") != std::string::npos,
         "each thread has its credit");
}

}  // namespace

int main() {
  CheckCredit();
  CheckStress();
  CheckTree();
  if (failures) {
    std::printf("check-pending-resume: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-pending-resume: ok\n");
  return 0;
}
