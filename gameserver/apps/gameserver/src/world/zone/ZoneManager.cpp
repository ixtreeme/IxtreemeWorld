#include "ZoneManager.h"

#include "common/Logging.h"

namespace gs::game {

void ZoneManager::BuildFromWorldLogic(const mx::map::WorldLogic& logic, float fallback_extent)
{
    zones_.clear();
    if (!logic.zones.empty()) {
        zones_.reserve(logic.zones.size());
        for (const auto& logic_zone : logic.zones) {
            zones_.push_back(std::make_unique<Zone>(logic_zone.id, logic_zone.name, logic_zone.bounds));
        }
    } else {
        zones_.push_back(std::make_unique<Zone>(1,
                                                "fallback",
                                                mx::map::Rect{0.0f, 0.0f, fallback_extent, fallback_extent}));
    }

    graph_.Rebuild(zones_);

    const auto now = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < zones_.size(); ++i) {
        zones_[i]->NextTick() = now;
        const auto& zone = *zones_[i];
        LOG_INFO("Game sim zone registered: id={} name='{}' bounds=({}, {})-({}, {}) neighbors={}",
                 zone.Id(),
                 zone.Name(),
                 zone.Bounds().min_x,
                 zone.Bounds().min_y,
                 zone.Bounds().max_x,
                 zone.Bounds().max_y,
                 graph_.Neighbors(i).size());
    }
}

void ZoneManager::Clear()
{
    zones_.clear();
    graph_.Rebuild(zones_);
}

std::size_t ZoneManager::FindIndexById(ZoneId id) const
{
    for (std::size_t i = 0; i < zones_.size(); ++i) {
        if (zones_[i]->Id() == id) {
            return i;
        }
    }
    return zones_.size();
}

std::size_t ZoneManager::FindIndexForPosition(float world_x, float world_y) const
{
    for (std::size_t i = 0; i < zones_.size(); ++i) {
        if (zones_[i]->Bounds().Contains(world_x, world_y)) {
            return i;
        }
    }
    return zones_.size();
}

const std::vector<std::size_t>& ZoneManager::NeighborsOf(std::size_t zone_index) const
{
    return graph_.Neighbors(zone_index);
}

bool ZoneManager::AnyTickInProgress() const
{
    for (const auto& zone : zones_) {
        if (zone->TickInProgress().load(std::memory_order_acquire)) {
            return true;
        }
    }
    return false;
}

} // namespace gs::game
