#pragma once

#include <cassert>
#include <cstdint>

#include "../WorldConstants.h"

// Global network identity allocation. Player ids and mob ids live in disjoint
// ranges so a NetId is unambiguous across zones without a (zone, local) pair.
namespace gs::game {

class NetIdAllocator {
public:
    std::uint32_t AllocatePlayerNetId()
    {
        assert(next_player_net_id_ > 0 && next_player_net_id_ < kFirstMobNetId);
        const std::uint32_t value = next_player_net_id_++;
        assert(value < kFirstMobNetId);
        return value;
    }

    std::uint32_t AllocateMobNetId()
    {
        assert(next_mob_net_id_ >= kFirstMobNetId);
        return next_mob_net_id_++;
    }

private:
    std::uint32_t next_player_net_id_ = 1;
    std::uint32_t next_mob_net_id_ = kFirstMobNetId;
};

} // namespace gs::game
