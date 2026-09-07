#pragma once

#include <atomic>
#include <cstdint>

// Topology control-plane observability (§25-26). All counters are
// supervisor-written; snapshots are plain data for diagnostics, the bench
// report and admin tooling. Timing totals accumulate per phase so the
// report can show averages (total / matching commits+aborts).
namespace gs::game {

struct PartitionMetrics {
    std::atomic<std::uint64_t> split_attempts{0}; // staged split transactions started
    std::atomic<std::uint64_t> split_commits{0};
    std::atomic<std::uint64_t> split_aborts{0};
    std::atomic<std::uint64_t> split_transfer_failures{0}; // forward transfers failed
    std::atomic<std::uint64_t> split_rollback_failures{0}; // reverse transfers failed on abort
    std::atomic<std::uint64_t> merge_attempts{0};
    std::atomic<std::uint64_t> merge_commits{0};
    std::atomic<std::uint64_t> merge_aborts{0};
    std::atomic<std::uint64_t> merge_transfer_failures{0};
    std::atomic<std::uint64_t> merge_rollback_failures{0};
    std::atomic<std::uint64_t> retire_rejected_nonempty{0}; // RetireZone refused (safety net fired)

    std::atomic<std::uint64_t> split_plan_us{0};
    std::atomic<std::uint64_t> split_transfer_us{0};
    std::atomic<std::uint64_t> split_commit_us{0};
    std::atomic<std::uint64_t> split_rollback_us{0};
    std::atomic<std::uint64_t> merge_plan_us{0};
    std::atomic<std::uint64_t> merge_transfer_us{0};
    std::atomic<std::uint64_t> merge_commit_us{0};
    std::atomic<std::uint64_t> merge_rollback_us{0};

    struct Snapshot {
        std::uint64_t split_attempts = 0;
        std::uint64_t split_commits = 0;
        std::uint64_t split_aborts = 0;
        std::uint64_t split_transfer_failures = 0;
        std::uint64_t split_rollback_failures = 0;
        std::uint64_t merge_attempts = 0;
        std::uint64_t merge_commits = 0;
        std::uint64_t merge_aborts = 0;
        std::uint64_t merge_transfer_failures = 0;
        std::uint64_t merge_rollback_failures = 0;
        std::uint64_t retire_rejected_nonempty = 0;
        std::uint64_t split_plan_us = 0;
        std::uint64_t split_transfer_us = 0;
        std::uint64_t split_commit_us = 0;
        std::uint64_t split_rollback_us = 0;
        std::uint64_t merge_plan_us = 0;
        std::uint64_t merge_transfer_us = 0;
        std::uint64_t merge_commit_us = 0;
        std::uint64_t merge_rollback_us = 0;
    };

    Snapshot TakeSnapshot() const noexcept
    {
        Snapshot snap;
        snap.split_attempts = split_attempts.load(std::memory_order_relaxed);
        snap.split_commits = split_commits.load(std::memory_order_relaxed);
        snap.split_aborts = split_aborts.load(std::memory_order_relaxed);
        snap.split_transfer_failures = split_transfer_failures.load(std::memory_order_relaxed);
        snap.split_rollback_failures = split_rollback_failures.load(std::memory_order_relaxed);
        snap.merge_attempts = merge_attempts.load(std::memory_order_relaxed);
        snap.merge_commits = merge_commits.load(std::memory_order_relaxed);
        snap.merge_aborts = merge_aborts.load(std::memory_order_relaxed);
        snap.merge_transfer_failures = merge_transfer_failures.load(std::memory_order_relaxed);
        snap.merge_rollback_failures = merge_rollback_failures.load(std::memory_order_relaxed);
        snap.retire_rejected_nonempty = retire_rejected_nonempty.load(std::memory_order_relaxed);
        snap.split_plan_us = split_plan_us.load(std::memory_order_relaxed);
        snap.split_transfer_us = split_transfer_us.load(std::memory_order_relaxed);
        snap.split_commit_us = split_commit_us.load(std::memory_order_relaxed);
        snap.split_rollback_us = split_rollback_us.load(std::memory_order_relaxed);
        snap.merge_plan_us = merge_plan_us.load(std::memory_order_relaxed);
        snap.merge_transfer_us = merge_transfer_us.load(std::memory_order_relaxed);
        snap.merge_commit_us = merge_commit_us.load(std::memory_order_relaxed);
        snap.merge_rollback_us = merge_rollback_us.load(std::memory_order_relaxed);
        return snap;
    }
};

} // namespace gs::game
