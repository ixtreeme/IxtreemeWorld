#pragma once

#include <atomic>
#include <cstdint>
#include <functional>

// Shared vocabulary for the routing layer (world/distributed/).
//
// Delivery semantics contract by message type (domain requirements -- no
// transport protocol is implemented here):
//   Movement input : latest-state-wins / sequenced (stale sequence dropped
//                    by last_input_seq; loss is self-healing).
//   Attack command : reliable + ordered within the owning zone's queue.
//                    Local path: the zone command queue. Remote future path:
//                    the migration-style reliable transport, never best-effort.
//   Migration      : reliable, exactly-once EFFECT. At-least-once delivery
//                    plus idempotent processing (MigrationId + committed set)
//                    is the accepted mechanism.
//   Spawn/Despawn  : reliable, supervisor-originated (local command queue).
//   Replication    : unreliable/latest-state acceptable IN THE FUTURE; today
//                    the wire protocol sends full 20 Hz frames (unchanged).
//   Chat/social    : reliable (no system exists yet; contract recorded here).
namespace gs::game {

enum class DeliveryResult : std::uint8_t {
    DeliveredLocal = 0,
    DeliveredRemoteEmulated,
    DestinationUnavailable,
    DestinationDraining,
    RemoteNotSupported,
    QueueFull, // Reserved for the future remote transport (bounded queues).
               // The local path never refuses: FIFO is lossless by design.
};

// Message priority. The local zone queue stays FIFO (no reordering --
// behavior parity); priority matters at the future remote transport, where
// ReplicationLow may be dropped/coalesced under pressure while Critical
// (ownership/migration) must always go through. Threaded through request
// DTOs now so the classification exists when the transport does.
enum class MessagePriority : std::uint8_t {
    Critical = 0,    // ownership transfer, spawn/despawn lifecycle
    Gameplay = 1,    // inputs, combat commands
    ReplicationLow,  // transform/state fanout (droppable in the future)
};

using WakeFn = std::function<void()>;

// Lightweight routing observability. All atomics: produced on the
// supervisor thread, read by diagnostics/benchmarks. Never logged per
// message -- counters only.
struct RoutingMetrics {
    std::atomic<std::uint64_t> local_delivered{0};
    std::atomic<std::uint64_t> remote_emulated{0};
    std::atomic<std::uint64_t> unavailable{0};
    std::atomic<std::uint64_t> draining{0};
    std::atomic<std::uint64_t> remote_unsupported{0};
    std::atomic<std::uint64_t> directory_miss{0};

    struct Snapshot {
        std::uint64_t local_delivered = 0;
        std::uint64_t remote_emulated = 0;
        std::uint64_t unavailable = 0;
        std::uint64_t draining = 0;
        std::uint64_t remote_unsupported = 0;
        std::uint64_t directory_miss = 0;
    };

    Snapshot TakeSnapshot() const noexcept
    {
        return Snapshot{local_delivered.load(std::memory_order_relaxed),
                        remote_emulated.load(std::memory_order_relaxed),
                        unavailable.load(std::memory_order_relaxed),
                        draining.load(std::memory_order_relaxed),
                        remote_unsupported.load(std::memory_order_relaxed),
                        directory_miss.load(std::memory_order_relaxed)};
    }
};

} // namespace gs::game
