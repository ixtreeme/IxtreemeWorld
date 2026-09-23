#pragma once

#include <atomic>
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

    // `requested_workers` 0 = auto (min(zone_count, hardware_concurrency-1));
    // any positive value is clamped to [1, zone_count] (scheduler audit /
    // worker-count sweep).
    void Start(std::size_t zone_count, std::size_t requested_workers = 0);
    void Stop();
    void Enqueue(std::size_t zone_index);

    std::size_t WorkerCount() const noexcept
    {
        return workers_.size();
    }

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
        std::uint64_t tasks = 0;
        std::uint64_t work_micros = 0;
        std::uint64_t idle_micros = 0;
        char padding[40];
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
    std::queue<std::size_t> tasks_;
};

} // namespace gs::game
