#pragma once

// The engine's one worker pool, shared by every system that runs work in parallel (the frame's
// culling and preparation, physics through its Jolt adapter, ...), in the editor and the built game.
//
// Work is fork-join: a caller submits tasks counted by a Counter (or splits a range with ParallelFor)
// and waits for them; while it waits it runs queued tasks itself. Tasks write only their own output
// (ParallelFor hands each chunk its index, so results merge in a fixed order whatever thread ran them)
// and must not wait on anything but their own child counters.
//
// IX_JOBS=0 runs everything on the calling thread, in submission order, through the same calls;
// IX_JOBS_WORKERS=<n> sets the worker count (default: hardware threads - 1, at most kMaxWorkers).

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

namespace ixjobs
{

// Unfinished tasks of one group. Submit counts a task in; it counts itself out when it has run.
class Counter
{
public:
    bool Done() const { return m_pending.load(std::memory_order_acquire) == 0; }

private:
    friend class JobSystem;
    std::atomic<std::uint32_t> m_pending{0};
};

using TaskFunction = void (*)(void* data, std::uint32_t index);

class JobSystem
{
public:
    static constexpr std::uint32_t kMaxWorkers = 31;

    // The shared pool, started on first use from IX_JOBS / IX_JOBS_WORKERS.
    static JobSystem& Instance();

    // Starts (or restarts) the pool with this many workers; 0 runs everything on the callers.
    void Start(std::uint32_t workers);
    // Lets the workers finish what is queued and joins them. Called before the process tears down
    // what tasks may touch; later work runs on the callers.
    void Stop();

    std::uint32_t WorkerCount() const { return static_cast<std::uint32_t>(m_workers.size()); }
    bool Parallel() const { return !m_workers.empty(); }
    // 0 on any thread that is not one of the pool's workers, 1..WorkerCount() on them.
    static std::uint32_t CurrentWorker();

    // Queues fn(data, index), counted in `counter` (may be null: nobody waits for it). Runs it right
    // away when the pool has no workers.
    void Submit(TaskFunction fn, void* data, std::uint32_t index, Counter* counter);
    // fn(data, i) for i in [first, first + count), queued at once (one lock, one wake-up).
    void SubmitRange(TaskFunction fn, void* data, std::uint32_t first, std::uint32_t count, Counter* counter);
    // Returns once every task counted in `counter` has run; runs queued tasks meanwhile.
    void Wait(Counter& counter);

    // fn(begin, end, chunk) over [0, count) in chunks of about `grain` items (at least 1), in
    // parallel; returns when all have run. Chunk i always covers the same range, so per-chunk
    // outputs merged by chunk index come out the same in parallel and serial runs.
    template <typename Fn>
    void ParallelFor(std::uint32_t count, std::uint32_t grain, Fn&& fn);
    static std::uint32_t ChunkCount(std::uint32_t count, std::uint32_t grain)
    {
        grain = grain == 0 ? 1u : grain;
        return (count + grain - 1u) / grain;
    }

    // Tasks run and time spent in them, over the pool's life (diagnostics).
    std::uint64_t TasksRun() const { return m_tasksRun.load(std::memory_order_relaxed); }

    ~JobSystem();

private:
    struct Task
    {
        TaskFunction fn = nullptr;
        void* data = nullptr;
        std::uint32_t index = 0;
        Counter* counter = nullptr;
    };

    JobSystem() = default;
    void WorkerMain(std::uint32_t worker);
    bool TryRunOne();
    void Run(const Task& task);

    std::vector<std::thread> m_workers;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<Task> m_queue;
    std::atomic<std::uint32_t> m_queued{0};  // m_queue's size, read without the lock by spinning workers
    std::atomic<bool> m_stopping{false};
    bool m_stop = false;
    std::atomic<std::uint64_t> m_tasksRun{0};
};

// Whether an optional parallel path is on: IX_JOBS is not 0, and the named switch (e.g.
// "IX_PARALLEL_CULL") is not 0. Read once per name; for isolating a regression to one group.
bool FeatureEnabled(const char* environmentSwitch);

template <typename Fn>
void JobSystem::ParallelFor(std::uint32_t count, std::uint32_t grain, Fn&& fn)
{
    if (count == 0)
        return;
    grain = grain == 0 ? 1u : grain;
    const std::uint32_t chunks = ChunkCount(count, grain);
    using Body = std::remove_reference_t<Fn>;
    struct Range
    {
        Body* body;
        std::uint32_t count;
        std::uint32_t grain;
    };
    Range range{&fn, count, grain};
    const TaskFunction run = [](void* data, std::uint32_t chunk) {
        const Range& r = *static_cast<const Range*>(data);
        const std::uint32_t begin = chunk * r.grain;
        const std::uint32_t end = begin + r.grain < r.count ? begin + r.grain : r.count;
        (*r.body)(begin, end, chunk);
    };
    if (!Parallel() || chunks == 1)
    {
        for (std::uint32_t chunk = 0; chunk < chunks; ++chunk)
            run(&range, chunk);
        return;
    }
    Counter counter;
    // The first chunk stays with the caller: it would only wait otherwise.
    SubmitRange(run, &range, 1, chunks - 1, &counter);
    run(&range, 0);
    Wait(counter);
}

} // namespace ixjobs
