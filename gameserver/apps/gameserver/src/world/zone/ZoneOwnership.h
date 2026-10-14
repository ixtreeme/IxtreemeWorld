#pragma once

#include <thread>

// RAII zone write ownership. Invariants (unchanged from SimWorld):
//  1. A zone is writable by at most one thread at a time.
//  2. Direct writes into another zone's flecs world are forbidden;
//     cross-zone effects travel only via messages/handoff.
// Violations abort the process (fail-fast, as before).
namespace gs::game {

class Zone;

class ZoneWriteGuard {
public:
    ZoneWriteGuard(Zone& zone, const char* operation);
    ~ZoneWriteGuard();

    ZoneWriteGuard(const ZoneWriteGuard&) = delete;
    ZoneWriteGuard& operator=(const ZoneWriteGuard&) = delete;

private:
    Zone& zone_;
};

void AssertZoneOwner(const Zone& zone, const char* operation);

[[noreturn]] void FailZoneOwnerCheck(std::uint32_t zone_id,
                                    const char* zone_name,
                                    const char* operation,
                                    std::thread::id owner,
                                    std::thread::id caller);

} // namespace gs::game
