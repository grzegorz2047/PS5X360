// SPDX-License-Identifier: MIT
// Fix 34 (tools/source-1.0.2/fixes/34-pipeline-claim.patch): a pipeline a draw needs is created
// at once on the GPU thread when no worker has taken it yet. Drives
// xe::gpu::vulkan::pipeline_claim (the assembled core's xenia/gpu/vulkan/pipeline_claim.h) with
// the cache's own scheme: a max-heap of requests on priority, worker threads popping it under one
// mutex and condition, and a waiter for one pipeline. A warm start queues all 400 stored
// pipelines; the draw needs the last one (priority 0, a shadow pass):
//  * 1.0.2 (wait only): the GPU thread waits until the workers have created all 400;
//  * fix 34: it creates that one itself while the workers have made only a few;
//  * every pipeline is created exactly once, a worker skips the request taken from it, the
//    queue drains and the waiters of a pipeline a worker is creating still get it;
//  * the assembled vulkan_pipeline_cache.cc records both kinds of request and uses it.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "xenia/gpu/vulkan/pipeline_claim.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {
namespace pc = xe::gpu::vulkan::pipeline_claim;

struct Pipeline {
  std::atomic<bool> ready{false};
  std::atomic<int> created{0};
};
struct Request {
  Pipeline* pipeline = nullptr;
  int priority = 0;
  int index = 0;
};
struct ByPriority {
  bool operator()(const Request& a, const Request& b) const {
    return a.priority != b.priority ? a.priority < b.priority : a.index > b.index;
  }
};

// The cache's creation machinery in miniature.
class Cache {
 public:
  Cache(size_t count, bool claim) : pipelines_(count), claim_(claim) {}
  ~Cache() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      shutdown_ = true;
    }
    cond_.notify_all();
    for (auto& t : workers_) t.join();
  }
  // As the storage load: everything queued, the last at priority 0.
  void QueueAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < pipelines_.size(); ++i) {
      Request r{&pipelines_[i], i + 1 == pipelines_.size() ? 0 : 1 + int(i % 4), int(i)};
      queue_.push(r);
      queued_.Add(r.pipeline, r);
    }
  }
  void Start(int workers) {
    for (int i = 0; i < workers; ++i) workers_.emplace_back([this] { Worker(); });
  }
  // AwaitPipelineCompletion(pipeline).
  bool Await(Pipeline& p) {
    std::unique_lock<std::mutex> lock(mutex_);
    auto ready = [&p] { return p.ready.load(); };
    if (!claim_) {
      cond_.notify_all();
      cond_.wait(lock, ready);
      return false;
    }
    Request taken;
    return pc::AwaitOrCreate(lock, cond_, ready, [&] { return queued_.Take(&p, &taken); },
                             [&] { Create(*taken.pipeline); });
  }
  void Drain() {
    std::unique_lock<std::mutex> lock(mutex_);
    cond_.wait(lock, [this] { return queue_.empty() && busy_ == 0; });
  }
  std::vector<Pipeline>& pipelines() { return pipelines_; }
  int by_workers() const { return by_workers_.load(); }
  int skipped() const { return skipped_.load(); }
  size_t unclaimed() { std::lock_guard<std::mutex> lock(mutex_); return queued_.size(); }

 private:
  static void Create(Pipeline& p) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));  // a driver compile
    p.created.fetch_add(1);
    p.ready.store(true);
  }
  void Worker() {
    for (;;) {
      Request r;
      bool taken_elsewhere = false;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cond_.wait(lock, [this] { return !queue_.empty() || shutdown_; });
        if (shutdown_) return;
        r = queue_.top();
        queue_.pop();
        taken_elsewhere = claim_ && !queued_.Take(r.pipeline);
        ++busy_;
      }
      if (taken_elsewhere) {
        skipped_.fetch_add(1);
      } else {
        Create(*r.pipeline);
        by_workers_.fetch_add(1);
      }
      {
        std::lock_guard<std::mutex> lock(mutex_);
        --busy_;
      }
      cond_.notify_all();
    }
  }
  std::vector<Pipeline> pipelines_;
  bool claim_;
  std::mutex mutex_;
  std::condition_variable cond_;
  std::priority_queue<Request, std::vector<Request>, ByPriority> queue_;
  pc::Queued<Pipeline*, Request> queued_;
  std::vector<std::thread> workers_;
  int busy_ = 0;
  bool shutdown_ = false;
  std::atomic<int> by_workers_{0}, skipped_{0};
};

void CheckQueued() {
  pc::Queued<int, int> q;
  q.Add(1, 10);
  q.Add(2, 20);
  int v = 0;
  assert(q.size() == 2);
  assert(q.Take(2, &v) && v == 20);
  assert(!q.Take(2, &v));
  assert(q.Take(1) && q.size() == 0);
  assert(!q.Take(3));
  std::puts("Queued: ok");
}

void CheckWarmStart() {
  constexpr size_t kCount = 400;
  // 1.0.2: the draw waits until the workers reach its pipeline, the last in the queue.
  {
    Cache cache(kCount, false);
    cache.QueueAll();
    cache.Start(2);
    Pipeline& needed = cache.pipelines().back();
    assert(!cache.Await(needed));
    const int before = cache.by_workers();
    assert(before >= int(kCount) - 2);  // all of them first (the other worker may still be busy)
    cache.Drain();
    std::printf("1.0.2: the draw waited for %d of %zu pipelines\n", before, kCount);
  }
  // Fix 34: the GPU thread takes it and creates it at once.
  {
    Cache cache(kCount, true);
    cache.QueueAll();
    cache.Start(2);
    Pipeline& needed = cache.pipelines().back();
    const auto start = std::chrono::steady_clock::now();
    assert(cache.Await(needed));
    const auto waited = std::chrono::steady_clock::now() - start;
    const int before = cache.by_workers();
    assert(needed.ready.load() && needed.created.load() == 1);
    assert(before < 100);  // a few at most: 2 ms each against the draw's one
    cache.Drain();
    for (auto& p : cache.pipelines()) assert(p.created.load() == 1 && p.ready.load());
    assert(cache.skipped() == 1);
    assert(cache.by_workers() == int(kCount) - 1);
    assert(cache.unclaimed() == 0);
    std::printf("fix 34: created on the GPU thread after %lld ms, %d made by the workers meanwhile\n",
                static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(waited).count()),
                before);
  }
  // Already created: returns at once, creates nothing.
  {
    Cache cache(4, true);
    cache.QueueAll();
    cache.Start(1);
    cache.Drain();
    assert(!cache.Await(cache.pipelines()[3]));
    for (auto& p : cache.pipelines()) assert(p.created.load() == 1);
  }
  std::puts("warm start: ok");
}

// Many draws need pipelines while the workers run: still each one exactly once.
void CheckRace() {
  constexpr size_t kCount = 300;
  Cache cache(kCount, true);
  cache.QueueAll();
  cache.Start(3);
  std::atomic<int> here{0};
  std::vector<std::thread> waiters;
  for (int w = 0; w < 3; ++w) {
    waiters.emplace_back([&, w] {
      for (size_t i = size_t(w); i < kCount; i += 7) {
        if (cache.Await(cache.pipelines()[kCount - 1 - i])) here.fetch_add(1);
        assert(cache.pipelines()[kCount - 1 - i].ready.load());
      }
    });
  }
  for (auto& t : waiters) t.join();
  cache.Drain();
  for (auto& p : cache.pipelines()) assert(p.created.load() == 1);
  assert(cache.by_workers() + here.load() == int(kCount));
  assert(cache.skipped() == here.load());
  std::printf("race: %d created by waiters, %d by workers, each once\n", here.load(), cache.by_workers());
}

std::string Read(const char* path) {
  std::ifstream file(path, std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}

size_t Count(const std::string& text, const std::string& what) {
  size_t n = 0;
  for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) ++n;
  return n;
}

void CheckTree() {
  const std::string cc = Read(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_pipeline_cache.cc");
  const std::string h = Read(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_pipeline_cache.h");
  assert(!cc.empty() && !h.empty());
  assert(h.find("#include \"xenia/gpu/vulkan/pipeline_claim.h\"") != std::string::npos);
  assert(h.find("pipeline_claim::Queued<const Pipeline*, PipelineCreationArguments>") != std::string::npos);
  // Both pushes (runtime async requests and the storage load) are recorded.
  assert(Count(cc, "creation_queue_.push(") == 2);
  assert(Count(cc, "queued_requests_.Add(&pipeline_pair.second, creation_arguments);") == 2);
  assert(cc.find("pipeline_claim::AwaitOrCreate(") != std::string::npos);
  assert(cc.find("taken_elsewhere = !queued_requests_.Take(&creation_arguments.pipeline->second);") != std::string::npos);
  assert(cc.find("if (taken_elsewhere) {") != std::string::npos);
  std::puts("assembled vulkan_pipeline_cache: ok");
}
}  // namespace

int main() {
  CheckQueued();
  CheckWarmStart();
  CheckRace();
  CheckTree();
  std::puts("check-pipeline-claim: all passed");
  return 0;
}
