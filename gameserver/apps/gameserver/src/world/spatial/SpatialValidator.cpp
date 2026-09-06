#include "SpatialValidator.h"

#include <sstream>
#include <unordered_set>

#include "../components/MobComponents.h"
#include "../components/NetworkComponents.h"
#include "../components/Tags.h"
#include "../components/TransformComponents.h"
#include "../zone/Zone.h"
#include "SpatialGrid.h"
#include "SpatialTypes.h"

namespace gs::game {

bool ValidateSpatialIndex(Zone& zone, const SpatialGrid& grid, std::string& out_error)
{
    std::size_t expected = 0;

    auto check_one = [&](std::uint32_t net_id, const Position& position) -> bool {
        ++expected;
        if (!grid.Contains(net_id, position)) {
            std::ostringstream message;
            message << "zone " << zone.Id() << ": net " << net_id << " at (" << position.x << ", "
                    << position.y << ") missing/wrong cell in spatial index";
            out_error = message.str();
            return false;
        }
        return true;
    };

    for (const auto& [net_id, binding] : zone.Players()) {
        (void)binding;
        const auto entity = zone.FindEntity(net_id);
        if (!entity.is_valid()) {
            std::ostringstream message;
            message << "zone " << zone.Id() << ": indexed player net " << net_id << " has no entity";
            out_error = message.str();
            return false;
        }
        if (!check_one(net_id, entity.get<Position>())) {
            return false;
        }
    }

    std::unordered_set<std::uint32_t> ghost_nets;
    for (const auto& ghost : zone.Ghosts()) {
        ghost_nets.insert(ghost.snapshot.net_id);
    }
    bool mobs_ok = true;
    zone.World().query<const MobTag, const NetId, const Position>().each(
        [&](const MobTag&, const NetId& id, const Position& pos) {
            if (!mobs_ok || ghost_nets.contains(id.value)) {
                return;
            }
            if (!check_one(id.value, pos)) {
                mobs_ok = false;
            }
        });
    if (!mobs_ok) {
        return false;
    }

    for (const auto& ghost : zone.Ghosts()) {
        if (!check_one(ghost.snapshot.net_id, ghost.snapshot.position)) {
            return false;
        }
    }

    const std::size_t actual = grid.Size();
    if (actual != expected) {
        std::ostringstream message;
        message << "zone " << zone.Id() << ": spatial index holds " << actual << " entries, expected "
                << expected << " (stale entries present)";
        out_error = message.str();
        return false;
    }
    return true;
}

} // namespace gs::game
