// The worker pool: chunked ranges, nested waits, counters across many tasks, and the serial mode
// giving the same per-chunk results.

#include "JobSystem.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <vector>

namespace
{
int g_failures = 0;

void Check(bool condition, const char* what)
{
    if (!condition)
    {
        std::printf("FAILED: %s\n", what);
        ++g_failures;
    }
}

// Sums [0, count) per chunk into per-chunk slots, then merges them in chunk order.
std::vector<std::uint64_t> ChunkSums(ixjobs::JobSystem& jobs, std::uint32_t count, std::uint32_t grain)
{
    std::vector<std::uint64_t> sums(ixjobs::JobSystem::ChunkCount(count, grain), 0);
    jobs.ParallelFor(count, grain, [&](std::uint32_t begin, std::uint32_t end, std::uint32_t chunk) {
        std::uint64_t sum = 0;
        for (std::uint32_t i = begin; i < end; ++i)
            sum += i;
        sums[chunk] = sum;
    });
    return sums;
}

void RunChecks(ixjobs::JobSystem& jobs, const char* mode)
{
    std::printf("[%s] workers=%u\n", mode, jobs.WorkerCount());

    // Every index visited exactly once.
    constexpr std::uint32_t kCount = 100000;
    std::vector<std::atomic<std::uint32_t>> visits(kCount);
    jobs.ParallelFor(kCount, 777, [&](std::uint32_t begin, std::uint32_t end, std::uint32_t) {
        for (std::uint32_t i = begin; i < end; ++i)
            visits[i].fetch_add(1, std::memory_order_relaxed);
    });
    bool once = true;
    for (const auto& v : visits)
        once = once && v.load() == 1;
    Check(once, "ParallelFor visits each index once");

    // Per-chunk results are the same whatever ran them.
    const std::vector<std::uint64_t> sums = ChunkSums(jobs, kCount, 1000);
    std::uint64_t expected = 0;
    for (std::uint32_t i = 0; i < kCount; ++i)
        expected += i;
    Check(std::accumulate(sums.begin(), sums.end(), std::uint64_t{0}) == expected, "chunk sums add up");
    Check(sums.front() == 499500u, "chunk 0 covers [0, 1000)");

    // Nested: a task's own ParallelFor waits by running tasks, not by blocking a worker.
    std::atomic<std::uint64_t> nested{0};
    jobs.ParallelFor(16, 1, [&](std::uint32_t, std::uint32_t, std::uint32_t outer) {
        jobs.ParallelFor(64, 4, [&](std::uint32_t begin, std::uint32_t end, std::uint32_t) {
            nested.fetch_add((end - begin) * (outer + 1), std::memory_order_relaxed);
        });
    });
    Check(nested.load() == 64u * (16u * 17u / 2u), "nested ParallelFor");

    // Submit + Wait with a counter.
    ixjobs::Counter counter;
    std::atomic<std::uint32_t> ran{0};
    struct Data
    {
        std::atomic<std::uint32_t>* ran;
    } data{&ran};
    for (std::uint32_t i = 0; i < 1000; ++i)
    {
        jobs.Submit([](void* d, std::uint32_t) { static_cast<Data*>(d)->ran->fetch_add(1); }, &data, i, &counter);
    }
    jobs.Wait(counter);
    Check(ran.load() == 1000u && counter.Done(), "Submit/Wait runs every task");
}
} // namespace

int main()
{
    ixjobs::JobSystem& jobs = ixjobs::JobSystem::Instance();
    jobs.Start(4);
    RunChecks(jobs, "parallel");
    const std::vector<std::uint64_t> parallelSums = ChunkSums(jobs, 54321, 333);

    jobs.Start(0);
    RunChecks(jobs, "serial");
    const std::vector<std::uint64_t> serialSums = ChunkSums(jobs, 54321, 333);
    Check(parallelSums == serialSums, "serial and parallel per-chunk results match");

    jobs.Start(3);
    jobs.Stop();
    Check(!jobs.Parallel(), "stopped pool runs work on the caller");
    std::atomic<std::uint32_t> inline_runs{0};
    jobs.ParallelFor(10, 1, [&](std::uint32_t, std::uint32_t, std::uint32_t) { inline_runs.fetch_add(1); });
    Check(inline_runs.load() == 10u, "ParallelFor after Stop");

    std::printf(g_failures == 0 ? "JobSystemTest: all passed\n" : "JobSystemTest: %d failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
