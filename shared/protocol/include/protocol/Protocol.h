#pragma once

#include <cstdint>

namespace gs::protocol {

// Highest protocol version this build speaks.
constexpr std::uint32_t kProtocolVersion = 2;
// Oldest client protocol still accepted: version 1 clients keep working
// unchanged (they never receive anything they cannot parse).
constexpr std::uint32_t kMinProtocolVersion = 1;
// 3D-5C1: from this version the gameserver sends layered transform frames
// (opcode 0x12) that carry every entity's layered volume and layer.
constexpr std::uint32_t kLayeredFramesProtocolVersion = 2;

constexpr bool IsSupportedProtocolVersion(std::uint32_t version) noexcept
{
    return version >= kMinProtocolVersion && version <= kProtocolVersion;
}

// The version a session speaks: the client's, capped at this build's.
constexpr std::uint32_t NegotiatedProtocolVersion(std::uint32_t client_version) noexcept
{
    return client_version < kProtocolVersion ? client_version : kProtocolVersion;
}

} // namespace gs::protocol
