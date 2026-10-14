#include "ZoneWorkerPool.h"

#include <algorithm>
#include <chrono>
#include "../activity/WakeCaptureProfile.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#endif

namespace gs::game {
namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t ThreadCpuUs()
{
#ifdef _WIN32
    FILETIME created,exited,kernel,user;
    if(GetThreadTimes(GetCurrentThread(),&created,&exited,&kernel,&user)) {
        ULARGE_INTEGER k{},u{};k.LowPart=kernel.dwLowDateTime;k.HighPart=kernel.dwHighDateTime;
        u.LowPart=user.dwLowDateTime;u.HighPart=user.dwHighDateTime;return (k.QuadPart+u.QuadPart)/10;
    }
#endif
    return 0; // NOT MEASURED on unsupported platforms.
}

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

void ZoneWorkerPool::Start(std::size_t requested_workers)
{
    if (!workers_.empty()) {
        return;
    }
    // Sized from the hardware, never from the zone count at startup
    // (hardening H6): the pool is created once, while splits create zones at
    // runtime. Capping it at the seed zone count (3 on the test map, 1 on a
    // single-zone world -- the explicit override was clamped the same way)
    // meant a split could never run its children in parallel. Idle workers
    // block on the task condition variable and cost no CPU.
    const unsigned int hardware_threads = std::max(1u, std::thread::hardware_concurrency());
    const std::size_t automatic = hardware_threads > 1 ? hardware_threads - 1 : 1;
    const std::size_t worker_count =
        requested_workers > 0 ? std::clamp<std::size_t>(requested_workers, 1, kMaxWorkers)
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
    {
        // Hardening H10: the flag changes under the mutex the workers' wait
        // predicate is evaluated under. Stored + notified without it, the
        // pair could land between a worker's predicate check (false) and its
        // sleep: the notify is lost, the worker sleeps forever and join()
        // hangs (reproduced: a Start/Stop stress hung within ~1.5k cycles).
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
}

void ZoneWorkerPool::Enqueue(std::size_t zone_index, Clock::time_point due)
{
    {
        std::lock_guard lock(mutex_);
        tasks_.push({zone_index,due,Clock::now()});
    }
    cv_.notify_one();
}

void ZoneWorkerPool::WorkerLoop(std::size_t worker_index)
{
    WorkerStat* stats = worker_index < worker_stats_.size() ? &worker_stats_[worker_index]
                                                            : nullptr;
    while (true) {
        Task task{};
        const auto idle_start = Clock::now();
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] {
                return stopping_.load() || !tasks_.empty();
            });
            if (stopping_.load()) {
                if (stats != nullptr) {
                    stats->idle_micros.fetch_add(Micros(idle_start, Clock::now()),std::memory_order_relaxed);
                }
                return;
            }
            task = tasks_.front();
            tasks_.pop();
        }

        const auto work_start = Clock::now();
        const bool profiling=wake_profile::enabled.load(std::memory_order_relaxed);
        const auto cpu_before=profiling?ThreadCpuUs():0;
        tick_(task.index);
        const auto work_end = Clock::now();
        if(profiling) wake_profile::counters[wake_profile::worker_cpu_us].fetch_add(ThreadCpuUs()-cpu_before,std::memory_order_relaxed);
        if(task.due!=Clock::time_point{}) {
            due_enqueue_us_.fetch_add(Micros(task.due,task.enqueued),std::memory_order_relaxed);
            queue_us_.fetch_add(Micros(task.enqueued,work_start),std::memory_order_relaxed);
            due_start_us_.fetch_add(Micros(task.due,work_start),std::memory_order_relaxed);
            deadline_misses_.fetch_add(work_end>task.due+std::chrono::milliseconds(50),std::memory_order_relaxed);
            delay_samples_.fetch_add(1,std::memory_order_relaxed);
        }

        tasks_completed_.fetch_add(1, std::memory_order_relaxed);
        busy_micros_.fetch_add(Micros(work_start, work_end), std::memory_order_relaxed);
        if (stats != nullptr) {
            stats->tasks.fetch_add(1,std::memory_order_relaxed);
            stats->work_micros.fetch_add(Micros(work_start, work_end),std::memory_order_relaxed);
            stats->idle_micros.fetch_add(Micros(idle_start, work_start),std::memory_order_relaxed);
        }
    }
}

} // namespace gs::game
