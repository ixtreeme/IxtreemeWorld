#pragma once

#include <condition_variable>
#include <cstddef>
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

private:
    void WorkerLoop();

    TickFn tick_;
    std::vector<std::thread> workers_;
    std::atomic<bool> stopping_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<std::size_t> tasks_;
};

} // namespace gs::game
