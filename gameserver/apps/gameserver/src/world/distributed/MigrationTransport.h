#pragma once

#include <atomic>
#include <cstdint>

#include "Routing.h"
#include "ZoneLocation.h"

// Transport seam for cross-process entity migration. The LOCAL path never
// touches this class (the coordinator executes staged in-process transfer
// directly: zero-copy, zero overhead per §46). This transport only handles
// NON-LOCAL destinations:
//
//   EmulatedLoopback : counts the send and reports loopback; the caller
//                      then executes the local staged path (same process).
//                      This is the benchmark's logical-distribution proof.
//   Disabled (-prod) : reports RemoteNotSupported; the caller keeps the
//                      source authoritative and retries later. No loss.
//
// Remote failure policy (recorded here so the future transport implements
// it instead of rediscovering it):
//   destination down / timeout  -> source stays authoritative; transfer is
//                                  NOT released; retry with backoff. The
//                                  MigrateTo marker persists, so detection
//                                  re-enqueues automatically.
//   duplicate transfer          -> dropped by MigrationId committed-set
//                                  (idempotent exactly-once effect).
//   late ACK (after local retry)-> first commit wins; late duplicate drops
//                                  on the committed-set.
//   source restart              -> process-local NetId block may re-issue;
//                                  see RuntimeIds epoch TODO. Remotes holding
//                                  stale ghosts age them out via the normal
//                                  border-snapshot refresh.
//   destination restart         -> in-flight transfer has no destination;
//                                  source never released it, so authority
//                                  never forked. Retry.
//   stale routing info          -> resolve-then-validate on every attempt;
//                                  a move is only committed against a fresh
//                                  directory read.
//   zone moved to another proc  -> assignment change routes subsequent
//                                  attempts to the new location; in-flight
//                                  local executions finish first (guards).
namespace gs::game {

struct EntityTransfer;

enum class TransportOutcome : std::uint8_t {
    EmulatedLoopback, // Counted; caller proceeds with the local staged path.
    RemoteNotSupported,
};

class MigrationTransport {
public:
    enum class RemoteMode : std::uint8_t {
        Disabled = 0,      // Non-local destinations are not sendable (default).
        EmulatedLoopback,  // Count the send; caller executes locally.
    };

    // May be set from another thread (benchmark emulation setup).
    void SetRemoteMode(RemoteMode mode)
    {
        mode_.store(mode, std::memory_order_relaxed);
    }

    // Classifies a non-local destination. Local destinations must never
    // reach this call (the coordinator short-circuits them first).
    // Parameters are intentionally unused beyond counting: no serialization
    // exists yet, and inventing one now would be speculative (§12).
    TransportOutcome Send(const ZoneLocation& destination,
                          const EntityTransfer& transfer,
                          std::uint64_t migration_id);

    struct Counters {
        std::uint64_t emulated_loopback = 0;
        std::uint64_t remote_unsupported = 0;
    };
    Counters TakeCounters() const;

private:
    std::atomic<RemoteMode> mode_{RemoteMode::Disabled};
    std::atomic<std::uint64_t> emulated_loopback_{0};
    std::atomic<std::uint64_t> remote_unsupported_{0};
};

} // namespace gs::game
