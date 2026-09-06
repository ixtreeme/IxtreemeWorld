#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_set>
#include <vector>

// Event-driven migration work list. Zone-local movement steps enqueue a
// MigrationRequest when an entity leaves its zone bounds; the supervisor
// drains the queue instead of scanning every entity every tick:
//
//   OLD: O(zones x entities) per supervisor tick
//   NEW: O(actual border crossings)
//
// Classification: TRANSIENT WORK QUEUE (not authority, not an index).
// Identity is by stable NetId + ZoneIds -- never flecs::entity handles.
// Thread-safe: producers are zone workers, consumer is the supervisor.
namespace gs::game {

using ZoneId = std::uint32_t;

struct MigrationRequest {
    std::uint32_t net_id = 0;
    ZoneId source_zone_id = 0;
    ZoneId target_zone_id = 0;
};

class MigrationQueue {
public:
    // Enqueues unless the net is already pending. Returns true when the
    // request is newly pending. Only call when a nonzero target was
    // computed, so the common case (inside bounds) never locks.
    bool TryEnqueue(MigrationRequest request)
    {
        std::lock_guard lock(mutex_);
        if (!pending_nets_.insert(request.net_id).second) {
            return false;
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
    // border detection may enqueue the net again.
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
};

} // namespace gs::game
