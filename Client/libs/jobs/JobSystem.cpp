#include "JobSystem.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>

namespace ixjobs
{
namespace
{
thread_local std::uint32_t t_worker = 0;

// The variable's value, or empty (unset).
std::string EnvironmentValue(const char* name)
{
    std::string value;
#if defined(_WIN32)
    char* text = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&text, &length, name) == 0 && text)
    {
        value = text;
        std::free(text);
    }
#else
    if (const char* text = std::getenv(name))
        value = text;
#endif
    return value;
}

bool SwitchOff(const std::string& value)
{
    return value == "0" || value == "false" || value == "off";
}

std::uint32_t WorkersFromEnvironment()
{
    if (SwitchOff(EnvironmentValue("IX_JOBS")))
        return 0;
    const std::string requested = EnvironmentValue("IX_JOBS_WORKERS");
    if (!requested.empty())
    {
        const long count = std::strtol(requested.c_str(), nullptr, 10);
        return static_cast<std::uint32_t>(std::clamp<long>(count, 0, JobSystem::kMaxWorkers));
    }
    const unsigned hardware = std::thread::hardware_concurrency();
    return std::min<std::uint32_t>(hardware > 1 ? hardware - 1 : 0u, JobSystem::kMaxWorkers);
}
} // namespace

JobSystem& JobSystem::Instance()
{
    static JobSystem instance;
    static std::once_flag started;
    std::call_once(started, [] { instance.Start(WorkersFromEnvironment()); });
    return instance;
}

JobSystem::~JobSystem()
{
    Stop();
}

std::uint32_t JobSystem::CurrentWorker()
{
    return t_worker;
}

void JobSystem::Start(std::uint32_t workers)
{
    Stop();
    workers = std::min(workers, kMaxWorkers);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = false;
    }
    m_stopping.store(false, std::memory_order_relaxed);
    m_workers.reserve(workers);
    for (std::uint32_t worker = 1; worker <= workers; ++worker)
        m_workers.emplace_back([this, worker] { WorkerMain(worker); });
}

void JobSystem::Stop()
{
    if (m_workers.empty())
        return;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_stopping.store(true, std::memory_order_relaxed);
    m_wake.notify_all();
    for (std::thread& thread : m_workers)
    {
        if (thread.joinable())
            thread.join();
    }
    m_workers.clear();
    // Whatever was still queued runs here (its submitters may be waiting for it).
    while (TryRunOne())
    {
    }
}

void JobSystem::Submit(TaskFunction fn, void* data, std::uint32_t index, Counter* counter)
{
    if (counter)
        counter->m_pending.fetch_add(1, std::memory_order_relaxed);
    const Task task{fn, data, index, counter};
    if (m_workers.empty())
    {
        Run(task);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push_back(task);
        m_queued.fetch_add(1, std::memory_order_relaxed);
    }
    m_wake.notify_one();
}

void JobSystem::SubmitRange(TaskFunction fn, void* data, std::uint32_t first, std::uint32_t count, Counter* counter)
{
    if (count == 0)
        return;
    if (counter)
        counter->m_pending.fetch_add(count, std::memory_order_relaxed);
    if (m_workers.empty())
    {
        for (std::uint32_t i = 0; i < count; ++i)
            Run(Task{fn, data, first + i, counter});
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (std::uint32_t i = 0; i < count; ++i)
            m_queue.push_back(Task{fn, data, first + i, counter});
        m_queued.fetch_add(count, std::memory_order_relaxed);
    }
    if (count >= m_workers.size())
        m_wake.notify_all();
    else
        for (std::uint32_t i = 0; i < count; ++i)
            m_wake.notify_one();
}

void JobSystem::Wait(Counter& counter)
{
    std::uint32_t idle = 0;
    while (!counter.Done())
    {
        if (TryRunOne())
        {
            idle = 0;
            continue;
        }
        // Nothing queued: the counter's last tasks are running on workers. They are short (a frame's
        // chunks); yield rather than sleep, so the caller picks the result up as soon as it is there.
        if (++idle > 64)
            std::this_thread::yield();
    }
}

bool JobSystem::TryRunOne()
{
    Task task;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_queue.empty())
            return false;
        task = m_queue.front();
        m_queue.pop_front();
        m_queued.fetch_sub(1, std::memory_order_relaxed);
    }
    Run(task);
    return true;
}

void JobSystem::Run(const Task& task)
{
    task.fn(task.data, task.index);
    m_tasksRun.fetch_add(1, std::memory_order_relaxed);
    if (task.counter)
        task.counter->m_pending.fetch_sub(1, std::memory_order_acq_rel);
}

void JobSystem::WorkerMain(std::uint32_t worker)
{
    t_worker = worker;
    for (;;)
    {
        // A frame's tasks come in bursts of short ones; a worker woken from sleep starts tens of
        // microseconds late, which is the length of a task. So it looks for more for a moment first.
        const auto spinUntil = std::chrono::steady_clock::now() + std::chrono::microseconds(100);
        while (m_queued.load(std::memory_order_relaxed) == 0 && !m_stopping.load(std::memory_order_relaxed) &&
               std::chrono::steady_clock::now() < spinUntil)
            std::this_thread::yield();
        Task task;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wake.wait(lock, [this] { return m_stop || !m_queue.empty(); });
            if (m_queue.empty())
                return;  // stopping, nothing left
            task = m_queue.front();
            m_queue.pop_front();
            m_queued.fetch_sub(1, std::memory_order_relaxed);
        }
        Run(task);
    }
}

bool FeatureEnabled(const char* environmentSwitch)
{
    static std::mutex mutex;
    static std::unordered_map<std::string, bool> known;
    std::lock_guard<std::mutex> lock(mutex);
    const auto it = known.find(environmentSwitch);
    if (it != known.end())
        return it->second;
    const bool enabled = JobSystem::Instance().Parallel() && !SwitchOff(EnvironmentValue(environmentSwitch));
    known.emplace(environmentSwitch, enabled);
    return enabled;
}

} // namespace ixjobs
