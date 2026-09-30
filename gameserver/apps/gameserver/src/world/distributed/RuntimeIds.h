#pragma once

#include <cstdint>

// Explicit process topology identity for the distributed-ready runtime.
// Single-process deployments run as node=1/process=1 and pay nothing for
// this: every routing decision short-circuits on IsLocal().
//
// NOTE on ZoneId: zone ids come from map data (uint32) and are already
// decoupled from the local vector index (zone_index). zone_index stays a
// local-only fast-path cache; it must never cross a process boundary.
// The existing `using ZoneId = std::uint32_t` aliases (Zone.h,
// MigrationQueue.h) are intentionally left untouched.
namespace gs::game {

struct NodeId {
    std::uint32_t value = 1;

    constexpr bool operator==(const NodeId& other) const noexcept = default;
    constexpr bool operator!=(const NodeId& other) const noexcept = default;
};

struct ProcessId {
    std::uint32_t value = 1;

    constexpr bool operator==(const ProcessId& other) const noexcept = default;
    constexpr bool operator!=(const ProcessId& other) const noexcept = default;
};

// Identity of THIS process. Passed by value everywhere (two words).
struct RuntimeIdentity {
    NodeId node;
    ProcessId process;

    constexpr bool operator==(const RuntimeIdentity& other) const noexcept = default;
};

// 16-bit allocation namespace derived from (node, process). Distinct
// processes MUST be configured with distinct namespaces (see §43); the
// default single-process deployment uses namespace 0, which keeps
// GlobalEntityId bit-identical to the local NetId (zero-cost compat).
// TODO(distributed): restarts reuse the local counter block, so a process
// restart can re-issue ids held by stale remote state. Fix with either
// central allocation blocks or a persisted epoch/generation in the high
// bits once multi-process leaves emulation.
inline constexpr std::uint16_t NamespaceFor(NodeId node, ProcessId process) noexcept
{
    return static_cast<std::uint16_t>(((node.value & 0xFFu) << 8u) | (process.value & 0xFFu));
}

inline constexpr std::uint16_t NamespaceFor(RuntimeIdentity identity) noexcept
{
    return NamespaceFor(identity.node, identity.process);
}

} // namespace gs::game
