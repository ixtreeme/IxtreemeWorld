#include "MigrationTransport.h"

namespace gs::game {

TransportOutcome MigrationTransport::Send(const ZoneLocation& /*destination*/,
                                          const EntityTransfer& /*transfer*/,
                                          std::uint64_t /*migration_id*/)
{
    if (mode_.load(std::memory_order_relaxed) == RemoteMode::EmulatedLoopback) {
        emulated_loopback_.fetch_add(1, std::memory_order_relaxed);
        return TransportOutcome::EmulatedLoopback;
    }
    remote_unsupported_.fetch_add(1, std::memory_order_relaxed);
    return TransportOutcome::RemoteNotSupported;
}

MigrationTransport::Counters MigrationTransport::TakeCounters() const
{
    return Counters{emulated_loopback_.load(std::memory_order_relaxed),
                    remote_unsupported_.load(std::memory_order_relaxed)};
}

} // namespace gs::game
