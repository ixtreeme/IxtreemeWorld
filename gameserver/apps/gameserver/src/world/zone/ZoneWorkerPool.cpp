#include "ZoneWorkerPool.h"

#include <algorithm>
#include <chrono>

namespace gs::game {
namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t Micros(const Clock::time_point& from, const Clock::time_point& to)
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(to - from).count());
}

} // namespace

ZoneWorkerPool::ZoneWorkerPool(TickFn tick)
    : tick_(std::move(tick))
{
}

ZoneWorkerPool::~ZoneWorkerPool()
{
    Stop();
}

void ZoneWorkerPool::Start(std::size_t zone_count, std::size_t requested_workers)
{
    if (!workers_.empty()) {
        return;
    }
    const unsigned int hardware_threads = std::max(1u, std::thread::hardware_concurrency());
    const std::size_t automatic = std::max<std::size_t>(
        1, std::min<std::size_t>(zone_count, hardware_threads > 1 ? hardware_threads - 1 : 1));
    const std::size_t worker_count =
        requested_workers > 0
            ? std::clamp<std::size_t>(requested_workers, 1, std::max<std::size_t>(1, zone_count))
            : automatic;
    workers_.reserve(worker_count);
    worker_stats_.assign(worker_count, WorkerStat{});
    stopping_ = false;
    for (std::size_t i = 0; i < worker_count; ++i) {
        workers_.emplace_back([this, i] {
            WorkerLoop(i);
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

void ZoneWorkerPool::WorkerLoop(std::size_t worker_index)
{
    WorkerStat* stats = worker_index < worker_stats_.size() ? &worker_stats_[worker_index]
                                                            : nullptr;
    while (true) {
        std::size_t zone_index = 0;
        const auto idle_start = Clock::now();
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] {
                return stopping_.load() || !tasks_.empty();
            });
            if (stopping_.load()) {
                if (stats != nullptr) {
                    stats->idle_micros += Micros(idle_start, Clock::now());
                }
                return;
            }
            zone_index = tasks_.front();
            tasks_.pop();
        }

        const auto work_start = Clock::now();
        tick_(zone_index);
        const auto work_end = Clock::now();

        tasks_completed_.fetch_add(1, std::memory_order_relaxed);
        busy_micros_.fetch_add(Micros(work_start, work_end), std::memory_order_relaxed);
        if (stats != nullptr) {
            ++stats->tasks;
            stats->work_micros += Micros(work_start, work_end);
            stats->idle_micros += Micros(idle_start, work_start);
        }
    }
}

} // namespace gs::game
