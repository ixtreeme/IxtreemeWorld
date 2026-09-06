#include "ZoneWorkerPool.h"

#include <algorithm>

namespace gs::game {

ZoneWorkerPool::ZoneWorkerPool(TickFn tick)
    : tick_(std::move(tick))
{
}

ZoneWorkerPool::~ZoneWorkerPool()
{
    Stop();
}

void ZoneWorkerPool::Start(std::size_t zone_count)
{
    if (!workers_.empty()) {
        return;
    }
    const unsigned int hardware_threads = std::max(1u, std::thread::hardware_concurrency());
    const std::size_t worker_count = std::max<std::size_t>(
        1, std::min<std::size_t>(zone_count, hardware_threads > 1 ? hardware_threads - 1 : 1));
    workers_.reserve(worker_count);
    stopping_ = false;
    for (std::size_t i = 0; i < worker_count; ++i) {
        workers_.emplace_back([this] {
            WorkerLoop();
        });
    }
}

void ZoneWorkerPool::Stop()
{
    stopping_ = true;
    cv_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
}

void ZoneWorkerPool::Enqueue(std::size_t zone_index)
{
    {
        std::lock_guard lock(mutex_);
        tasks_.push(zone_index);
    }
    cv_.notify_one();
}

void ZoneWorkerPool::WorkerLoop()
{
    while (!stopping_) {
        std::size_t zone_index = 0;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] {
                return stopping_.load() || !tasks_.empty();
            });
            if (stopping_.load()) {
                return;
            }
            zone_index = tasks_.front();
            tasks_.pop();
        }

        tick_(zone_index);
    }
}

} // namespace gs::game
