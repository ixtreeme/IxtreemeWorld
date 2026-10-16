#include "map/WorldAreas.h"

#include <algorithm>
#include <numeric>

namespace mx::map {

const char* ToString(PvpMode mode) noexcept
{
    switch (mode) {
    case PvpMode::Inherit: return "inherit";
    case PvpMode::Peaceful: return "peaceful";
    case PvpMode::DuelOnly: return "duelOnly";
    case PvpMode::Free: return "free";
    }
    return "?";
}

AreaTable::AreaTable(std::vector<AreaDef> defs)
    : defs_(std::move(defs))
    , slot_(static_cast<std::size_t>(kMaxAreaId) + 1, 0)
    , effective_(defs_.size())
    , depth_(defs_.size(), 0)
{
    for (std::size_t i = 0; i < defs_.size(); ++i) {
        slot_[defs_[i].id] = static_cast<std::uint16_t>(i + 1);
        for (const std::uint32_t volume : defs_[i].volumes) {
            volume_area_.emplace(volume, defs_[i].id);
        }
    }
    for (std::size_t i = 0; i < defs_.size(); ++i) {
        std::size_t depth = 1;
        for (AreaId up = defs_[i].parent_id; up != 0; up = Find(up)->parent_id) {
            ++depth;
        }
        depth_[i] = static_cast<std::uint8_t>(depth);
    }
    // Parents before children, so each child folds over a finished parent.
    std::vector<std::size_t> order(defs_.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return depth_[a] < depth_[b];
    });
    for (const std::size_t i : order) {
        const AreaRules& own = defs_[i].rules;
        const EffectiveAreaRules& up = Rules(defs_[i].parent_id);
        EffectiveAreaRules& out = effective_[i];
        out.pvp = own.pvp == PvpMode::Inherit ? up.pvp : own.pvp;
        const bool own_range = own.level_min != 0 || own.level_max != 0;
        out.level_min = own_range ? own.level_min : up.level_min;
        out.level_max = own_range ? own.level_max : up.level_max;
        out.forbidden = (up.forbidden | own.forbid) & ~own.allow;
        out.respawn = own.respawn ? own.respawn : up.respawn;
        out.music_id = own.music_id != 0 ? own.music_id : up.music_id;
        out.ambient_id = own.ambient_id != 0 ? own.ambient_id : up.ambient_id;
    }
}

const AreaDef* AreaTable::Find(AreaId id) const noexcept
{
    if (id == 0 || id >= slot_.size() || slot_[id] == 0) {
        return nullptr;
    }
    return &defs_[slot_[id] - 1u];
}

const EffectiveAreaRules& AreaTable::WorldDefaults() noexcept
{
    static const EffectiveAreaRules defaults{};
    return defaults;
}

const EffectiveAreaRules& AreaTable::Rules(AreaId id) const noexcept
{
    const AreaDef* def = Find(id);
    return def != nullptr ? effective_[static_cast<std::size_t>(def - defs_.data())] : WorldDefaults();
}

AreaId AreaTable::Parent(AreaId id) const noexcept
{
    const AreaDef* def = Find(id);
    return def != nullptr ? def->parent_id : 0;
}

std::size_t AreaTable::Depth(AreaId id) const noexcept
{
    const AreaDef* def = Find(id);
    return def != nullptr ? depth_[static_cast<std::size_t>(def - defs_.data())] : 0;
}

bool AreaTable::IsWithin(AreaId id, AreaId ancestor) const noexcept
{
    if (ancestor == 0) {
        return false;
    }
    for (AreaId at = id; at != 0; at = Parent(at)) {
        if (at == ancestor) {
            return true;
        }
    }
    return false;
}

AreaId AreaTable::CommonAncestor(AreaId a, AreaId b) const noexcept
{
    std::size_t da = Depth(a);
    std::size_t db = Depth(b);
    if (da == 0 || db == 0) {
        return 0;
    }
    while (da > db) {
        a = Parent(a);
        --da;
    }
    while (db > da) {
        b = Parent(b);
        --db;
    }
    while (a != b) {
        a = Parent(a);
        b = Parent(b);
    }
    return a;
}

AreaId AreaTable::VolumeArea(std::uint32_t volume_id) const noexcept
{
    const auto it = volume_area_.find(volume_id);
    return it != volume_area_.end() ? it->second : 0;
}

} // namespace mx::map
