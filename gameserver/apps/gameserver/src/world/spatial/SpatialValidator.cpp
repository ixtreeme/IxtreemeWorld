#include "SpatialValidator.h"

#include <sstream>
#include <unordered_set>

#include "../components/GridSlot.h"
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

    // Phase 5D index audit: every entry's stored position (the value the AOI
    // radius scan reads) must equal the authoritative position, and the
    // entity's GridSlot must resolve back to this exact cell/slot. A stale
    // stored position is a silent AOI false negative, so it is a hard fail.
    bool entries_ok = true;
    grid.ForEachEntry([&](const GridEntry& entry, std::int64_t cell_key, std::size_t slot_index) {
        if (!entries_ok) {
            return;
        }
        const auto entity = zone.FindEntity(entry.net_id);
        if (entity.is_valid() && entity.has<Position>()) {
            const auto pos = entity.get<Position>();
            if (pos.x != entry.x || pos.y != entry.y || pos.z != entry.z) {
                std::ostringstream message;
                message << "zone " << zone.Id() << ": grid entry net " << entry.net_id
                        << " stored position (" << entry.x << ", " << entry.y << ", " << entry.z
                        << ") != authority (" << pos.x << ", " << pos.y << ", " << pos.z << ")";
                out_error = message.str();
                entries_ok = false;
                return;
            }
        } else if (const GhostRecord* ghost = zone.FindGhost(entry.net_id)) {
            if (ghost->snapshot.position.x != entry.x || ghost->snapshot.position.y != entry.y ||
                ghost->snapshot.position.z != entry.z) {
                std::ostringstream message;
                message << "zone " << zone.Id() << ": grid entry ghost net " << entry.net_id
                        << " stored position does not match the ghost snapshot";
                out_error = message.str();
                entries_ok = false;
                return;
            }
        } else {
            std::ostringstream message;
            message << "zone " << zone.Id() << ": grid entry net " << entry.net_id
                    << " has no resident or ghost (stale index entry)";
            out_error = message.str();
            entries_ok = false;
            return;
        }
        const auto* slot = entity.is_valid() ? entity.try_get<GridSlot>() : nullptr;
        if (slot == nullptr) {
            if (entity.is_valid()) {
                std::ostringstream message;
                message << "zone " << zone.Id() << ": grid entry net " << entry.net_id
                        << " has no GridSlot bookkeeping";
                out_error = message.str();
                entries_ok = false;
            }
            return;
        }
        if (slot->cell_key != cell_key || slot->index != slot_index) {
            std::ostringstream message;
            message << "zone " << zone.Id() << ": net " << entry.net_id
                    << " GridSlot mismatch (slot cell=" << slot->cell_key << " index=" << slot->index
                    << ", entry cell=" << cell_key << " index=" << slot_index << ")";
            out_error = message.str();
            entries_ok = false;
        }
    });
    if (!entries_ok) {
        return false;
    }
    return true;
}

} // namespace gs::game
