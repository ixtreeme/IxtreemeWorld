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

    void Start(std::size_t zone_count);
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

private:
    void WorkerLoop();

    TickFn tick_;
    std::vector<std::thread> workers_;
    std::atomic<bool> stopping_{false};
    std::atomic<std::uint64_t> tasks_completed_{0};
    std::atomic<std::uint64_t> busy_micros_{0};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<std::size_t> tasks_;
};

} // namespace gs::game
