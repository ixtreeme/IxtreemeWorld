#include "SnapshotBuilder.h"

#include "../components/CombatComponents.h"
#include "../components/MobComponents.h"
#include "../components/MovementComponents.h"
#include "../components/NetworkComponents.h"
#include "../components/Tags.h"
#include "../components/TransformComponents.h"
#include "../zone/Zone.h"

namespace gs::game {

BorderEntitySnapshot BuildPlayerSnapshot(Zone& zone, flecs::entity entity)
{
    BorderEntitySnapshot snapshot;
    snapshot.net_id = entity.get<NetId>().value;
    snapshot.position = entity.get<Position>();
    snapshot.heading = entity.get<Heading>();
    snapshot.move_state = entity.get<MoveIntent>().state;
    const auto hp = entity.get<Hp>();
    snapshot.hp_current = hp.current;
    snapshot.hp_max = hp.max;
    snapshot.mob_type_id = 0;
    snapshot.level = 1;
    if (const auto* binding = zone.FindPlayer(snapshot.net_id)) {
        snapshot.name = binding->character.name;
        snapshot.class_id = binding->character.class_id;
    }
    return snapshot;
}

BorderEntitySnapshot BuildMobSnapshot(flecs::entity entity)
{
    BorderEntitySnapshot snapshot;
    snapshot.net_id = entity.get<NetId>().value;
    snapshot.position = entity.get<Position>();
    snapshot.heading = entity.get<Heading>();
    snapshot.move_state = entity.get<MoveIntent>().state;
    const auto hp = entity.get<Hp>();
    snapshot.hp_current = hp.current;
    snapshot.hp_max = hp.max;
    snapshot.mob_type_id = entity.get<MobTypeRef>().id;
    const auto profile = entity.get<MobProfile>();
    snapshot.name = profile.name;
    snapshot.class_id = static_cast<std::uint16_t>(profile.model_id);
    snapshot.level = profile.level == 0 ? 1 : profile.level;
    return snapshot;
}

std::optional<BorderEntitySnapshot> ResolveVisibleSnapshot(Zone& zone, std::uint32_t net_id)
{
    const auto entity = zone.FindEntity(net_id);
    if (entity.is_valid()) {
        if (entity.has<PlayerTag>()) {
            return BuildPlayerSnapshot(zone, entity);
        }
        if (entity.has<MobTag>()) {
            return BuildMobSnapshot(entity);
        }
        return std::nullopt;
    }
    for (const auto& ghost : zone.Ghosts()) {
        if (ghost.snapshot.net_id == net_id) {
            return ghost.snapshot;
        }
    }
    return std::nullopt;
}

} // namespace gs::game
