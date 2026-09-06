#include "ZoneGraph.h"

#include "../spatial/SpatialTypes.h"
#include "Zone.h"

namespace gs::game {

void ZoneGraph::Rebuild(const std::vector<std::unique_ptr<Zone>>& zones)
{
    adjacency_.assign(zones.size(), {});
    for (std::size_t i = 0; i < zones.size(); ++i) {
        for (std::size_t j = 0; j < zones.size(); ++j) {
            if (i == j) {
                continue;
            }
            if (RectsTouchOrOverlap(zones[i]->Bounds(), zones[j]->Bounds())) {
                adjacency_[i].push_back(j);
            }
        }
    }
}

const std::vector<std::size_t>& ZoneGraph::Neighbors(std::size_t zone_index) const
{
    static const std::vector<std::size_t> kEmpty;
    if (zone_index >= adjacency_.size()) {
        return kEmpty;
    }
    return adjacency_[zone_index];
}

} // namespace gs::game
