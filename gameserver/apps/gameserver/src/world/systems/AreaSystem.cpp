#include "AreaSystem.h"

#include "common/Logging.h"
#include "protocol/Protocol.h"

#include "../components/AreaComponents.h"
#include "../components/LayerComponents.h"
#include "../components/TransformComponents.h"
#include "../replication/ProtocolEncoder.h"
#include "../terrain/TerrainService.h"
#include "../zone/Zone.h"

namespace gs::game {

std::optional<mx::map::AreaId> ResolveArea(const TerrainService& terrain,
                                           const mx::map::WorldLogic& logic,
                                           const mx::map::AreaTable* areas,
                                           float x,
                                           float y,
                                           std::uint32_t volume_id) noexcept
{
    if (areas == nullptr) {
        for (const auto& area : logic.zones) {
            if (area.bounds.ContainsHalfOpen(x, y)) {
                return area.id;
            }
        }
        return mx::map::AreaId{0};
    }
    if (volume_id != 0) {
        if (const mx::map::AreaId whole = areas->VolumeArea(volume_id); whole != 0) {
            return whole;
        }
    }
    const auto sample = terrain.Area(x, y);
    switch (sample.status) {
    case mx::map::TerrainStatus::Ok:
        return sample.area;
    case mx::map::TerrainStatus::OutsideWorld:
        return mx::map::AreaId{0};
    case mx::map::TerrainStatus::NotResident:
    case mx::map::TerrainStatus::InvalidData:
        break;
    }
    return std::nullopt;
}

void AreaSystem::Step(Zone& zone, ZoneTickContext& ctx)
{
    for (const auto& [net_id, binding] : zone.Players()) {
        const auto entity = zone.FindEntity(net_id);
        if (!entity.is_valid() || !entity.has<Position>()) {
            continue;
        }
        const auto position = entity.get<Position>();
        const auto* layer = entity.try_get<LayerPresence>();
        const std::uint32_t volume = layer != nullptr ? layer->volume_id : 0;
        const auto* held = entity.try_get<AreaPresence>();
        if (held != nullptr && held->known && held->x == position.x && held->y == position.y &&
            held->volume_id == volume) {
            continue; // not moved since its last resolution
        }
        const auto area = ResolveArea(ctx.terrain, ctx.world_logic, ctx.areas, position.x, position.y, volume);
        if (!area) {
            continue; // no terrain data yet: keep the previous area, ask again
        }
        const bool first = held == nullptr || !held->known;
        const mx::map::AreaId from = first ? 0 : held->area;
        entity.set<AreaPresence>(AreaPresence{*area, true, position.x, position.y, volume});
        if (first || from != *area) {
            if (ctx.area_transitions != nullptr) {
                ctx.area_transitions->fetch_add(1, std::memory_order_relaxed);
            }
            LOG_DEBUG("area: player net {} {} -> {}{}", net_id, from, *area, first ? " (first)" : "");
            // Protocol 3 clients learn the chain and the effective rules;
            // older clients never receive a message they cannot parse.
            if (binding.session && ctx.send &&
                binding.session->ProtocolVersion() >= gs::protocol::kAreaProtocolVersion) {
                ctx.send(binding.session, MakeAreaChanged(ctx.areas, ctx.world_logic, *area));
            }
        }
    }
}

} // namespace gs::game
