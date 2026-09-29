#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

// Fixed worker pool that ticks zones. Deliberately gameplay-agnostic: it only
// runs a caller-provided tick callback for queued zone indices. Zone write
// ownership is enforced inside Zone::Tick via ZoneWriteGuard.
namespace gs::game {

class ZoneWorkerPool {
public:
    using TickFn = std::function<void(std::size_t zone_index)>;

    explicit ZoneWorkerPool(TickFn tick);
    ~ZoneWorkerPool();

    ZoneWorkerPool(const ZoneWorkerPool&) = delete;
    ZoneWorkerPool& operator=(const ZoneWorkerPool&) = delete;

    // `requested_workers` 0 = auto (hardware_concurrency - 1, at least 1);
    // any positive value is honored, clamped to [1, kMaxWorkers] (scheduler
    // audit / worker-count sweep). Deliberately independent of the zone
    // count at startup: zones are created by splits after Start (H6).
    static constexpr std::size_t kMaxWorkers = 256;
    void Start(std::size_t requested_workers = 0);
    void Stop();
    void Enqueue(std::size_t zone_index, std::chrono::steady_clock::time_point due = {});

    std::size_t WorkerCount() const noexcept
    {
        return workers_.size();
    }

    struct Delays {
        std::uint64_t samples=0, due_to_enqueue_us=0, queue_us=0, due_to_start_us=0, finish_deadline_misses=0;
    };
    Delays GetDelays() const noexcept { return {delay_samples_.load(),due_enqueue_us_.load(),queue_us_.load(),due_start_us_.load(),deadline_misses_.load()}; }
    struct Utilization {
        std::uint64_t tasks_completed = 0;
        std::uint64_t busy_micros = 0;
    };
    Utilization GetUtilization() const noexcept
    {
        return Utilization{tasks_completed_.load(std::memory_order_relaxed),
                           busy_micros_.load(std::memory_order_relaxed)};
    }

    // Per-worker scheduler audit (phase 7): tasks executed, busy time and the
    // time spent waiting on the shared queue. Counters are padded to a cache
    // line because they are written by different worker threads.
    struct WorkerStat {
        // Written by one worker, but read concurrently by scheduler reports.
        // Atomic observations avoid a C++ data race; a live multi-counter
        // sample is still approximate, not a transaction/window boundary.
        std::atomic<std::uint64_t> tasks{0};
        std::atomic<std::uint64_t> work_micros{0};
        std::atomic<std::uint64_t> idle_micros{0};
        char padding[40];
        WorkerStat() = default;
        WorkerStat(const WorkerStat& other)
            : tasks(other.tasks.load(std::memory_order_relaxed)),
              work_micros(other.work_micros.load(std::memory_order_relaxed)),
              idle_micros(other.idle_micros.load(std::memory_order_relaxed)) {}
        WorkerStat& operator=(const WorkerStat& other) {
            tasks.store(other.tasks.load(std::memory_order_relaxed),std::memory_order_relaxed);
            work_micros.store(other.work_micros.load(std::memory_order_relaxed),std::memory_order_relaxed);
            idle_micros.store(other.idle_micros.load(std::memory_order_relaxed),std::memory_order_relaxed);
            return *this;
        }
    };
    static_assert(sizeof(WorkerStat) == 64);
    const std::vector<WorkerStat>& WorkerStats() const noexcept
    {
        return worker_stats_;
    }

private:
    void WorkerLoop(std::size_t worker_index);

    TickFn tick_;
    std::vector<std::thread> workers_;
    std::atomic<bool> stopping_{false};
    std::atomic<std::uint64_t> tasks_completed_{0};
    std::atomic<std::uint64_t> busy_micros_{0};
    mutable std::vector<WorkerStat> worker_stats_;
    std::mutex mutex_;
    std::condition_variable cv_;
    struct Task { std::size_t index; std::chrono::steady_clock::time_point due, enqueued; };
    std::queue<Task> tasks_;
    std::atomic<std::uint64_t> delay_samples_{0},due_enqueue_us_{0},queue_us_{0},due_start_us_{0},deadline_misses_{0};
};

} // namespace gs::game
