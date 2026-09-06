#pragma once

#include <cstdint>
#include <functional>

#include "RuntimeIds.h"

// Stable, process-independent entity identity for everything that crosses a
// zone/process/node boundary: routing, migration, ghosts, and -- in the
// future -- party/guild/trade/ship-crew references (§22). Gameplay code must
// reference remote entities by this id, never by flecs::entity.
//
// Layout: [16-bit allocation namespace | 48-bit local id]. The local 32-bit
// NetId occupies the low bits, so with namespace 0 (single-process default)
// a GlobalEntityId is bit-identical to the NetId: zero-cost compatibility,
// no wire-format change. 0 is invalid, matching the existing NetId model.
//
// Cross-zone references (§22): party/guild/trade/ship-crew systems must key
// remote entities by this id (resolvable via WorldDirectory + OwnerMap),
// never by flecs::entity. No such systems exist yet; the identity type is
// the seam they will build on.
namespace gs::game {

struct GlobalEntityId {
    std::uint64_t value = 0;

    constexpr bool IsValid() const noexcept
    {
        return value != 0;
    }
    constexpr bool operator==(const GlobalEntityId& other) const noexcept = default;
    constexpr bool operator!=(const GlobalEntityId& other) const noexcept = default;
    constexpr bool operator<(const GlobalEntityId& other) const noexcept
    {
        return value < other.value;
    }
};

inline constexpr GlobalEntityId ToGlobalEntityId(std::uint32_t net_id,
                                                 std::uint16_t namespace_id) noexcept
{
    return GlobalEntityId{(static_cast<std::uint64_t>(namespace_id) << 48u) |
                          static_cast<std::uint64_t>(net_id)};
}

inline constexpr std::uint32_t ToNetId(GlobalEntityId id) noexcept
{
    return static_cast<std::uint32_t>(id.value & 0xFFFFFFFFu);
}

inline constexpr std::uint16_t NamespaceOf(GlobalEntityId id) noexcept
{
    return static_cast<std::uint16_t>((id.value >> 48u) & 0xFFFFu);
}

} // namespace gs::game

namespace std {

template <>
struct hash<gs::game::GlobalEntityId> {
    std::size_t operator()(gs::game::GlobalEntityId id) const noexcept
    {
        return std::hash<std::uint64_t>{}(id.value);
    }
};

} // namespace std
