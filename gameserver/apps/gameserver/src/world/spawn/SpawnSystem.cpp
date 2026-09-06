#include "SpawnSystem.h"

#include "common/Logging.h"

#include "../components/AiComponents.h"
#include "../components/CombatComponents.h"
#include "../components/MigrationComponents.h"
#include "../components/MobComponents.h"
#include "../components/MovementComponents.h"
#include "../components/NetworkComponents.h"
#include "../components/Tags.h"
#include "../WorldConstants.h"
#include "../zone/Zone.h"
#include "MobPrototypeRegistry.h"
#include "SpawnLoader.h"
#include "SpawnRandom.h"

namespace gs::game {

void SpawnSystem::SpawnPlayer(Zone& zone,
                              std::shared_ptr<gs::network::Session> session,
                              gs::db::Character character,
                              const Position& position,
                              std::uint32_t net_id)
{
    const auto session_id = session ? session->Id() : 0;
    auto entity = zone.World()
                      .entity()
                      .set<Position>(position)
                      .set<Heading>({})
                      .set<Velocity>({})
                      .set<MoveIntent>({})
                      .set<MoveSpeed>({kPlayerWalkSpeed, kPlayerRunSpeed})
                      .set<Hp>({kPlayerHpMax, kPlayerHpMax})
                      .set<CombatStats>({kPlayerDamage, kPlayerDefense, kPlayerAttackRange, kPlayerAttackCooldown})
                      .set<AttackCooldown>({})
                      .set<NetId>({net_id})
                      .set<SessionRef>({session_id})
                      .set<MigrateTo>({0})
                      .add<PlayerTag>();

    Zone::PlayerBinding binding;
    binding.session = std::move(session);
    binding.character = std::move(character);
    zone.InsertPlayerBinding(net_id, std::move(binding));
    zone.IndexEntity(net_id, entity);
    zone.Grid().Insert(net_id, position);
    zone.RefreshResidentCounts();
}

void SpawnSystem::SpawnMob(Zone& zone,
                           const MobSpawnPoint& spawn,
                           std::size_t spawn_point_index,
                           const MobTypeDefinition& type,
                           const Position& position,
                           std::uint32_t net_id)
{
    auto& rng = zone.MobRng(net_id, type.id);

    WanderState wander;
    wander.mode = WanderState::Mode::Idle;
    wander.spawn_center = Vec2{spawn.x, spawn.y};
    wander.spawn_radius = spawn.radius;
    wander.target = Vec2{position.x, position.y};
    wander.timer = RandomRange(rng, type.wander_idle_min, type.wander_idle_max);

    auto entity = zone.World()
                      .entity()
                      .set<Position>(position)
                      .set<Heading>({})
                      .set<Velocity>({})
                      .set<MoveIntent>({})
                      .set<MoveSpeed>({type.wander_speed, type.wander_speed})
                      .set<WanderState>(wander)
                      .set<Hp>({static_cast<float>(type.hp_max), static_cast<float>(type.hp_max)})
                      .set<CombatStats>({static_cast<float>(type.damage),
                                         type.defense,
                                         type.attack_range,
                                         type.attack_cooldown})
                      .set<AttackCooldown>({})
                      .set<NetId>({net_id})
                      .set<MobTypeRef>({type.id})
                      .set<MobSpawnRef>({spawn_point_index})
                      .set<MobProfile>({type.model_id, 1, type.name})
                      .set<MigrateTo>({0})
                      .add<MobTag>();

    zone.IndexEntity(net_id, entity);
    zone.Grid().Insert(net_id, position);
    zone.RefreshResidentCounts();

    LOG_INFO("mob spawned: net_id={} type={} spawn_point={} zone={} pos=({}, {}, {})",
             net_id,
             type.id,
             spawn_point_index,
             zone.Id(),
             position.x,
             position.y,
             position.z);
}

} // namespace gs::game
