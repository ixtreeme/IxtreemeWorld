#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_set>
#include <vector>

#include "../distributed/Routing.h"
#include "../distributed/ZoneLocation.h"

// Monotonic migration attempt id, unique per process (paired with the source
// ZoneLocation it is globally unique; see RuntimeIds). Assigned at enqueue;
// the coordinator's committed-set drops late duplicates for exactly-once
// EFFECT. 0 is invalid / never assigned.
namespace gs::game {

struct MigrationId {
    std::uint64_t value = 0;

    constexpr bool IsValid() const noexcept
    {
        return value != 0;
    }
    constexpr bool operator==(const MigrationId& other) const noexcept = default;
    constexpr bool operator!=(const MigrationId& other) const noexcept = default;
};

} // namespace gs::game

namespace std {

template <>
struct hash<gs::game::MigrationId> {
    std::size_t operator()(gs::game::MigrationId id) const noexcept
    {
        return std::hash<std::uint64_t>{}(id.value);
    }
};

} // namespace std

namespace gs::game {

// Event-driven migration work list. Zone-local movement steps enqueue a
// MigrationRequest when an entity leaves its zone bounds; the supervisor
// drains the queue instead of scanning every entity every tick:
//
//   OLD: O(zones x entities) per supervisor tick
//   NEW: O(actual border crossings)
//
// Classification: TRANSIENT WORK QUEUE (not authority, not an index).
// Identity is by stable NetId + ZoneIds (+ MigrationId) -- never
// flecs::entity handles. Thread-safe: producers are zone workers, consumer
// is the supervisor.
struct MigrationRequest {
    std::uint32_t net_id = 0;
    ZoneId source_zone_id = 0;
    ZoneId target_zone_id = 0;
    MigrationId migration_id;
    MessagePriority priority = MessagePriority::Critical; // ownership transfer
};

class MigrationQueue {
public:
    // Enqueues unless the net is already pending. Assigns a fresh
    // MigrationId (unless the request already carries a valid one -- remote
    // redelivery MUST preserve the original id so the coordinator's
    // committed-set recognizes the replay) and returns true when newly
    // pending. Only call when a nonzero target was computed, so the common
    // case (inside bounds) never locks.
    bool TryEnqueue(MigrationRequest request)
    {
        std::lock_guard lock(mutex_);
        if (!pending_nets_.insert(request.net_id).second) {
            return false;
        }
        if (!request.migration_id.IsValid()) {
            request.migration_id = MigrationId{next_id_.fetch_add(1, std::memory_order_relaxed)};
        }
        requests_.push_back(request);
        return true;
    }

    // Moves all queued requests out for processing. The dedup set is kept
    // until Complete() so concurrent producers cannot duplicate in-flight
    // work; a duplicate that slips through is still safe (stale validation
    // drops it on the second execution attempt).
    std::vector<MigrationRequest> TakeAll()
    {
        std::lock_guard lock(mutex_);
        std::vector<MigrationRequest> taken;
        taken.swap(requests_);
        return taken;
    }

    // Marks a dequeued request finished (committed or dropped). A later
    // border detection may enqueue the net again (with a NEW id -- replays
    // of an old id hit the coordinator's committed-set instead).
    void Complete(std::uint32_t net_id)
    {
        std::lock_guard lock(mutex_);
        pending_nets_.erase(net_id);
    }

    std::size_t PendingCount() const
    {
        std::lock_guard lock(mutex_);
        return requests_.size();
    }

    std::vector<std::uint32_t> PendingSnapshot() const
    {
        std::lock_guard lock(mutex_);
        return std::vector<std::uint32_t>(pending_nets_.begin(), pending_nets_.end());
    }

    std::vector<MigrationRequest> RequestSnapshot() const
    {
        std::lock_guard lock(mutex_);
        return requests_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<MigrationRequest> requests_;
    std::unordered_set<std::uint32_t> pending_nets_;
    std::atomic<std::uint64_t> next_id_{1};
};

} // namespace gs::game
