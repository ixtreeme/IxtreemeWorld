#pragma once

#include <cstdint>

#include "RuntimeIds.h"

// Explicit location of a zone in the (future) cluster topology. Answers
// "WHERE is this zone?" without touching any runtime object -- the routing
// layer branches on IsLocal() and gameplay code never sees the difference.
//
// A ZoneLocation is plain data: comparable, hashable, serializable, safe to
// store in queues, DTOs and (later) directory snapshots.
namespace gs::game {

using ZoneId = std::uint32_t;

struct ZoneLocation {
    NodeId node;
    ProcessId process;
    ZoneId zone = 0;

    constexpr bool operator==(const ZoneLocation& other) const noexcept = default;
    constexpr bool operator!=(const ZoneLocation& other) const noexcept = default;
};

inline constexpr bool IsLocal(ZoneLocation location, RuntimeIdentity identity) noexcept
{
    return location.node == identity.node && location.process == identity.process;
}

inline ZoneLocation LocalZoneLocation(RuntimeIdentity identity, ZoneId zone) noexcept
{
    return ZoneLocation{identity.node, identity.process, zone};
}

} // namespace gs::game

namespace std {

template <>
struct hash<gs::game::ZoneLocation> {
    std::size_t operator()(gs::game::ZoneLocation location) const noexcept
    {
        // node/process are small; zone carries the entropy.
        return (static_cast<std::size_t>(location.node.value) * 1315423911u) ^
               (static_cast<std::size_t>(location.process.value) * 1315423911u >> 7u) ^
               static_cast<std::size_t>(location.zone);
    }
};

} // namespace std
