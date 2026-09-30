#include "ZoneOwnership.h"

#include <sstream>

#include "common/Logging.h"

#include "Zone.h"

namespace gs::game {

ZoneWriteGuard::ZoneWriteGuard(Zone& zone, const char* operation)
    : zone_(zone)
{
    const auto caller = std::this_thread::get_id();
    auto expected_owner = std::thread::id{};
    if (!zone_.OwnerThreadId().compare_exchange_strong(expected_owner,
                                                       caller,
                                                       std::memory_order_acq_rel,
                                                       std::memory_order_acquire)) {
        FailZoneOwnerCheck(zone_.Id(), zone_.Name().c_str(), operation, expected_owner, caller);
    }
    AssertZoneOwner(zone_, operation);
}

ZoneWriteGuard::~ZoneWriteGuard()
{
    AssertZoneOwner(zone_, "ZoneWriteGuard release");
    zone_.OwnerThreadId().store(std::thread::id{}, std::memory_order_release);
}

void AssertZoneOwner(const Zone& zone, const char* operation)
{
    const auto owner = zone.OwnerThreadId().load(std::memory_order_acquire);
    const auto caller = std::this_thread::get_id();
    if (owner != caller) {
        FailZoneOwnerCheck(zone.Id(), zone.Name().c_str(), operation, owner, caller);
    }
}

[[noreturn]] void FailZoneOwnerCheck(std::uint32_t zone_id,
                                    const char* zone_name,
                                    const char* operation,
                                    std::thread::id owner,
                                    std::thread::id caller)
{
    std::ostringstream owner_stream;
    std::ostringstream caller_stream;
    owner_stream << owner;
    caller_stream << caller;
    LOG_ERROR("ZoneWorld accessed by non-owner thread (invariant violation): zone={} ('{}') operation={} owner={} caller={}",
              zone_id,
              zone_name ? zone_name : "?",
              operation ? operation : "?",
              owner_stream.str(),
              caller_stream.str());
    std::abort();
}

} // namespace gs::game
