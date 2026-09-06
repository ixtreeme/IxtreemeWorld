#pragma once

#include <cstdint>

#include "common/Types.h"

// Network identity components. NetId is the global (cross-zone) identity and
// doubles as the replication id on the wire. SessionRef is informational;
// the authoritative session binding (shared_ptr + visibility set) lives in
// Zone::PlayerBinding, NOT here, so no session lifetime is tied to the ECS.
namespace gs::game {

struct NetId {
    std::uint32_t value = 0;
};

struct SessionRef {
    gs::common::SessionId session = 0;
};

} // namespace gs::game
