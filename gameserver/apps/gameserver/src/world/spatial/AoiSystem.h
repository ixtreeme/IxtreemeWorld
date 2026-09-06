#pragma once

#include <cstdint>
#include <vector>

#include "../components/TransformComponents.h"

// AOI answers ONLY: "which net_ids are physically near the viewer?"
// It returns sorted, capped candidate ids. It never decides visibility
// (VisibilitySystem) and never sends anything (ReplicationSystem).
namespace gs::game {

class Zone;

class AoiSystem {
public:
    static void RebuildIndex(Zone& zone);
    static std::vector<std::uint32_t> QueryCandidates(Zone& zone,
                                                      std::uint32_t viewer_net_id,
                                                      const Position& viewer_position);
};

} // namespace gs::game
