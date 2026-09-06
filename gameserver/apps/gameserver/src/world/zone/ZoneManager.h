#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "map/MapData.h"

#include "Zone.h"
#include "ZoneGraph.h"

// Owns zone lifetimes and answers zone-lookup questions. Knows nothing about
// gameplay, threads, or networking: creation/lookup/destruction only.
namespace gs::game {

class ZoneManager {
public:
    void BuildFromWorldLogic(const mx::map::WorldLogic& logic, float fallback_extent);
    void Clear();

    std::size_t ZoneCount() const noexcept
    {
        return zones_.size();
    }
    Zone& GetZone(std::size_t index)
    {
        return *zones_.at(index);
    }
    const Zone& GetZone(std::size_t index) const
    {
        return *zones_.at(index);
    }

    // Returns ZoneCount() when not found (matches old FindZone* semantics).
    std::size_t FindIndexById(ZoneId id) const;
    std::size_t FindIndexForPosition(float world_x, float world_y) const;

    const std::vector<std::size_t>& NeighborsOf(std::size_t zone_index) const;
    bool AnyTickInProgress() const;

    ZoneGraph& Graph() noexcept
    {
        return graph_;
    }

private:
    std::vector<std::unique_ptr<Zone>> zones_;
    ZoneGraph graph_;
};

} // namespace gs::game
