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

    // Phase-3 stability controller diagnostics: WHY the controller did not
    // mutate. These make the stability behavior tunable instead of opaque.
    std::atomic<std::uint64_t> merge_candidates_evaluated{0};
    std::atomic<std::uint64_t> merge_suppressed_not_eligible{0};
    std::atomic<std::uint64_t> merge_suppressed_not_sustained{0};
    std::atomic<std::uint64_t> merge_suppressed_recent_split{0};
    std::atomic<std::uint64_t> merge_suppressed_recent_merge{0};
    std::atomic<std::uint64_t> merge_suppressed_post_merge_unsafe{0};
    std::atomic<std::uint64_t> merge_suppressed_min_improvement{0};
    std::atomic<std::uint64_t> merge_suppressed_transaction{0};
    std::atomic<std::uint64_t> split_suppressed_merge_cooldown{0};
    std::atomic<std::uint64_t> split_emergency_bypasses{0};
    // A split and a merge on the same node inside the oscillation window
    // (diagnostic; the directional cooldowns are the hard guards).
    std::atomic<std::uint64_t> oscillation_guard_trips{0};

    // Control-plane phase timings (microseconds, cumulative). The readiness
    // benchmark separates telemetry cost (observe), decision cost (score) and
    // the rest of the control cycle (decide/execute).
    std::atomic<std::uint64_t> control_us{0};   // whole ExecutePartitionControl
    std::atomic<std::uint64_t> observe_us{0};   // ZoneLoadMonitor::Update
    std::atomic<std::uint64_t> score_us{0};     // PartitionScorer calls
    std::atomic<std::uint64_t> migration_us{0}; // supervisor migration processing

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
        std::uint64_t merge_candidates_evaluated = 0;
        std::uint64_t merge_suppressed_not_eligible = 0;
        std::uint64_t merge_suppressed_not_sustained = 0;
        std::uint64_t merge_suppressed_recent_split = 0;
        std::uint64_t merge_suppressed_recent_merge = 0;
        std::uint64_t merge_suppressed_post_merge_unsafe = 0;
        std::uint64_t merge_suppressed_min_improvement = 0;
        std::uint64_t merge_suppressed_transaction = 0;
        std::uint64_t split_suppressed_merge_cooldown = 0;
        std::uint64_t split_emergency_bypasses = 0;
        std::uint64_t oscillation_guard_trips = 0;
        std::uint64_t control_us = 0;
        std::uint64_t observe_us = 0;
        std::uint64_t score_us = 0;
        std::uint64_t migration_us = 0;
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
        snap.merge_candidates_evaluated = merge_candidates_evaluated.load(std::memory_order_relaxed);
        snap.merge_suppressed_not_eligible =
            merge_suppressed_not_eligible.load(std::memory_order_relaxed);
        snap.merge_suppressed_not_sustained =
            merge_suppressed_not_sustained.load(std::memory_order_relaxed);
        snap.merge_suppressed_recent_split =
            merge_suppressed_recent_split.load(std::memory_order_relaxed);
        snap.merge_suppressed_recent_merge =
            merge_suppressed_recent_merge.load(std::memory_order_relaxed);
        snap.merge_suppressed_post_merge_unsafe =
            merge_suppressed_post_merge_unsafe.load(std::memory_order_relaxed);
        snap.merge_suppressed_min_improvement =
            merge_suppressed_min_improvement.load(std::memory_order_relaxed);
        snap.merge_suppressed_transaction =
            merge_suppressed_transaction.load(std::memory_order_relaxed);
        snap.split_suppressed_merge_cooldown =
            split_suppressed_merge_cooldown.load(std::memory_order_relaxed);
        snap.split_emergency_bypasses = split_emergency_bypasses.load(std::memory_order_relaxed);
        snap.oscillation_guard_trips = oscillation_guard_trips.load(std::memory_order_relaxed);
        snap.control_us = control_us.load(std::memory_order_relaxed);
        snap.observe_us = observe_us.load(std::memory_order_relaxed);
        snap.score_us = score_us.load(std::memory_order_relaxed);
        snap.migration_us = migration_us.load(std::memory_order_relaxed);
        return snap;
    }
};

} // namespace gs::game
